#include "PlayerProfileStore.h"

#include "Connection.h"
#include "ConnectionPool.h"
#include "Logging.h"
#include "PlayerProfileDefaults.h"
#include "Utf8Text.h"

#include <mysql/mysql.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace {

std::mutex g_mu;

bool Finite(float v) { return std::isfinite(v); }

std::string SqlLit(Connection *conn, const std::string &s) {
    if (conn)
        return conn->EscapeSql(s);
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '\'')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

bool FetchRow(MYSQL_RES *res, PlayerProfileRow *out) {
    if (!res || !out)
        return false;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        out->exists = false;
        return true;
    }
    out->exists = true;
    out->player_id = static_cast<uint64_t>(std::strtoull(row[0] ? row[0] : "0", nullptr, 10));
    out->player_name = row[1] ? row[1] : "";
    out->hp = row[2] ? std::atoi(row[2]) : 0;
    out->max_hp = row[3] ? std::atoi(row[3]) : 0;
    out->mp = row[4] ? std::atoi(row[4]) : 0;
    out->max_mp = row[5] ? std::atoi(row[5]) : 0;
    out->attack = row[6] ? std::atoi(row[6]) : 0;
    out->spell_power = row[7] ? std::atoi(row[7]) : 0;
    out->defense = row[8] ? std::atoi(row[8]) : 0;
    out->magic_resistance = row[9] ? std::atoi(row[9]) : 0;
    out->crit_chance = row[10] ? static_cast<float>(std::atof(row[10])) : 0.0f;
    out->crit_damage = row[11] ? static_cast<float>(std::atof(row[11])) : 0.0f;
    out->move_speed = row[12] ? static_cast<float>(std::atof(row[12])) : 0.0f;
    out->attack_speed = row[13] ? static_cast<float>(std::atof(row[13])) : 0.0f;
    out->stats_version = static_cast<uint64_t>(std::strtoull(row[14] ? row[14] : "1", nullptr, 10));
    return true;
}

const char *kSelectCols =
    "player_id,player_name,hp,max_hp,mp,max_mp,attack,spell_power,defense,magic_resistance,"
    "crit_chance,crit_damage,move_speed,attack_speed,stats_version";

}  // namespace

PlayerProfileStore &PlayerProfileStore::Instance() {
    static PlayerProfileStore g;
    return g;
}

void PlayerProfileStore::FillDefaults(uint64_t player_id, const std::string &player_name,
                                      PlayerProfileRow *out) {
    if (!out)
        return;
    const auto d = PlayerProfileDefaults::Get();
    *out = PlayerProfileRow{};
    out->player_id = player_id;
    out->player_name = player_name.empty() ? "player" : player_name;
    if (out->player_name.size() > 64)
        out->player_name.resize(64);
    out->hp = d.hp;
    out->max_hp = d.max_hp;
    out->mp = d.mp;
    out->max_mp = d.max_mp;
    out->attack = d.attack;
    out->spell_power = d.spell_power;
    out->defense = d.defense;
    out->magic_resistance = d.magic_resistance;
    out->crit_chance = d.crit_chance;
    out->crit_damage = d.crit_damage;
    out->move_speed = d.move_speed;
    out->attack_speed = d.attack_speed;
    out->stats_version = 1;
    out->exists = true;
}

