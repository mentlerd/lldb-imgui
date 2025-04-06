
#include "lldb/API/LLDB.h"

#include "imgui.h"

void Draw() {
    ImGui::ShowDemoWindow();
}

void DrawDebugger(lldb::SBDebugger& debugger) {
    ImGui::Text("DrawDebugger");
}
