/*
 * Companions: the 40-player raids (Onyxia's Lair, Molten Core, Blackwing Lair, Temple of Ahn'Qiraj, Naxxramas) tuned for a
 * raid of 20. Creatures there get a share of their health when spawned (core hook CreatureScaleHooks), and what they deal
 * to players and their pets a share of its damage. Settings Raid20.Enable, Raid20.Health, Raid20.Damage.
 */

#ifndef COMPANION_SCALING_H
#define COMPANION_SCALING_H

#include "Define.h"

class Creature;
class Unit;

namespace Companions
{
    bool IsScaledRaid(uint32 mapId);
    float RaidHealthScale(Creature const* creature);   // 1 outside the scaled raids
    float RaidDamageScale(Unit const* attacker, Unit const* target);

    void AddScalingScripts();
}

#endif
