#include "Expose.h"

#include "spdlog/spdlog.h"

#include "lldb/API/SBDebugger.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

#include <dlfcn.h>
#include <unistd.h>

namespace lldb::imgui {

static void LogAdapter(const char* message, void* baton) {
    spdlog::info("[SelfDebugger] {}", message);
}

static std::optional<std::string> GetImageUUID(void* imageBase) {
    auto headerAddr = reinterpret_cast<uintptr_t>(imageBase);

    auto header = reinterpret_cast<struct mach_header_64*>(headerAddr);
    if (header->magic != MH_MAGIC_64) {
        return std::nullopt;
    }

    auto cmdAddr = headerAddr + sizeof(struct mach_header_64);

    for (uint32_t cmdIndex = 0; cmdIndex < header->ncmds; cmdIndex++) {
        auto* command = reinterpret_cast<struct load_command*>(cmdAddr);

        if (command->cmd == LC_UUID) {
            auto* command = reinterpret_cast<struct uuid_command*>(cmdAddr);

            std::string uuid;

            for (uint32_t i = 0; i < sizeof(command->uuid); i++) {
                if (i == 4 || i == 6 || i == 8 || i == 10) {
                    uuid.append("-");
                }
                uuid.append(std::format("{:02X}", command->uuid[i]));
            }
            return uuid;
        }

        cmdAddr += command->cmdsize;
    }

    return std::nullopt;
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

        auto uuid = GetImageUUID(info.dli_fbase);
        if (!uuid) {
            spdlog::error("Failed to determine UUID for '{}'", label);
            return;
        }

        auto mod = target.AddModule(info.dli_fname, nullptr, uuid->c_str());

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

    // Always reflect on LLDB - having this first makes it the preferred source of symbols
    loadModuleWithAddr("lldb", (void*) SBDebugger::Initialize);

    // Reflect on host executable if it isn't us
    if (auto* header = dlsym(RTLD_MAIN_ONLY, MH_EXECUTE_SYM)) {
        if (header != &_mh_execute_header) {
            loadModuleWithAddr("host", header);
        }
    }

    return target;
}

void* Expose(const char* name) {
    static SBTarget self = CreateSelfReflection();

    for (uint32_t i = 0; i < self.GetNumModules(); i++) {
        auto mod = self.GetModuleAtIndex(i);

        auto symbol = mod.FindSymbol(name);
        if (!symbol.IsValid()) {
            continue;
        }

        addr_t loadAddr = symbol.GetStartAddress().GetLoadAddress(self);
        return reinterpret_cast<void*>(loadAddr);
    }

    return nullptr;
}

}
