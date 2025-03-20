#include "MainThreadHijacker.h"
#include "Logging.h"

#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include <lldb/API/LLDB.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>

#include <map>
#include <filesystem>

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

void ReloadPlugin();

void MainLoop() {
    static std::once_flag flag;

    std::call_once(flag, [] {
        ProcessSerialNumber psn = { 0, kCurrentProcess };
        TransformProcessType(&psn, kProcessTransformToForegroundApplication);

        NSApplication* application = [NSApplication sharedApplication];

        AppDelegate* applicationDelegate = [[AppDelegate alloc] init];

        [application setDelegate:applicationDelegate];
        [application run];

        ReloadPlugin();
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

using PluginDraw = void (*)();
using PluginDrawDebugger = void(*)(lldb::SBDebugger&);

void* g_plugin = nullptr;

PluginDraw g_pluginDraw = nullptr;
PluginDrawDebugger g_pluginDrawDebugger = nullptr;

void ReloadPlugin() {
    using namespace lldb::imgui;

    if (g_plugin) {
        if (dlclose(g_plugin)) {
            Log("Failed to unload plugin: {}", dlerror());
            return;
        }

        g_plugin = nullptr;
        g_pluginDraw = nullptr;
        g_pluginDrawDebugger = nullptr;
    }

    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(ReloadPlugin), &info) == 0) {
        Log("Failed to determine plugin host path");
        return;
    }

    auto hostPath = std::filesystem::path(info.dli_fname);

    // TODO: This is specific to building with Xcode
    auto pluginPath = hostPath.parent_path().parent_path().parent_path() / "Plugin/Debug/libPlugin.dylib";

    Log("Trying to load plugin from '{}'", pluginPath.c_str());
    g_plugin = dlopen(pluginPath.c_str(), RTLD_LOCAL | RTLD_NOW);

    if (!g_plugin) {
        Log("Failed to load plugin: {}", dlerror());
        return;
    }

    g_pluginDraw = reinterpret_cast<PluginDraw>(dlsym(g_plugin, "_ZN4lldb5imgui4DrawEv"));
    g_pluginDrawDebugger = reinterpret_cast<PluginDrawDebugger>(dlsym(g_plugin, "_ZN4lldb5imgui4DrawERNS_10SBDebuggerE"));

    Log("Plugin loaded: {} {}", (void*) g_pluginDraw, (void*) g_pluginDrawDebugger);
}

std::mutex g_debuggersMutex;
std::map<lldb::user_id_t, lldb::SBDebugger> g_debuggers;

void DrawFrame() {
    ImGui::NewFrame();

    static bool s_showDemo = false;
    static bool s_showLogs = false;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::Button("Reload plugin")) {
            ReloadPlugin();
        }

        ImGui::Checkbox("Show demo", &s_showDemo);
        ImGui::Checkbox("Show logs", &s_showLogs);
        ImGui::EndMainMenuBar();
    }
    if (s_showDemo) {
        ImGui::ShowDemoWindow(&s_showDemo);
    }
    if (s_showLogs) {
        if (ImGui::Begin("Logs", &s_showLogs)) {
            using namespace lldb::imgui;

            std::scoped_lock lock(g_logMutex);

            for (auto i = 0; i < 64; i++) {
                auto size = g_log.size();

                if (size <= i) {
                    break;
                }
                ImGui::Text("%s", g_log.at(size - i - 1).c_str());
            }
        }
        ImGui::End();
    }

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

    if (g_pluginDraw) {
        g_pluginDraw();
    }

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
        if (g_pluginDrawDebugger) {
            g_pluginDrawDebugger(debugger);
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
