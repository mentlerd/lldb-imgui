#include "Debuggable.h"

#include "DebugWindow.h"

#include "imgui.h"

#include "llvm/ADT/IntervalTree.h"
#include "llvm/ADT/SmallVector.h"

#include <cxxabi.h>

#include <format>
#include <unordered_map>

namespace lldb::imgui {

/// `Debuggable` should be an empty class, so there is not ABI difference when debugging support is turned off
static_assert(std::is_empty_v<Debuggable<std::string>>);

/// Canonical registry for `Debuggable` instances
class Debuggables final : public Registry<DebuggableTag> {
    Debuggables() = default;

public:
    static Debuggables& GetInstance() {
        static Debuggables instance;
        return instance;
    }

    struct Key {
        uintptr_t startAddr;
        uintptr_t endAddr;

        Key(void* derived, size_t size)
        : startAddr(reinterpret_cast<uintptr_t>(derived))
        , endAddr(startAddr + size)
        {}

        bool operator==(const Key&) const = default;
    };
    struct KeyHasher {
        size_t operator()(const Key& key) const {
            return key.startAddr ^ key.endAddr; // TODO: Shitty hash
        }
    };

    struct TypeInfo {
        std::string name;
        Invoker invoker;

        TypeInfo(Invoker invoker, const std::type_info& tinfo)
        : invoker(invoker)
        {
            size_t len;
            int status;

            char* buffer = abi::__cxa_demangle(tinfo.name(), nullptr, &len, &status);
            if (status != 0) {
                name.assign(tinfo.name());
            } else {
                name.assign(buffer, len);
            }
            free(buffer);
        }
    };
    std::unordered_map<Invoker, TypeInfo> _typeInfos;

    struct Entry {
        TypeInfo& typeInfo;

        /// Logical parent
        std::optional<Key> parent;

        /// Logical children
        llvm::SmallVector<Key> children;

        /// User facing label - autogenerated, can be customized
        std::string customLabel;

        explicit Entry(TypeInfo& typeInfo): typeInfo(typeInfo) {}
    };
    std::unordered_map<Key, Entry, KeyHasher> _registry;

    /// Root elements of the debuggable hierarchy
    std::unordered_set<Key, KeyHasher> _root;

    /// Entries which are awaiting parent discovery
    std::unordered_set<Key, KeyHasher> _pendingParentDiscovery;

    void Register(Key key, Invoker invoker, const std::type_info& tinfo);
    void Unregister(Key key);

    void SetParent(Key childKey, Key parentKey);
    void ClearParent(Key childKey, Entry& child);

    void Draw();

private:
    void DoParentDiscovery();

