#include "GameDbAssetStore.h"

#include "ConnectionPool.h"
#include "GameDbOutbox.h"
#include "Logging.h"
#include "PlayerItemStore.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

namespace {

constexpr size_t kMaxIdempotencyKeyLen = 128;

int64_t NowSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string Fnv1aHex(const std::string &s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

std::string SnapshotRequestHash(uint64_t expected_version,
                                const std::map<uint32_t, uint32_t> &bag) {
    std::ostringstream os;
    os << "SAVE_SNAPSHOT|v=" << expected_version;
    for (const auto &kv : bag)
        os << "|" << kv.first << ":" << kv.second;
    return Fnv1aHex(os.str());
}

std::string MutationRequestHash(uint64_t player_id, uint64_t expected_version,
                                const std::string &mutation_type, uint32_t item_id, uint32_t count) {
    std::ostringstream os;
    os << mutation_type << "|player=" << player_id << "|v=" << expected_version
       << "|item=" << item_id << "|count=" << count;
    return Fnv1aHex(os.str());
}

}  // namespace

bool GameDbAssetStore::ValidateIdempotencyKey(const std::string &key, std::string *err) {
    if (key.empty()) {
        if (err)
            *err = "idempotency_key empty";
        return false;
    }
    if (key.size() > kMaxIdempotencyKeyLen) {
        if (err)
            *err = "idempotency_key too long";
        return false;
    }
    for (unsigned char c : key) {
        if (c < 0x20 || c == 0x7f) {
            if (err)
                *err = "idempotency_key has control chars";
            return false;
        }
    }
    return true;
}

GameDbAssetStore &GameDbAssetStore::Instance() {
    static GameDbAssetStore g;
    return g;
}

bool GameDbAssetStore::EnsureTables() {
#ifdef WEBSERVER_ENABLE_MYSQL
    if (tables_ready_)
        return available_;
    auto pool = ConnectionPool::getconnectionPool();
    if (!pool || !pool->isInitialized()) {
        available_ = false;
        return false;
    }
    auto conn = pool->getConnection();
    if (!conn) {
        available_ = false;
        return false;
    }
    const char *meta =
        "CREATE TABLE IF NOT EXISTS player_asset_meta ("
        "player_id BIGINT UNSIGNED NOT NULL PRIMARY KEY,"
        "asset_version BIGINT UNSIGNED NOT NULL DEFAULT 1,"
        "updated_at BIGINT NOT NULL DEFAULT 0"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    const char *idem =
        "CREATE TABLE IF NOT EXISTS player_asset_idem ("
        "idempotency_key VARCHAR(128) NOT NULL PRIMARY KEY,"
        "player_id BIGINT UNSIGNED NOT NULL,"
        "ok TINYINT NOT NULL,"
        "error_code VARCHAR(64) NOT NULL DEFAULT '',"
        "message VARCHAR(256) NOT NULL DEFAULT '',"
        "asset_version BIGINT UNSIGNED NOT NULL DEFAULT 0,"
        "remain_count INT UNSIGNED NOT NULL DEFAULT 0,"
        "operation_type VARCHAR(32) NOT NULL DEFAULT '',"
        "request_hash VARCHAR(128) NOT NULL DEFAULT '',"
        "created_at BIGINT NOT NULL,"
        "KEY idx_player_op (player_id, operation_type)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    const char *bag =
        "CREATE TABLE IF NOT EXISTS player_asset_bag ("
        "player_id BIGINT UNSIGNED NOT NULL,"
        "item_id INT UNSIGNED NOT NULL,"
        "count INT UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY (player_id, item_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    available_ = conn->update(meta) && conn->update(idem) && conn->update(bag);
    // 兼容旧表：仅当缺列时补齐（避免重复 ALTER 抢元数据锁）
    {
        MYSQL_RES *cols = conn->query(
            "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
            "AND TABLE_NAME='player_asset_idem' AND COLUMN_NAME='operation_type'");
        bool need_op = true;
        if (cols) {
            MYSQL_ROW r = mysql_fetch_row(cols);
            if (r && r[0] && std::atoi(r[0]) > 0)
                need_op = false;
            mysql_free_result(cols);
        }
        if (need_op)
            conn->update("ALTER TABLE player_asset_idem ADD COLUMN operation_type VARCHAR(32) NOT "
                         "NULL DEFAULT ''");
        MYSQL_RES *cols2 = conn->query(
            "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
            "AND TABLE_NAME='player_asset_idem' AND COLUMN_NAME='request_hash'");
        bool need_hash = true;
        if (cols2) {
            MYSQL_ROW r = mysql_fetch_row(cols2);
            if (r && r[0] && std::atoi(r[0]) > 0)
                need_hash = false;
            mysql_free_result(cols2);
        }
        if (need_hash)
            conn->update("ALTER TABLE player_asset_idem ADD COLUMN request_hash VARCHAR(128) NOT "
                         "NULL DEFAULT ''");
    }
    tables_ready_ = available_;
    if (available_)
        LOG_INFO << "GameDbAssetStore tables ready";
    PlayerItemStore::Instance().EnsureTable();
    return available_;
#else
    available_ = false;
    return false;
#endif
}

bool GameDbAssetStore::LoadMeta(uint64_t player_id, uint64_t *version, bool *exists) {
#ifdef WEBSERVER_ENABLE_MYSQL
    if (!EnsureTables() || !version || !exists)
        return false;
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "SELECT asset_version FROM player_asset_meta WHERE player_id=%llu",
                  (unsigned long long)player_id);
    MYSQL_RES *res = conn->query(sql);
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        mysql_free_result(res);
        *exists = false;
        *version = 0;
        return true;
    }
    *exists = true;
    *version = std::strtoull(row[0], nullptr, 10);
    mysql_free_result(res);
    return true;
