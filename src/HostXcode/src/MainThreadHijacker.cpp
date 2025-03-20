#include "MainThreadHijacker.h"

#include "Logging.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <signal.h>
#include <unistd.h>

#include <vector>
#include <functional>
#include <format>
#include <thread>
#include <span>

namespace lldb::imgui {
namespace {

static constexpr std::string_view kSocketVTable = "_ZTVN10rpc_common19RPCConnectionSocketE";
static constexpr std::string_view kSocketIsConnected = "_ZNK10rpc_common19RPCConnectionSocket11IsConnectedEv";
static constexpr std::string_view kSocketRead = "_ZN10rpc_common19RPCConnectionSocket4ReadERNSt3__112basic_stringIhNS1_11char_traitsIhEENS1_9allocatorIhEEEEb";

int g_socketFD = -1;
int* g_socketFDPtr = nullptr;

std::atomic<bool> g_hijackedReadCalled;

std::mutex g_readBufferMutex;
std::string g_readBuffer;

std::function<void()> g_mainLoop = []{};
std::function<void()> g_mainLoopInterrupt = []{};

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

        Log("Hijacked Read() called for the first time");
    }

    while (true) {
        std::string data;
        {
            std::scoped_lock lock(g_readBufferMutex);
            data = std::exchange(g_readBuffer, std::string());
        }

        if (auto read = data.size()) {
            if (!append) {
                buffer.clear();
            }
            buffer.append(std::move(data));
            return read;
        }

        g_mainLoop();
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
            g_readBuffer.append(std::string_view(buffer, n_read));
        }

        g_mainLoopInterrupt();
    }
}

template<typename T>
T* LookupGlobalPointerTo(const char* symbol) {
    auto addr = reinterpret_cast<void*>(dlsym(RTLD_MAIN_ONLY, symbol));
    if (!addr) {
        Log("'{}' -> nullptr", symbol);
        return nullptr;
    }

    auto value = *reinterpret_cast<void**>(addr);

    if (!malloc_zone_from_ptr(value)) {
        Log("'{}' -> {} = {} // not malloc'd!", symbol, addr, value);
        return nullptr;
    }

    size_t actual = malloc_size(value);
    Log("'{}' -> {} = {} // malloc({})", symbol, addr, value, actual);

    size_t minSize = sizeof(T);
    size_t maxSize = sizeof(T) * 1.5;

    if (actual < minSize) {
        Log(" ... expected at least {}", minSize);
        return nullptr;
    }
    if (actual > maxSize) {
        Log(" ... expected at max {}", maxSize);
        return nullptr;
    }

    return reinterpret_cast<T*>(value);
}

