
#include "Cache.h"
#include "PathTree.h"

#include "CocoaWrapper.h"
#include "LLDB_Exposer.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "lldb/API/LLDB.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <map>
#include <any>

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
    SBValueUnwrapper() = delete;

    static lldb::ValueObjectSP Unwrap(const SBValue& value) {
        return reinterpret_cast<const SBValueUnwrapper&>(value).GetSP();
    }
    static SBValue Wrap(const lldb::ValueObjectSP& value) {
        SBValue result;
        reinterpret_cast<SBValueUnwrapper&>(result).SetSP(value);
        return result;
    }
};

lldb::ValueObjectSP Unwrap(SBValue value) {
    return SBValueUnwrapper::Unwrap(value);
}
SBValue Wrap(const lldb::ValueObjectSP& value) {
    return SBValueUnwrapper::Wrap(value);
}

}

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

/// Private LLDB APIs
namespace lldb_private {

class ExecutionContextScope {
    ExecutionContextScope() = delete;
};

class Target {
    Target() = delete;

public:
    auto ToCtx() {
        return reinterpret_cast<ExecutionContextScope*>(uintptr_t(this) + 0x240);
    }
};

class Process {
    Process() = delete;

public:
    auto ToCtx() {
        return reinterpret_cast<ExecutionContextScope*>(uintptr_t(this) + 0x78);
    }
};

class ValueObjectVariable {
    ValueObjectVariable() = delete;

public:
    static lldb::ValueObjectSP Create(ExecutionContextScope *exe_scope, const lldb::VariableSP &var_sp);
};
IFUNC(_ZN12lldb_private19ValueObjectVariable6CreateEPNS_21ExecutionContextScopeERKNSt3__110shared_ptrINS_8VariableEEE);

class VariableList {
    VariableList() = delete;

public:
    size_t GetSize() const;

    lldb::VariableSP GetVariableAtIndex(size_t idx) const;
};
IFUNC(_ZNK12lldb_private12VariableList18GetVariableAtIndexEm);
IFUNC(_ZNK12lldb_private12VariableList7GetSizeEv);

class CompileUnit {
    CompileUnit() = delete;

public:
    lldb::LanguageType GetLanguage();

    lldb::VariableListSP GetVariableList(bool);

    lldb::FunctionSP FindFunction(llvm::function_ref<bool(const lldb::FunctionSP&)> predicate);
};
IFUNC(_ZN12lldb_private11CompileUnit11GetLanguageEv);
IFUNC(_ZN12lldb_private11CompileUnit15GetVariableListEb);
IFUNC(_ZN12lldb_private11CompileUnit12FindFunctionEN4llvm12function_refIFbRKNSt3__110shared_ptrINS_8FunctionEEEEEE)

};


namespace lldb::imgui {

namespace {

static ImGuiStorage g_storage;

template<typename T>
auto& Store(const char* id) {
    std::any*& slot = *reinterpret_cast<std::any**>(g_storage.GetVoidPtrRef(ImGui::GetID(id), nullptr));

    if (slot == nullptr) {
        slot = new std::any(T());
    }
    return *std::any_cast<T>(slot);
}

template<typename T>
void Desc(T&& value) {
    lldb::SBStream stream;
    value.GetDescription(stream);
    ImGui::Text("%.*s", int(stream.GetSize()), stream.GetData());
}

void DrawModules(lldb::SBTarget& target) {
    using namespace ImGui;

    bool* onlyWithValidCompileUnits = g_storage.GetBoolRef(GetID("cu"), true);
    {
        Checkbox("With CUs", onlyWithValidCompileUnits);

        if (BeginItemTooltip()) {
            Text("Only show SBModules with valid SBCompileUnits");
            TextDisabled("(This is often a good indicator of debug info being present)");

            EndTooltip();
        }
    }

    ImGuiTableFlags flags = 0;

    flags |= ImGuiTableFlags_Hideable;
    flags |= ImGuiTableFlags_Reorderable;
    flags |= ImGuiTableFlags_BordersV;
    flags |= ImGuiTableFlags_BordersOuterH;
    flags |= ImGuiTableFlags_RowBg;
    flags |= ImGuiTableFlags_NoBordersInBody;
    flags |= ImGuiTableFlags_ScrollY;

    if (!BeginTable("modules", 4, flags)) {
        return;
    }

    TableSetupScrollFreeze(0, 1);
    TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder);
    TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed);
    TableSetupColumn("Triple", ImGuiTableColumnFlags_WidthFixed);
    TableSetupColumn("UUID", ImGuiTableColumnFlags_WidthFixed);
    TableHeadersRow();

