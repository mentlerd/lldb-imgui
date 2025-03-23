#pragma once

#include <type_traits>
#include <unordered_set>

namespace lldb::imgui {

/// A simple registry of runtime values - See `Registered`, `RegisteredInto`
template<typename T>
class Registry {
public:
    using PointerType = T*;

    /// Returns a reference to the default instance of this `Registry`
    ///
    /// Implementations are expected to be explicit instantiations of this template in the host application.
    static Registry& Default();

    Registry() = default;

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    /// Access to the underlying collection of the registry
    const auto& View() const {
        return _pointers;
    }

private:
    template<typename Derived, typename Registry, Registry* Into>
    friend class AutoRegistrar;

    void Register(PointerType pointer, size_t unused) {
        _pointers.insert(pointer);
    }
    void Unregister(PointerType pointer, size_t unused) {
        _pointers.erase(pointer);
    }

    std::unordered_set<PointerType> _pointers;
};

/// CRTP base class for automatic instance registration. For most usecases you shouldn't
/// use this as a baseclass directly, use `Registered` or `RegisteredInto` instead.
template<typename Derived, typename Registry, Registry* Into>
class AutoRegistrar {
protected:
    static Registry* GetRegistry() {
        if constexpr (Into) {
            return Into;
        }
        return &Registry::Default();
    }

    AutoRegistrar() {
        GetRegistry()->Register(static_cast<Derived*>(this), sizeof(Derived));
    }

    AutoRegistrar(const AutoRegistrar&) : AutoRegistrar() {}
    AutoRegistrar(AutoRegistrar&&) : AutoRegistrar() {}

    AutoRegistrar& operator=(const AutoRegistrar&) = default;
    AutoRegistrar& operator=(AutoRegistrar&&) = default;

    ~AutoRegistrar() {
        GetRegistry()->Unregister(static_cast<Derived*>(this), sizeof(Derived));
    }
};

/// CRTP base class for automatic registration of `T` instances into `Registry<T>::Default()`
template<typename Derived, typename T = Derived>
using Registered = AutoRegistrar<Derived, Registry<T>, nullptr>;

/// CRTP base class for automatic registraion of `T` instances into the specified `Registry<T>`
template<typename Derived, auto& Into>
using RegisteredInto = AutoRegistrar<Derived, std::remove_cvref_t<decltype(*Into)>, &Into>;

}
