/*
 * Companions: boss mechanics. What the bots know about the scripted encounters of the dungeons and raids (Custom/Classic
 * instance scripts, spell and creature ids from them): which casts to step away from or interrupt first, debuffs to run
 * out with, who to kill first, when to stop attacking, tank swaps, fears, spreading, polarity... The AI applies the rules
 * of the enemies it fights (CompanionAI::UpdateMechanics), and the brain gets each boss's note with the tactical review.
 * Spells nobody wrote a rule for are still read generically (casts around the caster, cones, heals and crowd control).
 */

#ifndef COMPANION_MECHANICS_H
#define COMPANION_MECHANICS_H

#include "Define.h"
#include <string>
#include <vector>

class Creature;
class SpellInfo;
class Unit;

namespace Companions::Mechanics
{
    enum class Kind : uint8
    {
        Interrupt,      // Spell: interrupt it before anything else
        AvoidCast,      // Spell (cast, channel or aura on the caster): bots within Radius of it leave until it ends
        AvoidFront,     // Entry: nobody but its tank stands in front of it (breath, cleave, cone)
        AvoidSides,     // Entry: front and tail both hit: the others stand at its flanks
        RunWithDebuff,  // Spell (aura): a bot with it runs Radius away from everyone; the others keep away from it
        AvoidNpc,       // Entry (add, cloud, tornado, void zone): keep Radius away from it
        Spread,         // Entry: while it fights, ranged and healers stand Radius apart
        KillFirst,      // Entry: damage dealers switch to this add (higher Value first)
        DontAttack,     // Entry: never attack it (Value 1: only melee, 2: only casters)
        StopOnAura,     // Spell (aura on the enemy): who it hurts stops attacking (Value 1: melee, 2: casters, 3: all)
        TankSwap,       // Spell (aura on the tank): at Value stacks another tank taunts the boss
        Fear,           // Entry: fears; priests keep Fear Ward on the tank, shamans Tremor Totem, warriors Berserker Rage
        NoDispel,       // Spell (aura): leave it (it is needed, or dispelling it hurts)
        DispelFirst,    // Spell (aura): dispel it before anything else
        Polarity,       // Thaddius: positive charge (Spell) to the left of Entry, negative (Value) to the right
        HideBehind,     // Spell (cast): hide behind the game object Value (line of sight) until it ends
        CrowdControlNpc,// Entry: may be crowd controlled (polymorph, banish, shackle...) instead of killed
        Tranquilize,    // Spell (aura on the enemy, Frenzy): hunters Tranquilizing Shot it
        Purge,          // Spell (aura on the enemy): priests Dispel Magic, shamans Purge it
        AvoidLine,      // Spell (cast): leave the band Value yards wide, Radius long, in front of the caster (Deep Breath, Impale)
        NoHealWithAura, // Spell (aura on the bot): its heals hurt, it does not heal while it has it
        FreezeWithDebuff, // Spell (aura on the bot): it does nothing at all while it has it (Threatening Gaze)
    };

    enum : uint8
    {
        ROLES_ALL       = 0x0F,
        ROLE_BIT_TANK   = 0x01,
        ROLE_BIT_HEALER = 0x02,
        ROLE_BIT_MELEE  = 0x04,
        ROLE_BIT_RANGED = 0x08,
        ROLES_NOT_TANK  = ROLE_BIT_HEALER | ROLE_BIT_MELEE | ROLE_BIT_RANGED,
    };

    struct Rule
    {
        Kind Type;
        uint32 Entry;       // the boss or add the rule belongs to (0: any)
        uint32 Spell;
        float Radius;
        uint32 Value;
        uint8 Roles;        // who follows it (ROLE_BIT_*)
    };

    struct Encounter
    {
        uint32 Entry;
        char const* Name;
        char const* Note;   // tactics in a sentence or two (the brain gets it, ".bot tactics" shows it)
    };

    std::vector<Rule const*> const& RulesFor(uint32 entry);          // the rules of this creature (and of its adds)
    std::vector<Rule const*> const& RulesForSpell(uint32 spellId);   // the rules that name this spell
    Encounter const* EncounterFor(uint32 entry);
    bool HasRule(uint32 entry, Kind type);
    Rule const* FindSpellRule(uint32 spellId, Kind type);
    size_t RuleCount();
    size_t EncounterCount();

    // generic readings of a spell nobody wrote a rule for
    float CasterAreaRadius(SpellInfo const* spell);    // hostile area around its caster (Arcane Explosion, War Stomp...), 0 if not
    bool IsCone(SpellInfo const* spell);                // breath, cleave in front
    bool IsUrgentInterrupt(SpellInfo const* spell);     // heals, fears, polymorph, mind control
}

#endif
