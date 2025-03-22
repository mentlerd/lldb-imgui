#pragma once

#include <type_traits>
#include <string_view>

namespace lldb::imgui {

/// Simple type erased reference type for callback lambdas. Inspired by LLVM made simple by `requires`
template<typename F>
class FuncRef;

template<typename R, typename... Args>
class FuncRef<R(Args...)> {
public:
    FuncRef() = delete;

    FuncRef(const FuncRef&) = default;
    FuncRef& operator=(const FuncRef&) = default;

    template<typename L>
        requires(!std::is_same_v<std::remove_cvref_t<L>, FuncRef>)
    FuncRef(L&& lambda) : FuncRef(&lambda, &invoke<std::remove_reference_t<L>>) {}

    R operator()(Args... args) {
        return _invoker(_lambda, std::forward<Args>(args)...);
    }

protected:
    using Invoker = R(*)(void*, Args...);

    FuncRef(void* lambda, Invoker invoker)
    : _lambda(lambda)
    , _invoker(invoker)
    {}

private:
    template<typename L>
    static R invoke(void* raw, Args... args) {
        auto& lambda = *reinterpret_cast<L*>(raw);

        return lambda(std::forward<Args>(args)...);
    }

    void* _lambda;
    Invoker _invoker;
};

/// Simple type erased reference type for visitor lambdas. The provided lambda may stop visitation by optionally returning `true`
template<typename... Args>
class Visitor : public FuncRef<bool(Args...)> {
    using Base = FuncRef<bool(Args...)>;

public:
    template<typename L>
        requires(!std::is_same_v<std::remove_cvref_t<L>, Visitor>)
    Visitor(L&& lambda) : Base(&lambda, &invoke<std::remove_reference_t<L>>) {}

private:
    template<typename L>
    static bool invoke(void* raw, Args... args) {
        auto& lambda = *reinterpret_cast<L*>(raw);

        // For convenience interpret void returns as `false` to continue visitation
        if constexpr (std::is_same_v<std::invoke_result_t<L, Args...>, void>) {
            lambda(std::forward<Args>(args)...);
            return false;
        } else {
            return lambda(std::forward<Args>(args)...);
        }
    }
};

template<typename T>
struct CViewPolicy;

/// Utility which can be used to easily declare a "constant view" parameter to a function as the appropriate type.
///
/// For example: `Visitor<CView<T>>` with `T` will take `T` by value instead of by const reference.
template<typename T>
using CView = CViewPolicy<T>::Type;

/// By default every type is passed as a "view" by const reference
template<typename T>
struct CViewPolicy {
    using Type = const T&;
};

/// Anything small enough to be passed in registers is passed by value
template<typename T>
    requires(sizeof(T) <= 8 && std::is_trivially_copyable_v<T>)
struct CViewPolicy<T> {
    using Type = T;
};

}
