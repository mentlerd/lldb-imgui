#pragma once

#include "SDL3/SDL.h"

#include <string_view>
#include <span>

namespace lldb::imgui {

class App {
public:
    SDL_AppResult Init(std::span<const std::string_view> args);
    SDL_AppResult Iterate();
    SDL_AppResult Event(const SDL_Event& event);
    void Quit();

private:
    SDL_Window* _window = nullptr;
    SDL_GPUDevice* _gpu = nullptr;
};

}
