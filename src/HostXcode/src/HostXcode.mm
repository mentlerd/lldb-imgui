#include "Debuggable.h"
#include "DebugWindow.h"
#include "MainThreadHijacker.h"
#include "Logging.h"

#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include <lldb/API/LLDB.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>

#include <map>
#include <filesystem>
#include <algorithm>

@interface AppViewController : NSViewController<NSWindowDelegate>
@end

@interface AppViewController () <MTKViewDelegate>
@property (nonatomic, readonly) MTKView *mtkView;
@property (nonatomic, strong) id <MTLDevice> device;
@property (nonatomic, strong) id <MTLCommandQueue> commandQueue;
@end

// MARK: AppViewController
@implementation AppViewController

-(instancetype)initWithNibName:(nullable NSString *)nibNameOrNil bundle:(nullable NSBundle *)nibBundleOrNil {
    self = [super initWithNibName:nibNameOrNil bundle:nibBundleOrNil];

    _device = MTLCreateSystemDefaultDevice();
    _commandQueue = [_device newCommandQueue];

    if (!self.device) {
        NSLog(@"Metal is not supported");
        abort();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    // Save settings between runs
    {
        auto path = std::make_unique<std::string>();

        path->append(NSHomeDirectory().UTF8String);
        path->append("/.lldb-imgui.ini");

        lldb::imgui::Log("Will save ImGui settings to '{}'", *path);

        io.IniFilename = path.release()->c_str();
        io.IniSavingRate = 5.0f;
    }

    // Assertions don't really help, we are inside the debugger itself
    io.ConfigErrorRecoveryEnableAssert = false;

    ImGui::StyleColorsDark();
    ImGui_ImplMetal_Init(_device);

    return self;
}

-(MTKView *)mtkView {
    return (MTKView *)self.view;
}

-(void)loadView {
    self.view = [[MTKView alloc] initWithFrame:CGRectMake(0, 0, 1200, 720)];
}

-(void)viewDidLoad {
    [super viewDidLoad];

    self.mtkView.device = self.device;
    self.mtkView.delegate = self;

    ImGui_ImplOSX_Init(self.view);
    [NSApp activateIgnoringOtherApps:YES];
}

void DrawFrame();

-(void)drawInMTKView:(MTKView*)view {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = view.bounds.size.width;
    io.DisplaySize.y = view.bounds.size.height;

    CGFloat framebufferScale = view.window.screen.backingScaleFactor ?: NSScreen.mainScreen.backingScaleFactor;
    io.DisplayFramebufferScale = ImVec2(framebufferScale, framebufferScale);

    id<MTLCommandBuffer> commandBuffer = [self.commandQueue commandBuffer];

    MTLRenderPassDescriptor* renderPassDescriptor = view.currentRenderPassDescriptor;
    if (renderPassDescriptor == nil) {
        [commandBuffer commit];
        return;
    }

    // Start the Dear ImGui frame
    ImGui_ImplMetal_NewFrame(renderPassDescriptor);
    ImGui_ImplOSX_NewFrame(view);

    DrawFrame();

    // Rendering
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();

    auto clearColor = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(
        clearColor.x * clearColor.w,
        clearColor.y * clearColor.w,
        clearColor.z * clearColor.w,
        clearColor.w
    );
    id <MTLRenderCommandEncoder> renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    [renderEncoder pushDebugGroup:@"Dear ImGui rendering"];
    ImGui_ImplMetal_RenderDrawData(drawData, commandBuffer, renderEncoder);
    [renderEncoder popDebugGroup];
    [renderEncoder endEncoding];

    // Present
    [commandBuffer presentDrawable:view.currentDrawable];
    [commandBuffer commit];
}

-(void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {

}

- (void)viewWillAppear {
    [super viewWillAppear];
    self.view.window.delegate = self;
}

- (void)windowWillClose:(NSNotification *)notification {
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplOSX_Shutdown();
    ImGui::DestroyContext();
}

@end

// MARK: AppDelegate

@interface AppDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSWindow *window;
@end

@implementation AppDelegate

-(instancetype)init {
    if (self = [super init]) {
        NSViewController *rootViewController = [[AppViewController alloc] initWithNibName:nil bundle:nil];
        self.window = [[NSWindow alloc] initWithContentRect:NSZeroRect
                                                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        self.window.contentViewController = rootViewController;
        self.window.title = @"lldb-rpc-server GUI";
    }
    return self;
}

-(void)applicationDidFinishLaunching:(NSNotification *)notification {
    [self.window center];
    [self.window makeKeyAndOrderFront:self];

    // We don't truly stop, just exit [NSApplication run]
    [[NSApplication sharedApplication] stop:nil];
}

@end

std::filesystem::path GetExecutablePath() {
    uint32_t capacity = 0;
    _NSGetExecutablePath(nullptr, &capacity);

    std::string buffer(capacity, 'A');
    _NSGetExecutablePath(buffer.data(), &capacity);

    return buffer;
}

void MainLoop() {
    static std::once_flag flag;

    std::call_once(flag, [] {
        ProcessSerialNumber psn = { 0, kCurrentProcess };
        TransformProcessType(&psn, kProcessTransformToForegroundApplication);

        NSApplication* application = [NSApplication sharedApplication];

        AppDelegate* applicationDelegate = [[AppDelegate alloc] init];

        [application setDelegate:applicationDelegate];
        [application run];
    });

    while (true) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantFuture]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];

        if (event.type == NSEventTypeApplicationDefined) {
            break; // Interrupted
        }

        [NSApp sendEvent: event];
    }
}

