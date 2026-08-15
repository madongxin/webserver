#include "PlayerProfileDefaults.h"

#include "GameMeshPaths.h"
#include "Logging.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace {

std::mutex g_mu;
PlayerProfileDefaults g_defaults;
std::atomic<bool> g_loaded{false};

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        ++i;
    return s.substr(i);
}

std::string ConfigPath() {
    if (const char *e = std::getenv("GAMEMESH_PROFILE_DEFAULTS")) {
        if (e && *e)
            return e;
    }
    std::string resolved;
    if (GameMeshPaths::ResolveProjectSubdir("config/player_profile_defaults.conf", &resolved))
        return resolved;
    return "config/player_profile_defaults.conf";
}

}  // namespace

bool PlayerProfileDefaults::LoadFromConfig() {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::string path = ConfigPath();
    std::ifstream in(path);
    PlayerProfileDefaults v;
    if (!in) {
        LOG_WARN << "PlayerProfileDefaults: cannot read " << path << ", using compiled defaults";
        g_defaults = v;
        g_loaded.store(true);
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));
        if (key == "hp")
            v.hp = std::atoi(val.c_str());
        else if (key == "max_hp")
            v.max_hp = std::atoi(val.c_str());
        else if (key == "mp")
            v.mp = std::atoi(val.c_str());
        else if (key == "max_mp")
            v.max_mp = std::atoi(val.c_str());
        else if (key == "attack")
            v.attack = std::atoi(val.c_str());
        else if (key == "spell_power")
            v.spell_power = std::atoi(val.c_str());
        else if (key == "defense")
            v.defense = std::atoi(val.c_str());
        else if (key == "magic_resistance")
            v.magic_resistance = std::atoi(val.c_str());
        else if (key == "crit_chance")
            v.crit_chance = static_cast<float>(std::atof(val.c_str()));
        else if (key == "crit_damage")
            v.crit_damage = static_cast<float>(std::atof(val.c_str()));
        else if (key == "move_speed")
            v.move_speed = static_cast<float>(std::atof(val.c_str()));
        else if (key == "attack_speed")
            v.attack_speed = static_cast<float>(std::atof(val.c_str()));
    }
    if (v.max_hp <= 0)
        v.max_hp = 100;
    if (v.hp < 0 || v.hp > v.max_hp)
        v.hp = v.max_hp;
    if (v.max_mp <= 0)
        v.max_mp = 100;
    if (v.mp < 0 || v.mp > v.max_mp)
        v.mp = v.max_mp;
    if (v.move_speed <= 0.0f)
        v.move_speed = 10.0f;
    g_defaults = v;
    g_loaded.store(true);
    LOG_INFO << "PlayerProfileDefaults: max_hp=" << g_defaults.max_hp
             << " move_speed=" << g_defaults.move_speed;
    return true;
}

PlayerProfileDefaults PlayerProfileDefaults::Get() {
    if (!g_loaded.load())
        LoadFromConfig();
    std::lock_guard<std::mutex> lk(g_mu);
    return g_defaults;
}
