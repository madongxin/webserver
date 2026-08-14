#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** 健康 GameLogic 的唯一权威快照；Session 分配与 Placement 选 Owner 必须读同一对象。 */
struct HealthyLogicSnapshot {
    enum class Source : uint8_t { kStatic = 0, kRegistry = 1 };
    enum class State : uint8_t { kBootstrap = 0, kActive = 1, kEmpty = 2, kStale = 3 };

    uint64_t version = 0;
    Source source = Source::kStatic;
    State state = State::kBootstrap;
    std::vector<std::string> instance_ids;
    int64_t updated_unix_sec = 0;
};

class HealthyLogicSnapshotStore {
public:
    static HealthyLogicSnapshotStore &Instance();

    std::shared_ptr<const HealthyLogicSnapshot> Current() const;
    std::vector<std::string> InstanceIds() const;
    uint64_t version() const;
    bool HasNonempty() const;

    /** version==0 时自动分配更大 version；旧 version 不得覆盖新 version。 */
    bool Publish(std::shared_ptr<HealthyLogicSnapshot> next);

    void ResetForTest();

private:
    HealthyLogicSnapshotStore() = default;
    std::shared_ptr<const HealthyLogicSnapshot> snap_{std::make_shared<HealthyLogicSnapshot>()};
};
