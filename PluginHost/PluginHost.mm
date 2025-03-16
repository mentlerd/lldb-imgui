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

        // Load plugin
        auto plugin = GetExecutablePath().replace_filename("libPlugin.dylib");

        lldb::imgui::Log("Will try to load {}", plugin.generic_string());
    });

    while (true) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:nil
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

std::mutex g_debuggersMutex;
std::map<lldb::user_id_t, lldb::SBDebugger> g_debuggers;

void DrawFrame() {
    ImGui::NewFrame();
    ImGui::ShowDemoWindow();

    ImGui::Begin("Debuggers");
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

    ImGui::End();
    ImGui::Render();
}

bool CollapsingHeader2(const char* label, ImGuiTreeNodeFlags flags = 0) {
    struct Scope {
        Scope() :padding(ImGui::GetCurrentWindow()->WindowPadding.x) {
            backup = std::exchange(padding, 0);
        }
        ~Scope() {
            padding = backup;
        }

        float& padding;
        float backup;
    } scope;

    return ImGui::CollapsingHeader(label, flags);
}

void Draw(lldb::SBFrame frame) {
    using namespace ImGui;

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "#%d: %s ###frame", frame.GetFrameID(), frame.GetDisplayFunctionName());

    bool isOpen = false;

    PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2());
    if (BeginTable("FrameHeaderTable", 2)) {
        TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        TableSetupColumn("Focus", ImGuiTableColumnFlags_WidthFixed);

        TableNextRow();
        TableNextColumn();

        PushID(frame.GetFrameID());
        isOpen = CollapsingHeader2(buffer);
        PopID();

        TableNextColumn();

        if (Button("!", ImVec2(GetTextLineHeight() * 2, 0))) {
            auto thread = frame.GetThread();
            auto debugger = thread.GetProcess().GetTarget().GetDebugger();

            lldb::SBCommandReturnObject result;

            snprintf(buffer, sizeof(buffer), "thread select %d", thread.GetIndexID());
            debugger.GetCommandInterpreter().HandleCommand(buffer, result);

            snprintf(buffer, sizeof(buffer), "frame select %d", frame.GetFrameID());
            debugger.GetCommandInterpreter().HandleCommand(buffer, result);
        }
        if (IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
            SetTooltip("Select this frame as the active (will synchronize with Xcode)");
        }
        EndTable();
    }
    PopStyleVar();

    if (isOpen) {
        TreePush((void*) uint64_t(frame.GetFrameID()));
        Text("Dummy");
        TreePop();
    }
}

void Draw(lldb::SBThread thread) {
    using namespace ImGui;

    char buffer[128];
    snprintf(buffer,
             sizeof(buffer),
             "%s (%d) %s ###thread",
             thread.GetName() ? thread.GetName() : "Thread",
             thread.GetIndexID(),
             thread.GetQueueName() ? thread.GetQueueName() : "");

    bool isOpen = false;

    PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2());
    if (BeginTable("FrameHeaderTable", 2)) {
        TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        TableSetupColumn("Pin", ImGuiTableColumnFlags_WidthFixed);

        TableNextRow();
        TableNextColumn();

        PushID((void*) thread.GetThreadID());
        isOpen = CollapsingHeader2(buffer);
        PopID();

        TableNextColumn();

        bool checked = false;
        if (Checkbox("###pin", &checked)) {
            // TODO: Build own datamodel, or piggyback on ImGui?
        }
        if (IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
            SetTooltip("Pin this thread");
        }

        EndTable();
    }
    PopStyleVar();

    if (isOpen) {
        TreePush((void*) thread.GetThreadID());
        for (uint32_t f = 0; f < thread.GetNumFrames(); f++) {
            Draw(thread.GetFrameAtIndex(f));
        }
        TreePop();
    }
}

void Draw(lldb::SBTarget target) {
    using namespace ImGui;

    lldb::SBStream stream;
    target.GetDescription(stream, lldb::DescriptionLevel::eDescriptionLevelBrief);

    Text("%.*s", int(stream.GetSize()), stream.GetData());

    auto proc = target.GetProcess();
    if (!proc.IsValid()) {
        return;
    }

    if (Button("Step over")) {
        proc.GetSelectedThread().StepOver();
    }

    for (uint32_t i = 0; i < proc.GetNumThreads(); i++) {
        Draw(proc.GetThreadAtIndex(i));
    }
}

void Draw(lldb::SBDebugger& debugger) {
    for (uint32_t j = 0; j < debugger.GetNumTargets(); j++) {
        Draw(debugger.GetTargetAtIndex(j));
    }
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
        Draw(debugger);
        return true;
    }
};

}

namespace lldb {

#define API __attribute__((used))

API bool PluginInitialize(lldb::SBDebugger debugger) {
    static std::once_flag once;

    std::call_once(once, []{
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