#else
    (void)player_id;
    (void)version;
    (void)exists;
    return false;
#endif
}

bool GameDbAssetStore::LoadInventory(uint64_t player_id, std::map<uint32_t, uint32_t> *bag,
                                     uint64_t *version) {
#ifdef WEBSERVER_ENABLE_MYSQL
    if (!EnsureTables() || !bag)
        return false;
    bag->clear();
    uint64_t ver = 1;
    bool exists = false;
    if (!LoadMeta(player_id, &ver, &exists))
        return false;
    if (!exists)
        ver = 1;
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "SELECT item_id,count FROM player_asset_bag WHERE player_id=%llu AND count>0",
                  (unsigned long long)player_id);
    MYSQL_RES *res = conn->query(sql);
    if (!res)
        return false;
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        if (row[0] && row[1])
            (*bag)[static_cast<uint32_t>(std::strtoul(row[0], nullptr, 10))] =
                static_cast<uint32_t>(std::strtoul(row[1], nullptr, 10));
    }
    mysql_free_result(res);
    if (bag->empty())
        PlayerItemStore::Instance().LoadInventoryAggregate(player_id, bag);
    if (version)
        *version = ver;
    return true;
#else
    (void)player_id;
    (void)bag;
    (void)version;
    return false;
#endif
}

