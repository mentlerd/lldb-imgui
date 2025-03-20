#pragma once

#include <format>
#include <deque>
#include <mutex>

namespace lldb::imgui {

extern std::mutex g_logMutex;
extern std::deque<std::string> g_log;

template<typename... Args>
void Log(std::format_string<Args...> fmt, Args&&... args) {
    std::scoped_lock lock(g_logMutex);

    g_log.push_back(std::vformat(fmt.get(), std::make_format_args(args...)));

    if (g_log.size() > 512) {
        g_log.pop_front();
    }
}

}
