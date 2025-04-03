#include "Expose.h"

#include "spdlog/spdlog.h"

#include "lldb/API/SBDebugger.h"

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

#include <dlfcn.h>
#include <unistd.h>

namespace lldb::imgui {

static void LogAdapter(const char* message, void* baton) {
    spdlog::info("[SelfDebugger] {}", message);
}

static SBTarget CreateSelfReflection() {
    auto status = SBDebugger::InitializeWithErrorHandling();
    if (!status.Success()) {
        spdlog::error("Failed to initialize LLDB API: {}", status.GetCString());
        return {};
    }
    
    auto debugger = SBDebugger::Create(false, LogAdapter, nullptr);
    if (!debugger.IsValid()) {
        spdlog::error("Failed to create self-debugger!");
        return {};
    }

    auto target = debugger.CreateTarget(nullptr);

    auto loadModuleWithAddr = [&](std::string_view label, void* addr) {
        Dl_info info;
        if (!dladdr(addr, &info)) {
            spdlog::warn("Failed to load module for '{}'", label);
            return;
        }

        auto mod = target.AddModule(info.dli_fname, nullptr, nullptr);

        for (auto i = 0; i < mod.GetNumSections(); i++) {
            auto segment = mod.GetSectionAtIndex(i);

            for (auto j = 0; j < segment.GetNumSubSections(); j++) {
                auto section = segment.GetSubSectionAtIndex(j);

                unsigned long size = 0;
                void* base = getsectiondata((struct mach_header_64*) info.dli_fbase, segment.GetName(), section.GetName(), &size);

                auto err = target.SetSectionLoadAddress(section, reinterpret_cast<addr_t>(base));
                if (!err.Success()) {
                    spdlog::warn("Failed to slide {}.{} in {}", segment.GetName(), section.GetName(), info.dli_fname);
                }
            }
        }
    };

    loadModuleWithAddr("executable", dlsym(RTLD_MAIN_ONLY, MH_EXECUTE_SYM));
    loadModuleWithAddr("lldb", (void*) SBDebugger::Initialize);

    return target;
}

void* Expose(const char* name) {
    static SBTarget self = CreateSelfReflection();

    SBSymbolContextList list = self.FindSymbols(name);
    if (list.GetSize() != 1) {
        return nullptr;
    }

    SBSymbol symbol = list.GetContextAtIndex(0).GetSymbol();
    addr_t loadAddr = symbol.GetStartAddress().GetLoadAddress(self);
    return reinterpret_cast<void*>(loadAddr);
}

}