bool GameDbAssetStore::ApplyMutation(uint64_t player_id, const std::string &idempotency_key,
                                     uint64_t expected_version, const std::string &mutation_type,
                                     uint32_t item_id, uint32_t count, MutationResult *out) {
#ifdef WEBSERVER_ENABLE_MYSQL
    if (!out)
        return false;
    *out = MutationResult{};
    std::string key_err;
    if (!EnsureTables() || player_id == 0 || item_id == 0 || count == 0 ||
        (mutation_type != "GRANT" && mutation_type != "CONSUME")) {
        out->error_code = "INVALID_ARG";
        out->message = "invalid mutation";
        return false;
    }
    if (!ValidateIdempotencyKey(idempotency_key, &key_err)) {
        out->error_code = "INVALID_ARGUMENT";
        out->message = key_err;
        return false;
    }
    const std::string req_hash =
        MutationRequestHash(player_id, expected_version, mutation_type, item_id, count);
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        out->error_code = "DB_UNAVAILABLE";
        out->message = "no connection";
        return false;
    }
    const std::string esc_key = conn->EscapeSql(idempotency_key);
    const std::string esc_hash = conn->EscapeSql(req_hash);
    const std::string esc_op = conn->EscapeSql(mutation_type);

    auto fill_conflict = [&](const char *msg) {
        out->ok = false;
        out->idempotent_hit = true;
        out->error_code = "IDEMPOTENCY_CONFLICT";
        out->message = msg;
    };

    // 处理已有幂等行：SUCCEEDED / FAILED / CONFLICT；IN_PROGRESS 返回 false 继续占键/轮询
    auto consume_idem_row = [&](MYSQL_ROW row) -> bool {
        if (!row || !row[0])
            return false;
        const uint64_t pid = row[6] ? std::strtoull(row[6], nullptr, 10) : 0;
        const std::string old_hash = row[5] ? row[5] : "";
        const std::string old_op = row[7] ? row[7] : "";
        if (pid != 0 && pid != player_id) {
            fill_conflict("idempotency_key bound to other player");
            return true;
        }
        // 旧数据缺 operation_type / request_hash：禁止默认匹配任意新请求
        if (old_op.empty() || old_hash.empty()) {
            fill_conflict("legacy idempotency row incomplete");
            return true;
        }
        if (old_op != mutation_type) {
            fill_conflict("same key different operation_type");
            return true;
        }
        if (old_hash != req_hash) {
            fill_conflict("same key different payload");
            return true;
        }
        if (std::atoi(row[0]) != 0) {
            out->ok = true;
            out->idempotent_hit = true;
            out->error_code = row[1] ? row[1] : "";
            out->message = row[2] ? row[2] : "idempotent";
            out->asset_version = row[3] ? std::strtoull(row[3], nullptr, 10) : 0;
            out->remain_count =
                row[4] ? static_cast<uint32_t>(std::strtoul(row[4], nullptr, 10)) : 0;
            return true;
        }
        const char *ecode = row[1] ? row[1] : "";
        if (std::strcmp(ecode, "IN_PROGRESS") != 0) {
            // 已持久化 FAILED：直接返回原始失败，禁止当成 BUSY
            out->ok = false;
            out->idempotent_hit = true;
            out->error_code = ecode;
            out->message = row[2] ? row[2] : "failed";
            out->asset_version = row[3] ? std::strtoull(row[3], nullptr, 10) : 0;
            out->remain_count =
                row[4] ? static_cast<uint32_t>(std::strtoul(row[4], nullptr, 10)) : 0;
            return true;
        }
        return false;
    };

    {
        char q[768];
        std::snprintf(q, sizeof(q),
                      "SELECT ok,error_code,message,asset_version,remain_count,request_hash,"
                      "player_id,operation_type FROM player_asset_idem WHERE idempotency_key='%s'",
                      esc_key.c_str());
        MYSQL_RES *res = conn->query(q);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row && row[0] && consume_idem_row(row)) {
                mysql_free_result(res);
                return out->ok;
            }
            mysql_free_result(res);
        }
    }

    if (!conn->begin()) {
        out->error_code = "TX_BEGIN";
        return false;
    }
    {
        char claim[900];
        std::snprintf(claim, sizeof(claim),
                      "INSERT INTO player_asset_idem(idempotency_key,player_id,ok,error_code,"
                      "message,asset_version,remain_count,operation_type,request_hash,created_at) "
                      "VALUES('%s',%llu,0,'IN_PROGRESS','in progress',0,0,'%s','%s',%lld)",
                      esc_key.c_str(), (unsigned long long)player_id, esc_op.c_str(),
                      esc_hash.c_str(), (long long)NowSec());
        if (!conn->update(claim)) {
            conn->rollback();
            for (int i = 0; i < 40; ++i) {
                char q2[768];
                std::snprintf(q2, sizeof(q2),
                              "SELECT ok,error_code,message,asset_version,remain_count,"
                              "request_hash,player_id,operation_type FROM player_asset_idem "
                              "WHERE idempotency_key='%s'",
                              esc_key.c_str());
                MYSQL_RES *res2 = conn->query(q2);
                if (res2) {
                    MYSQL_ROW row2 = mysql_fetch_row(res2);
                    if (row2 && row2[0] && consume_idem_row(row2)) {
                        mysql_free_result(res2);
                        return out->ok;
                    }
                    mysql_free_result(res2);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            out->ok = false;
            out->error_code = "IDEMPOTENCY_BUSY";
            out->message = "idempotency in progress";
            return false;
        }
    }

    // 资产事务内任一步失败必须 ROLLBACK；禁止在含资产修改的事务里 COMMIT FAILED
    auto fail = [&](const char *code, const char *msg) -> bool {
        conn->rollback();
        out->ok = false;
        out->error_code = code;
        out->message = msg;
        return false;
    };

    auto failpoint = [](const char *env) -> bool {
        const char *v = std::getenv(env);
        return v && v[0] != '\0' && v[0] != '0';
    };

    uint64_t ver = 1;
    {
        char q[256];
        std::snprintf(q, sizeof(q),
                      "SELECT asset_version FROM player_asset_meta WHERE player_id=%llu FOR UPDATE",
                      (unsigned long long)player_id);
        MYSQL_RES *res = conn->query(q);
        if (!res)
            return fail("DB_ERROR", "meta query");
        MYSQL_ROW row = mysql_fetch_row(res);
        if (!row || !row[0]) {
            mysql_free_result(res);
            char ins[256];
            std::snprintf(ins, sizeof(ins),
                          "INSERT INTO player_asset_meta(player_id,asset_version,updated_at) "
                          "VALUES(%llu,1,%lld)",
                          (unsigned long long)player_id, (long long)NowSec());
            if (!conn->update(ins))
                return fail("DB_ERROR", "meta insert");
            ver = 1;
        } else {
            ver = std::strtoull(row[0], nullptr, 10);
            mysql_free_result(res);
        }
    }
    if (expected_version != 0 && expected_version != ver)
        return fail("VERSION_CONFLICT", "expected_version mismatch");

    uint32_t cur = 0;
    {
        char q[256];
        std::snprintf(q, sizeof(q),
                      "SELECT count FROM player_asset_bag WHERE player_id=%llu AND item_id=%u FOR "
                      "UPDATE",
                      (unsigned long long)player_id, item_id);
        MYSQL_RES *res = conn->query(q);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row && row[0])
                cur = static_cast<uint32_t>(std::strtoul(row[0], nullptr, 10));
            mysql_free_result(res);
        }
    }

    uint32_t remain = cur;
    if (mutation_type == "GRANT") {
        remain = cur + count;
    } else {
        if (cur < count)
            return fail("NOT_ENOUGH", "not enough item");
        remain = cur - count;
    }

    {
        char ups[320];
        std::snprintf(ups, sizeof(ups),
                      "INSERT INTO player_asset_bag(player_id,item_id,count) VALUES(%llu,%u,%u) "
                      "ON DUPLICATE KEY UPDATE count=%u",
                      (unsigned long long)player_id, item_id, remain, remain);
        if (!conn->update(ups))
            return fail("DB_ERROR", "bag upsert");
    }
    if (failpoint("GAMEMESH_FAILPOINT_MUTATION_AFTER_BAG"))
        return fail("FAILPOINT", "after bag");

    const uint64_t new_ver = ver + 1;
    {
        char ups[256];
        std::snprintf(ups, sizeof(ups),
                      "UPDATE player_asset_meta SET asset_version=%llu,updated_at=%lld WHERE "
                      "player_id=%llu",
                      (unsigned long long)new_ver, (long long)NowSec(),
                      (unsigned long long)player_id);
        if (!conn->update(ups))
            return fail("DB_ERROR", "meta bump");
    }
    if (failpoint("GAMEMESH_FAILPOINT_MUTATION_AFTER_VERSION"))
        return fail("FAILPOINT", "after version");

    GameDbOutbox::Instance().EnsureTable();
    std::ostringstream payload;
    payload << "{\"player_id\":" << player_id << ",\"type\":\"" << mutation_type
            << "\",\"item_id\":" << item_id << ",\"count\":" << count << ",\"version\":" << new_ver
            << "}";
    if (failpoint("GAMEMESH_FAILPOINT_MUTATION_OUTBOX_FAIL") ||
        !GameDbOutbox::Instance().InsertOnConnection(
            conn.get(), "asset.mutated", "player", std::to_string(player_id), idempotency_key,
            payload.str(), NowSec())) {
        return fail("OUTBOX_FAILED", "outbox insert failed; asset tx rolled back");
    }

    {
        char fin[640];
        std::snprintf(fin, sizeof(fin),
                      "UPDATE player_asset_idem SET ok=1,error_code='',message='ok',"
                      "asset_version=%llu,remain_count=%u WHERE idempotency_key='%s'",
                      (unsigned long long)new_ver, remain, esc_key.c_str());
        if (failpoint("GAMEMESH_FAILPOINT_MUTATION_FINALIZE_FAIL") || !conn->update(fin))
            return fail("DB_ERROR", "idem finalize");
    }

    // COMMIT 失败：结果未知。禁止二次 COMMIT / 禁止直接标 FAILED；新连接查询。
    if (failpoint("GAMEMESH_FAILPOINT_MUTATION_COMMIT_FAIL")) {
        conn->rollback();
        conn.reset();
        out->ok = false;
        out->error_code = "UNKNOWN_RESULT";
        out->message = "commit failpoint; result unknown";
        return false;
    }
    if (!conn->commit()) {
        (void)conn->rollback();
        conn.reset();
        OperationQuery q;
        if (QueryOperationResult(player_id, idempotency_key, mutation_type, &q) && q.found) {
            if (q.status == "SUCCEEDED") {
                out->ok = true;
                out->idempotent_hit = true;
                out->message = "recovered_after_commit";
                out->asset_version = q.asset_version;
                out->remain_count = q.remain_count;
                return true;
            }
            if (q.status == "FAILED") {
                out->ok = false;
                out->idempotent_hit = true;
                out->error_code = q.error_code.empty() ? "OPERATION_FAILED" : q.error_code;
                out->message = q.message.empty() ? "operation recorded as failed" : q.message;
                out->asset_version = q.asset_version;
                out->remain_count = q.remain_count;
                return false;
            }
            if (q.status == "IN_PROGRESS") {
                out->ok = false;
                out->error_code = "UNKNOWN_RESULT";
                out->message = "commit unknown; still in progress";
                return false;
            }
        }
        out->ok = false;
        out->error_code = "UNKNOWN_RESULT";
        out->message = "commit result unknown";
        return false;
    }

    out->ok = true;
    out->message = "ok";
    out->asset_version = new_ver;
    out->remain_count = remain;
    return true;