void MainLoopInterrupt() {
    @autoreleasepool {
        NSEvent* event = [NSEvent otherEventWithType: NSEventTypeApplicationDefined
                                            location: NSMakePoint(0,0)
                                       modifierFlags: 0
                                           timestamp: 0.0
                                        windowNumber: 0
                                             context: nil
                                             subtype: 0
                                               data1: 0
                                               data2: 0];
        [NSApp postEvent: event atStart: YES];
    }
}

namespace lldb::imgui {

template<>
Registry<DebugWindow>& Registry<DebugWindow>::Default() {
    static Registry<DebugWindow> instance;
    return instance;
}

struct Demo final : DebugWindow {
    void Init(State& state) {
        state.title = "ImGui Demo";
        state.custom = true;
    }
    void Update(State& state) {
        if (!state.isOpen) {
            return;
        }
        ImGui::ShowDemoWindow(&state.isOpen);
    }
    void Draw() {
        // Nothing
    }
} g_demo;

struct Logs final : DebugWindow {
    void Init(State& state) {
        state.title = "Logs";
    }
    void Draw() {
        std::scoped_lock lock(g_logMutex);

        for (auto i = 0; i < 64; i++) {
            auto size = g_log.size();

            if (size <= i) {
                break;
            }
            ImGui::Text("%s", g_log.at(size - i - 1).c_str());
        }
    }
} g_logs;


void DrawDebugWindows() {
    static std::unordered_map<DebugWindow*, DebugWindow::State> s_states;

    // Update window states
    {
        auto oldStates = std::exchange(s_states, {});

        for (DebugWindow* window : Registry<DebugWindow>::Default().View()) {
            DebugWindow::State* state = nullptr;

            auto node = oldStates.extract(window);
            if (!node) {
                // New window, initialize state
                state = &s_states[window];
                state->title = std::format("DebugWindow({:X})", reinterpret_cast<uintptr_t>(window));

                window->Init(*state);
            } else {
                // Returning window, retain state
                state = &s_states.insert(std::move(node)).position->second;
            }

            window->Update(*state);
        }
    }

    // Draw windows
    using namespace ImGui;

    for (auto& [window, state] : s_states) {
        if (state.custom) {
            continue;
        }
        if (!state.isOpen) {
            continue;
        }

        if (Begin(state.title.c_str(), &state.isOpen)) {
            window->Draw();
        }
        End();
    }

    // Draw menu
    if (BeginMainMenuBar()) {
        if (BeginMenu("Debug")) {
            // Draw debug windows in alphabetical order, break ties with pointer
            struct Entry {
                std::string_view title;

                DebugWindow* window;
                DebugWindow::State* state;

                auto operator<=>(const Entry&) const = default;
            };
            std::vector<Entry> windowsToDraw;

            for (auto& [window, state] : s_states) {
                windowsToDraw.push_back(Entry{
                    .title = state.title,
                    .window = window,
                    .state = &state,
                });
            }
            std::sort(windowsToDraw.begin(), windowsToDraw.end());

            for (Entry& entry : windowsToDraw) {
                Checkbox(entry.state->title.c_str(), &entry.state->isOpen);
            }
            EndMenu();
        }
        EndMainMenuBar();
    }
}

}

