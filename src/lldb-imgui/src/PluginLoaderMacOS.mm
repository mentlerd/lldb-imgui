#include "PluginLoader.h"

#include "Expose.h"

#include "imgui.h"

#include <Foundation/Foundation.h>
#include <CoreServices/CoreServices.h>

#include <dlfcn.h>
#include <mach-o/utils.h>
#include <mach-o/getsect.h>

#include <print>
#include <filesystem>
#include <functional>

namespace lldb::imgui {

class FileSystemWatcher {
public:
    FileSystemWatcher(std::filesystem::path fsPath, bool isDirectory, std::function<void()> callback)
    : _callback(std::move(callback)) {
        CFAllocatorRef allocator = kCFAllocatorDefault;

        CFStringRef path = CFStringCreateWithCString(allocator, fsPath.string().c_str(), kCFStringEncodingUTF8);
        CFArrayRef pathsToWatch = CFArrayCreate(allocator, (const void**) &path, 1, NULL);

        CFAbsoluteTime latency = 1.0;

        FSEventStreamCallback fsCallback = [](ConstFSEventStreamRef stream,
                                              void* info,
                                              size_t numEvents,
                                              void* eventPaths,
                                              const FSEventStreamEventFlags* eventFlags,
                                              const FSEventStreamEventId* eventIds) {
            reinterpret_cast<FileSystemWatcher*>(info)->_callback();
        };
        FSEventStreamContext fsContext {
            .version = 0,
            .info = this,
            .retain = nullptr,
            .release = nullptr,
            .copyDescription = nullptr
        };
        _stream = FSEventStreamCreate(allocator,
                                      fsCallback,
                                      &fsContext,
                                      pathsToWatch,
                                      kFSEventStreamEventIdSinceNow,
                                      latency,
                                      isDirectory ? kFSEventStreamCreateFlagNone : kFSEventStreamCreateFlagFileEvents);

        CFRelease(pathsToWatch);
        CFRelease(path);

        FSEventStreamSetDispatchQueue(_stream, dispatch_get_main_queue());
        FSEventStreamStart(_stream);
    }

    ~FileSystemWatcher() {
        FSEventStreamStop(_stream);
        FSEventStreamInvalidate(_stream);
        FSEventStreamRelease(_stream);
    }

private:
    FSEventStreamRef _stream;
    std::function<void()> _callback;
};

class PluginLoaderMacOS final : public PluginLoader {
public:
    void Update(PluginID, PluginSpec) override;
    void Remove(PluginID) override;

    void DrawMenu(PluginID) override;

    void DrawPlugins() override;
    void DrawDebugger(lldb::SBDebugger&) override;

private:
    struct Plugin {
        PluginSpec spec;

        void* handle = nullptr;
        std::string status;

        std::optional<FileSystemWatcher> watcher;

        // DSO
        void (*draw)() = nullptr;
        void (*drawDebugger)(lldb::SBDebugger&) = nullptr;

        void Load();
        void Unload();
    };

    std::unordered_map<PluginID, Plugin> _plugins;
};

void PluginLoaderMacOS::Update(PluginID id, PluginSpec spec) {
    auto [it, added] = _plugins.try_emplace(id);
    auto& plugin = it->second;

    if (added) {
        plugin.spec.isEnabled = false;
        plugin.spec.isAutoReload = false;
    }

    const auto old = std::exchange(plugin.spec, spec);

    if (spec.path != old.path || spec.isEnabled != old.isEnabled) {
        if (spec.isEnabled) {
            plugin.Load();
        } else {
            plugin.Unload();
        }
    }
    if (spec.path != old.path || spec.isAutoReload != old.isAutoReload) {
        if (spec.isAutoReload) {
            auto callback = [this, id] {
                auto it = _plugins.find(id);
                if (it == _plugins.end()) {
                    return;
                }

                if (it->second.spec.isEnabled) {
                    it->second.Load();
                }
            };
            plugin.watcher.emplace(spec.path, false, callback);
        } else {
            plugin.watcher.reset();
        }
    }

    return true;
}

void PluginLoaderMacOS::Remove(PluginID id) {
    _plugins.erase(id);
}

void PluginLoaderMacOS::DrawMenu(PluginID id) {
    using namespace ImGui;

    auto it = _plugins.find(id);
    if (it == _plugins.end()) {
        return;
    }

    const Plugin& plugin = it->second;

    if (plugin.status.size() != 0) {
        TextDisabled("%s", plugin.status.c_str());
    }
}

void PluginLoaderMacOS::DrawPlugins() {
    for (auto& [_, plugin] : _plugins) {
        if (plugin.draw) {
            plugin.draw();
        }
    }
}

void PluginLoaderMacOS::DrawDebugger(lldb::SBDebugger& debugger) {
    for (auto& [_, plugin] : _plugins) {
        if (plugin.drawDebugger) {
            plugin.drawDebugger(debugger);
        }
    }
}

void PluginLoaderMacOS::Plugin::Load() {
    std::string path = spec.path.string();

    // Reset status
    status = "";

    // Normal dyld preflight
    if (!dlopen_preflight(path.c_str())) {
        status = dlerror();
        return;
    }

    // Check whether all EXPOSE'd symbols are available
    macho_best_slice(path.c_str(), ^(const struct mach_header* slice, uint64_t sliceOffset, size_t sliceSize) {
        auto* header = reinterpret_cast<const struct mach_header_64*>(slice);

        unsigned long size = 0;
        auto symbol = (const char*) getsectiondata(header, "__CONST", "exposed_symbols", &size);
        auto end = symbol + size;

        for (; symbol < end; symbol += strlen(symbol) + 1) {
            if (Expose(symbol)) {
                continue;
            }

            if (status.empty()) {
                status = "Failed to load: EXPOSE'd symbol(s) missing:";
            }
            status.append(std::format("\n - {}", symbol));
        }
    });
    if (!status.empty()) {
        return;
    }

    // Preflights passed, displace current version
    Unload();

    handle = dlopen(spec.path.c_str(), RTLD_LOCAL | RTLD_NOW);

    if (!handle) {
        status = std::format("Failed to load: {}", dlerror());
        return;
    }

    draw = reinterpret_cast<decltype(draw)>(dlsym(handle, "_Z4Drawv"));
    drawDebugger = reinterpret_cast<decltype(drawDebugger)>(dlsym(handle, "_Z12DrawDebuggerRN4lldb10SBDebuggerE"));

    status = "Loaded";
}

void PluginLoaderMacOS::Plugin::Unload() {
    draw = nullptr;
    drawDebugger = nullptr;

    dlclose(std::exchange(handle, nullptr));
}

std::unique_ptr<PluginLoader> PluginLoader::Create() {
    return std::make_unique<PluginLoaderMacOS>();
}

}
