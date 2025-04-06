#pragma once

#include "PluginLoader.h"

#include "SDL3/SDL_init.h"
#include "SDL3/SDL_gpu.h"

#include <unordered_map>
#include <string_view>
#include <span>
#include <vector>

namespace lldb {

class SBDebugger;

}

namespace lldb::imgui {

class App {
public:
    App();
    ~App();

    SDL_AppResult Init(std::span<const std::string_view> args);
    SDL_AppResult Iterate();
    SDL_AppResult Event(const SDL_Event& event);
    void Quit();

    void AddDebugger(SBDebugger& debugger);

private:
    static bool EventWatch(void* userdata, SDL_Event* event);

    void Draw();

    SDL_Window* _window = nullptr;
    SDL_GPUDevice* _gpu = nullptr;

    class PluginHandler;
    std::unique_ptr<PluginHandler> _pluginHandler;
    std::unique_ptr<PluginLoader> _pluginLoader;

    std::vector<SBDebugger> _debuggers;
};

}
