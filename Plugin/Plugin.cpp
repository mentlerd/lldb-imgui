
#include "imgui.h"

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

}

#define API __attribute__((used))

API void Draw() {
    ImGui::Text("Hello from plugin 3!");
}

API void Draw(lldb::SBDebugger& debugger) {
    using namespace ImGui;

    Text("Debugger #%llu", debugger.GetID());

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

}
