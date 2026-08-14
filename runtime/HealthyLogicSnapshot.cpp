#include "HealthyLogicSnapshot.h"

#include <atomic>
#include <chrono>

HealthyLogicSnapshotStore &HealthyLogicSnapshotStore::Instance() {
    static HealthyLogicSnapshotStore g;
    return g;
}

std::shared_ptr<const HealthyLogicSnapshot> HealthyLogicSnapshotStore::Current() const {
    return std::atomic_load_explicit(&snap_, std::memory_order_acquire);
}

std::vector<std::string> HealthyLogicSnapshotStore::InstanceIds() const {
    auto s = Current();
    return s ? s->instance_ids : std::vector<std::string>{};
}

uint64_t HealthyLogicSnapshotStore::version() const {
    auto s = Current();
    return s ? s->version : 0;
}

bool HealthyLogicSnapshotStore::HasNonempty() const {
    auto s = Current();
    return s && s->state == HealthyLogicSnapshot::State::kActive && !s->instance_ids.empty();
}

bool HealthyLogicSnapshotStore::Publish(std::shared_ptr<HealthyLogicSnapshot> next) {
    if (!next)
        return false;
    for (;;) {
        auto cur = std::atomic_load_explicit(&snap_, std::memory_order_acquire);
        const uint64_t cur_ver = cur ? cur->version : 0;
        if (next->version == 0)
            next->version = cur_ver + 1;
        if (next->version <= cur_ver)
            return false;
        if (next->updated_unix_sec == 0) {
            next->updated_unix_sec = std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
        }
        std::shared_ptr<const HealthyLogicSnapshot> desired = next;
        if (std::atomic_compare_exchange_weak_explicit(&snap_, &cur, desired,
                                                       std::memory_order_release,
                                                       std::memory_order_acquire))
            return true;
    }
}

void HealthyLogicSnapshotStore::ResetForTest() {
    auto empty = std::make_shared<HealthyLogicSnapshot>();
    std::atomic_store_explicit(&snap_, std::shared_ptr<const HealthyLogicSnapshot>(empty),
                               std::memory_order_release);
}