    // PathTree from all modules
    PathTree<lldb::SBModule> tree;

    uint32_t numFiltered = 0;

    for (uint32_t i = 0; i < target.GetNumModules(); i++) {
        auto mod = target.GetModuleAtIndex(i);

        if (*onlyWithValidCompileUnits) {
            bool anyValid = false;

            for (uint32_t i = 0; i < mod.GetNumCompileUnits(); i++) {
                if (mod.GetCompileUnitAtIndex(i).IsValid()) {
                    anyValid = true;
                    break;
                }
            }

            if (!anyValid) {
                numFiltered++;
                continue; // Filtered
            }
        }

        tree.Put(mod.GetFileSpec()).value = mod;
    }

    // Render table entries
    const auto treeNode = [=](const std::string& path, const std::string& stem, PathTree<lldb::SBModule>& node) {
        auto& mod = node.value;

        TableNextRow();
        TableNextColumn();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns;

        if (mod) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        } else {
            flags |= ImGuiTreeNodeFlags_LabelSpanAllColumns;
        }

        bool isOpen = TreeNodeEx(stem.c_str(), flags);

        if (BeginPopupContextItem()) {
            TextDisabled("%s", path.c_str());

            if (Selectable("Copy")) {
                SetClipboardText(path.c_str());
            }
            if (Selectable("Show in Finder")) {
                if (node.value) {
                    OpenFileInFinder(path);
                } else {
                    OpenFolderInFinder(path);
                }
            }
            EndPopup();
        }

        if (!mod) {
            return isOpen;
        }

        if (TableNextColumn()) {
            uint32_t versions[3];

            // Unfortunately the SB API is quite wonky here, and doesn't really do
            // what is expected. Even if the is all zeros, the returned values
            // is are UINT32_MAX.0.0
            mod.GetVersion(versions, 3);

            if (versions[0] == UINT32_MAX) {
                TextDisabled("N/A");
            } else {
                auto buffer = std::to_string(versions[0]);

                if (versions[1] != UINT32_MAX) {
                    buffer.append(".");
                    buffer.append(std::to_string(versions[1]));

                    if (versions[2] != UINT32_MAX) {
                        buffer.append(".");
                        buffer.append(std::to_string(versions[2]));
                    }
                }

                Text("%s", buffer.c_str());
            }
        }
        if (TableNextColumn()) {
            Text("%s", mod.GetTriple());
        }
        if (TableNextColumn()) {
            Text("%s", mod.GetUUIDString());
        }
        return isOpen;
    };
    tree.Traverse(treeNode, TreePop);

    if (numFiltered) {
        TableNextRow();
        TableNextColumn();

        ImGuiTreeNodeFlags flags = 0;

        flags |= ImGuiTreeNodeFlags_Leaf;
        flags |= ImGuiTreeNodeFlags_SpanAllColumns;

        PushStyleColor(ImGuiCol_Text, GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (TreeNodeEx("###filtered", flags, "... (%d items filtered)", numFiltered)) {
            TreePop();
        }
        PopStyleColor();

        if (IsItemClicked()) {
            *onlyWithValidCompileUnits = false;
        }
        if (IsItemActive() && IsKeyPressed(ImGuiKey_Enter)) {
            *onlyWithValidCompileUnits = false;
        }

        if (BeginItemTooltip()) {
            Text("Click to clear filters");
            EndTooltip();
        }
    }

    EndTable();
}

