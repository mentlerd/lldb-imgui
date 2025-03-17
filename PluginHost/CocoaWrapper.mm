#include "CocoaWrapper.h"

#import <Foundation/Foundation.h>
#import <Cocoa/Cocoa.h>

namespace lldb::imgui {

namespace {

NSString* ToString(const std::filesystem::path& path) {
    return [[NSString alloc] initWithUTF8String: reinterpret_cast<const char*>(path.u8string().c_str())];
}

}

void OpenFileInFinder(const std::filesystem::path& file) {
    [[NSWorkspace sharedWorkspace] selectFile:ToString(file) inFileViewerRootedAtPath:ToString(file.parent_path())];
}
void OpenFolderInFinder(const std::filesystem::path& folder) {
    [[NSWorkspace sharedWorkspace] selectFile:nil inFileViewerRootedAtPath:ToString(folder)];
}

}
