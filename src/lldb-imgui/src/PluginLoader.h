#pragma once

#include <cstddef>
#include <filesystem>

namespace lldb {
class SBDebugger;
}

namespace lldb::imgui {

/// Unique identifier of a plugin instance
using PluginID = uint32_t;

/// Specification of a plugin
struct PluginSpec {
    std::filesystem::path path;

    bool isEnabled = true;
    bool isAutoReload = false;
};

/// Platform specific plugin loader implementation
class PluginLoader {
public:
    static std::unique_ptr<PluginLoader> Create();

    virtual ~PluginLoader() = default;

    virtual void Update(PluginID, PluginSpec) = 0;
    virtual void Remove(PluginID) = 0;

    virtual void DrawMenu(PluginID) = 0;

    // TODO: Create a proper extension manager
    virtual void DrawPlugins() = 0;
    virtual void DrawDebugger(lldb::SBDebugger&) = 0;
};

}
