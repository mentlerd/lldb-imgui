
#include "Cache.h"
#include "PathTree.h"

#include "CocoaWrapper.h"
#include "LLDB_Private.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "lldb/API/LLDB.h"

#include <map>
#include <any>

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

template<typename T, typename... Args>
void Desc(T&& value, Args&&... args) {
    lldb::SBStream stream;
    value.GetDescription(stream, std::forward<Args>(args)...);
    ImGui::Text("%.*s", int(stream.GetSize()), stream.GetData());
}

bool BeginValueTable() {
    using namespace ImGui;

    ImGuiTableFlags flags = 0;

    flags |= ImGuiTableFlags_Hideable;
    flags |= ImGuiTableFlags_Reorderable;
    flags |= ImGuiTableFlags_Resizable;
    flags |= ImGuiTableFlags_BordersV;
    flags |= ImGuiTableFlags_BordersOuterH;
    flags |= ImGuiTableFlags_RowBg;
    flags |= ImGuiTableFlags_NoBordersInBody;
    flags |= ImGuiTableFlags_ScrollY;

    ImVec2 size(0, 200);

    if (!BeginTable("valueTable", 5, flags, size)) {
        return false;
    }

    TableSetupScrollFreeze(0, 1);
    TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder);
    TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
    TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
    TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed);
    TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);

    TableHeadersRow();
    return true;
}

void DrawValueTableEntry(lldb::SBValue value) {
    using namespace ImGui;

    ImGuiTreeNodeFlags flags = 0;

    // Posing questions to SBValues can be very expensive if scripted
    // synthetics are involved. Cache all we can
    struct Data {
        std::string value;

        bool mightHaveChildren;
        bool doesHaveChildren;

        uint32_t numChildren;
        Cache<uint32_t, SBValue> children;
    };
    static Cache<lldb::user_id_t, Data> s_cache(__func__);

    Data& data = s_cache.GetOrCreate(value.GetID(), [&](auto) {
        return Data {
            .value = GetValueAsString(value),
            .mightHaveChildren = value.MightHaveChildren(),
            .doesHaveChildren = value.MightHaveChildren() && value.GetNumChildren(1) != 0,
            .numChildren = 0,
            .children = "SBValue.children",
        };
    });

    if (data.mightHaveChildren) {
        if (data.doesHaveChildren) {
            // Normal state
        } else {
            flags |= ImGuiTreeNodeFlags_Bullet;
        }
    } else {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    TableNextRow();
    TableSetColumnIndex(0);

    bool isOpen = false;

    if (TableSetColumnIndex(0)) {
        isOpen = TreeNodeEx((void*) value.GetID(), flags, "%s", value.GetName());
    }
    if (TableSetColumnIndex(1)) {
        Text("%s", data.value.c_str());
    }
    if (TableSetColumnIndex(2)) {
        Text("%s", value.GetDisplayTypeName());
    }
    if (TableSetColumnIndex(3)) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%016llx", value.GetLoadAddress());

        std::string_view addr(buffer);

        auto pos = addr.find_first_not_of('0');
        if (pos == std::string::npos) {
            pos = 0;
        }

        TextDisabled("0x%.*s", int(pos), addr.data());
        SameLine(0, 0);
        Text("%s", addr.substr(pos).data());
    }
    if (TableSetColumnIndex(4)) {
        Text("%zu", value.GetByteSize());
    }

    if (!isOpen) {
        return;
    }

    // Populate child count lazily
    if (data.numChildren == 0 && data.doesHaveChildren) {
        data.numChildren = value.GetNumChildren();
    }

    // Draw children
    for (uint32_t i = 0; i < data.numChildren; i++) {
        auto child = data.children.GetOrCreate(i, [&](auto) {
            return value.GetChildAtIndex(i);
        });
        DrawValueTableEntry(child);
    }

    TreePop();
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

    auto treeBuilder = [&](SBCompileUnit cu) {
        Tree tree;

        ForEachVariable(cu, target, [&](SBValue value) {
            auto spec = value.GetDeclaration().GetFileSpec();
            if (!spec) {
                return;
            }

            tree.Put(spec).value.globals.push_back(value);
        });

        ForEachFunction(cu, [&](SBFunction func) {
            auto spec = func.GetStartAddress().GetLineEntry().GetFileSpec();
            if (!spec) {
                return;
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
        });

        return tree;
    };

    // Building a tree of statics takes a while, cache the data
    struct Hasher {
        size_t operator()(const SBCompileUnit& cu) const {
            return std::hash<void*>{}(Unwrap(cu));
        }
    };
    static Cache<SBCompileUnit, Tree, Hasher> g_cache(__func__);

    Tree& tree = g_cache.GetOrCreate(cu, treeBuilder);

    // The rest is UI code
    using namespace ImGui;

    auto treeNode = [&](auto&, const std::string& stem, PathTree<Data>& node) {
        if (!TreeNode(stem.c_str())) {
            return false;
        }

        if (!node.value.globals.empty()) {
            if (TreeNodeEx("globals", ImGuiTreeNodeFlags_Bullet, "(globals)")) {
                if (BeginValueTable()) {
                    for (SBValue& value : node.value.globals) {
                        DrawValueTableEntry(value);
                    }
                    EndTable();
                }
                TreePop();
            }
        }
        for (auto& [func, globals] : node.value.functions) {
            if (TreeNodeEx(Unwrap(func), ImGuiTreeNodeFlags_Bullet, "(func) %s", func.GetDisplayName())) {
                if (BeginValueTable()) {
                    for (SBValue& value : globals) {
                        DrawValueTableEntry(value);
                    }
                    EndTable();
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

void DrawFrame(lldb::SBFrame frame) {
    using namespace ImGui;

    const int frameID = frame.GetFrameID();

    char buffer[512];
    snprintf(buffer, sizeof(buffer), "#%d: %s ###frame", frameID, frame.GetDisplayFunctionName());

    auto func = frame.GetFunction();
    if (func) {
        if (auto clazz = GetClassOfMemberFunction(func)) {
            auto base = GetFunctionBaseName(func);

            snprintf(buffer, sizeof(buffer), "#%d %s::%s ###frame", frameID, clazz.GetDisplayTypeName(), base.c_str());
        }
    };

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

    if (!isOpen) {
        return;
    }
    if (!func) {
        TextDisabled("No SBFunction available");
        return;
    }

    // We want value tables of each frame to synchronize
    PushOverrideID(0);
    if (BeginValueTable()) {
        // But don't want the values themselves to clash
        PushID(frameID);

        auto vars = frame.GetFunction().GetBlock().GetVariables(frame, true, true, false, lldb::eNoDynamicValues);
        for (auto i = 0; i < vars.GetSize(); i++) {
            DrawValueTableEntry(vars.GetValueAtIndex(i));
        }

        PopID();
        EndTable();
    }
    PopID();
}

void DrawThread(lldb::SBThread thread) {
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
            DrawFrame(thread.GetFrameAtIndex(f));
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
        DrawThread(proc.GetThreadAtIndex(i));
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
