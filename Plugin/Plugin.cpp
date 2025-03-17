
#include "imgui.h"
#include "imgui_internal.h"

#include "lldb/API/LLDB.h"

#include <map>

namespace lldb::imgui {

namespace {

void DrawModules(lldb::SBTarget& target) {
    using namespace ImGui;

    struct Node {
        std::map<std::string_view, Node> children;
        std::optional<lldb::SBModule> mod;

        Node& Put(std::string_view dir, std::string_view name) {
            if (dir.empty()) {
                return children[name];
            }

            auto index = dir.find('/');
            if (index == 0) {
                return Put(dir.substr(1), name);
            }
            if (index == std::string::npos) {
                return children[dir].Put("", name);
            }

            return children[dir.substr(0, index)].Put(dir.substr(index + 1), name);
        }

        void DrawChildren() {
            std::string buffer;

            for (auto& [name, child] : children) {
                buffer.clear();
                buffer.append(name);

                // Descend until we hit a leaf/junction
                Node* current = &child;

                while (current->children.size() == 1) {
                    auto& child = *current->children.begin();

                    buffer.append("/");
                    buffer.append(child.first);

                    current = &child.second;
                }

                TableNextRow();
                TableNextColumn();

                if (auto& mod = current->mod) {
                    if (TreeNodeEx(buffer.c_str(), ImGuiTreeNodeFlags_Leaf)) {
                        TreePop();
                    }

                    if (TableNextColumn()) {
                        uint32_t versions[3];

                        // Unfortunately the SB API is quite wonky here, and doesn't really do
                        // what is expected. Even if the is all zeros, the returned values
                        // is are UINT32_MAX.0.0
                        mod->GetVersion(versions, 3);

                        if (versions[0] == UINT32_MAX) {
                            TextDisabled("N/A");
                        } else {
                            buffer.clear();
                            buffer.append(std::to_string(versions[0]));

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
                        Text("%s", mod->GetTriple());
                    }
                    if (TableNextColumn()) {
                        Text("%s", mod->GetUUIDString());
                    }
                } else {
                    if (TreeNodeEx(buffer.c_str(), ImGuiTreeNodeFlags_LabelSpanAllColumns)) {
                        current->DrawChildren();
                        TreePop();
                    }
                }
            }
        }

        void DrawTable() {
            ImGuiTableFlags flags = 0;

            flags |= ImGuiTableFlags_Hideable;
            flags |= ImGuiTableFlags_Reorderable;

            flags |= ImGuiTableFlags_BordersV;
            flags |= ImGuiTableFlags_BordersOuterH;
            flags |= ImGuiTableFlags_RowBg;
            flags |= ImGuiTableFlags_NoBordersInBody;

            flags |= ImGuiTableFlags_ScrollY;

            ImVec2 size(0, GetTextLineHeightWithSpacing() * 20);

            if (BeginTable("modules", 4, flags, size)) {
                TableSetupScrollFreeze(0, 1);
                TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder);
                TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed);
                TableSetupColumn("Triple", ImGuiTableColumnFlags_WidthFixed);
                TableSetupColumn("UUID", ImGuiTableColumnFlags_WidthFixed);
                TableHeadersRow();

                DrawChildren();

                EndTable();
            }
        }
    } tree;

    for (uint32_t i = 0; i < target.GetNumModules(); i++) {
        auto mod = target.GetModuleAtIndex(i);
        auto spec = mod.GetFileSpec();

        tree.Put(spec.GetDirectory(), spec.GetFilename()).mod = mod;
    }

    tree.DrawTable();
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

void Draw(lldb::SBTarget target) {
    using namespace ImGui;

    lldb::SBStream stream;
    target.GetDescription(stream, lldb::DescriptionLevel::eDescriptionLevelBrief);

    Text("%.*s", int(stream.GetSize()), stream.GetData());

    auto proc = target.GetProcess();
    if (!proc.IsValid()) {
        return;
    }

    if (Button("Step over")) {
        proc.GetSelectedThread().StepOver();
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

    if (Begin("Debuggers")) {
        Text("Debugger #%llu", debugger.GetID());

        for (uint32_t j = 0; j < debugger.GetNumTargets(); j++) {
            Draw(debugger.GetTargetAtIndex(j));
        }

        Text("Module");
        SameLine();

        auto target = debugger.GetSelectedTarget();
        if (!target.IsValid()) {
            return;
        }

        if (BeginCombo("###foo", "Preview", ImGuiComboFlags_HeightLargest)) {
            DrawModules(target);
            EndCombo();
        }
    }
    End();
}

}
