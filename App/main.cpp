
#include <filesystem>
#include <iostream>
#include <print>
#include <unordered_map>

#include <dlfcn.h>
#include <mach-o/dyld.h>

static std::string g_static = "Hello!";

std::filesystem::path GetExecutablePath() {
    uint32_t capacity = 0;
    _NSGetExecutablePath(nullptr, &capacity);

    std::string buffer(capacity, 'A');
    _NSGetExecutablePath(buffer.data(), &capacity);

    return buffer;
}

int main(int argc, const char* argv[]) {
    // This is the command you are looking for :)
    std::println("plugin load {}", GetExecutablePath().replace_filename("libPluginHost.dylib").generic_string());

    // Create some stack variables for inspecting
    std::unordered_map<int, std::unordered_map<int, std::string>> maps;

    for (auto i = 0; i < 16; i++) {
        for (auto j = 0; j < 100; j++) {
            maps[i][j] = std::format("{}, {}", i, j);
        }
    }

    __builtin_debugtrap();
    return (int) g_static.size();
}
