#pragma once

#include <stddef.h>

namespace lldb::imgui {

/// Alternate implementation of `dlsym` using LLDB to find private symbols inside
/// - `lldb-rpc-server` if we happen to be loaded into it
/// - `LLDB.framework` if it happens to be loaded
void* Expose(const char* symbol);

template<typename T>
T* Expose(const char* symbol) {
    return reinterpret_cast<T*>(Expose(symbol));
}

/// Forbidden magic for linking to private LLDB symbols - https://maskray.me/blog/2021-01-18-gnu-indirect-function
///
/// Annoyingly the `NAME_resolver()` functions keep getting generated in a way that clobbers x8, which
/// is used for return value optimization in C++ ...
///
/// To not ruin the call, `x8` is saved in `x16` which is specified to be clobberable across call boundaries
/// https://developer.arm.com/documentation/102374/0102/Procedure-Call-Standard
#define EXPOSE(NAME)                                                                 \
    extern "C" {                                                                     \
        __attribute__((section("__CONST,exposed_symbols")))                          \
        static const char* NAME##_symbol = #NAME;                                    \
                                                                                     \
        void* NAME##_addr = lldb::imgui::Expose(NAME##_symbol);                      \
        void* NAME##_resolver() {                                                    \
            __asm__ volatile("mov x16,x8");                                          \
            auto tmp = NAME##_addr;                                                  \
            __asm__ volatile("mov x8,x16");                                          \
            return tmp;                                                              \
        }                                                                            \
        __attribute__((ifunc(#NAME "_resolver")))                                    \
        void NAME();                                                                 \
    }

}