#include <unordered_map>
#include <cstdlib>

using PluginID = uint32_t;

struct Plugin {
    using TickGlobal = void(*)();
    using TickDebugger = void(*)(lldb::SBDebugger&);

    inline static PluginID g_nextID = 0;
    inline static std::unordered_map<PluginID, Plugin> g_plugins = {};

    /// Persistent state
    std::filesystem::path path;
    bool enabled = false;

    std::string status = "Pending";
    void* handle = nullptr;

    TickGlobal tickGlobal = nullptr;
    TickDebugger tickDebugger = nullptr;

    void Unload() {
        tickGlobal = nullptr;
        tickDebugger = nullptr;

        if (dlclose(handle)) {
            status = std::format("Failed to unload: {}", dlerror());
            return;
        }

        handle = nullptr;
    }
    void Load() {
        Unload();

        handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_NOW);

        if (!handle) {
            status = std::format("Failed to load: {}", dlerror());
            return;
        }

        tickGlobal = reinterpret_cast<TickGlobal>(dlsym(handle, "_ZN4lldb5imgui4DrawEv"));
        tickDebugger = reinterpret_cast<TickDebugger>(dlsym(handle, "_ZN4lldb5imgui4DrawERNS_10SBDebuggerE"));
        status = "Loaded";
    }
};

static ImGuiSettingsHandler g_pluginSettingsHandler = []{
    ImGuiSettingsHandler handler;

    handler.TypeName = "Plugins";
    handler.TypeHash = ImHashStr(handler.TypeName);

    handler.ClearAllFn = [](auto, auto) {
        // Ignore
    };
    handler.ReadInitFn = [](auto, auto) {
        Plugin::g_nextID = 0;
    };
    handler.ReadOpenFn = [](auto, auto, const char* name) -> void* {
        PluginID key = atoi(name);

        Plugin::g_nextID = std::max(Plugin::g_nextID, key + 1);
        return &Plugin::g_plugins[key];
    };
    handler.ReadLineFn = [](auto, auto, void* ptr, const char* rawLine) {
        auto& plugin = *static_cast<Plugin*>(ptr);

        std::string_view line = rawLine;

        if (line.starts_with("path=")) {
            plugin.path = line.substr(6);
        }
        if (line.starts_with("enabled=1")) {
            plugin.enabled = true;
        }
    };
    handler.ApplyAllFn = [](auto, auto) {
        for (auto& [key, plugin] : Plugin::g_plugins) {
            if (plugin.enabled) {
                plugin.Load();
            }
        }
    };
    handler.WriteAllFn = [](auto, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer) {
        for (const auto& [key, plugin] : Plugin::g_plugins) {
            buffer->appendf("[%s][%d]\n", handler->TypeName, key);

            buffer->appendf("path=%s\n", plugin.path.c_str());
            buffer->appendf("enabled=%d\n", plugin.enabled);
        }
    };

    return handler;
}();

void SaveIniSettingsNow() {
    ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
}

void DrawPluginsMenu() {
    using namespace ImGui;

    if (FindSettingsHandler(g_pluginSettingsHandler.TypeName) == nullptr) {
        AddSettingsHandler(&g_pluginSettingsHandler);
        LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
    }

    const auto drawPluginItems = [] {
        // Draw plugins in alphabetical order, break ties with pointer
        struct Entry {
            std::string_view name;
            PluginID key;

            auto operator<=>(const Entry&) const = default;
        };
        std::vector<Entry> plugins;

        for (auto& [key, plugin] : Plugin::g_plugins) {
            plugins.push_back(Entry{
                .name = plugin.path.stem().generic_string(),
                .key = key,
            });
        }
        std::sort(plugins.begin(), plugins.end());

        if (plugins.empty()) {
            TextDisabled("None");
            return;
        }

        for (Entry& entry : plugins) {
            Plugin& plugin = Plugin::g_plugins.at(entry.key);

            PushID(entry.key);
            if (Checkbox(entry.name.data(), &plugin.enabled)) {
                if (plugin.enabled) {
                    plugin.Load();
                } else {
                    plugin.Unload();
                }
                SaveIniSettingsNow();
            }
            if (BeginItemTooltip()) {
                Text("Path: %s", plugin.path.c_str());
                Text("Status: %s", plugin.status.c_str());
                EndTooltip();
            }
            PopID();
        }
    };

    bool popup = false;

    if (BeginMainMenuBar()) {
        if (BeginMenu("Plugins")) {
            drawPluginItems();
            Separator();

            popup = MenuItem("Add");
            EndMenu();
        }
        EndMainMenuBar();
    }

    if (popup) {
        OpenPopup("AddPlugin");
    }
    if (BeginPopup("AddPlugin")) {
        static char rawPath[2048];
        InputTextWithHint("###path", "Enter absolute path...", rawPath, sizeof(rawPath));

        bool exists = std::filesystem::exists(rawPath);
        if (!exists) {
            BeginDisabled();
        }
        if (Button("Add plugin")) {
            Plugin::g_plugins[Plugin::g_nextID++] = Plugin {
                .path = rawPath,
            };
            SaveIniSettingsNow();
            CloseCurrentPopup();
        }
        if (!exists) {
            EndDisabled();
        }
        EndPopup();
    }
}

