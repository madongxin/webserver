#pragma once

// Process-local login/reconnect inflight budget. Sits in front of the existing
// auth-flow slot and offload queue. Does not replace per-IP connect limits.
// Stage budgets are separate from the outer login slot and default to the same cap.

class LoginAdmission {
public:
    enum class Stage { Auth = 0, Session = 1, Bind = 2, Enter = 3 };

    class StagePermit {
    public:
        StagePermit() = default;
        StagePermit(StagePermit &&other) noexcept;
        StagePermit(const StagePermit &) = delete;
        StagePermit &operator=(const StagePermit &) = delete;
        ~StagePermit();
        explicit operator bool() const { return held_; }
        static StagePermit Try(Stage stage, bool reconnect);

    private:
        StagePermit(Stage stage, bool held) : stage_(stage), held_(held) {}
        Stage stage_ = Stage::Auth;
        bool held_ = false;
        friend class LoginAdmission;
    };

    static LoginAdmission &Instance();

    // reconnect may use the reserved tail of the budget. Fresh login may not.
    bool TryAcquire(bool reconnect);
    void Release();

private:
    LoginAdmission();

    int max_inflight_ = 256;
    int reconnect_reserve_ = 32;
    int inflight_ = 0;
    int stage_max_[4] = {256, 256, 256, 256};
    int stage_inflight_[4] = {0, 0, 0, 0};
};