void DrawFunc(lldb::SBFunction& func, lldb::SBTarget& target) {
    using namespace ImGui;

    if (CollapsingHeader("Blocks")) {
        std::function<void(SBBlock)> drawBlocks = [&](SBBlock start) {
            char buffer[128];

            uint32_t index = 0;
            for (auto block = start; block; block = block.GetSibling()) {
                snprintf(buffer, sizeof(buffer), "Block###%d", index++);

                auto child = block.GetFirstChild();

                ImGuiTreeNodeFlags flags = 0;

                if (!child) {
                    flags |= ImGuiTreeNodeFlags_Leaf;
                }

                if (TreeNodeEx(buffer, 0, "Block %s", block.GetInlinedName() ? "(inlined)" : "")) {
                    for (uint32_t i = 0; i < block.GetNumRanges(); i++) {
                        TextDisabled("[%llx-%llx)",
                                     block.GetRangeStartAddress(i).GetFileAddress(),
                                     block.GetRangeEndAddress(i).GetFileAddress());
                    }

                    for (auto i = 0; i < 3; i++) {
                        static const char* kLabel[3] = {
                            "arg", "local", "static"
                        };
                        auto vars = block.GetVariables(target, i == 0, i == 1, i == 2);

                        for (uint32_t j = 0; j < vars.GetSize(); j++) {
                            auto var = vars.GetValueAtIndex(j);

                            BulletText("%s", kLabel[i]);

                            SameLine();
                            Text("%s", var.GetName());

                            SameLine();
                            TextDisabled("%s", var.GetDisplayTypeName());
                        }
                    }

                    if (child) {
                        drawBlocks(child);
                    }
                    TreePop();
                }
            }
        };
        drawBlocks(func.GetBlock());
    }

    if (CollapsingHeader("Instructions")) {
        if (auto list = func.GetInstructions(target)) {
            SBLineEntry currLine;

            for (uint32_t i = 0; i < list.GetSize(); i++) {
                auto inst = list.GetInstructionAtIndex(i);

                if (auto line = inst.GetAddress().GetLineEntry()) {
                    if (currLine != line) {
                        currLine = line;

                        lldb::SBStream stream;
                        currLine.GetDescription(stream);

                        char buffer[256];
                        snprintf(buffer, sizeof(buffer), "%.*s", int(stream.GetSize()), stream.GetData());

                        SeparatorText(buffer);
                    }
                } else {
                    SameLine();
                    TextDisabled("No LineEntry");
                }

                Desc(inst);

                if (inst.DoesBranch()) {
                    SameLine();

                    addr_t rawAddr;
                    if (std::sscanf(inst.GetOperands(target), "%llx", &rawAddr) != 1) {
                        TextDisabled("(unknown branch)");
                    } else {
                        if (auto addr = target.ResolveFileAddress(rawAddr)) {
                            if (addr.GetFunction() == func) {
                                TextDisabled("(inner branch)");
                            } else {
                                SameLine();
                                Text("// %s", addr.GetFunction().GetDisplayName());
                            }
                        } else {
                            TextDisabled("(invalid branch)");
                        }
                    }
                }
            }
        }
    }
}

