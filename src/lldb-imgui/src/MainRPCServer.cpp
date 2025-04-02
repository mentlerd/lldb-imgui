#include "App.h"

#include "lldb/API/LLDB.h"

#include "SDL3/SDL.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/ringbuffer_sink.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <signal.h>
#include <unistd.h>

#include <vector>
#include <thread>
#include <span>

// MARK: Custom main loop
namespace lldb::imgui {
namespace {

Uint32 g_interruptEvent;

std::unique_ptr<App> g_app;
SDL_AppResult g_appState;

void Quit() {
    g_app->Quit();
    g_app.reset();

    SDL_Quit();
}
void Init() {
    g_interruptEvent = SDL_RegisterEvents(1);

    g_app = std::make_unique<App>();
    g_appState = g_app->Init({});

    if (g_appState != SDL_APP_CONTINUE) {
        Quit();
        return;
    }

    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        Quit();
    }
}

void SocketIdle() {
    if (g_app == nullptr) {
        return;
    }

    SDL_Event event;

    while (true) {
        if (!SDL_WaitEvent(&event)) {
            break;
        }
        if (event.type == g_interruptEvent) {
            return;
        }

        g_appState = g_app->Event(event);
        if (g_appState != SDL_APP_CONTINUE) {
            break;
        }

        g_appState = g_app->Iterate();
        if (g_appState != SDL_APP_CONTINUE) {
            break;
        }
    }

    Quit();
}

void SocketIdleInterrupt() {
    SDL_Event event {
        .type = g_interruptEvent
    };
    SDL_PushEvent(&event);
}

}
}