#else
    (void)player_id;
    (void)idempotency_key;
    (void)expected_version;
    (void)mutation_type;
    (void)item_id;
    (void)count;
    if (out) {
        out->error_code = "MYSQL_DISABLED";
        out->message = "mysql not enabled";
    }
    return false;
#endif
}

bool GameDbAssetStore::SaveSnapshot(uint64_t player_id, uint64_t expected_version,
                                    const std::map<uint32_t, uint32_t> &bag,
                                    const std::string &idempotency_key, SnapshotResult *out) {
#ifdef WEBSERVER_ENABLE_MYSQL
    if (!out)
        return false;
    *out = SnapshotResult{};
    if (!EnsureTables() || player_id == 0) {
        out->error_code = "INVALID_ARG";
        out->message = "unavailable";
        return false;
    }
    if (idempotency_key.empty()) {
        out->error_code = "IDEMPOTENCY_REQUIRED";
        out->message = "idempotency_key required";
        return false;
    }
    std::string key_err;
    if (!ValidateIdempotencyKey(idempotency_key, &key_err)) {
        out->error_code = "INVALID_ARGUMENT";
        out->message = key_err;
        return false;
    }
    const std::string req_hash = SnapshotRequestHash(expected_version, bag);
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        out->error_code = "DB_UNAVAILABLE";
        out->message = "no connection";
        return false;
    }
    const std::string esc_key = conn->EscapeSql(idempotency_key);
    const std::string esc_hash = conn->EscapeSql(req_hash);
    {
        char q[640];
        std::snprintf(q, sizeof(q),
                      "SELECT ok,error_code,message,asset_version,request_hash,player_id FROM "
                      "player_asset_idem WHERE idempotency_key='%s'",
                      esc_key.c_str());
        MYSQL_RES *res = conn->query(q);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row && row[0]) {
                const uint64_t pid =
                    row[5] ? std::strtoull(row[5], nullptr, 10) : 0;
                const std::string old_hash = row[4] ? row[4] : "";
                if (pid != 0 && pid != player_id) {
                    out->idempotent_hit = true;
                    out->ok = false;
                    out->error_code = "IDEMPOTENCY_CONFLICT";
                    out->message = "idempotency_key bound to other player";
                    mysql_free_result(res);
                    return false;
                }
                if (!old_hash.empty() && old_hash != req_hash) {
                    out->idempotent_hit = true;
                    out->ok = false;
                    out->error_code = "IDEMPOTENCY_CONFLICT";
                    out->message = "same key different payload";
                    mysql_free_result(res);
                    return false;
                }
                if (std::atoi(row[0]) != 0) {
                    out->idempotent_hit = true;
                    out->ok = true;
                    out->error_code = row[1] ? row[1] : "";
                    out->message = row[2] ? row[2] : "idempotent";
                    out->asset_version = row[3] ? std::strtoull(row[3], nullptr, 10) : 0;
                    mysql_free_result(res);
                    return true;
                }
                // ok=0：进行中，落到下方短等回读
                mysql_free_result(res);
            } else {
                mysql_free_result(res);
            }
        }
    }
    if (!conn->begin()) {
        out->error_code = "TX_BEGIN";
        return false;
    }
    // 先占幂等键，避免并发双写资产
    {
        char claim[768];
        std::snprintf(claim, sizeof(claim),
                      "INSERT INTO player_asset_idem(idempotency_key,player_id,ok,error_code,message,"
                      "asset_version,remain_count,operation_type,request_hash,created_at) "
                      "VALUES('%s',%llu,0,'IN_PROGRESS','in progress',0,0,'SAVE_SNAPSHOT','%s',%lld)",
                      esc_key.c_str(), (unsigned long long)player_id, esc_hash.c_str(),
                      (long long)NowSec());
        if (!conn->update(claim)) {
            conn->rollback();
            for (int i = 0; i < 40; ++i) {
                char q2[640];
                std::snprintf(q2, sizeof(q2),
                              "SELECT ok,error_code,message,asset_version,request_hash FROM "
                              "player_asset_idem WHERE idempotency_key='%s'",
                              esc_key.c_str());
                MYSQL_RES *res2 = conn->query(q2);
                if (res2) {
                    MYSQL_ROW row2 = mysql_fetch_row(res2);
                    if (row2 && row2[0] && std::atoi(row2[0]) != 0) {
                        const std::string old_hash = row2[4] ? row2[4] : "";
                        if (!old_hash.empty() && old_hash != req_hash) {
                            out->ok = false;
                            out->idempotent_hit = true;
                            out->error_code = "IDEMPOTENCY_CONFLICT";
                            out->message = "same key different payload";
                            mysql_free_result(res2);
                            return false;
                        }
                        out->idempotent_hit = true;
                        out->ok = true;
                        out->error_code = row2[1] ? row2[1] : "";
                        out->message = row2[2] ? row2[2] : "idempotent";
                        out->asset_version = row2[3] ? std::strtoull(row2[3], nullptr, 10) : 0;
                        mysql_free_result(res2);
                        return true;
                    }
                    mysql_free_result(res2);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            out->ok = false;
            out->error_code = "IDEMPOTENCY_BUSY";
            out->message = "idempotency in progress";
            return false;
        }
    }
    auto fail = [&](const char *code, const char *msg) -> bool {
        conn->rollback();
        out->ok = false;
        out->error_code = code;
        out->message = msg;
        return false;
    };
    uint64_t ver = 1;
    char q[256];
    std::snprintf(q, sizeof(q),
                  "SELECT asset_version FROM player_asset_meta WHERE player_id=%llu FOR UPDATE",
                  (unsigned long long)player_id);
    MYSQL_RES *res = conn->query(q);
    if (!res)
        return fail("DB_ERROR", "meta query");
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        mysql_free_result(res);
        char ins[256];
        std::snprintf(ins, sizeof(ins),
                      "INSERT INTO player_asset_meta(player_id,asset_version,updated_at) "
                      "VALUES(%llu,1,%lld)",
                      (unsigned long long)player_id, (long long)NowSec());
        if (!conn->update(ins))
            return fail("DB_ERROR", "meta insert");
        ver = 1;
    } else {
        ver = std::strtoull(row[0], nullptr, 10);
        mysql_free_result(res);
    }
    if (expected_version != 0 && expected_version != ver)
        return fail("VERSION_CONFLICT", "expected_version mismatch");
    char del[128];
    std::snprintf(del, sizeof(del), "DELETE FROM player_asset_bag WHERE player_id=%llu",
                  (unsigned long long)player_id);
    if (!conn->update(del))
        return fail("DB_ERROR", "bag clear");
    for (const auto &kv : bag) {
        if (kv.second == 0)
            continue;
        char ups[256];
        std::snprintf(ups, sizeof(ups),
                      "INSERT INTO player_asset_bag(player_id,item_id,count) VALUES(%llu,%u,%u)",
                      (unsigned long long)player_id, kv.first, kv.second);
        if (!conn->update(ups))
            return fail("DB_ERROR", "bag upsert");
    }
    const uint64_t nv = ver + 1;
    char ups[256];
    std::snprintf(ups, sizeof(ups),
                  "UPDATE player_asset_meta SET asset_version=%llu,updated_at=%lld WHERE "
                  "player_id=%llu",
                  (unsigned long long)nv, (long long)NowSec(), (unsigned long long)player_id);
    if (!conn->update(ups))
        return fail("DB_ERROR", "meta bump");
    GameDbOutbox::Instance().EnsureTable();
    if (!GameDbOutbox::Instance().InsertOnConnection(
            conn.get(), "asset.snapshot", "player", std::to_string(player_id), idempotency_key,
            "{\"asset_version\":" + std::to_string(nv) + "}", NowSec()))
        return fail("OUTBOX_FAIL", "outbox insert failed");
    char ups_idem[512];
    std::snprintf(ups_idem, sizeof(ups_idem),
                  "UPDATE player_asset_idem SET ok=1,error_code='',message='ok',"
                  "asset_version=%llu WHERE idempotency_key='%s'",
                  (unsigned long long)nv, esc_key.c_str());
    if (!conn->update(ups_idem))
        return fail("IDEMPOTENCY_FAIL", "idem finalize failed");
    if (!conn->commit())
        return fail("TX_COMMIT", "commit failed");
    out->ok = true;
    out->asset_version = nv;
    out->message = "ok";
    return true;
#else
    (void)player_id;
    (void)expected_version;
    (void)bag;
    (void)idempotency_key;
    if (out) {
        out->error_code = "MYSQL_DISABLED";
        out->message = "mysql not enabled";
    }
    return false;
#endif
}

