#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * 解除 hold 时，先把停着的旧事件交给 emit，再放开标志。
 * emit 期间新来的事件继续停住，下一轮再交给 emit，所以新事件排在旧事件后面。
 * emit 在锁外调用，调用方用它把批次推进 AOI 队列。
 */
template <typename Event>
class AoiHoldGate {
public:
    void Hold(uint64_t id) {
        if (id == 0)
            return;
        std::lock_guard<std::mutex> lk(mu_);
        held_.insert(id);
    }

    bool Held(uint64_t id) const {
        std::lock_guard<std::mutex> lk(mu_);
        return held_.count(id) != 0;
    }

    /** 人还被按住时收下事件并返回 true。否则返回 false，调用方自己入队。 */
    bool Admit(uint64_t id, const Event &ev) {
        std::lock_guard<std::mutex> lk(mu_);
        if (held_.count(id) == 0)
            return false;
        parked_[id].push_back(ev);
        return true;
    }

    template <typename Emit>
    void Release(uint64_t id, Emit emit) {
        if (id == 0)
            return;
        for (;;) {
            std::vector<Event> batch;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = parked_.find(id);
                if (it == parked_.end() || it->second.empty()) {
                    held_.erase(id);
                    return;
                }
                batch.swap(it->second);
                parked_.erase(it);
            }
            emit(std::move(batch));
        }
    }

private:
    mutable std::mutex mu_;
    std::unordered_set<uint64_t> held_;
    std::unordered_map<uint64_t, std::vector<Event>> parked_;
};
