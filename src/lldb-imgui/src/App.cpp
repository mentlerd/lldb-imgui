#include "App.h"

#include "lldb/API/LLDB.h"

#include "SDL3/SDL.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include "spdlog/spdlog.h"

#include <filesystem>
#include <algorithm>

/// PLANS:
///
/// - Standalone app
///   - GLFW
///   - Choose your own LLDB
///
/// - Plugin system
///   - List of plugins
///   - Autoreload support
///   - Plugin tint chooser
///   - Plugin crash blame
///   - Auto-disable plugins on crash
///   - Blame plugins for lagging
///   - Plugin perf sampler

namespace lldb::imgui {

class App::PluginHandler {
    static bool StripPrefix(std::string_view& from, std::string_view prefix) {
        if (!from.starts_with(prefix)) {
            return false;
        }

        from = from.substr(prefix.size());
        return true;
    }

    std::unordered_map<PluginID, PluginSpec> _plugins;

    void SaveIniSettingsNow() {
        ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
    }

    void DrawPluginMenuItem(PluginID id, PluginSpec& spec) {
        using namespace ImGui;

        auto name = spec.path.filename().generic_string();

        if (!BeginMenu(name.c_str())) {
            return;
        }

        TextDisabled("Path: %s", spec.path.c_str());

        _loader.DrawMenu(id);

        bool changed = false;

        changed |= Checkbox("Enabled", &spec.isEnabled);
        changed |= Checkbox("AutoReload", &spec.isAutoReload);

        if (changed) {
            SaveIniSettingsNow();

            _loader.Update(id, spec);
        }

        if (Button("Remove")) {
            _plugins.erase(id);
            SaveIniSettingsNow();

            _loader.Remove(id);
        }

        EndMenu();
    }

    void DrawPluginsMenu() {
        using namespace ImGui;

        // Draw plugins in alphabetical order
        struct Entry {
            std::string name;
            PluginID key;

            auto operator<=>(const Entry&) const = default;
        };
        std::vector<Entry> plugins;

        for (auto& [key, plugin] : _plugins) {
            plugins.push_back(Entry{
                .name = plugin.path.stem().generic_string(),
                .key = key,
            });
        }
        std::sort(plugins.begin(), plugins.end());

        if (plugins.empty()) {
            TextDisabled("None");
        }
        for (Entry& entry : plugins) {
            DrawPluginMenuItem(entry.key, _plugins.at(entry.key));
        }

        Separator();
        if (MenuItem("Add")) {
            static std::array kFilters = {
                SDL_DialogFileFilter {
                    .name = "LLDB ImGui plugin",
                    .pattern = "so",
                },
            };

            SDL_DialogFileCallback completion = [](void *userdata, const char* const *filelist, int filter) {
                if (!filelist || !filelist[0]) {
                    return;
                }

                reinterpret_cast<PluginHandler*>(userdata)->AddPlugin(filelist[0]);
            };
            SDL_ShowOpenFileDialog(completion, this, _window, kFilters.data(), kFilters.size(), nullptr, false);
        }
    }

    void AddPlugin(std::filesystem::path path) {
        PluginID newID = 0;

        for (const auto& [id, plugin] : _plugins) {
            if (plugin.path == path) {
                return;
            }
            newID = std::max(newID, id);
        }

        _plugins[newID] = PluginSpec {
            .path = path,
            .isEnabled = true,
            .isAutoReload = true,
        };
        SaveIniSettingsNow();

        _loader.Update(newID, _plugins.at(newID));
    }

