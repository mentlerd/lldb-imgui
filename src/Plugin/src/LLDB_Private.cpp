#include "LLDB_Private.h"

#include "LLDB_Exposer.h"

#include "lldb/Core/RichManglingContext.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/VariableList.h"

#include "clang/AST/Decl.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Casting.h"


// MARK: - IFUNC

/// Forbidden magic for linking to private LLDB symbols - https://maskray.me/blog/2021-01-18-gnu-indirect-function
///
/// Annoyingly the `NAME_resolver()` functions keep getting generated in a way that clobbers x8, which
/// is used for return value optimization in C++ ...
///
/// To not ruin the call, `x8` is saved in `x16` which is specified to be clobberable across call boundaries
/// https://developer.arm.com/documentation/102374/0102/Procedure-Call-Standard
#define IFUNC(NAME)                                                    \
    extern "C" {                                                       \
        void* NAME##_addr = lldb::imgui::ResolvePrivateSymbol(#NAME);  \
        void* NAME##_resolver() {                                      \
            __asm__ volatile("mov x16,x8");                            \
            auto tmp = NAME##_addr;                                    \
            __asm__ volatile("mov x8,x16");                            \
            return tmp;                                                \
        }                                                              \
        __attribute__((ifunc(#NAME "_resolver")))                      \
        void NAME();                                                   \
    }

namespace lldb_private {

ExecutionContextScope* ToExeScope(Target* ptr) {
    return reinterpret_cast<ExecutionContextScope*>(uintptr_t(ptr) + 0x240);
}
ExecutionContextScope* ToExeScope(Process* ptr) {
    return reinterpret_cast<ExecutionContextScope*>(uintptr_t(ptr) + 0x78);
}

class ValueObjectVariable {
public:
    /// Bringing in `ValueObjectVariable.h` would require additional symbols, redeclare the needed function instead
    static lldb::ValueObjectSP Create(ExecutionContextScope* exe_scope, const lldb::VariableSP& var_sp);
};
IFUNC(_ZN12lldb_private19ValueObjectVariable6CreateEPNS_21ExecutionContextScopeERKNSt3__110shared_ptrINS_8VariableEEE);

class VariableList;
IFUNC(_ZNK12lldb_private12VariableList7GetSizeEv);
IFUNC(_ZNK12lldb_private12VariableList18GetVariableAtIndexEm);

class CompileUnit;
IFUNC(_ZN12lldb_private11CompileUnit15GetVariableListEb);
IFUNC(_ZN12lldb_private11CompileUnit12FindFunctionEN4llvm12function_refIFbRKNSt3__110shared_ptrINS_8FunctionEEEEEE)

class Function;
IFUNC(_ZN12lldb_private8Function14GetDeclContextEv);

class Mangled;
IFUNC(_ZN12lldb_private7Mangled19GetRichManglingInfoERNS_19RichManglingContextEPFbN4llvm9StringRefENS0_14ManglingSchemeEE);

class RichManglingContext;
IFUNC(_ZN12lldb_private19RichManglingContextD1Ev);
IFUNC(_ZN12lldb_private19RichManglingContext21ParseFunctionBaseNameEv);

class CompilerDecl;
IFUNC(_ZNK12lldb_private12CompilerDecl14GetDeclContextEv);

class TypeSystemClang {
public:
    /// This function is redeclared as non-virtual to devirtualize the call to it. The mangled name happens to be the same
    CompilerType GetTypeForDecl(void *opaque_decl);
};
IFUNC(_ZN12lldb_private15TypeSystemClang14GetTypeForDeclEPv);

}

namespace clang {

class Decl;
IFUNC(_ZN5clang4Decl19castFromDeclContextEPKNS_11DeclContextE);

}

// MARK: - Unwrap
namespace lldb::imgui {

/// SB API objects are a thin ABI stable wrapper around pointers to LLDB implementation details. This
/// trick allows access to the underlying pointer for further shenanigans
#define UNWRAP(API, IMPL)                          \
    static_assert(alignof(API) == alignof(IMPL));  \
    static_assert(sizeof(API) == sizeof(IMPL));    \
                                                   \
    IMPL Unwrap(API value) {                       \
        return reinterpret_cast<IMPL&>(value);     \
    }                                              \
    API Wrap(IMPL value) {                         \
        return reinterpret_cast<API&>(value);      \
    }

UNWRAP(SBDebugger, DebuggerSP);
UNWRAP(SBTarget, TargetSP);
UNWRAP(SBProcess, ProcessWP);
UNWRAP(SBCompileUnit, lldb_private::CompileUnit*);
UNWRAP(SBFunction, lldb_private::Function*);

/// SBValue is a little different, the implementation contains some additional fields, but luckily they provide
/// protected varsions of getters/setters. We can abuse that to build our wrap/unwrap primitives
class SBValueUnwrapper final : public SBValue {
public:
    using SBValue::SBValue;

    static lldb::ValueObjectSP Unwrap(const SBValue& value) {
        return reinterpret_cast<const SBValueUnwrapper&>(value).GetSP();
    }
    static SBValue Wrap(const lldb::ValueObjectSP& value) {
        return SBValueUnwrapper(value);
    }
};

lldb::ValueObjectSP Unwrap(SBValue value) {
    return SBValueUnwrapper::Unwrap(value);
}
SBValue Wrap(const lldb::ValueObjectSP& value) {
    return SBValueUnwrapper::Wrap(value);
}

class SBTypeUnwrapper final : public SBType {
public:
    using SBType::SBType;

    static SBType Wrap(lldb_private::CompilerType value) {
        return SBTypeUnwrapper(value);
    }
};

SBType Wrap(lldb_private::CompilerType value) {
    return SBTypeUnwrapper::Wrap(value);
}

}

// MARK: - API
namespace lldb::imgui {

using namespace lldb_private;

void ForEachVariable(SBCompileUnit cu, SBTarget target, Visitor<SBValue> visitor) {
    if (!cu.IsValid()) {
        return;
    }

    auto vars = Unwrap(cu)->GetVariableList(true);
    if (!vars) {
        return;
    }

    ExecutionContextScope* scope = ToExeScope(Unwrap(target).get());

    if (auto proc = target.GetProcess()) {
        scope = ToExeScope(Unwrap(proc).lock().get());
    }

    for (size_t i = 0; i < vars->GetSize(); i++) {
        SBValue value = Wrap(ValueObjectVariable::Create(scope, vars->GetVariableAtIndex(i)));

        if (bool stop = visitor(value)) {
            return;
        }
    }
}

void ForEachFunction(SBCompileUnit cu, Visitor<SBFunction> visitor) {
    if (!cu.IsValid()) {
        return;
    }

    Unwrap(cu)->FindFunction([&](const lldb::FunctionSP& ptr) {
        SBFunction func = Wrap(ptr.get());

        if (bool stop = visitor(func)) {
            return true;
        }

        return false;
    });
}

std::string GetFunctionBaseName(SBFunction func) {
    if (!func.IsValid()) {
        return "";
    }

    RichManglingContext ctx;

    auto& mangled = const_cast<Mangled&>(Unwrap(func)->GetMangled());
    if (!mangled.GetRichManglingInfo(ctx, nullptr)) {
        return "<error>";
    }

    return ctx.ParseFunctionBaseName().str();
}

SBType GetClassOfMemberFunction(SBFunction func) {
    if (!func) {
        return SBType();
    }

    CompilerDeclContext ctx = Unwrap(func)->GetDeclContext();

    TypeSystem* typeSystemRaw = ctx.GetTypeSystem();
    TypeSystemClang* typeSystem = reinterpret_cast<lldb_private::TypeSystemClang*>(typeSystemRaw);

    SBType clazz;

    for (auto i = 0; i < 3; i++) {
        auto* ctxClang = reinterpret_cast<clang::DeclContext*>(ctx.GetOpaqueDeclContext());

        auto decl = clang::dyn_cast_or_null<clang::Decl>(ctxClang);
        if (!decl) {
            break;
        }

        if (auto clazz = Wrap(typeSystem->GetTypeForDecl(decl))) {
            return clazz;
        }

        ctx = lldb_private::CompilerDecl(typeSystemRaw, decl).GetDeclContext();
    }

    return SBType();
}

}
