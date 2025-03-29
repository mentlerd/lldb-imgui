#include "App.h"

#define SDL_MAIN_USE_CALLBACKS 1
#include "SDL3/SDL_main.h"

#include <vector>

SDL_AppResult SDLCALL SDL_AppInit(void** appstate, int argc, char* argv[]) {
    std::vector<std::string_view> args;

    for (int i = 0; i < argc; i++) {
        args.push_back(argv[i]);
    }

    auto* app = new lldb::imgui::App();
    *appstate = app;

    return app->Init(args);
}

SDL_AppResult SDLCALL SDL_AppIterate(void* appstate) {
    auto* app = reinterpret_cast<lldb::imgui::App*>(appstate);

    return app->Iterate();
}

SDL_AppResult SDLCALL SDL_AppEvent(void* appstate, SDL_Event *event) {
    auto* app = reinterpret_cast<lldb::imgui::App*>(appstate);

    return app->Event(*event);
}

void SDLCALL SDL_AppQuit(void* appstate, SDL_AppResult result) {
    auto* app = reinterpret_cast<lldb::imgui::App*>(appstate);

    app->Quit();
    delete app;
}
