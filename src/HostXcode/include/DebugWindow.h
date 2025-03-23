#pragma once

#include "Registry.h"

#include <string>

namespace lldb::imgui {

class DebugWindow : public Registered<DebugWindow> {
public:
    struct State {
        /// Window and debug menu title
        std::string title;

        /// Whether this window is currently open
        bool isOpen = false;

        /// Whether this window has custom rendering
        bool custom = false;
    };

    virtual void Init(State& state) {};
    virtual void Update(State& state) {};
    virtual void Draw() = 0;
};

}
