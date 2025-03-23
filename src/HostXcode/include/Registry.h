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

    void Register(PointerType pointer) {
        _pointers.insert(pointer);
    }
    void Unregister(PointerType pointer) {
        _pointers.erase(pointer);
    }

    std::unordered_set<PointerType> _pointers;
};

/// CRTP base class for automatic instance registration. For most usecases you shouldn't
/// use this as a baseclass directly, use `Registered` or `RegisteredInto` instead.
///
/// **Moving** the implementation takes a conservative approach in assuming that moved-from
/// objects are left in an unspecified state where nothing but the destructor is safe to call.
///
/// Based on this assumption `AutoRegistrar` objects remove themselves from their registry
/// when moved-from.
template<typename Derived, typename Registry, Registry* Into>
class AutoRegistrar {
protected:
    /// `Registry<T>` and `Derived` must be convertible
    static_assert(std::is_nothrow_convertible_v<Derived*, typename Registry::PointerType>);

    static Registry* GetRegistry() {
        if constexpr (Into) {
            return Into;
        }
        return &Registry::Default();
    }

    inline AutoRegistrar() {
        Register();
    }

    inline AutoRegistrar(const AutoRegistrar& other) {
        Register();
    }
    inline AutoRegistrar& operator=(const AutoRegistrar& other) {
        return *this;
    }

    inline AutoRegistrar(AutoRegistrar&& victim) : AutoRegistrar() {
        victim.Unregister();
    }
    inline AutoRegistrar& operator=(AutoRegistrar&& victim) {
        victim.Unregister();
        return *this;
    }

    inline ~AutoRegistrar() {
        Unregister();
    }

    void Register() {
        GetRegistry()->Register(static_cast<Derived*>(this));
    }
    void Unregister() {
        GetRegistry()->Unregister(static_cast<Derived*>(this));
    }
};

/// CRTP base class for automatic registration of `T` instances into `Registry<T>::Default()`
template<typename Derived, typename T = Derived>
using Registered = AutoRegistrar<Derived, Registry<T>, nullptr>;

/// CRTP base class for automatic registraion of `T` instances into the specified `Registry<T>`
template<typename Derived, auto& Into>
using RegisteredInto = AutoRegistrar<Derived, std::remove_cvref_t<decltype(*Into)>, &Into>;

}
