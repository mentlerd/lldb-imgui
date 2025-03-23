#pragma once

#include "Registry.h"

#include <typeinfo>
#include <string_view>

namespace lldb::imgui {

class DebuggableTag;

/// CRTP base class which registers instances of `Derived` into the Debuggables system
///
/// Registered classes can be viewed in a tree, and have a chance to draw their own ImGui based
/// debugging UI if they implement a public `void DrawDebugUI()` method.
template<typename Derived>
class Debuggable : public Registered<Derived, DebuggableTag> {
public:
    /// Registered debuggables are automatically collected into a tree based on static allocation containment.
    ///
    /// Heap based containment cannot be automatically detected, instead this function can be used to declare it.
    template<typename Parent>
    void SetParent(Parent* parent);

    /// Set the title for this debuggable to be displayed in the debuggables tree.
    void SetLabel(std::string_view label);
};

template<>
class Registry<DebuggableTag> {
public:
    using PointerType = void*;

    /// Returns a reference to the default instance of this `Registry`
    static Registry& Default();

    Registry() = default;

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

protected:
    using Invoker = void(*)(void*);

private:
    template<typename Derived, typename Registry, Registry* Into>
    friend class AutoRegistrar;

    template<typename T>
    static void DrawDebugUI(void* raw) {
        reinterpret_cast<T*>(raw)->DrawDebugUI();
    }

    template<typename Derived>
    void Register(Derived* derived, size_t size) {
        // This check must be done rather late, as `Derived` may refer to an incomplete class for a while
        static_assert(!std::is_empty_v<Derived>, "Empty Debuggables are not permitted");

        Register(derived, sizeof(Derived), &DrawDebugUI<Derived>, typeid(Derived));
    }

    void Register(void* derived, size_t size, Invoker invoker, const std::type_info& tinfo);
    void Unregister(void* derived, size_t size);

    void SetParent(void* derived, size_t size, void* parent, size_t psize);
    void SetLabel(void* derived, size_t size, std::string_view label);
};

template<typename Derived>
template<typename Parent>
void Debuggable<Derived>::SetParent(Parent* parent) {
    Debuggable::GetRegistry()->SetParent(static_cast<Derived*>(this), sizeof(Derived), parent, sizeof(Parent));
}

template<typename Derived>
void Debuggable<Derived>::SetLabel(std::string_view label) {
    Debuggable::GetRegistry()->SetLabel(static_cast<Derived*>(this), sizeof(Derived), label);
}

}