    void DrawEntry(Key key, Entry& entry);
};

void Debuggables::Register(Key key, Invoker invoker, const std::type_info& tinfo) {
    auto& typeInfo = _typeInfos.try_emplace(invoker, invoker, tinfo).first->second;

    auto [it, added] = _registry.try_emplace(key, typeInfo);
    if (!added) {
        assert(false);
        return;
    }

    _pendingParentDiscovery.insert(key);
}

void Debuggables::Unregister(Key key) {
    auto node = _registry.extract(key);
    if (!node) {
        assert(false);
        return;
    }

    // Detach children, and move them to root
    for (Key childKey : node.mapped().children) {
        auto cit = _registry.find(childKey);
        if (cit == _registry.end()) {
            continue;
        }

        cit->second.parent.reset();
        _root.insert(childKey);
    }

    // Erase all traces
    _root.erase(key);
    _pendingParentDiscovery.erase(key);
}

void Debuggables::SetParent(Key childKey, Key parentKey) {
    auto cit = _registry.find(childKey);
    if (cit == _registry.end()) {
        assert(false);
        return;
    }

    auto pit = _registry.find(parentKey);
    if (pit == _registry.end()) {
        assert(false);
        return;
    }

    Entry& child = cit->second;
    Entry& parent = pit->second;

    // Drop child from old parent
    ClearParent(childKey, child);

    // Child now has a parent
    child.parent = parentKey;
    _root.erase(childKey);

    // Parent now has this child
    auto pos = std::lower_bound(parent.children.begin(), parent.children.end(), childKey, [](const Key& lhs, const Key& rhs) {
        return lhs.startAddr < rhs.startAddr;
    });
    parent.children.insert(pos, childKey);
}

void Debuggables::ClearParent(Key childKey, Entry& child) {
    if (!child.parent) {
        return;
    }

    auto pit = _registry.find(*child.parent);
    if (pit == _registry.end()) {
        assert(false);
        return;
    }

    // Child no longer has a parent
    child.parent.reset();
    _root.insert(childKey);

    // Parent no longer has this child
    Entry& parent = pit->second;

    auto pos = std::find(parent.children.begin(), parent.children.end(), childKey);
    parent.children.erase(pos);
}

void Debuggables::DoParentDiscovery() {
    // Build interval tree of already known debuggable objects
    llvm::BumpPtrAllocator allocator;
    llvm::IntervalTree<uintptr_t, bool> tree(allocator);

    using Tree = decltype(tree);

    for (auto& [key, entry] : _registry) {
        tree.insert(key.startAddr, key.endAddr, &entry);
    }
    tree.create();

    for (Key child : _pendingParentDiscovery) {
        auto kMaxSize = std::numeric_limits<uintptr_t>::max();

        // Start with a very bad estimate for the parent
        Key parent(nullptr, kMaxSize);

        // Keep testing intervals overlapping child.startAddr - the smaller the interval the better
        for (auto it = tree.find(child.startAddr); it != tree.find_end(); it++) {
            if (it->left() < parent.startAddr) {
                continue; // Worse than current candidate
            }
            if (parent.endAddr < it->right()) {
                continue; // Worse than current candidate
            }
            if (it->right() < child.endAddr) {
                continue; // Does not encapsulate child
            }
            if (it->left() == child.startAddr && it->right() == child.endAddr) {
                continue; // This is the child itself
            }

            parent.startAddr = it->left();
            parent.endAddr = it->right();
        }

        // If our best guess is still the max size, then this is a new root
        if (parent.startAddr == 0 && parent.endAddr == kMaxSize) {
            _root.insert(child);
            continue;
        }

        // Otherwise we have found the parent
        SetParent(child, parent);
    }

    // Auto-assignment has assigned everyone to a parent. Free allocation
    _pendingParentDiscovery = {};
}

void Debuggables::Draw() {
    using namespace ImGui;

    DoParentDiscovery();

    if (_registry.empty()) {
        TextDisabled("No Debuggable instances");
        return;
    }

    for (const Key& key : _root) {
        auto it = _registry.find(key);
        if (it == _registry.end()) {
            assert(false);
            continue;
        }

        DrawEntry(key, it->second);
    }
}

void Debuggables::DrawEntry(Key key, Entry& entry) {
    using namespace ImGui;

    void* startAddr = reinterpret_cast<void*>(key.startAddr);
    void* endAddr = reinterpret_cast<void*>(key.endAddr);

    PushID(startAddr);
    PushID(endAddr);

    bool isOpen;

    if (entry.customLabel.empty()) {
        isOpen = TreeNode("", "%s %p", entry.typeInfo.name.c_str(), startAddr);
    } else {
        isOpen = TreeNode("", "%s %p", entry.customLabel.c_str(), startAddr);
    }

    if (isOpen) {
        for (const Key& key : entry.children) {
            auto it = _registry.find(key);
            if (it == _registry.end()) {
                assert(false);
                continue;
            }

            DrawEntry(key, it->second);
        }
        TreePop();
    }

    PopID();
    PopID();
}

class DebuggablesWindow : public DebugWindow {
    void Init(State& state) override {
        state.title = "Debuggables";
    }
    void Draw() override {
        Debuggables::GetInstance().Draw();
    }
} g_debuggablesWindow;

Registry<DebuggableTag>& Registry<DebuggableTag>::Default() {
    return Debuggables::GetInstance();
}

void Registry<DebuggableTag>::Register(void* derived, size_t size, Invoker invoker, const std::type_info& tinfo) {
    Debuggables::GetInstance().Register(Debuggables::Key(derived, size), invoker, tinfo);
}

void Registry<DebuggableTag>::Unregister(void* derived, size_t size) {
    Debuggables::GetInstance().Unregister(Debuggables::Key(derived, size));
}

void Registry<DebuggableTag>::SetParent(void* derived, size_t size, void* parent, size_t psize) {
    auto& instance = Debuggables::GetInstance();

    Debuggables::Key childKey(derived, size);
    Debuggables::Key parentKey(parent, psize);

    instance.SetParent(childKey, parentKey);
    instance._pendingParentDiscovery.erase(childKey);
}

void Registry<DebuggableTag>::SetLabel(void* derived, size_t size, std::string_view label) {
    auto& instance = Debuggables::GetInstance();

    auto it = instance._registry.find(Debuggables::Key(derived, size));
    if (it == instance._registry.end()) {
        assert(false);
        return;
    }

    it->second.customLabel = label;
}

}
