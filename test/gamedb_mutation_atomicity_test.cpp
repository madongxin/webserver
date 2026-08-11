/**
 * 阶段一 P0：ApplyMutation 事务原子性 + failpoint 回滚断言
 */
#include "ConnectionPool.h"
#include "GameDbAssetStore.h"
#include "GameDbOutbox.h"
#include "Logging.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unistd.h>

namespace {

int Fail(const char *m) {
    std::printf("FAIL %s\n", m);
    return 1;
}

void ClearFailpoints() {
    unsetenv("GAMEMESH_FAILPOINT_MUTATION_AFTER_BAG");
    unsetenv("GAMEMESH_FAILPOINT_MUTATION_AFTER_VERSION");
    unsetenv("GAMEMESH_FAILPOINT_MUTATION_OUTBOX_FAIL");
    unsetenv("GAMEMESH_FAILPOINT_MUTATION_FINALIZE_FAIL");
    unsetenv("GAMEMESH_FAILPOINT_MUTATION_COMMIT_FAIL");
}

struct Snapshot {
    uint32_t bag = 0;
    uint64_t ver = 0;
    bool idem_found = false;
    std::string idem_status;
    int outbox = 0;
};

Snapshot ReadState(uint64_t pid, uint32_t item, const std::string &key, const std::string &op) {
    Snapshot s;
    std::map<uint32_t, uint32_t> bag;
    uint64_t ver = 0;
    GameDbAssetStore::Instance().LoadInventory(pid, &bag, &ver);
    s.ver = ver;
    auto it = bag.find(item);
    s.bag = it == bag.end() ? 0 : it->second;
    GameDbAssetStore::OperationQuery q;
    if (GameDbAssetStore::Instance().QueryOperationResult(pid, key, op, &q) && q.found) {
        s.idem_found = true;
        s.idem_status = q.status;
    }
    s.outbox = GameDbOutbox::Instance().CountByIdempotency(key);
    return s;
}

bool AssertClean(const Snapshot &s, const char *tag) {
    // LoadInventory 在无 meta 时默认 version=1；以 bag/idem/outbox 判定无部分提交
    if (s.bag != 0 || s.idem_found || s.outbox != 0) {
        std::printf("FAIL %s bag=%u ver=%llu idem=%d/%s outbox=%d\n", tag, s.bag,
                    (unsigned long long)s.ver, (int)s.idem_found, s.idem_status.c_str(), s.outbox);
        return false;
    }
    return true;
}

}  // namespace