void DrawCompileUnitGlobals(lldb::SBTarget& target, lldb::SBCompileUnit& cu) {
    struct Data {
        std::vector<SBValue> globals;
        std::vector<std::pair<SBFunction, std::vector<SBValue>>> functions;
    };
    using Tree = PathTree<Data>;

    auto treeBuilder = [&](lldb_private::CompileUnit* cu) {
        Tree tree;

        lldb_private::ExecutionContextScope* exe_ctx = Unwrap(target)->ToCtx();

        if (auto proc = target.GetProcess()) {
            exe_ctx = Unwrap(proc).lock()->ToCtx();
        }

        if (auto list = cu->GetVariableList(true)) {
            for (size_t i = 0; i < list->GetSize(); i++) {
                auto var = list->GetVariableAtIndex(i);
                if (!var) {
                    continue;
                }

                auto valobj = lldb_private::ValueObjectVariable::Create(exe_ctx, var);
                if (!valobj) {
                    continue;
                }

                SBValue value = Wrap(valobj);

                if (auto spec = value.GetDeclaration().GetFileSpec()) {
                    tree.Put(spec).value.globals.push_back(value);
                }
            }
        }

        cu->FindFunction([&](const lldb::FunctionSP& ptr) mutable {
            SBFunction func = Wrap(ptr.get());

            auto spec = func.GetStartAddress().GetLineEntry().GetFileSpec();
            if (!spec) {
                return false;
            }

            std::vector<SBValue> globals;

            std::function<void(SBBlock)> visit = [&](SBBlock start) {
                for (auto block = start; block.IsValid(); block = block.GetSibling()) {
                    auto vars = block.GetVariables(target, false, false, true);
                    for (auto i = 0; i < vars.GetSize(); i++) {
                        globals.push_back(vars.GetValueAtIndex(i));
                    }

                    visit(block.GetFirstChild());
                }
            };
            visit(func.GetBlock());

            if (!globals.empty()) {
                tree.Put(spec).value.functions.emplace_back(func, globals);
            }
            return false;
        });

        return tree;
    };

    // Building a tree of statics takes a while, cache the data
    static Cache<lldb_private::CompileUnit*, Tree> g_cache(__func__);

    Tree& tree = g_cache.GetOrCreate(Unwrap(cu), treeBuilder);

    // The rest is UI code
    using namespace ImGui;

    auto treeNode = [&](auto&, const std::string& stem, PathTree<Data>& node) {
        if (!TreeNode(stem.c_str())) {
            return false;
        }

        if (!node.value.globals.empty()) {
            if (TreeNodeEx("globals", ImGuiTreeNodeFlags_Bullet, "(globals)")) {
                for (SBValue& value : node.value.globals) {
                    Desc(value);
                    SameLine();
                    Desc(value.GetAddress().GetSymbol());
                }
                TreePop();
            }
        }
        for (auto& [func, globals] : node.value.functions) {
            if (TreeNodeEx(Unwrap(func), ImGuiTreeNodeFlags_Bullet, "(func) %s", func.GetDisplayName())) {
                for (SBValue& value : globals) {
                    Desc(value);
                    SameLine();
                    Desc(value.GetAddress().GetSymbol());
                }
                TreePop();
            }
        }
        return true;
    };
    tree.Traverse(treeNode, TreePop);
}

void DrawCompileUnits(lldb::SBTarget& target) {
    using namespace ImGui;

    for (uint32_t i = 0; i < target.GetNumModules(); i++) {
        auto mod = target.GetModuleAtIndex(i);

        PathTree<lldb::SBCompileUnit> tree;
        bool anyValid = false;

        for (uint32_t j = 0; j < mod.GetNumCompileUnits(); j++) {
            auto cu = mod.GetCompileUnitAtIndex(j);
            if (!cu.IsValid()) {
                continue;
            }

            anyValid = true;
            tree.Put(cu.GetFileSpec()).value = std::move(cu);
        }

        if (!anyValid) {
            continue;
        }

        SeparatorText(mod.GetFileSpec().GetFilename());
        PushID(i);

        const auto treeNode = [&](auto&, const std::string& stem, PathTree<lldb::SBCompileUnit>& node) {
            SBCompileUnit& cu = node.value;

            if (!cu) {
                return TreeNode(stem.c_str());
            }

            if (TreeNode(stem.c_str())) {
                DrawCompileUnitGlobals(target, cu);
                TreePop();
            }

            return false;
        };
        tree.Traverse(treeNode, TreePop);

        PopID();
    }
}

bool CollapsingHeader2(const char* label, ImGuiTreeNodeFlags flags = 0) {
    struct Scope {
        Scope() :padding(ImGui::GetCurrentWindow()->WindowPadding.x) {
            backup = std::exchange(padding, 0);
        }
        ~Scope() {
            padding = backup;
        }

        float& padding;
        float backup;
    } scope;

    return ImGui::CollapsingHeader(label, flags);
}