bool GameDbAssetStore::QueryOperationResult(uint64_t player_id, const std::string &idempotency_key,
                                            const std::string &operation_type,
                                            OperationQuery *out) {
#ifdef WEBSERVER_ENABLE_MYSQL
    if (!out)
        return false;
    *out = OperationQuery{};
    out->status = "NOT_FOUND";
    if (!EnsureTables() || player_id == 0 || idempotency_key.empty())
        return false;
    if (!ValidateIdempotencyKey(idempotency_key, nullptr))
        return false;
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    const std::string esc_key = conn->EscapeSql(idempotency_key);
    char q[640];
    std::snprintf(q, sizeof(q),
                  "SELECT ok,error_code,message,asset_version,remain_count,request_hash,"
                  "operation_type,player_id FROM player_asset_idem WHERE idempotency_key='%s'",
                  esc_key.c_str());
    MYSQL_RES *res = conn->query(q);
    if (!res)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        out->found = false;
        out->status = "NOT_FOUND";
        return true;
    }
    const uint64_t pid = row[7] ? std::strtoull(row[7], nullptr, 10) : 0;
    const std::string op = row[6] ? row[6] : "";
    if (pid != player_id || (!operation_type.empty() && !op.empty() && op != operation_type)) {
        mysql_free_result(res);
        out->found = false;
        out->status = "NOT_FOUND";
        return true;
    }
    out->found = true;
    out->completed_ok = row[0] && std::atoi(row[0]) != 0;
    out->error_code = row[1] ? row[1] : "";
    out->message = row[2] ? row[2] : "";
    out->asset_version = row[3] ? std::strtoull(row[3], nullptr, 10) : 0;
    out->remain_count =
        row[4] ? static_cast<uint32_t>(std::strtoul(row[4], nullptr, 10)) : 0;
    out->request_hash = row[5] ? row[5] : "";
    out->operation_type = op;
    if (out->completed_ok) {
        out->status = "SUCCEEDED";
    } else if (out->error_code == "IN_PROGRESS") {
        out->status = "IN_PROGRESS";
    } else {
        out->status = "FAILED";
    }
    mysql_free_result(res);
    return true;
#else
    (void)player_id;
    (void)idempotency_key;
    (void)operation_type;
    if (out) {
        out->found = false;
        out->status = "NOT_FOUND";
    }
    return false;
#endif
}
