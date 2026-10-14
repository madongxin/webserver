#include "LoginAdmission.h"

#include <cstdio>
#include <cstdlib>

int main() {
    if (setenv("GAMEMESH_LOGIN_INFLIGHT_MAX", "4", 1) != 0 ||
        setenv("GAMEMESH_LOGIN_RECONNECT_RESERVE", "1", 1) != 0) {
        std::printf("FAIL setenv\n");
        return 1;
    }
    // The singleton reads env in its constructor. This process must be the first user.
    auto &adm = LoginAdmission::Instance();
    int got_login = 0;
    for (int i = 0; i < 8; ++i) {
        if (adm.TryAcquire(false))
            ++got_login;
    }
    if (got_login != 3) {
        std::printf("FAIL login budget got=%d\n", got_login);
        return 1;
    }
    if (!adm.TryAcquire(true)) {
        std::printf("FAIL reconnect reserve\n");
        return 1;
    }
    if (adm.TryAcquire(true) || adm.TryAcquire(false)) {
        std::printf("FAIL budget exceeded\n");
        return 1;
    }
    adm.Release();
    if (!adm.TryAcquire(true)) {
        std::printf("FAIL release did not free a slot\n");
        return 1;
    }
    std::printf("OK login_admission_test\n");
    return 0;
}
