
#include "imgui.h"

#include <cassert>
#include <string_view>

#include <typeinfo>
#include <cxxabi.h>

namespace lldb::imgui::inspector {

/// Special alias that signifies that the underlying view is of a compile time string literal,
/// which are subject to many special properties:
///
/// - Their lifetime is static
/// - They are always null terminated (so converting them to `const char*` for C APIs is safe)
using StringLiteral = std::string_view;

class Visitor {
public:
    virtual bool TryEnter(StringLiteral type, StringLiteral name, void* ptrToValue) {
        return false;
    }
    virtual void Leave() {
        // Noop
    }
    virtual void Error(std::string_view error) {
        // Ignore
    }
};

/// Special function that can be used to inspect a named type
using Driver = void (*)(Visitor& visitor, void* ptr);

/// Type erased registration endpoint for `Driver` implementations
void RegisterDriver(StringLiteral type, Driver inspector);

/// Type erased invoker for available drivers
bool CallDriver(Visitor& visitor, StringLiteral type, void* ptr) {
    visitor.Error("Unsupported type");
}

class Dumper {
public:
    template<typename T>
    static void Driver(Visitor& visitor, void* ptr) {
        Dumper dumper(visitor);
        __builtin_dump_struct(reinterpret_cast<T*>(ptr), Dumper::printf, dumper);
    }

private:
    Dumper(Visitor& visitor)
    : _visitor(visitor)
    {}

    /// Visitor we are engaging with
    Visitor& _visitor;

    /// Current depth in the print hierarchy
    size_t _depth = 0;

    /// Currently pending aggregate member info
    std::string_view _aggregateName;
    std::string_view _aggregateType;

    /// Shorthand to make `printf` funtion signatures bearable
    using SL = StringLiteral;

public:
    // These functions are intended to be ONLY called by __builtin_dump_struct

    static void printf(Dumper& d, SL fmt) {
        /// ` {\n` = Scope entry, `}\n` = Root scope exit
        assert(fmt == " {\n" || "}\n");
    }
    static void printf(Dumper& d, SL fmt, SL tNameOrIndent) {
        /// `%s` = Root scope entry, `%s}\n` = Inner scope exit
        assert(fmt == "%s" || fmt == "%s}\n");

        if (fmt == "%s}\n") {
            d._depth--;
        }
    }
    static void printf(Dumper& d, SL fmt, SL indent, SL tName) {
        /// `%s%s` - Baseclass scope header
        assert(fmt == "%s%s");

        d._depth++;
    }
    static void printf(Dumper& d, SL fmt, SL indent, SL tName, SL mName) {
        /// `%s%s %s =` - Aggretate member scope header
        assert(fmt == "%s%s %s =");

        d._depth++;
        d._aggregateType = tName;
        d._aggregateName = mName;

        ImGui::Text("[%zu] Aggregate primed", d._depth);
    }

    template<class T>
    static void printf(Dumper& d, SL fmt, SL indent, SL tName, SL mName, T&& ref) {
        /// Depending on the exact format string we receive `refOrPtr` is either a
        /// reference to the member, or a reference to a pointer to the member
        void* ptrToMember;

        if (fmt == "%s%s %s = *%p\n") {
            ptrToMember = reinterpret_cast<void*>(ref);
        } else if (fmt.starts_with("%s%s %s = %")) {
            ptrToMember = &ref;
        } else {
            assert(false);
        }

        if (d._depth == 1) {
            if (d._aggregateName.empty()) {
                return;
            }

            if (d._visitor.TryEnter(d._aggregateType, d._aggregateName, ptrToMember)) {
                CallDriver(d._visitor, d._aggregateType, ptrToMember);
                d._visitor.Leave();
            }
            d._aggregateName = {};
            d._aggregateType = {};
            return;
        }

        if (d._visitor.TryEnter(tName, mName, ptrToMember)) {
            CallDriver(d._visitor, tName, ptrToMember);
            d._visitor.Leave();
        }
    }
};

template<typename T>
struct Inspector;



/*

template<typename T>
struct AutoInspector;


template<typename T>
constexpr bool is_std(T* = nullptr) {
    return std::string_view(__PRETTY_FUNCTION__).starts_with("bool lldb::imgui::inspector::is_std(T *) [T = std::");
}

static_assert(is_std<std::string_view>());

template<typename T>
void DrawTypename(T&& value) {
    int status;
    char* buffer = abi::__cxa_demangle(typeid(value).name(), nullptr, nullptr, &status);
    if (status != 0) {
        ImGui::TextDisabled("%s", typeid(value).name());
    } else {
        ImGui::TextDisabled("%s", buffer);
    }
    free(buffer);
}

template<class T>
struct AutoVisitImpl {
    static void Visit(const T& value) {
        ImGui::TextDisabled("Unsupported:");
        ImGui::SameLine();
        DrawTypename(value);

        is_std<T>();
    }
};



}

// (Illegal?) trick to avoid including all the containers auto-visitation supports
_LIBCPP_BEGIN_NAMESPACE_STD

template <class T, class A>
class _LIBCPP_TEMPLATE_VIS vector;

template<class K, class V, class H, class E, class A>
class _LIBCPP_TEMPLATE_VIS unordered_map;

_LIBCPP_END_NAMESPACE_STD

namespace lldb::imgui {

template<class T, class A>
struct AutoVisitImpl<std::vector<T, A>> {
    static void Visit(const std::vector<T, A>& vector) {
        ImGui::Text("vector");

        for (auto i = 0; i < vector.size(); i++) {
            Dumper().Accept(vector[i]);
        }
    }
};

template<class K, class V, class H, class E, class A>
struct AutoVisitImpl<std::unordered_map<K, V, H, E, A>> {
    static void Visit(const std::unordered_map<K, V, H, E, A>& map) {
        ImGui::Text("unordered_map");

        for (const auto& [k, v] : map) {
            Dumper().Accept(k);
            Dumper().Accept(v);
        }
    }
};

*/

}
