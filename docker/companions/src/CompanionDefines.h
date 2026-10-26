/*
 * Companions: player bots for the TrinityCore "forever" branch (WoW Forever 1.60).
 * Bots are real characters logged in through a socketless WorldSession and driven by a server-side AI.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef COMPANION_DEFINES_H
#define COMPANION_DEFINES_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <string_view>

namespace Companions
{
    enum class Role : uint8
    {
        Auto      = 0,  // picked from class and talents
        Tank      = 1,
        Healer    = 2,
        MeleeDps  = 3,
        RangedDps = 4,
    };

    // what the bot does when nobody gives it an order
    enum class Stance : uint8
    {
        Follow = 0,     // follow the owner in formation
        Stay   = 1,     // hold the current position (still fights what comes in range)
    };

    enum class Aggression : uint8
    {
        Assist     = 0, // fights what the group fights (default, like a careful player)
        Passive    = 1, // never attacks, only heals/buffs/follows
        Aggressive = 2, // also attacks hostile mobs that come close
    };

    enum RaidIcon : uint8
    {
        ICON_STAR     = 0,
        ICON_CIRCLE   = 1,
        ICON_DIAMOND  = 2,
        ICON_TRIANGLE = 3,
        ICON_MOON     = 4,  // crowd control target: never break it
        ICON_SQUARE   = 5,
        ICON_CROSS    = 6,  // kill second
        ICON_SKULL    = 7,  // kill first
    };

    inline char const* RoleName(Role role)
    {
        switch (role)
        {
            case Role::Tank:      return "tank";
            case Role::Healer:    return "healer";
            case Role::MeleeDps:  return "melee dps";
            case Role::RangedDps: return "ranged dps";
            default:              return "auto";
        }
    }

    // "dps" resolves to melee or ranged from the class (classId 0 = melee)
    bool ParseRole(std::string_view text, Role& role, uint8 classId);
    Role DpsRoleForClass(uint8 classId);
    std::string ToLower(std::string_view text);
    // word appears in text as a whole word (not inside another one): "rend" in "kill rend" but not in "friend"
    bool ContainsWord(std::string_view text, std::string_view word);
}

#endif
