#pragma once

#include <cstdint>
#include <string>

/** 新号默认属性；数值来自 config/player_profile_defaults.conf，禁止散落魔法数。 */
struct PlayerProfileDefaults {
    int32_t hp = 100;
    int32_t max_hp = 100;
    int32_t mp = 100;
    int32_t max_mp = 100;
    int32_t attack = 10;
    int32_t spell_power = 10;
    int32_t defense = 5;
    int32_t magic_resistance = 5;
    float crit_chance = 0.05f;
    float crit_damage = 1.5f;
    float move_speed = 10.0f;
    float attack_speed = 1.0f;

    static PlayerProfileDefaults Get();
    static bool LoadFromConfig();
};
