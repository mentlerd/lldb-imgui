
#include <lldb/API/LLDB.h>

#include <dlfcn.h>

namespace lldb {

#define API __attribute__((used))

API bool PluginInitialize(lldb::SBDebugger debugger) {
    return false;
}

}
