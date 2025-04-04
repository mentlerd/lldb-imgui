#include "App.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include "spdlog/spdlog.h"

#include "Expose.h"

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

void App::Draw() {
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();

    ImGui::NewFrame();
    ImGui::ShowDemoWindow();
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