bool PlayerProfileStore::Validate(const PlayerProfileRow &row, std::string *err_code,
                                  std::string *err) {
    auto fail = [&](const char *code, const char *msg) {
        if (err_code)
            *err_code = code;
        if (err)
            *err = msg;
        return false;
    };
    if (row.player_id == 0)
        return fail("ERR_PROFILE_INVALID", "player_id required");
    std::string tec;
    if (!utf8text::ValidBoundedText(row.player_name, 1, 64, false, &tec))
        return fail(tec.empty() ? "ERR_PROFILE_NAME" : tec.c_str(), "invalid player_name");
    if (row.max_hp < 1 || row.max_hp > 999999)
        return fail("ERR_PROFILE_RANGE", "max_hp out of range");
    if (row.hp < 0 || row.hp > row.max_hp)
        return fail("ERR_PROFILE_RANGE", "hp out of range");
    if (row.max_mp < 1 || row.max_mp > 999999)
        return fail("ERR_PROFILE_RANGE", "max_mp out of range");
    if (row.mp < 0 || row.mp > row.max_mp)
        return fail("ERR_PROFILE_RANGE", "mp out of range");
    if (row.attack < 0 || row.attack > 999999)
        return fail("ERR_PROFILE_RANGE", "attack out of range");
    if (row.spell_power < 0 || row.spell_power > 999999)
        return fail("ERR_PROFILE_RANGE", "spell_power out of range");
    if (row.defense < 0 || row.defense > 999999)
        return fail("ERR_PROFILE_RANGE", "defense out of range");
    if (row.magic_resistance < 0 || row.magic_resistance > 999999)
        return fail("ERR_PROFILE_RANGE", "magic_resistance out of range");
    if (!Finite(row.crit_chance) || row.crit_chance < 0.0f || row.crit_chance > 1.0f)
        return fail("ERR_PROFILE_RANGE", "crit_chance out of range");
    if (!Finite(row.crit_damage) || row.crit_damage < 1.0f || row.crit_damage > 10.0f)
        return fail("ERR_PROFILE_RANGE", "crit_damage out of range");
    if (!Finite(row.move_speed) || row.move_speed < 0.1f || row.move_speed > 100.0f)
        return fail("ERR_PROFILE_RANGE", "move_speed out of range");
    if (!Finite(row.attack_speed) || row.attack_speed < 0.1f || row.attack_speed > 10.0f)
        return fail("ERR_PROFILE_RANGE", "attack_speed out of range");
    return true;
}

bool PlayerProfileStore::EnsureTable() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (table_ready_)
        return true;
    if (!ConnectionPool::getconnectionPool()->isInitialized()) {
        available_ = false;
        return false;
    }
    available_ = true;
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn)
        return false;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS player_profile ("
        "player_id BIGINT NOT NULL PRIMARY KEY,"
        "player_name VARCHAR(64) NOT NULL DEFAULT 'player',"
        "hp INT NOT NULL,"
        "max_hp INT NOT NULL,"
        "mp INT NOT NULL,"
        "max_mp INT NOT NULL,"
        "attack INT NOT NULL,"
        "spell_power INT NOT NULL,"
        "defense INT NOT NULL,"
        "magic_resistance INT NOT NULL,"
        "crit_chance FLOAT NOT NULL,"
        "crit_damage FLOAT NOT NULL,"
        "move_speed FLOAT NOT NULL,"
        "attack_speed FLOAT NOT NULL,"
        "stats_version BIGINT NOT NULL DEFAULT 1,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "KEY idx_player_profile_name (player_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    if (!conn->update(sql))
        return false;
    table_ready_ = true;
    LOG_INFO << "PlayerProfileStore: table player_profile ready";
    return true;
}

bool PlayerProfileStore::InsertDefaultOnConnection(Connection *conn, uint64_t player_id,
                                                   const std::string &player_name,
                                                   std::string *err) {
    if (!conn || player_id == 0) {
        if (err)
            *err = "bad arg";
        return false;
    }
    PlayerProfileRow row;
    FillDefaults(player_id, player_name, &row);
    std::string code;
    if (!Validate(row, &code, err))
        return false;
    std::ostringstream sql;
    sql << "INSERT IGNORE INTO player_profile (player_id,player_name,hp,max_hp,mp,max_mp,attack,"
           "spell_power,defense,magic_resistance,crit_chance,crit_damage,move_speed,attack_speed,"
           "stats_version) VALUES ("
        << player_id << ",'" << SqlLit(conn, row.player_name) << "'," << row.hp << "," << row.max_hp
        << "," << row.mp << "," << row.max_mp << "," << row.attack << "," << row.spell_power << ","
        << row.defense << "," << row.magic_resistance << "," << row.crit_chance << ","
        << row.crit_damage << "," << row.move_speed << "," << row.attack_speed << ","
        << row.stats_version << ")";
    if (!conn->update(sql.str())) {
        if (err)
            *err = "profile insert failed";
        return false;
    }
    return true;
}

bool PlayerProfileStore::EnsureDefault(uint64_t player_id, const std::string &player_name,
                                       std::string *err) {
    if (!EnsureTable()) {
        if (err)
            *err = "db unavailable";
        return false;
    }
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        if (err)
            *err = "no connection";
        return false;
    }
    return InsertDefaultOnConnection(conn.get(), player_id, player_name, err);
}

