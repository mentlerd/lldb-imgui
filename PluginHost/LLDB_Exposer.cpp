#include "LLDB_Exposer.h"

#include "Logging.h"

#include "lldb/API/LLDB.h"

#include <unistd.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>

namespace lldb::imgui {

void* ResolvePrivateSymbol(const char* symbol) {
    struct Resolver {
        Resolver() {
            // Some symbol from LLDB.framework that is guaranteed to exist
            const char* kSBDebuggerCreate = "_ZN4lldb10SBDebugger6CreateEb";

            // Find where it is loaded into the current process
            auto loadAddr = dlsym(RTLD_SELF, kSBDebuggerCreate);
            if (!loadAddr) {
                return;
            }
            if (!dladdr(loadAddr, &_anchor)) {
                return;
            }

            // Parse lib into an SBModule
            _module = lldb::SBDebugger::Create(false).CreateTarget(_anchor.dli_fname).GetModuleAtIndex(0);

            // Find same symbol to calculate slide
            auto symAddr = _module.FindSymbol(kSBDebuggerCreate).GetStartAddress();
            if (!symAddr.IsValid()) {
                return;
            }

            _slide = uintptr_t(loadAddr) - symAddr.GetOffset();
        }

        void* Resolve(const char* symbol) {
            Log("Resolve: {}", symbol);

            auto addr = _module.FindSymbol(symbol).GetStartAddress();
            if (!addr.IsValid()) {
                Log("Not found");
                return nullptr;
            }

            void* loadAddr = reinterpret_cast<void*>(_slide + addr.GetOffset());
            Log("{}", loadAddr);

            Dl_info info;
            if (!dladdr(loadAddr, &info)) {
                Log("dladdr failed");
                return nullptr;
            }
            if (info.dli_fbase != _anchor.dli_fbase) {
                Log("Incorrect image: {} vs {}", info.dli_fbase, _anchor.dli_fbase);
                return nullptr; // Address is not even in the correct image
            }
            if (std::strcmp(info.dli_sname, symbol) != 0) {
                Log("Unexpected symbol: {}", info.dli_sname);
                return nullptr; // Found something else
            }
            if (info.dli_saddr != loadAddr) {
                Log("Misaligned symbol: {} vs {}", info.dli_saddr, loadAddr);
                return nullptr; // Misaligned function
            }

            return loadAddr;
        }

        lldb::SBModule _module;
        Dl_info _anchor;

        ptrdiff_t _slide = 0;
    };
    static Resolver g_resolver;

    return g_resolver.Resolve(symbol);
}

}
