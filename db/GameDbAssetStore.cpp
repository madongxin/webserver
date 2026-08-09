#include "GameDbAssetStore.h"

#include "ConnectionPool.h"
#include "GameDbOutbox.h"
#include "Logging.h"
#include "PlayerItemStore.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

int64_t NowSec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string Escape(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\'')
            o += "''";
        else
            o.push_back(c);
    }
    return o;
}

}  // namespace

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
        "created_at BIGINT NOT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    const char *bag =
        "CREATE TABLE IF NOT EXISTS player_asset_bag ("
        "player_id BIGINT UNSIGNED NOT NULL,"
        "item_id INT UNSIGNED NOT NULL,"
        "count INT UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY (player_id, item_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    available_ = conn->update(meta) && conn->update(idem) && conn->update(bag);
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
    if (!EnsureTables() || player_id == 0 || idempotency_key.empty() || item_id == 0 || count == 0) {
        out->error_code = "INVALID_ARG";
        out->message = "invalid mutation";
        return false;
    }
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        out->error_code = "DB_UNAVAILABLE";
        out->message = "no connection";
        return false;
    }
    {
        char q[512];
        std::snprintf(q, sizeof(q),
                      "SELECT ok,error_code,message,asset_version,remain_count FROM "
                      "player_asset_idem WHERE idempotency_key='%s'",
                      Escape(idempotency_key).c_str());
        MYSQL_RES *res = conn->query(q);
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row && row[0]) {
                out->ok = std::atoi(row[0]) != 0;
                out->idempotent_hit = true;
                out->error_code = row[1] ? row[1] : "";
                out->message = row[2] ? row[2] : "idempotent";
                out->asset_version = row[3] ? std::strtoull(row[3], nullptr, 10) : 0;
                out->remain_count =
                    row[4] ? static_cast<uint32_t>(std::strtoul(row[4], nullptr, 10)) : 0;
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
    auto fail = [&](const char *code, const char *msg) -> bool {
        conn->rollback();
        out->ok = false;
        out->error_code = code;
        out->message = msg;
        return false;
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
    } else if (mutation_type == "CONSUME") {
        if (cur < count)
            return fail("NOT_ENOUGH", "not enough item");
        remain = cur - count;
    } else {
        return fail("BAD_TYPE", "mutation_type GRANT|CONSUME");
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

    GameDbOutbox::Instance().EnsureTable();
    std::ostringstream payload;
    payload << "{\"player_id\":" << player_id << ",\"type\":\"" << mutation_type
            << "\",\"item_id\":" << item_id << ",\"count\":" << count << ",\"version\":" << new_ver
            << "}";
    if (!GameDbOutbox::Instance().InsertOnConnection(
            conn.get(), "asset.mutated", "player", std::to_string(player_id), idempotency_key,
            payload.str(), NowSec())) {
        LOG_WARN << "GameDbAssetStore outbox insert failed player=" << player_id;
    }

    {
        char ins[768];
        std::snprintf(ins, sizeof(ins),
                      "INSERT INTO player_asset_idem(idempotency_key,player_id,ok,error_code,"
                      "message,asset_version,remain_count,created_at) VALUES('%s',%llu,1,'','ok',"
                      "%llu,%u,%lld)",
                      Escape(idempotency_key).c_str(), (unsigned long long)player_id,
                      (unsigned long long)new_ver, remain, (long long)NowSec());
        if (!conn->update(ins))
            return fail("DB_ERROR", "idem insert");
    }
    if (!conn->commit())
        return fail("TX_COMMIT", "commit failed");

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
                                    const std::string &idempotency_key, uint64_t *new_version,
                                    std::string *err) {
#ifdef WEBSERVER_ENABLE_MYSQL
    if (!EnsureTables() || player_id == 0) {
        if (err)
            *err = "unavailable";
        return false;
    }
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        if (err)
            *err = "no connection";
        return false;
    }
    if (!conn->begin()) {
        if (err)
            *err = "tx";
        return false;
    }
    uint64_t ver = 1;
    char q[256];
    std::snprintf(q, sizeof(q),
                  "SELECT asset_version FROM player_asset_meta WHERE player_id=%llu FOR UPDATE",
                  (unsigned long long)player_id);
    MYSQL_RES *res = conn->query(q);
    if (!res) {
        conn->rollback();
        return false;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        mysql_free_result(res);
        char ins[256];
        std::snprintf(ins, sizeof(ins),
                      "INSERT INTO player_asset_meta(player_id,asset_version,updated_at) "
                      "VALUES(%llu,1,%lld)",
                      (unsigned long long)player_id, (long long)NowSec());
        if (!conn->update(ins)) {
            conn->rollback();
            return false;
        }
        ver = 1;
    } else {
        ver = std::strtoull(row[0], nullptr, 10);
        mysql_free_result(res);
    }
    if (expected_version != 0 && expected_version != ver) {
        conn->rollback();
        if (err)
            *err = "VERSION_CONFLICT";
        return false;
    }
    char del[128];
    std::snprintf(del, sizeof(del), "DELETE FROM player_asset_bag WHERE player_id=%llu",
                  (unsigned long long)player_id);
    conn->update(del);
    for (const auto &kv : bag) {
        if (kv.second == 0)
            continue;
        char ups[256];
        std::snprintf(ups, sizeof(ups),
                      "INSERT INTO player_asset_bag(player_id,item_id,count) VALUES(%llu,%u,%u)",
                      (unsigned long long)player_id, kv.first, kv.second);
        if (!conn->update(ups)) {
            conn->rollback();
            return false;
        }
    }
    const uint64_t nv = ver + 1;
    char ups[256];
    std::snprintf(ups, sizeof(ups),
                  "UPDATE player_asset_meta SET asset_version=%llu,updated_at=%lld WHERE "
                  "player_id=%llu",
                  (unsigned long long)nv, (long long)NowSec(), (unsigned long long)player_id);
    if (!conn->update(ups) || !conn->commit()) {
        conn->rollback();
        return false;
    }
    (void)idempotency_key;
    if (new_version)
        *new_version = nv;
    return true;
#else
    (void)player_id;
    (void)expected_version;
    (void)bag;
    (void)idempotency_key;
    (void)new_version;
    if (err)
        *err = "MYSQL_DISABLED";
    return false;
#endif
}
