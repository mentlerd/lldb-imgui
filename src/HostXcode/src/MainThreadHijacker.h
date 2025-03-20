#pragma once

#include <functional>
#include <span>

namespace lldb::imgui {

void HijackMainThread(std::function<void()> mainLoop, std::function<void()> mainLoopInterrupt);

}
