#include "Debuggable.h"
#include "Functional.h"

#include "imgui.h"

#include <unordered_set>
#include <unordered_map>

namespace lldb::imgui {

/// Global switch that enables cache rendering from `Tick()` - useful for debugging
constexpr bool kDebugCaches = false;

class CacheBase {
public:
    /// Advance the tick number of all currently alive caches, and reap expired values
    static void Tick();

protected:
    /// Global frame counter for cache expiration
    static uint64_t g_tickCounter;

private:
    virtual void RemoveExpired() = 0;
};

/// Having absolutely no data model so far makes the "app" very slow at times when doing
/// repeated traversal of debug information on a per-frame basis...
///
/// This class implements a simple timed cache with a frame based grace period **specifically**
/// for storing computed data to render in ImGui using LLDB types as keys.
template<typename K, typename V, typename Hash = std::hash<K>, typename Eq = std::equal_to<K>>
class Cache final : public CacheBase,
                    public Registered<Cache<K, V, Hash, Eq>, CacheBase>,
                    public Debuggable<Cache<K, V, Hash, Eq>> {
public:
    V& GetOrCreate(const K& key, FuncRef<V(CView<K>)> builder) {
        Entry& entry = _contents[key];

        if (entry.expiresAt == 0) {
            new (entry.value()) V(builder(key));
        }
        entry.expiresAt = g_tickCounter + _timeToLive;

        return *entry.value();
    }

    void DrawDebugUI() {
        ImGui::InputScalar("TTL", ImGuiDataType_U64, &_timeToLive);

        for (const auto& pair : _contents) {
            ImGui::Text("%p TTL: %llu", &pair.first, pair.second.expiresAt - g_tickCounter);
        }
    }

private:
    void RemoveExpired() override {
        std::erase_if(_contents, [](const auto& pair) {
            return pair.second.expiresAt <= g_tickCounter;
        });
    }

    struct Entry {
        alignas(V) char buffer[sizeof(V)];
        uint64_t expiresAt = 0;

        V* value() {
            return reinterpret_cast<V*>(&buffer);
        }
        ~Entry() {
            if (expiresAt != 0) {
                value()->~V();
            }
        }
    };

    /// Number of frames a cached entry is supposed to survive without access
    uint64_t _timeToLive = 120;

    /// Contents of the cache
    std::unordered_map<K, Entry, Hash, Eq> _contents;
};

}
