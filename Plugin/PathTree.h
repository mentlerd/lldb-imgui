#pragma once

#include "lldb/API/SBFileSpec.h"
#include "lldb/API/SBFileSpecList.h"

#include <concepts>
#include <map>
#include <string>
#include <optional>

namespace lldb::imgui {

/// Utility to build an ordered tree structure from filesystem paths
template<typename T>
class PathTree {
public:
    std::optional<T> value;

    PathTree() = default;

    void Put(const lldb::SBFileSpecList& spec);

    PathTree& Put(const lldb::SBFileSpec& spec);
    PathTree& Put(std::string_view dir, std::string_view name);

    void Traverse(auto&& descend, auto&& ascend = []{}) {
        std::string prefix;
        Traverse(descend, ascend, prefix);
    }

    void Traverse(auto&& descend, auto&& ascend, std::string& parent) {
        std::string path;

        for (auto& [name, child] : _children) {
            path.clear();
            path.append(name);

            // Descend until we hit a leaf/junction
            auto* current = &child;

            while (current->_children.size() == 1) {
                auto& [name, child] = *current->_children.begin();

                path.append("/");
                path.append(name);

                current = &child;
            }

            // Adjust absolute path
            parent.append("/");
            parent.append(path);

            // Try entering this level
            bool enter = descend(std::as_const(parent), std::as_const(path), *current);

            parent.resize(parent.size() - path.size());

            if (enter) {
                current->Traverse(descend, ascend, parent);
                ascend();
            }
        }
    }

private:
    inline PathTree& GetChild(std::string_view name) {
        return _children.try_emplace(std::string(name)).first->second;
    }

    std::map<std::string, PathTree> _children;
};

template<typename T>
void PathTree<T>::Put(const lldb::SBFileSpecList& list) {
    for (uint32_t i = 0; i < list.GetSize(); i++) {
        Put(list.GetFileSpecAtIndex(i));
    }
}

template<typename T>
auto PathTree<T>::Put(const lldb::SBFileSpec& spec) -> PathTree& {
   return Put(spec.GetDirectory(), spec.GetFilename());
}

template<typename T>
auto PathTree<T>::Put(std::string_view dir, std::string_view name) -> PathTree& {
    if (dir.empty()) {
        return GetChild(name);
    }

    auto index = dir.find('/');
    if (index == 0) {
        return Put(dir.substr(1), name);
    }
    if (index == std::string::npos) {
        return GetChild(dir).Put("", name);
    }

    return GetChild(dir.substr(0, index)).Put(dir.substr(index + 1), name);
}

}
