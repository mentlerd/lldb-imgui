#pragma once

#include <stddef.h>

namespace lldb::imgui {

/// Alternate implementation of `dlsym` using LLDB to find private symbols inside
/// - `lldb-rpc-server` if we happen to be loaded into it
/// - `LLDB.framework` if it happens to be loaded
void* Expose(const char* symbol);

}