std::mutex g_debuggersMutex;
std::map<lldb::user_id_t, lldb::SBDebugger> g_debuggers;

void DrawFrame() {
    ImGui::NewFrame();

    DrawPluginsMenu();

    {
        std::scoped_lock lock(g_debuggersMutex);
        std::erase_if(g_debuggers, [](auto& pair) {
            lldb::SBDebugger& debugger = pair.second;

            if (!debugger.IsValid()) {
                return true;
            }
            if (!lldb::SBDebugger::FindDebuggerWithID(debugger.GetID())) {
                return true;
            }

            // Dispatching through the interpreter locks the API lock, blocking RPC calls until we finish rendering.
            lldb::SBCommandReturnObject result;
            debugger.GetCommandInterpreter().HandleCommand("imgui draw", result);

            return false;
        });
    }

    for (auto& [_, plugin] : Plugin::g_plugins) {
        if (plugin.tickGlobal) {
            plugin.tickGlobal();
        }
    }

    lldb::imgui::DrawDebugWindows();

    ImGui::EndFrame();
    ImGui::Render();
}

namespace lldb::imgui {

class LogsCommand : public lldb::SBCommandPluginInterface {
public:
    bool DoExecute(lldb::SBDebugger debugger, char** command, lldb::SBCommandReturnObject& result) override {
        std::scoped_lock lock(g_logMutex);
        for (const auto& msg : g_log) {
            result.AppendMessage(msg.c_str());
        }
        return true;
    }
};

class DrawCommand : public lldb::SBCommandPluginInterface {
    bool DoExecute(lldb::SBDebugger debugger, char** command, lldb::SBCommandReturnObject& result) override {
        for (auto& [_, plugin] : Plugin::g_plugins) {
            if (plugin.tickDebugger) {
                plugin.tickDebugger(debugger);
            }
        }
        return true;
    }
};

}

namespace lldb {

#define API __attribute__((used))

API bool PluginInitialize(lldb::SBDebugger debugger) {
    using namespace lldb::imgui;

    static std::once_flag once;

    std::call_once(once, []{
        uint32_t capacity = 0;
        _NSGetExecutablePath(nullptr, &capacity);

        std::string buffer(capacity, 'A');
        _NSGetExecutablePath(buffer.data(), &capacity);

        buffer.resize(capacity - 1);

        Log("Loaded into '{}'", buffer);

        if (!buffer.ends_with("lldb-rpc-server")) {
            Log("This doesn't appear to be lldb-rpc-server!");
            return;
        }

        lldb::imgui::HijackMainThread(MainLoop, MainLoopInterrupt);
    });

    auto toplevel = debugger.GetCommandInterpreter().AddMultiwordCommand("imgui", "");
    toplevel.SetHelp("Graphical user interface plugin for lldb-rpc-server");

    toplevel.AddCommand("logs", new lldb::imgui::LogsCommand(), "Show log ring-buffer");
    toplevel.AddCommand("draw", new lldb::imgui::DrawCommand(), "Internal function, do not use").SetFlags(eCommandTryTargetAPILock);

    {
        std::scoped_lock lock(g_debuggersMutex);
        g_debuggers.emplace(debugger.GetID(), debugger);
    }
    return true;
}

}
