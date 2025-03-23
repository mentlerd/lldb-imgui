#include "Cache.h"

namespace lldb::imgui {

template<>
Registry<CacheBase>& Registry<CacheBase>::Default() {
    static Registry<CacheBase> instance;
    return instance;
}

uint64_t CacheBase::g_tickCounter = 0;

void CacheBase::Tick() {
    g_tickCounter++;

    for (auto* ptr : Registry<CacheBase>::Default().View()) {
        ptr->RemoveExpired();
    }
}

}