int main() {
    Logger::setLogLevel(Logger::WARN);
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        std::printf("ERROR: MySQL required\n");
        return 1;
    }
    if (!GameDbAssetStore::Instance().EnsureTables() || !GameDbOutbox::Instance().EnsureTable())
        return Fail("tables");

    ClearFailpoints();
    const uint64_t base = 940000000ull + static_cast<uint64_t>(::getpid() % 100000);

    auto run_failpoint = [&](const char *env, const char *tag, uint64_t pid) -> int {
        ClearFailpoints();
        setenv(env, "1", 1);
        const std::string key = std::string("atom_") + tag + "_" + std::to_string(pid);
        GameDbAssetStore::MutationResult r;
        const bool ok =
            GameDbAssetStore::Instance().ApplyMutation(pid, key, 0, "GRANT", 3001, 5, &r);
        ClearFailpoints();
        if (ok)
            return Fail((std::string(tag) + " should fail").c_str());
        const Snapshot s = ReadState(pid, 3001, key, "GRANT");
        if (!AssertClean(s, tag))
            return 1;
        return 0;
    };

    if (run_failpoint("GAMEMESH_FAILPOINT_MUTATION_AFTER_BAG", "after_bag", base + 1))
        return 1;
    if (run_failpoint("GAMEMESH_FAILPOINT_MUTATION_AFTER_VERSION", "after_ver", base + 2))
        return 1;
    if (run_failpoint("GAMEMESH_FAILPOINT_MUTATION_OUTBOX_FAIL", "outbox", base + 3))
        return 1;
    if (run_failpoint("GAMEMESH_FAILPOINT_MUTATION_FINALIZE_FAIL", "finalize", base + 4))
        return 1;

    // commit failpoint → UNKNOWN_RESULT，且无部分提交
    {
        ClearFailpoints();
        setenv("GAMEMESH_FAILPOINT_MUTATION_COMMIT_FAIL", "1", 1);
        const uint64_t pid = base + 5;
        const std::string key = "atom_commit_" + std::to_string(pid);
        GameDbAssetStore::MutationResult r;
        const bool ok =
            GameDbAssetStore::Instance().ApplyMutation(pid, key, 0, "GRANT", 3001, 5, &r);
        ClearFailpoints();
        if (ok || r.error_code != "UNKNOWN_RESULT")
            return Fail("commit failpoint UNKNOWN_RESULT");
        if (!AssertClean(ReadState(pid, 3001, key, "GRANT"), "commit_fp"))
            return 1;
    }

    // 成功提交后可查询 SUCCEEDED，重放不重复写
    {
        const uint64_t pid = base + 6;
        const std::string key = "atom_ok_" + std::to_string(pid);
        GameDbAssetStore::MutationResult r;
        if (!GameDbAssetStore::Instance().ApplyMutation(pid, key, 0, "GRANT", 3001, 3, &r) || !r.ok)
            return Fail("happy grant");
        GameDbAssetStore::OperationQuery q;
        if (!GameDbAssetStore::Instance().QueryOperationResult(pid, key, "GRANT", &q) ||
            q.status != "SUCCEEDED")
            return Fail("query SUCCEEDED");
        GameDbAssetStore::MutationResult r2;
        if (!GameDbAssetStore::Instance().ApplyMutation(pid, key, 0, "GRANT", 3001, 3, &r2) ||
            !r2.ok || !r2.idempotent_hit || r2.remain_count != 3)
            return Fail("idempotent replay");
        const Snapshot s = ReadState(pid, 3001, key, "GRANT");
        if (s.bag != 3 || s.outbox != 1 || s.idem_status != "SUCCEEDED")
            return Fail("happy invariants");
    }

    // 已存 FAILED 重试返回原始失败，不 BUSY
    {
        const uint64_t pid = base + 7;
        const std::string key = "atom_failed_" + std::to_string(pid);
        auto conn = ConnectionPool::getconnectionPool()->getConnection();
        if (!conn)
            return Fail("conn");
        // request_hash 与 GRANT pid/v0/item3001/count1 对齐
        GameDbAssetStore::MutationResult probe;
        // 先算 hash：用一次真实成功路径拿 hash 太重；直接写 FAILED 行后用同 payload 请求
        // 插入时填空 hash 会 LEGACY conflict；需填正确 hash。用 Apply 成功再手工改行更简单。
        if (!GameDbAssetStore::Instance().ApplyMutation(pid, key, 0, "GRANT", 3001, 1, &probe) ||
            !probe.ok)
            return Fail("seed for failed");
        char sql[512];
        std::snprintf(sql, sizeof(sql),
                      "UPDATE player_asset_idem SET ok=0,error_code='VERSION_CONFLICT',"
                      "message='seeded fail' WHERE idempotency_key='%s'",
                      conn->EscapeSql(key).c_str());
        if (!conn->update(sql))
            return Fail("seed failed row");
        GameDbAssetStore::MutationResult again;
        if (GameDbAssetStore::Instance().ApplyMutation(pid, key, 0, "GRANT", 3001, 1, &again))
            return Fail("failed retry should not ok");
        if (again.error_code != "VERSION_CONFLICT")
            return Fail("failed retry code");
        if (!again.idempotent_hit)
            return Fail("failed retry hit");
    }

    std::printf("OK gamedb_mutation_atomicity_test\n");
    std::fflush(stdout);
    std::_Exit(0);
}
