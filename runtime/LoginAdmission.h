#pragma once

// Process-local login/reconnect inflight budget. Sits in front of the existing
// auth-flow slot and offload queue. Does not replace per-IP connect limits.

class LoginAdmission {
public:
    static LoginAdmission &Instance();

    // reconnect may use the reserved tail of the budget. Fresh login may not.
    bool TryAcquire(bool reconnect);
    void Release();

private:
    LoginAdmission();

    int max_inflight_ = 256;
    int reconnect_reserve_ = 32;
    int inflight_ = 0;
};