bool PlayerProfileStore::Load(uint64_t player_id, PlayerProfileRow *out, std::string *err) {
    if (!out || player_id == 0) {
        if (err)
            *err = "bad arg";
        return false;
    }
    if (!EnsureTable()) {
        if (err)
            *err = "db unavailable";
        return false;
    }
    *out = PlayerProfileRow{};
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        if (err)
            *err = "no connection";
        return false;
    }
    std::ostringstream sql;
    sql << "SELECT " << kSelectCols << " FROM player_profile WHERE player_id=" << player_id
        << " LIMIT 1";
    MYSQL_RES *res = conn->query(sql.str());
    if (!res) {
        if (err)
            *err = "query failed";
        return false;
    }
    const bool ok = FetchRow(res, out);
    mysql_free_result(res);
    if (!ok) {
        if (err)
            *err = "parse failed";
        return false;
    }
    return true;
}

bool PlayerProfileStore::Save(const PlayerProfileRow &row, uint64_t expected_stats_version,
                              uint64_t *out_version, std::string *err, std::string *err_code) {
    std::string code, msg;
    if (!Validate(row, &code, &msg)) {
        if (err_code)
            *err_code = code;
        if (err)
            *err = msg;
        return false;
    }
    if (!EnsureTable()) {
        if (err)
            *err = "db unavailable";
        if (err_code)
            *err_code = "ERR_DB_UNAVAILABLE";
        return false;
    }
    auto conn = ConnectionPool::getconnectionPool()->getConnection();
    if (!conn) {
        if (err)
            *err = "no connection";
        if (err_code)
            *err_code = "ERR_DB_UNAVAILABLE";
        return false;
    }
    if (!conn->begin()) {
        if (err)
            *err = "begin failed";
        if (err_code)
            *err_code = "ERR_DB_UNAVAILABLE";
        return false;
    }
    std::ostringstream sel;
    sel << "SELECT stats_version FROM player_profile WHERE player_id=" << row.player_id
        << " FOR UPDATE";
    MYSQL_RES *res = conn->query(sel.str());
    if (!res) {
        conn->rollback();
        if (err)
            *err = "lock failed";
        if (err_code)
            *err_code = "ERR_DB_UNAVAILABLE";
        return false;
    }
    MYSQL_ROW r = mysql_fetch_row(res);
    if (!r || !r[0]) {
        mysql_free_result(res);
        conn->rollback();
        if (err)
            *err = "profile not found";
        if (err_code)
            *err_code = "ERR_PROFILE_NOT_FOUND";
        return false;
    }
    const uint64_t cur = static_cast<uint64_t>(std::strtoull(r[0], nullptr, 10));
    mysql_free_result(res);
    if (expected_stats_version != 0 && expected_stats_version != cur) {
        conn->rollback();
        if (err)
            *err = "stats version conflict";
        if (err_code)
            *err_code = "ERR_PROFILE_VERSION";
        return false;
    }
    const uint64_t next = cur + 1;
    std::ostringstream upd;
    upd << "UPDATE player_profile SET player_name='" << SqlLit(conn.get(), row.player_name)
        << "',hp=" << row.hp << ",max_hp=" << row.max_hp << ",mp=" << row.mp
        << ",max_mp=" << row.max_mp << ",attack=" << row.attack << ",spell_power=" << row.spell_power
        << ",defense=" << row.defense << ",magic_resistance=" << row.magic_resistance
        << ",crit_chance=" << row.crit_chance << ",crit_damage=" << row.crit_damage
        << ",move_speed=" << row.move_speed << ",attack_speed=" << row.attack_speed
        << ",stats_version=" << next << " WHERE player_id=" << row.player_id
        << " AND stats_version=" << cur;
    if (!conn->update(upd.str())) {
        conn->rollback();
        if (err)
            *err = "update failed";
        if (err_code)
            *err_code = "ERR_PROFILE_VERSION";
        return false;
    }
    if (!conn->commit()) {
        conn->rollback();
        if (err)
            *err = "commit failed";
        if (err_code)
            *err_code = "ERR_DB_UNAVAILABLE";
        return false;
    }
    if (out_version)
        *out_version = next;
    return true;
}