    PluginLoader& _loader;
    SDL_Window* _window;

public:
    PluginHandler(PluginLoader& loader, SDL_Window* window)
    : _loader(loader)
    , _window(window)
    {
        ImGuiSettingsHandler handler;

        handler.TypeName = "Plugins";
        handler.TypeHash = ImHashStr(handler.TypeName);
        handler.UserData = this;

        handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, const char* name) -> void* {
            auto* impl = reinterpret_cast<PluginHandler*>(handler->UserData);

            return &impl->_plugins[atoi(name)];
        };
        handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, void* ptr, const char* rawLine) {
            auto* spec = reinterpret_cast<PluginSpec*>(ptr);

            std::string_view line = rawLine;

            if (StripPrefix(line, "path=")) {
                spec->path = line;
            } else if (StripPrefix(line, "isEnabled=1")) {
                spec->isEnabled = true;
            } else if (StripPrefix(line, "isAutoReload=1")) {
                spec->isAutoReload = true;
            }
        };
        handler.ApplyAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler) {
            auto* impl = reinterpret_cast<PluginHandler*>(handler->UserData);

            for (auto& [id, spec] : impl->_plugins) {
                impl->_loader.Update(id, spec);
            }
        };
        handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer) {
            auto* impl = reinterpret_cast<PluginHandler*>(handler->UserData);

            for (const auto& [key, plugin] : impl->_plugins) {
                buffer->appendf("[%s][%d]\n", handler->TypeName, key);

                buffer->appendf("path=%s\n", plugin.path.c_str());
                buffer->appendf("isEnabled=%d\n", plugin.isEnabled);
                buffer->appendf("isAutoReload=%d\n", plugin.isAutoReload);
            }
        };

        ImGui::AddSettingsHandler(&handler);
        ImGui::LoadIniSettingsFromDisk(ImGui::GetIO().IniFilename);
    }
    ~PluginHandler() {
        ImGui::RemoveSettingsHandler("Plugins");
    }

    void Draw() {
        using namespace ImGui;

        if (BeginMainMenuBar()) {
            if (BeginMenu("Plugins")) {
                DrawPluginsMenu();
                EndMenu();
            }
            EndMainMenuBar();
        }
    }
};

static void LogAdapter(void* userdata, int rawCategory, SDL_LogPriority priority, const char* message) {
    using namespace spdlog;
    using namespace spdlog::level;

    level_enum level = level_enum::info;
    switch (priority) {
        case SDL_LOG_PRIORITY_TRACE:    level = level_enum::trace;    break;
        case SDL_LOG_PRIORITY_VERBOSE:  level = level_enum::trace;    break;
        case SDL_LOG_PRIORITY_DEBUG:    level = level_enum::debug;    break;
        case SDL_LOG_PRIORITY_INFO:     level = level_enum::info;     break;
        case SDL_LOG_PRIORITY_WARN:     level = level_enum::warn;     break;
        case SDL_LOG_PRIORITY_ERROR:    level = level_enum::err;      break;
        case SDL_LOG_PRIORITY_CRITICAL: level = level_enum::critical; break;

        case SDL_LOG_PRIORITY_INVALID:
        case SDL_LOG_PRIORITY_COUNT: {
            break;
        }
    }

    const char* category = nullptr;
    switch (rawCategory) {
        case SDL_LOG_CATEGORY_APPLICATION: category = "APPLICATION"; break;
        case SDL_LOG_CATEGORY_ERROR:       category = "ERROR";       break;
        case SDL_LOG_CATEGORY_ASSERT:      category = "ASSERT";      break;
        case SDL_LOG_CATEGORY_SYSTEM:      category = "SYSTEM";      break;
        case SDL_LOG_CATEGORY_AUDIO:       category = "AUDIO";       break;
        case SDL_LOG_CATEGORY_VIDEO:       category = "VIDEO";       break;
        case SDL_LOG_CATEGORY_RENDER:      category = "RENDER";      break;
        case SDL_LOG_CATEGORY_INPUT:       category = "INPUT";       break;
        case SDL_LOG_CATEGORY_TEST:        category = "TEST";        break;
        case SDL_LOG_CATEGORY_GPU:         category = "GPU";         break;
    }

    if (category) {
        spdlog::log(level, "[SDL][{}] {}", category, message);
    } else {
        spdlog::log(level, "[SDL][{}] {}", rawCategory, message);
    }
}

bool App::EventWatch(void* userdata, SDL_Event* event) {
    auto* app = reinterpret_cast<App*>(userdata);

    switch (event->type) {
        // Redraw window contents immediately to avoid the "jelly" resizing effect
        case SDL_EVENT_WINDOW_EXPOSED: {
            app->Draw();
            return false;
        }
    }
    return true;
}

