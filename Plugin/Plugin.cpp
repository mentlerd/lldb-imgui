
#include "imgui.h"

#include "lldb/API/LLDB.h"

namespace lldb::imgui {

#define API __attribute__((used))

API void Draw() {
    ImGui::Text("Hello from plugin!");
}

API void Draw(lldb::SBDebugger& debugger) {
    ImGui::Text("Debugger #%llu", debugger.GetID());
}

}
