#pragma once

#include "lldb/API/LLDB.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace lldb::imgui {

lldb_private::CompileUnit* Unwrap(SBCompileUnit value);
lldb_private::Function* Unwrap(SBFunction value);

template<typename T>
using Visitor = llvm::function_ref<bool(T)>;

void ForEachVariable(SBCompileUnit cu, SBTarget target, Visitor<SBValue> visitor);
void ForEachFunction(SBCompileUnit cu, Visitor<SBFunction> visitor);

std::string GetFunctionBaseName(SBFunction func);
SBType GetClassOfMemberFunction(SBFunction func);

}
