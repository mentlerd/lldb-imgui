
#include "PathTree.h"
#include "CocoaWrapper.h"

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

    ImVec2 size(0, GetTextLineHeightWithSpacing() * 20);

    if (!BeginTable("modules", 4, flags, size)) {
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
        TableNextRow();
        TableNextColumn();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAllColumns;

        if (node.value) {
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

        if (!node.value) {
            return isOpen;
        }
        auto& mod = *node.value;

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

        TextDisabled(" ... (%d items filtered)", numFiltered);

        if (BeginItemTooltip()) {
            Text("Click to clear filters");
            EndTooltip();
        }
        if (IsItemClicked()) {
            *onlyWithValidCompileUnits = false;
        }
    }

    EndTable();
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
    // Nothing
}

API void Draw(lldb::SBDebugger& debugger) {
    using namespace ImGui;

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "Debugger %llu###SBDebugger(%llu)", debugger.GetID(), debugger.GetID());

    if (Begin(buffer)) {
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
                EndTabBar();
            }
        }
    }
    End();
}

}
