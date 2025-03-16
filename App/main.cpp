
#include <iostream>
#include <filesystem>
#include <vector>

#include <dlfcn.h>
#include <mach-o/dyld.h>

std::filesystem::path GetExecutablePath() {
    uint32_t capacity = 0;
    _NSGetExecutablePath(nullptr, &capacity);

    std::string buffer(capacity, 'A');
    _NSGetExecutablePath(buffer.data(), &capacity);

    return buffer;
}

int main(int argc, const char* argv[]) {
    auto plugin = GetExecutablePath().replace_filename("libPlugin.dylib");

    dlerror();

    auto handle = dlopen(plugin.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (!handle) {
        std::cout << dlerror() << std::endl;
        return 1;
    }

    auto symbol = dlsym(handle, "_ZN4lldb16PluginInitializeENS_10SBDebuggerE");
    if (!symbol) {
        std::cout << dlerror() << std::endl;
        return 2;
    }

    return 0;
}
