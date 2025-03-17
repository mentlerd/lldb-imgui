#pragma once

#include <filesystem>

/// Libraries using Cocoa cannot be unloaded - to prevent this all Cocoa functionality
/// required by plugins is exposed by the plugin host instead as pure old C++ APIs
namespace lldb::imgui {

void OpenFileInFinder(const std::filesystem::path& file);
void OpenFolderInFinder(const std::filesystem::path& folder);

}