void Draw(lldb::SBFrame frame) {
    using namespace ImGui;

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "#%d: %s ###frame", frame.GetFrameID(), frame.GetDisplayFunctionName());

    bool isOpen = false;

    PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2());
    if (BeginTable("FrameHeaderTable", 2)) {
        TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        TableSetupColumn("Focus", ImGuiTableColumnFlags_WidthFixed);

        TableNextRow();
        TableNextColumn();

        PushID(frame.GetFrameID());
        isOpen = CollapsingHeader2(buffer);
        PopID();

        TableNextColumn();

        if (Button("!", ImVec2(GetTextLineHeight() * 2, 0))) {
            auto thread = frame.GetThread();
            auto debugger = thread.GetProcess().GetTarget().GetDebugger();

            lldb::SBCommandReturnObject result;

            snprintf(buffer, sizeof(buffer), "thread select %d", thread.GetIndexID());
            debugger.GetCommandInterpreter().HandleCommand(buffer, result);

            snprintf(buffer, sizeof(buffer), "frame select %d", frame.GetFrameID());
            debugger.GetCommandInterpreter().HandleCommand(buffer, result);
        }
        if (IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
            SetTooltip("Select this frame as the active (will synchronize with Xcode)");
        }
        EndTable();
    }
    PopStyleVar();

    if (isOpen) {
        TreePush((void*) uint64_t(frame.GetFrameID()));
        Text("Dummy");
        TreePop();
    }
}

void Draw(lldb::SBThread thread) {
    using namespace ImGui;

    char buffer[128];
    snprintf(buffer,
             sizeof(buffer),
             "%s (%d) %s ###thread",
             thread.GetName() ? thread.GetName() : "Thread",
             thread.GetIndexID(),
             thread.GetQueueName() ? thread.GetQueueName() : "");

    bool isOpen = false;

    PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2());
    if (BeginTable("FrameHeaderTable", 2)) {
        TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        TableSetupColumn("Pin", ImGuiTableColumnFlags_WidthFixed);

        TableNextRow();
        TableNextColumn();

        PushID((void*) thread.GetThreadID());
        isOpen = CollapsingHeader2(buffer);
        PopID();

        TableNextColumn();

        bool checked = false;
        if (Checkbox("###pin", &checked)) {
            // TODO: Build own datamodel, or piggyback on ImGui?
        }
        if (IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
            SetTooltip("Pin this thread");
        }

        EndTable();
    }
    PopStyleVar();

    if (isOpen) {
        TreePush((void*) thread.GetThreadID());
        for (uint32_t f = 0; f < thread.GetNumFrames(); f++) {
            Draw(thread.GetFrameAtIndex(f));
        }
        TreePop();
    }
}

void DrawThreads(lldb::SBTarget target) {
    auto proc = target.GetProcess();
    if (!proc.IsValid()) {
        ImGui::TextDisabled("No process");
        return;
    }

    for (uint32_t i = 0; i < proc.GetNumThreads(); i++) {
        Draw(proc.GetThreadAtIndex(i));
    }
}

}

#define API __attribute__((used))

API void Draw() {
    CacheBase::Tick();
}

API void Draw(lldb::SBDebugger& debugger) {
    using namespace ImGui;

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "Debugger %llu###SBDebugger(%llu)", debugger.GetID(), debugger.GetID());

    SetNextWindowSize(ImVec2(1050, 600), ImGuiCond_Once);

    if (Begin(buffer, nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        auto target = debugger.GetSelectedTarget();
        if (!target) {
            TextDisabled("Debugger has no selected target.");
        } else {
            lldb::SBStream stream;
            target.GetDescription(stream, lldb::DescriptionLevel::eDescriptionLevelBrief);

            Text("Selected target: %.*s", int(stream.GetSize()), stream.GetData());

            if (BeginTabBar("tabs")) {
                if (BeginTabItem("Threads")) {
                    DrawThreads(target);
                    EndTabItem();
                }
                if (BeginTabItem("Modules")) {
                    DrawModules(target);
                    EndTabItem();
                }
                if (BeginTabItem("CUs")) {
                    DrawCompileUnits(target);
                    EndTabItem();
                }
                EndTabBar();
            }
        }
    }
    End();
}

}