void Inject() {
    auto* connections = LookupGlobalPointerTo<std::vector<std::shared_ptr<void>>>("g_connections");
    auto* mutex = LookupGlobalPointerTo<std::mutex>("g_connections_mutex_ptr");

    if (!connections || !mutex) {
        Log("Required symbols for RPC connections not found");
        return;
    }

    // This has to be done before pausing the main thread lest we deadlock ourselves
    std::scoped_lock lock(*mutex);

    // The RPC server uses the main thread to poll the connections. Freeze it for the duration of the injection
    mach_port_t mainThreadPort;
    {
        thread_act_array_t threads;
        mach_msg_type_number_t threadsCount;

        if (auto err = task_threads(mach_task_self(), &threads, &threadsCount)) {
            Log("Cannot enumerate our threads");
            return;
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
        Log("Unexpected count of connections: {}", connections->size());
        return;
    }
    void* connection = connections->at(0).get();

    if (!malloc_zone_from_ptr(connection)) {
        Log("Connection is not heap allocated?!");
        return;
    }

    Log("Scanning for RPCConnectionSocket pointer...");
    auto asPointers = std::span(reinterpret_cast<void**>(connection), malloc_size(connection) / sizeof(void*));

    for(void* pointer : asPointers) {
        if (!malloc_zone_from_ptr(pointer)) {
            Log("- {}: not malloc'd", pointer);
            continue;
        }

        auto size = malloc_size(pointer);
        if (size < sizeof(void*) + sizeof(int)) {
            Log("- {}: too small ({} bytes)", pointer, size);
            continue;
        }

        void*& vtablePtr = *reinterpret_cast<void**>(pointer);

        Dl_info info;
        if (!dladdr(vtablePtr, &info)) {
            Log("- {}: object not virtual/vtable is private", pointer);
            continue;
        }
        if (std::string_view(info.dli_sname) != kSocketVTable) {
            Log("- {}: object has incorrect vtable ({})", pointer, info.dli_sname);
            continue;
        }

        Log("- {}: RPCConnectionSocket found! ({} bytes)", pointer, size);

        constexpr auto kSafeSize = 256;

        void* newVTablePtr = new char[kSafeSize];
        std::memcpy(newVTablePtr, vtablePtr, kSafeSize);

        Log("Creating new vtable...");

        // Displace key functions
        void* socketRead = nullptr;
        void* socketIsConnected = nullptr;

        for (size_t i = 0; i < 16; i++) {
            void*& func = reinterpret_cast<void**>(newVTablePtr)[i];

            if (!dladdr(func, &info)) {
                Log("- #{} {}: vtable ended. Pointer is not a known symbol", i, func);
                break;
            }
            if (func != info.dli_saddr) {
                Log("- #{} {}: vtable ended. Pointer is misaligned from closest symbol '{}'", i, func, info.dli_sname);
                break;
            }

            std::string_view sname(info.dli_sname);
            Log("- #{} {}: {}", i, func, sname);

            if (sname == kSocketRead) {
                socketRead = std::exchange(func, reinterpret_cast<void*>(Read));
            }
            if (sname == kSocketIsConnected) {
                socketIsConnected = std::exchange(func, reinterpret_cast<void*>(IsConnected));
            }
        }

        if (!socketRead) {
            Log("Failed to find RPCConnectionSocket::Read virtual function");
            return;
        }
        if (!socketIsConnected) {
            Log("Failed to find RPCConnectionSocket::IsConnected virtual function");
            return;
        }

        Log("Scanning for underying file descriptor...");

        auto asInts = std::span(reinterpret_cast<int*>(pointer), size / sizeof(int));

        for (size_t i = 0; i < asInts.size(); i++) {
            int fd = asInts[i];

            struct stat stat;
            if (auto err = fstat(asInts[i], &stat)) {
                Log("- {} is not a file descriptor", fd);
                continue;
            }
            if (!S_ISSOCK(stat.st_mode)) {
                Log("- {} is not a socket", fd);
                continue;
            }

            Log("- {} is a socket file descriptor!", fd);

            g_socketFD = fd;
            g_socketFDPtr = &asInts[i];
            break;
        }

        if (!g_socketFDPtr) {
            Log("Failed to find file descriptor in RPCConnectionSocket");
            return;
        }

        // Spin-up a background thread to read the socket
        std::thread(ReadThread).detach();

        Log("Overriding connection vtable");
        vtablePtr = newVTablePtr;

        // Dislodge the main thread from the likely ongoing read() operation by sending SIGINT
        {
            struct sigaction action;

            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_RESETHAND;
            action.sa_handler = [](int) {
                Log("SIGINT arrived");
            };

            if (sigaction(SIGINT, &action, nullptr) != 0) {
                Log("Failed to setup sigaction, skipping SIGINT");
            } else {
                Log("SIGINT handler installed, interrupting main thread");

                pthread_t mainThread = pthread_from_mach_thread_np(mainThreadPort);

                if (auto err = pthread_kill(mainThread, SIGINT)) {
                    Log("Failed to send SIGINT: {}", err);
                }
            }
        }

        // Wake the main thread
        mainThreadSuspension.reset();

        Log("Injection complete");
        return;
    }

    Log("RPCConnectionSocket not found, injection failed!");
}

}

void HijackMainThread(std::function<void()> mainLoop, std::function<void()> mainLoopInterrupt) {
    static std::once_flag flag;

    g_mainLoop = mainLoop;
    g_mainLoopInterrupt = mainLoopInterrupt;

    std::call_once(flag, Inject);
};

}