// MARK: - Injection
namespace lldb::imgui {
namespace {

auto loggerBuffer = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);
auto logger = spdlog::logger("Injection", loggerBuffer);

static constexpr std::string_view kSocketVTable = "_ZTVN10rpc_common19RPCConnectionSocketE";
static constexpr std::string_view kSocketIsConnected = "_ZNK10rpc_common19RPCConnectionSocket11IsConnectedEv";
static constexpr std::string_view kSocketRead = "_ZN10rpc_common19RPCConnectionSocket4ReadERNSt3__112basic_stringIhNS1_11char_traitsIhEENS1_9allocatorIhEEEEb";

int g_socketFD = -1;
int* g_socketFDPtr = nullptr;

std::atomic<bool> g_hijackedReadCalled;

std::mutex g_readBufferMutex;
std::condition_variable g_readBufferDataAvailable;

std::string g_readBuffer;

bool IsConnected(void* socket) {
    if (g_hijackedReadCalled.load()) {
        return true;
    }

    // Who's calling?
    void* caller = __builtin_extract_return_addr(__builtin_return_address(0));

    Dl_info info;
    if (!dladdr(caller, &info)) {
        std::terminate();
    }
    if (std::string_view(info.dli_sname) == kSocketRead) {
        // We are called from the original `Read()` function which we are trying
        // to escape. Pretend that the socket is closed
        *g_socketFDPtr = -1;
        return false;
    }

    // As far as anyone else is concerned, we are open
    *g_socketFDPtr = g_socketFD;
    return true;
}

size_t Read(void* socket, std::string& buffer, bool append) {
    if (g_hijackedReadCalled.load() == false) {
        g_hijackedReadCalled.store(true);

        Init();
    }

    while (true) {
        std::string data;
        {
            std::unique_lock lock(g_readBufferMutex);

            if (g_app) {
                g_readBufferDataAvailable.wait_for(lock, std::chrono::milliseconds(0));
            } else {
                g_readBufferDataAvailable.wait(lock);
            }

            data = std::exchange(g_readBuffer, std::string());
        }

        if (auto read = data.size()) {
            if (!append) {
                buffer.clear();
            }
            buffer.append(std::move(data));
            return read;
        }

        SocketIdle();
    }
}

void ReadThread() {
    char buffer[1024];

    while (true) {
        auto n_read = read(g_socketFD, buffer, sizeof(buffer));
        if (n_read == 0) {
            continue;
        }

        if (n_read == -1) {
            std::terminate();
        }

        {
            std::scoped_lock lock(g_readBufferMutex);

            g_readBufferDataAvailable.notify_one();
            g_readBuffer.append(std::string_view(buffer, n_read));
        }

        SocketIdleInterrupt();
    }
}

template<typename T>
T* LookupGlobalPointerTo(const char* symbol) {
    auto addr = reinterpret_cast<void*>(dlsym(RTLD_MAIN_ONLY, symbol));
    if (!addr) {
        logger.info("'{}' -> nullptr", symbol);
        return nullptr;
    }

    auto value = *reinterpret_cast<void**>(addr);

    if (!malloc_zone_from_ptr(value)) {
        logger.info("'{}' -> {} = {} // not malloc'd!", symbol, addr, value);
        return nullptr;
    }

    size_t actual = malloc_size(value);
    logger.info("'{}' -> {} = {} // malloc({})", symbol, addr, value, actual);

    size_t minSize = sizeof(T);
    size_t maxSize = sizeof(T) * 1.5;

    if (actual < minSize) {
        logger.info(" ... expected at least {}", minSize);
        return nullptr;
    }
    if (actual > maxSize) {
        logger.info(" ... expected at max {}", maxSize);
        return nullptr;
    }

    return reinterpret_cast<T*>(value);
}

bool Inject() {
    auto* connections = LookupGlobalPointerTo<std::vector<std::shared_ptr<void>>>("g_connections");
    auto* mutex = LookupGlobalPointerTo<std::mutex>("g_connections_mutex_ptr");

    if (!connections || !mutex) {
        logger.error("Required symbols for RPC connections not found");
        return false;
    }

    // This has to be done before pausing the main thread lest we deadlock ourselves
    std::scoped_lock lock(*mutex);

    // The RPC server uses the main thread to poll the connections. Freeze it for the duration of the injection
    mach_port_t mainThreadPort;
    {
        thread_act_array_t threads;
        mach_msg_type_number_t threadsCount;

        if (auto err = task_threads(mach_task_self(), &threads, &threadsCount)) {
            logger.error("Cannot enumerate our threads");
            return false;
        }

        mainThreadPort = threads[0];
    }

    struct ThreadSuspension {
        mach_port_t port;

        ThreadSuspension(mach_port_t port): port(port) {
            thread_suspend(port);
        }
        ~ThreadSuspension() {
            thread_resume(port);
        }
    };
    std::optional<ThreadSuspension> mainThreadSuspension(mainThreadPort);

    // We expect a single connection which reads from the unix socket shared between the server and Xcode
    if (connections->size() != 1) {
        logger.error("Unexpected count of connections: {}", connections->size());
        return false;
    }
    void* connection = connections->at(0).get();

    if (!malloc_zone_from_ptr(connection)) {
        logger.error("Connection is not heap allocated?!");
        return false;
    }

    logger.info("Scanning for RPCConnectionSocket pointer...");
    auto asPointers = std::span(reinterpret_cast<void**>(connection), malloc_size(connection) / sizeof(void*));

    for(void* pointer : asPointers) {
        if (!malloc_zone_from_ptr(pointer)) {
            logger.info("- {}: not malloc'd", pointer);
            continue;
        }

        auto size = malloc_size(pointer);
        if (size < sizeof(void*) + sizeof(int)) {
            logger.info("- {}: too small ({} bytes)", pointer, size);
            continue;
        }

        void*& vtablePtr = *reinterpret_cast<void**>(pointer);

        Dl_info info;
        if (!dladdr(vtablePtr, &info)) {
            logger.info("- {}: object not virtual/vtable is private", pointer);
            continue;
        }
        if (std::string_view(info.dli_sname) != kSocketVTable) {
            logger.info("- {}: object has incorrect vtable ({})", pointer, info.dli_sname);
            continue;
        }

        logger.info("- {}: RPCConnectionSocket found! ({} bytes)", pointer, size);

        constexpr auto kSafeSize = 256;

        void* newVTablePtr = new char[kSafeSize];
        std::memcpy(newVTablePtr, vtablePtr, kSafeSize);

        logger.info("Creating new vtable...");

        // Displace key functions
        void* socketRead = nullptr;
        void* socketIsConnected = nullptr;

        for (size_t i = 0; i < 16; i++) {
            void*& func = reinterpret_cast<void**>(newVTablePtr)[i];

            if (!dladdr(func, &info)) {
                logger.info("- #{} {}: vtable ended. Pointer is not a known symbol", i, func);
                break;
            }
            if (func != info.dli_saddr) {
                logger.info("- #{} {}: vtable ended. Pointer is misaligned from closest symbol '{}'", i, func, info.dli_sname);
                break;
            }

            std::string_view sname(info.dli_sname);
            logger.info("- #{} {}: {}", i, func, sname);

            if (sname == kSocketRead) {
                socketRead = std::exchange(func, reinterpret_cast<void*>(Read));
            }
            if (sname == kSocketIsConnected) {
                socketIsConnected = std::exchange(func, reinterpret_cast<void*>(IsConnected));
            }
        }

        if (!socketRead) {
            logger.warn("Failed to find RPCConnectionSocket::Read virtual function");
            return;
        }
        if (!socketIsConnected) {
            logger.warn("Failed to find RPCConnectionSocket::IsConnected virtual function");
            return;
        }

        logger.info("Scanning for underying file descriptor...");

        auto asInts = std::span(reinterpret_cast<int*>(pointer), size / sizeof(int));

        for (size_t i = 0; i < asInts.size(); i++) {
            int fd = asInts[i];

            struct stat stat;
            if (auto err = fstat(asInts[i], &stat)) {
                logger.info("- {} is not a file descriptor", fd);
                continue;
            }
            if (!S_ISSOCK(stat.st_mode)) {
                logger.info("- {} is not a socket", fd);
                continue;
            }

            logger.info("- {} is a socket file descriptor!", fd);

            g_socketFD = fd;
            g_socketFDPtr = &asInts[i];
            break;
        }

        if (!g_socketFDPtr) {
            logger.warn("Failed to find file descriptor in RPCConnectionSocket");
            return;
        }

        // Spin-up a background thread to read the socket
        std::thread(ReadThread).detach();

        logger.info("Overriding connection vtable");
        vtablePtr = newVTablePtr;

        // Dislodge the main thread from the likely ongoing read() operation by sending SIGINT
        {
            struct sigaction action;

            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_RESETHAND;
            action.sa_handler = [](int) {
                logger.info("SIGINT arrived");
            };

            if (sigaction(SIGINT, &action, nullptr) != 0) {
                logger.warn("Failed to setup sigaction, skipping SIGINT");
            } else {
                logger.info("SIGINT handler installed, interrupting main thread");

                pthread_t mainThread = pthread_from_mach_thread_np(mainThreadPort);

                if (auto err = pthread_kill(mainThread, SIGINT)) {
                    logger.warn("Failed to send SIGINT: {}", err);
                }
            }
        }

        // Wake the main thread
        mainThreadSuspension.reset();

        logger.info("Injection complete");
        return true;
    }

    logger.error("RPCConnectionSocket not found, injection failed!");
    return false;
}

} // namespace
} // namespace lldb::imgui

namespace lldb {

#define API __attribute__((used))

struct InjectionLogsCommand : public lldb::SBCommandPluginInterface {
    bool DoExecute(lldb::SBDebugger debugger, char** command, lldb::SBCommandReturnObject& result) override {
        for (auto line : imgui::loggerBuffer->last_formatted()) {
            result.AppendMessage(line.c_str());
        }
        return false;
    }
};

API bool PluginInitialize(lldb::SBDebugger debugger) {
    static bool s_success = lldb::imgui::Inject();

    if (!s_success) {
        static const char* kCommand = "imgui-injection-logs";

        debugger.GetCommandInterpreter().AddCommand(kCommand, new InjectionLogsCommand(), "Displays injection logs of lldb-imgui");
        debugger.HandleCommand(kCommand);
    }

    return s_success;
}

} // namespace lldb