App::App() = default;
App::~App() = default;

SDL_AppResult App::Init(std::span<const std::string_view> args) {
    SDL_SetLogOutputFunction(LogAdapter, nullptr);

    if (!SDL_Init(SDL_INIT_EVENTS | SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        return SDL_APP_FAILURE;
    }

    _window = SDL_CreateWindow("lldb-imgui", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    _gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB, true, nullptr);

    if (!_gpu || !_window || !SDL_ClaimWindowForGPUDevice(_gpu, _window)) {
        return SDL_APP_FAILURE;
    }

    SDL_SetGPUSwapchainParameters(_gpu, _window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
    SDL_AddEventWatch(EventWatch, this);

    // Setup ImGui
    ImGui::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        // TODO: Find a way to make these assertions continuable, and they'll be great!
        io.ConfigErrorRecoveryEnableAssert = false;
    }
    ImGui::StyleColorsDark();

    // Setup ImGui Platform/Renderer
    ImGui_ImplSDL3_InitForSDLGPU(_window);

    ImGui_ImplSDLGPU3_InitInfo initInfo {
        .Device = _gpu,
        .ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(_gpu, _window),
        .MSAASamples = SDL_GPU_SAMPLECOUNT_1,
    };
    ImGui_ImplSDLGPU3_Init(&initInfo);

    // Raise the newly created window for the sake of RPC main, where this doesnt
    // happen automatically the second time we enter foreground mode
    SDL_RaiseWindow(_window);

    _pluginLoader = PluginLoader::Create();
    _pluginHandler = std::make_unique<PluginHandler>(*_pluginLoader, _window);

    return SDL_APP_CONTINUE;
}

SDL_AppResult App::Iterate() {
    if (SDL_GetWindowFlags(_window) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(10);

        return SDL_APP_CONTINUE;
    }

    Draw();

    return SDL_APP_CONTINUE;
}

SDL_AppResult App::Event(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);

    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(_window)) {
        return SDL_APP_SUCCESS;
    }
    if (event.type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }

    return SDL_APP_CONTINUE;
}

void App::Quit() {
    _pluginHandler.reset();
    _pluginLoader.reset();

    SDL_WaitForGPUIdle(_gpu);

    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();

    SDL_RemoveEventWatch(EventWatch, this);
    SDL_ReleaseWindowFromGPUDevice(_gpu, _window);
    SDL_DestroyGPUDevice(_gpu);
    SDL_DestroyWindow(_window);

    SDL_QuitSubSystem(SDL_INIT_EVENTS | SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

void App::AddDebugger(SBDebugger& debugger) {
    _debuggers.push_back(debugger);
}

void App::Draw() {
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();

    ImGui::NewFrame();

    _pluginHandler->Draw();
    _pluginLoader->DrawPlugins();

    std::erase_if(_debuggers, [&](auto& debugger) {
        _pluginLoader->DrawDebugger(debugger);

        return !debugger.IsValid();
    });

    ImGui::EndFrame();

    // Rendering
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(_gpu); // Acquire a GPU command buffer

    SDL_GPUTexture* swapchain_texture;
    SDL_AcquireGPUSwapchainTexture(command_buffer, _window, &swapchain_texture, nullptr, nullptr); // Acquire a swapchain texture

    if (swapchain_texture != nullptr && !is_minimized) {
        // This is mandatory: call Imgui_ImplSDLGPU3_PrepareDrawData() to upload the vertex/index buffer!
        Imgui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

        // Setup and start a render pass
        SDL_GPUColorTargetInfo target_info = {};
        target_info.texture = swapchain_texture;
        target_info.clear_color = SDL_FColor { 0.45f, 0.55f, 0.60f, 1.00f };
        target_info.load_op = SDL_GPU_LOADOP_CLEAR;
        target_info.store_op = SDL_GPU_STOREOP_STORE;
        target_info.mip_level = 0;
        target_info.layer_or_depth_plane = 0;
        target_info.cycle = false;
        SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);

        // Render ImGui
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);

        SDL_EndGPURenderPass(render_pass);
    }

    // Submit the command buffer
    SDL_SubmitGPUCommandBuffer(command_buffer);
}

}
