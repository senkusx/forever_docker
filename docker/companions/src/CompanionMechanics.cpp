#include "CompanionMechanics.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <unordered_map>

namespace Companions::Mechanics
{
    namespace
    {
        using enum Kind;

        // Spell and creature ids come from the Custom/Classic instance scripts and the SmartAI of the world database
        // (the same ids the encounters use on this server). Radius in yards.
        Rule const Rules[] =
        {
            // ---------------------------------------------------------------- Molten Core
            { DispelFirst,   12118, 19702,  0, 0, ROLES_ALL },                  // Lucifron: Impending Doom
            { DispelFirst,   12118, 19703,  0, 0, ROLES_ALL },                  // Lucifron's Curse
            { KillFirst,     12119,     0,  0, 2, ROLES_ALL },                  // Flamewaker Protector (Dominate Mind)
            { Fear,          11982,     0,  0, 0, ROLES_ALL },                  // Magmadar: Panic
            { Tranquilize,   11982, 19451,  0, 0, ROLES_ALL },                  // Magmadar: Frenzy
            { DispelFirst,   12259, 19716,  0, 0, ROLES_ALL },                  // Gehennas' Curse
            { KillFirst,     11661,     0,  0, 1, ROLES_ALL },                  // Flamewaker (Gehennas)
            { CrowdControlNpc, 12099,   0,  0, 0, ROLES_ALL },                  // Firesworn (Garr): banish
            { KillFirst,     12099,     0,  0, 1, ROLES_ALL },
            { DispelFirst,   12057, 19496,  0, 0, ROLES_ALL },                  // Garr: Magma Shackles
            { RunWithDebuff, 12056, 20475, 25, 0, ROLES_ALL },                  // Baron Geddon: Living Bomb
            { AvoidCast,     12056, 19695, 22, 0, ROLES_ALL },                  // Inferno
            { DispelFirst,   12056, 19659,  0, 0, ROLES_ALL },                  // Ignite Mana
            { DispelFirst,   12264, 19713,  0, 0, ROLES_ALL },                  // Shazzrah's Curse
            { Purge,         12264, 19714,  0, 0, ROLES_ALL },                  // Deaden Magic on Shazzrah
            { Spread,        12264,     0,  6, 0, ROLE_BIT_RANGED | ROLE_BIT_HEALER },
            { KillFirst,     11662,     0,  0, 2, ROLES_ALL },                  // Flamewaker Priest (Sulfuron)
            { Interrupt,     11662, 19775,  0, 0, ROLES_ALL },                  // Dark Mending
            { DontAttack,    11672,     0,  0, 0, ROLES_NOT_TANK },             // Core Rager: dies with Golemagg
            { AvoidCast,     11988, 19798, 12, 0, ROLE_BIT_MELEE | ROLE_BIT_HEALER },   // Golemagg: Earthquake (10%)
            { KillFirst,     11663,     0,  0, 2, ROLES_ALL },                  // Flamewaker Healer (Majordomo)
            { KillFirst,     11664,     0,  0, 1, ROLES_ALL },                  // Flamewaker Elite
            { CrowdControlNpc, 11663,   0,  0, 0, ROLES_ALL },
            { CrowdControlNpc, 11664,   0,  0, 0, ROLES_ALL },
            { StopOnAura,    0,     20619,  0, 2, ROLES_ALL },                  // Magic Reflection: casters stop
            { StopOnAura,    0,     21075,  0, 1, ROLES_ALL },                  // Damage Shield: melee stop
            { DontAttack,    12018,     0,  0, 0, ROLES_ALL },                  // Majordomo submits when his guards die
            { KillFirst,     12143,     0,  0, 2, ROLES_ALL },                  // Son of Flame (Ragnaros)
            { AvoidFront,    11502,     0,  0, 0, ROLES_NOT_TANK },             // Ragnaros

            // ---------------------------------------------------------------- Onyxia's Lair
            { AvoidSides,    10184,     0,  0, 0, ROLES_NOT_TANK },             // Flame Breath in front, Tail Sweep behind
            { Fear,          10184,     0,  0, 0, ROLES_ALL },                  // Bellowing Roar (phase 3)
            { KillFirst,     11262,     0,  0, 1, ROLE_BIT_MELEE | ROLE_BIT_RANGED },   // Onyxian Whelp
            { AvoidLine,     10184, 18576, 80, 14, ROLES_ALL },                 // Deep Breath, every direction
            { AvoidLine,     10184, 18617, 80, 14, ROLES_ALL },
            { AvoidLine,     10184, 17086, 80, 14, ROLES_ALL },
            { AvoidLine,     10184, 18584, 80, 14, ROLES_ALL },
            { AvoidLine,     10184, 18564, 80, 14, ROLES_ALL },
            { AvoidLine,     10184, 18351, 80, 14, ROLES_ALL },
            { AvoidLine,     10184, 18596, 80, 14, ROLES_ALL },
            { AvoidLine,     10184, 18609, 80, 14, ROLES_ALL },

            // ---------------------------------------------------------------- Blackwing Lair
            { AvoidFront,    12435,     0,  0, 0, ROLES_NOT_TANK },             // Razorgore: Cleave
            { RunWithDebuff, 12435, 23023, 10, 0, ROLES_ALL },                  // Conflagration
            { RunWithDebuff, 13020, 23620, 20, 0, ROLES_ALL },                  // Vaelastrasz: Burning Adrenaline
            { TankSwap,      13020, 23620,  0, 1, ROLES_ALL },
            { AvoidSides,    13020,     0,  0, 0, ROLES_NOT_TANK },             // Flame Breath, Tail Sweep
            { AvoidFront,    12017,     0,  0, 0, ROLES_NOT_TANK },             // Broodlord: Cleave
            { AvoidSides,    11983,     0,  0, 0, ROLES_NOT_TANK },             // Firemaw: Shadow Flame, Wing Buffet
            { AvoidSides,    14601,     0,  0, 0, ROLES_NOT_TANK },             // Ebonroc
            { TankSwap,      14601, 23340,  0, 1, ROLES_ALL },                  // Shadow of Ebonroc heals him on the tank
            { AvoidSides,    11981,     0,  0, 0, ROLES_NOT_TANK },             // Flamegor
            { Tranquilize,   11981, 23342,  0, 0, ROLES_ALL },                  // Frenzy
            { AvoidFront,    14020,     0,  0, 0, ROLES_NOT_TANK },             // Chromaggus: breaths
            { Tranquilize,   14020, 23128,  0, 0, ROLES_ALL },
            { DispelFirst,   14020, 23153,  0, 0, ROLES_ALL },                  // Brood Affliction: Blue
            { DispelFirst,   14020, 23154,  0, 0, ROLES_ALL },                  // Black
            { DispelFirst,   14020, 23155,  0, 0, ROLES_ALL },                  // Red
            { DispelFirst,   14020, 23169,  0, 0, ROLES_ALL },                  // Green
            { Fear,          10162,     0,  0, 0, ROLES_ALL },                  // Lord Victor Nefarius
            { AvoidSides,    11583,     0,  0, 0, ROLES_NOT_TANK },             // Nefarian: Shadow Flame, Tail Lash
            { Fear,          11583,     0,  0, 0, ROLES_ALL },                  // Bellowing Roar
            { StopOnAura,    11583, 23418,  0, 1, ROLES_ALL },                  // paladin call: Blessing of Protection on him
            { NoHealWithAura, 0,    23401,  0, 0, ROLES_ALL },                  // priest call: their heals hurt
            { KillFirst,     14662,     0,  0, 3, ROLES_ALL },                  // shaman call: corrupted totems
            { KillFirst,     14663,     0,  0, 3, ROLES_ALL },
            { KillFirst,     14664,     0,  0, 3, ROLES_ALL },
            { KillFirst,     14666,     0,  0, 3, ROLES_ALL },
            { KillFirst,     14668,     0,  0, 2, ROLES_ALL },                  // warlock call: infernals

            // ---------------------------------------------------------------- Zul'Gurub
            { Interrupt,     14507, 23860,  0, 0, ROLES_ALL },                  // Venoxis: Holy Fire
            { Purge,         14507, 23895,  0, 0, ROLES_ALL },                  // Renew
            { Spread,        14507,     0,  8, 0, ROLE_BIT_RANGED | ROLE_BIT_HEALER },  // Holy Wrath jumps
            { KillFirst,     11373,     0,  0, 1, ROLES_ALL },                  // Razzashi Cobra
            { Interrupt,     14517, 23954,  0, 0, ROLES_ALL },                  // Jeklik: Great Heal
            { Interrupt,     14517, 23953,  0, 0, ROLES_ALL },                  // Mind Flay
            { KillFirst,     15041,     0,  0, 2, ROLES_ALL },                  // Spawn of Mar'li
            { KillFirst,     15101,     0,  0, 1, ROLES_ALL },                  // Zulian Prowler (Arlokk)
            { NoDispel,      14834, 24321,  0, 0, ROLES_ALL },                  // Hakkar: Poisonous Blood (stops Blood Siphon)
            { Spread,        14834,     0,  8, 0, ROLE_BIT_RANGED | ROLE_BIT_HEALER },  // Corrupted Blood
            { KillFirst,     15112,     0,  0, 3, ROLES_ALL },                  // Jin'do: Brain Wash Totem
            { KillFirst,     14987,     0,  0, 3, ROLES_ALL },                  // Powerful Healing Ward
            { DispelFirst,   11380, 17172,  0, 0, ROLES_ALL },                  // Hex
            { DispelFirst,   11380, 24306,  0, 0, ROLES_ALL },                  // Delusions of Jin'do
            { FreezeWithDebuff, 11382, 24314, 0, 0, ROLES_ALL },                // Mandokir: Threatening Gaze, do nothing
            { KillFirst,     14988,     0,  0, 1, ROLES_ALL },                  // Ohgan
            { AvoidFront,    15114,     0,  0, 0, ROLES_NOT_TANK },             // Gahz'ranka: Frost Breath
            // Gurubashi Axe Thrower: Axe Flurry (10 seconds of axes, one a second at someone within 30 yards: 1250 damage and
            // a stun; immune to stuns while it spins): kept stunned before it, killed first, the healers out of its reach
            { StunLock,      11350,     0,  0, 0, ROLES_ALL },
            { AvoidNpc,      11350,     0, 31, 0, ROLE_BIT_HEALER },             // Axe Flurry reaches 30 yards: heal from 31
            { KillFirst,     11350,     0,  0, 2, ROLES_ALL },
            { CrowdControlNpc, 11350,   0,  0, 0, ROLES_ALL },
            { Tranquilize,   11350,  8269,  0, 0, ROLES_ALL },                  // Frenzy

            // ---------------------------------------------------------------- Ruins of Ahn'Qiraj
            { TankSwap,      15348, 25646,  0, 5, ROLES_ALL },                  // Kurinnaxx: Mortal Wound
            { KillFirst,     15527,     0,  0, 2, ROLES_ALL },                  // Moam: Mana Fiend
            { KillFirst,     15555,     0,  0, 3, ROLES_ALL },                  // Ayamiss: Hive'Zara Larva
            { AvoidNpc,      15428,     0, 10, 0, ROLES_ALL },                  // Ossirian: Sand Vortex
            { AvoidFront,    15339,     0,  0, 0, ROLES_NOT_TANK },

            // ---------------------------------------------------------------- Temple of Ahn'Qiraj
            { Interrupt,     15263, 26192,  0, 0, ROLES_ALL },                  // Skeram: Arcane Explosion
            { Interrupt,     15543, 25807,  0, 0, ROLES_ALL },                  // Princess Yauj: Great Heal
            { Fear,          15543,     0,  0, 0, ROLES_ALL },
            { AvoidCast,     15516, 26083, 10, 0, ROLES_NOT_TANK },             // Sartura: Whirlwind
            { AvoidCast,     15984, 26038, 10, 0, ROLES_NOT_TANK },             // Sartura's Royal Guard: Whirlwind
            { KillFirst,     15984,     0,  0, 1, ROLES_ALL },
            { KillFirst,     15630,     0,  0, 2, ROLES_ALL },                  // Spawn of Fankriss
            { TankSwap,      15510, 25646,  0, 5, ROLES_ALL },                  // Fankriss: Mortal Wound
            { Tranquilize,   15509, 26051,  0, 0, ROLES_ALL },                  // Huhuran: Frenzy
            { NoDispel,      15509, 26180,  0, 0, ROLES_ALL },                  // Wyvern Sting hits when removed
            { DispelFirst,   15509, 26053,  0, 0, ROLES_ALL },                  // Noxious Poison
            { DontAttack,    15276,     0,  0, 0, ROLE_BIT_MELEE },             // Vek'lor: immune to weapons
            { DontAttack,    15275,     0,  0, 0, ROLE_BIT_RANGED },            // Vek'nilash: immune to spells
            { AvoidFront,    15517,     0,  0, 0, ROLES_NOT_TANK },             // Ouro: Sand Blast
            { Spread,        15589,     0, 10, 0, ROLES_ALL },                  // Eye of C'Thun: Eye Beam jumps
            { AvoidLine,     15589, 26029, 60, 6, ROLES_ALL },                  // Dark Glare
            { KillFirst,     15726,     0,  0, 2, ROLES_ALL },                  // Eye Tentacle
            { KillFirst,     15725,     0,  0, 1, ROLES_ALL },                  // Claw Tentacle

            // ---------------------------------------------------------------- Naxxramas
            { AvoidCast,     15956, 28785, 30, 0, ROLES_NOT_TANK },             // Anub'Rekhan: Locust Swarm
            { AvoidLine,     15956, 28783, 40, 4, ROLES_ALL },                  // Impale
            { KillFirst,     16573,     0,  0, 1, ROLES_ALL },                  // Crypt Guard
            { KillFirst,     16505,     0,  0, 1, ROLES_ALL },                  // Naxxramas Follower (Faerlina)
            { KillFirst,     16486,     0,  0, 3, ROLE_BIT_RANGED },            // Maexxna: Web Wrap
            { KillFirst,     17055,     0,  0, 1, ROLES_ALL },                  // Maexxna Spiderling
            { DispelFirst,   15952, 28776,  0, 0, ROLES_ALL },                  // Necrotic Poison
            { DispelFirst,   15954, 29213,  0, 0, ROLES_ALL },                  // Noth: Curse of the Plaguebringer
            { DispelFirst,   15936, 29998,  0, 0, ROLES_ALL },                  // Heigan: Decrepit Fever
            { KillFirst,     16286,     0,  0, 1, ROLE_BIT_MELEE | ROLE_BIT_RANGED },   // Loatheb: Spore
            { TankSwap,      16062, 28834,  0, 3, ROLES_ALL },                  // Four Horsemen: marks
            { TankSwap,      16063, 28835,  0, 3, ROLES_ALL },
            { TankSwap,      16064, 28832,  0, 3, ROLES_ALL },
            { TankSwap,      16065, 28833,  0, 3, ROLES_ALL },
            { AvoidNpc,      16697,     0,  6, 0, ROLES_ALL },                  // Void Zone
            { Spread,        16063,     0, 10, 0, ROLE_BIT_RANGED | ROLE_BIT_HEALER },  // Zeliek: Holy Wrath jumps
            { RunWithDebuff, 15931, 28169, 15, 0, ROLES_ALL },                  // Grobbulus: Mutating Injection
            { AvoidFront,    15931,     0,  0, 0, ROLES_NOT_TANK },             // Slime Spray
            { AvoidNpc,      16363,     0,  8, 0, ROLES_ALL },                  // Grobbulus Cloud
            { KillFirst,     16290,     0,  0, 1, ROLES_ALL },                  // Fallout Slime
            { Fear,          15932,     0,  0, 0, ROLES_ALL },                  // Gluth: Terrifying Roar
            { TankSwap,      15932, 25646,  0, 5, ROLES_ALL },                  // Mortal Wound
            { Polarity,      15928, 28059,  0, 28084, ROLES_ALL },              // Thaddius: positive left, negative right
            { HideBehind,    15989, 28524,  0, 181247, ROLES_ALL },             // Sapphiron: Frost Breath, behind an Ice Block
            { DispelFirst,   15989, 28542,  0, 0, ROLES_ALL },                  // Life Drain
            { AvoidSides,    15989,     0,  0, 0, ROLES_NOT_TANK },             // Cleave, Tail Sweep
            { AvoidNpc,      16474,     0,  8, 0, ROLES_ALL },                  // Sapphiron's Blizzard
            { Interrupt,     15990, 28478,  0, 0, ROLES_ALL },                  // Kel'Thuzad: Frostbolt
            { RunWithDebuff, 15990, 27819, 12, 0, ROLES_ALL },                  // Detonate Mana
            { Spread,        15990,     0, 10, 0, ROLE_BIT_RANGED | ROLE_BIT_HEALER },  // Frost Blast chains
            { AvoidNpc,      16129,     0,  5, 0, ROLES_ALL },                  // Shadow Fissure
            { CrowdControlNpc, 16441,   0,  0, 0, ROLES_ALL },                  // Guardian of Icecrown: shackle

            // ---------------------------------------------------------------- raid trash
            // (from the SmartAI and the Custom/Classic scripts of the trash: what wipes a raid that ignores it)
            // Molten Core
            { AvoidFront,    11673,     0,  0, 0, ROLES_NOT_TANK },             // Ancient Core Hound: Lava Breath
            { AvoidFront,    12076,     0,  0, 0, ROLES_NOT_TANK },             // Lava Elemental: Pyroclast Barrage stuns
            { AvoidFront,    11667,     0,  0, 0, ROLES_NOT_TANK },             // Flameguard: Cone of Fire
            { AvoidFront,    12100,     0,  0, 0, ROLES_NOT_TANK },             // Lava Reaver: Cleave
            { AvoidFront,    12119,     0,  0, 0, ROLES_NOT_TANK },             // Flamewaker Protector: Cleave
            { DispelFirst,   11668, 19393,  0, 0, ROLES_ALL },                  // Firelord: Soul Burn silences
            { KillFirst,     12265,     0,  0, 1, ROLES_ALL },                  // Lava Spawn (Firelord) splits if it lives
            // Blackwing Lair
            { DispelFirst,   12457, 22274,  0, 0, ROLES_ALL },                  // Blackwing Spellbinder: Greater Polymorph
            { KillFirst,     12457,     0,  0, 1, ROLES_ALL },
            { Interrupt,     12458, 22458,  0, 0, ROLES_ALL },                  // Blackwing Taskmaster: Healing Circle
            { KillFirst,     12458,     0,  0, 1, ROLES_ALL },
            { Interrupt,     12459, 22336,  0, 0, ROLES_ALL },                  // Blackwing Warlock: Shadow Bolt
            { Interrupt,     12459, 19717,  0, 0, ROLES_ALL },                  // Rain of Fire
            { KillFirst,     12459,     0,  0, 1, ROLES_ALL },
            { KillFirst,     14081,     0,  0, 2, ROLES_ALL },                  // its Demon Portal
            { AvoidFront,    12460,     0,  0, 0, ROLES_NOT_TANK },             // Death Talon Wyrmguard: Cleave
            { AvoidFront,    12461,     0,  0, 0, ROLES_NOT_TANK },             // Death Talon Overseer: Cleave
            { Tranquilize,   12464, 22428,  0, 0, ROLES_ALL },                  // Death Talon Seether: Frenzy
            { Interrupt,     12465, 22425,  0, 0, ROLES_ALL },                  // Death Talon Wyrmkin: Fireball Volley
            { KillFirst,     12467,     0,  0, 2, ROLES_ALL },                  // Death Talon Captain: Aura of Flames, marks
            { AvoidFront,    14456,     0,  0, 0, ROLES_NOT_TANK },             // Blackwing Guardsman: Cleave
            { DispelFirst,   12557, 22274,  0, 0, ROLES_ALL },                  // Grethok the Controller: Greater Polymorph
            // Zul'Gurub
            { KillFirst,     11338,     0,  0, 1, ROLES_ALL },                  // Hakkari Shadowcaster
            { Interrupt,     11338, 20741,  0, 0, ROLES_ALL },                  // Shadow Bolt Volley
            { KillFirst,     11830,     0,  0, 1, ROLES_ALL },                  // Hakkari Priest
            { Interrupt,     11830, 22883,  0, 0, ROLES_ALL },                  // Heal
            { Fear,          11830,     0,  0, 0, ROLES_ALL },                  // Psychic Scream
            { StopOnAura,    11830, 24021,  0, 2, ROLES_ALL },                  // Anti-Magic Shield: casters stop
            { KillFirst,     11831,     0,  0, 1, ROLES_ALL },                  // Hakkari Witch Doctor
            { DispelFirst,   11831, 24053,  0, 0, ROLES_ALL },                  // Hex
            { AvoidNpc,      15009,     0,  6, 0, ROLES_ALL },                  // its Voodoo Spirit: unkillable, kited
            { KillFirst,     11340,     0,  0, 1, ROLES_ALL },                  // Hakkari Blood Priest
            { Interrupt,     11340, 24618,  0, 0, ROLES_ALL },                  // Drain Life
            { Interrupt,     11353, 24435,  0, 0, ROLES_ALL },                  // Gurubashi Blood Drinker: Drain Life
            { Fear,          11352,     0,  0, 0, ROLES_ALL },                  // Gurubashi Berserker: Intimidating Roar
            { Fear,          11359,     0,  0, 0, ROLES_ALL },                  // Soulflayer
            { AvoidFront,    11372,     0,  0, 0, ROLES_NOT_TANK },             // Razzashi Adder: Venom Spit
            { DispelFirst,   11371, 24002,  0, 0, ROLES_ALL },                  // Razzashi Serpent: Tranquilizing Poison
            { DispelFirst,   11371, 24003,  0, 0, ROLES_ALL },
            { AvoidCast,     14750, 24024, 10, 0, ROLES_ALL },                  // Gurubashi Bat Rider blows up below 40%
            { DispelFirst,   14825, 24673,  0, 0, ROLES_ALL },                  // Withered Mistress: Curse of Blood
            { DispelFirst,   14825, 24674,  0, 0, ROLES_ALL },                  // Veil of Shadow
            { Interrupt,     11347, 24208,  0, 0, ROLES_ALL },                  // Zealot Lor'Khan: Great Heal
            // Ruins of Ahn'Qiraj
            { AvoidFront,    15319,     0,  0, 0, ROLES_NOT_TANK },             // Hive'Zara Collector: Web Spray
            { AvoidFront,    15462,     0,  0, 0, ROLES_NOT_TANK },             // Spitting Scarab: Acid Spit
            { AvoidFront,    15388,     0,  0, 0, ROLES_NOT_TANK },             // Major Pakkon: Sweeping Slam
            { Fear,          15391,     0,  0, 0, ROLES_ALL },                  // Captain Qeez: Intimidating Shout
            { DrainMana,     15338,     0,  0, 0, ROLES_ALL },                  // Obsidian Destroyer: Purge at full mana
            // Temple of Ahn'Qiraj
            { KillFirst,     15246,     0,  0, 1, ROLES_ALL },                  // Qiraji Mindslayer: Cause Insanity
            { Fear,          15246,     0,  0, 0, ROLES_ALL },                  // Mana Burn and Mind Flay fear
            { Interrupt,     15246, 26044,  0, 0, ROLES_ALL },
            { KillFirst,     15247,     0,  0, 1, ROLES_ALL },                  // Qiraji Brainwasher
            { Interrupt,     15247, 26044,  0, 0, ROLES_ALL },
            { AvoidCast,     15249, 26038, 10, 0, ROLES_NOT_TANK },             // Qiraji Lasher: Whirlwind
            { AvoidCast,     15250, 13736, 10, 0, ROLES_NOT_TANK },             // Qiraji Slayer: Whirlwind
            { Tranquilize,   15250, 26041,  0, 0, ROLES_ALL },                  // Frenzy
            { Fear,          15252,     0,  0, 0, ROLES_ALL },                  // Qiraji Champion: Intimidating Shout
            { Fear,          15311,     0,  0, 0, ROLES_ALL },                  // Anubisath Warder
            { RunWithDebuff, 15277, 26556,  8, 0, ROLES_ALL },                  // Anubisath Defender: Plague
            { DrainMana,     15262,     0,  0, 0, ROLES_ALL },                  // Obsidian Eradicator: Shock Blast at full mana
            { DrainMana,     15312,     0,  0, 0, ROLES_ALL },                  // Obsidian Nullifier: Nullify at full mana
            { Interrupt,     15240, 26601,  0, 0, ROLES_ALL },                  // Vekniss Hive Crawler: Poison Bolt
            // Naxxramas
            { AvoidFront,    15975,     0,  0, 0, ROLES_NOT_TANK },             // Carrion Spinner: Poison Spray
            { AvoidFront,    15978,     0,  0, 0, ROLES_NOT_TANK },             // Crypt Reaver: Cleave
            { Tranquilize,   15978, 28371,  0, 0, ROLES_ALL },                  // Frenzy
            { Interrupt,     15981, 28448,  0, 0, ROLES_ALL },                  // Naxxramas Acolyte: Shadow Bolt Volley
            { Interrupt,     16368, 28448,  0, 0, ROLES_ALL },                  // Necropolis Acolyte
            { AvoidFront,    16017,     0,  0, 0, ROLES_NOT_TANK },             // Patchwork Golem: Cleave
            { AvoidFront,    16018,     0,  0, 0, ROLES_NOT_TANK },             // Bile Retcher: Bile Vomit
            { Interrupt,     16020, 28306,  0, 0, ROLES_ALL },                  // Mad Scientist: Great Heal
            { Fear,          16021,     0,  0, 0, ROLES_ALL },                  // Living Monstrosity: Fear
            { Interrupt,     16021, 28293,  0, 0, ROLES_ALL },                  // Chain Lightning
            { KillFirst,     16385,     0,  0, 2, ROLES_ALL },                  // its Lightning Totem
            { Interrupt,     16022, 28310,  0, 0, ROLES_ALL },                  // Surgical Assistant: Mind Flay
            { AvoidNpc,      16024,     0,  9, 0, ROLES_NOT_TANK },             // Embalming Slime: Embalming Cloud
            { AvoidFront,    16025,     0,  0, 0, ROLES_NOT_TANK },             // Stitched Spewer: cone knockback
            { AvoidCast,     16145, 28333,  8, 0, ROLES_NOT_TANK },             // Deathknight Captain: Whirlwind
            { AvoidCast,     16145, 28334,  8, 0, ROLES_NOT_TANK },
            { Fear,          16146,     0,  0, 0, ROLES_ALL },                  // Deathknight: Intimidating Shout
            { AvoidFront,    16163,     0,  0, 0, ROLES_NOT_TANK },             // Deathknight Cavalier: Cleave
            { Fear,          16163,     0,  0, 0, ROLES_ALL },                  // Death Coil
            { AvoidFront,    16451,     0,  0, 0, ROLES_NOT_TANK },             // Deathknight Vindicator
            { Fear,          16451,     0,  0, 0, ROLES_ALL },
            { AvoidFront,    16861,     0,  0, 0, ROLES_NOT_TANK },             // Death Lord
            { Fear,          16861,     0,  0, 0, ROLES_ALL },
            { Interrupt,     16164, 28407,  0, 0, ROLES_ALL },                  // Shade of Naxxramas: Shadow Bolt Volley
            { AvoidFront,    16165,     0,  0, 0, ROLES_NOT_TANK },             // Necro Knight: Cone of Cold
            { AvoidFront,    16167,     0,  0, 0, ROLES_NOT_TANK },             // Bony Construct: Sweeping Slam
            { Interrupt,     16168, 28995,  0, 0, ROLES_ALL },                  // Stoneskin Gargoyle: Stoneskin heals it
            { Interrupt,     16446, 28995,  0, 0, ROLES_ALL },                  // Plagued Gargoyle
            { AvoidCast,     16194, 24236,  8, 0, ROLES_NOT_TANK },             // Unholy Axe: Whirlwind
            { AvoidCast,     16215, 28450, 20, 0, ROLES_NOT_TANK },             // Unholy Staff: Arcane Explosion
            { DispelFirst,   16215, 29848,  0, 0, ROLES_ALL },                  // Polymorph
            { Tranquilize,   16447, 24318,  0, 0, ROLES_ALL },                  // Plagued Ghoul: Enrage
            { AvoidFront,    16447,     0,  0, 0, ROLES_NOT_TANK },

            // ---------------------------------------------------------------- dungeons
            { Interrupt,      3977,  9232,  0, 0, ROLES_ALL },                  // Whitemane: Scarlet Resurrection
            { Interrupt,      3977, 12039,  0, 0, ROLES_ALL },                  // Heal
            { Interrupt,      5710, 12492,  0, 0, ROLES_ALL },                  // Jammal'an: Healing Wave
            { KillFirst,      7915,     0,  0, 2, ROLES_ALL },                  // Thermaplugg: Walking Bomb
            { KillFirst,      9178,     0,  0, 2, ROLES_ALL },                  // Flamelash: Burning Spirit
            { Fear,          10430,     0,  0, 0, ROLES_ALL },                  // The Beast
            { AvoidFront,    10430,     0,  0, 0, ROLES_NOT_TANK },
            { AvoidFront,     9568,     0,  0, 0, ROLES_NOT_TANK },             // Wyrmthalak: Cleave
            { AvoidFront,    10363,     0,  0, 0, ROLES_NOT_TANK },             // Drakkisath: Flamestrike, Cleave
            { Fear,          10440,     0,  0, 0, ROLES_ALL },                  // Rivendare
            { KillFirst,      8929,     0,  0, 1, ROLES_ALL },                  // Princess Moira heals the Emperor
            { AvoidFront,    11501,     0,  0, 0, ROLES_NOT_TANK },             // King Gordok
            { KillFirst,     14324,     0,  0, 1, ROLES_ALL },                  // Cho'Rush (heals Gordok)
        };

        Encounter const Encounters[] =
        {
            // Molten Core
            { 12118, "Lucifron", "Kill the two Flamewaker Protectors first (they mind control). Remove Lucifron's Curse and Impending Doom at once." },
            { 11982, "Magmadar", "Fears every 30 s: Fear Ward and Tremor Totem on the tank. Hunters Tranquilizing Shot the Frenzy. Step out of Lava Bombs." },
            { 12259, "Gehennas", "Kill the two Flamewakers first. Remove Gehennas' Curse (-75% healing). Move out of Rain of Fire." },
            { 12057, "Garr", "Banish or kill the Firesworn one by one away from the group: each one dying explodes and empowers Garr. Then Garr." },
            { 12056, "Baron Geddon", "Living Bomb: run away from everyone until it explodes. Everyone leaves him during Inferno. Ignite Mana: dispel it." },
            { 12264, "Shazzrah", "Blinks and casts Arcane Explosion around himself: ranged stand apart. Remove his curse, purge Deaden Magic from him." },
            { 12098, "Sulfuron Harbinger", "Kill the four Flamewaker Priests first, interrupting Dark Mending. Then Sulfuron." },
            { 11988, "Golemagg the Incinerator", "One tank holds the Core Ragers away; nobody attacks them, they die with him. Melee leave at 10% (Earthquake)." },
            { 12018, "Majordomo Executus", "He submits when his guards die: kill the Flamewaker Healers, then the Elites. Casters stop on Magic Reflection, melee on Damage Shield." },
            { 11502, "Ragnaros", "Melee behind him (Wrath of Ragnaros knocks back), ranged spread. When he submerges, kill the eight Sons of Flame." },
            // Onyxia
            { 10184, "Onyxia", "Stand at her flanks: breath in front, tail behind. Phase 2 (flying): kill whelps, never stand in the line of Deep Breath. Phase 3: Fear Ward for Bellowing Roar." },
            // Blackwing Lair
            { 12435, "Razorgore the Untamed", "Phase 1 needs a player controlling him through the orb to break the eggs. Phase 2: tank him, run away with Conflagration." },
            { 13020, "Vaelastrasz the Corrupt", "Stand at his flanks. Burning Adrenaline: run away from everyone; on the tank, the next tank taunts at once." },
            { 12017, "Broodlord Lashlayer", "Tank faces him away (Cleave); Knock Away drops threat, off-tanks taunt back." },
            { 11983, "Firemaw", "Stand at his flanks. Flame Buffet stacks: break line of sight to drop them." },
            { 14601, "Ebonroc", "Stand at his flanks. Shadow of Ebonroc on the tank heals him: the second tank taunts at once." },
            { 11981, "Flamegor", "Stand at his flanks. Hunters Tranquilizing Shot every Frenzy." },
            { 14020, "Chromaggus", "Tank him round the corner, the group out of his breaths. Remove the Brood Afflictions; Tranquilizing Shot the Frenzy." },
            { 10162, "Lord Victor Nefarius", "Phase 1: kill the drakonids, Fear Ward, CC the mind controlled. Phase 2: Nefarian lands." },
            { 11583, "Nefarian", "Stand at his flanks. Class calls: priests stop healing, kill the shaman totems and the warlock infernals, melee stop on his Blessing of Protection." },
            // Zul'Gurub
            { 14507, "High Priest Venoxis", "Interrupt Holy Fire, purge his Renew, kill the cobras. Snake form: step out of the poison clouds." },
            { 14517, "High Priestess Jeklik", "Interrupt Great Heal and Mind Flay. Step out of the fire the bat riders throw." },
            { 14510, "High Priestess Mar'li", "Kill the Spawn of Mar'li as they hatch. Cure the poison of her volleys." },
            { 14509, "High Priest Thekal", "Kill Zath, Lor'Khan and Thekal within 10 s of each other or they rise again. Interrupt Lor'Khan's Great Heal." },
            { 14515, "High Priestess Arlokk", "Kill the panthers that come for the marked player. Melee out of her Whirlwind." },
            { 14834, "Hakkar", "Keep Poisonous Blood (do not cure it) so Blood Siphon does not heal him. Spread for Corrupted Blood; CC the mind controlled." },
            { 11380, "Jin'do the Hexxer", "Kill the Brain Wash Totem and the Powerful Healing Ward first. Remove Hex and Delusions." },
            { 11382, "Bloodlord Mandokir", "Threatening Gaze: whoever has it does nothing at all until it ends. Kill Ohgan first." },
            { 15114, "Gahz'ranka", "Stand behind him (Frost Breath). Massive Geyser throws up: no ledges." },
            // Ruins of Ahn'Qiraj
            { 15348, "Kurinnaxx", "Two tanks swap at five Mortal Wounds. Step off the sand traps." },
            { 15340, "Moam", "Kill the Mana Fiends; drain or burn his mana before he reaches full (Arcane Eruption)." },
            { 15370, "Buru the Gorger", "He fixes on one player who kites him over the eggs; break eggs next to him. Below 20% burn him." },
            { 15369, "Ayamiss the Hunter", "Kill each Hive'Zara Larva before it reaches the altar." },
            { 15339, "Ossirian the Unscarred", "Pull him to a crystal to make him vulnerable; keep away from the sand vortexes." },
            // Temple of Ahn'Qiraj
            { 15263, "The Prophet Skeram", "Interrupt Arcane Explosion; kill the images; CC the mind controlled." },
            { 15543, "Princess Yauj", "Interrupt Great Heal; Fear Ward the tank. Kill the bugs one by one, step out of Kri's poison cloud." },
            { 15516, "Battleguard Sartura", "Kill her guards first; everyone but the tank leaves during Whirlwind." },
            { 15510, "Fankriss the Unyielding", "Kill the Spawns of Fankriss at once; tanks swap at five Mortal Wounds." },
            { 15299, "Viscidus", "Frost damage freezes him, then melee shatter him; kill the globs before they reach him." },
            { 15509, "Princess Huhuran", "Tranquilizing Shot every Frenzy; do not cure Wyvern Sting; below 30% everyone stacks in melee." },
            { 15276, "Emperor Vek'lor", "Immune to weapons: casters and a warlock tank him. The twins swap places; the tanks take the other one." },
            { 15275, "Emperor Vek'nilash", "Immune to spells: melee and a warrior tank him." },
            { 15517, "Ouro", "Stand behind him (Sand Blast). When he submerges, move from the dirt mounds." },
            { 15589, "Eye of C'Thun", "Everyone ten yards apart (Eye Beam jumps). Leave the line of Dark Glare. Kill the Eye and Claw Tentacles." },
            { 15727, "C'Thun", "Kill the Giant Tentacles outside, then inside his stomach; the Flesh Tentacles open him." },
            // Naxxramas
            { 15956, "Anub'Rekhan", "Locust Swarm: the tank kites him, everyone else stays well away. Step out of Impale's line." },
            { 15953, "Grand Widow Faerlina", "Kill the followers; her Enrage is removed when a worshipper dies next to her (needs a priest's mind control)." },
            { 15952, "Maexxna", "Ranged free the Web Wrapped; cure Necrotic Poison on the tank; kill the spiderlings." },
            { 15954, "Noth the Plaguebringer", "Remove his curse at once; kill the adds when he blinks away." },
            { 15936, "Heigan the Unclean", "Dance: move with the safe zone away from the eruptions; remove Decrepit Fever." },
            { 16011, "Loatheb", "Heal only when your Corrupted Mind allows; kill a Spore for the critical strike bonus." },
            { 16061, "Instructor Razuvious", "Needs priests mind controlling the Understudies to tank him." },
            { 16060, "Gothik the Harvester", "Split between the living and the dead side; kill what comes through." },
            { 16062, "Highlord Mograine", "Four Horsemen: tanks swap every three marks; stay out of void zones; spread for Zeliek." },
            { 16028, "Patchwerk", "Hateful Strike hits the healthiest player in melee besides the tank: off-tanks stand in melee." },
            { 15931, "Grobbulus", "Mutating Injection: run away from everyone; stand behind him (Slime Spray)." },
            { 15932, "Gluth", "Kite the zombies; Fear Ward the tank; tanks swap at five Mortal Wounds." },
            { 15928, "Thaddius", "Polarity Shift: positive charges stand left of him, negative charges right." },
            { 15989, "Sapphiron", "Stand at his flanks. When he flies, hide behind an Ice Block from Frost Breath. Remove Life Drain." },
            { 15990, "Kel'Thuzad", "Interrupt Frostbolt; ranged spread; Detonate Mana: run away; step out of Shadow Fissures; CC the mind controlled." },
            // dungeons
            { 3977, "High Inquisitor Whitemane", "Interrupt Scarlet Resurrection and her heals; kill Mograine again." },
            { 3975, "Herod", "Melee leave during Whirlwind; adds pour in after he dies." },
            { 6487, "Arcanist Doan", "Melee leave when he casts Arcane Explosion." },
            { 4543, "Bloodmage Thalnos", "Step out of Flame Spike and Fire Nova." },
            { 5710, "Jammal'an the Prophet", "Interrupt Healing Wave; CC the hexed." },
            { 7800, "Mekgineer Thermaplugg", "Kill the walking bombs before they reach the group." },
            { 9156, "Ambassador Flamelash", "Kill the Burning Spirits before they reach him." },
            { 9019, "Emperor Dagran Thaurissan", "Princess Moira heals him: kill or interrupt her first." },
            { 10363, "General Drakkisath", "Separate his two guards; tank him facing away." },
            { 10430, "The Beast", "Fear Ward the tank; stand behind him." },
            { 10440, "Baron Rivendare", "Kill the raised skeletons quickly; Death Pact heals him." },
            { 11501, "King Gordok", "Kill Cho'Rush first; tank Gordok facing away." },
            { 12201, "Princess Theradras", "Melee step out of Dust Field; ranged spread for Boulder." },
            { 10508, "Ras Frostwhisper", "Fire damage; dispel Frostbolt slows." },
            { 10503, "Jandice Barov", "Kill the real Jandice among the illusions (the one that takes damage)." },
        };

        struct Index
        {
            std::unordered_map<uint32, std::vector<Rule const*>> ByEntry;
            std::unordered_map<uint32, std::vector<Rule const*>> BySpell;
            std::unordered_map<uint32, Encounter const*> Encounters;
        };

        Index const& GetIndex()
        {
            static Index const index = []
            {
                Index i;
                for (Rule const& rule : Rules)
                {
                    i.ByEntry[rule.Entry].push_back(&rule);
                    if (rule.Spell)
                        i.BySpell[rule.Spell].push_back(&rule);
                }
                for (Encounter const& encounter : Encounters)
                    i.Encounters[encounter.Entry] = &encounter;
                return i;
            }();
            return index;
        }

        std::vector<Rule const*> const NoRules;
    }

    std::vector<Rule const*> const& RulesFor(uint32 entry)
    {
        Index const& index = GetIndex();
        auto itr = index.ByEntry.find(entry);
        return itr != index.ByEntry.end() ? itr->second : NoRules;
    }

    std::vector<Rule const*> const& RulesForSpell(uint32 spellId)
    {
        Index const& index = GetIndex();
        auto itr = index.BySpell.find(spellId);
        return itr != index.BySpell.end() ? itr->second : NoRules;
    }

    Encounter const* EncounterFor(uint32 entry)
    {
        Index const& index = GetIndex();
        auto itr = index.Encounters.find(entry);
        return itr != index.Encounters.end() ? itr->second : nullptr;
    }

    bool HasRule(uint32 entry, Kind type)
    {
        for (Rule const* rule : RulesFor(entry))
            if (rule->Type == type)
                return true;
        return false;
    }

    Rule const* FindSpellRule(uint32 spellId, Kind type)
    {
        for (Rule const* rule : RulesForSpell(spellId))
            if (rule->Type == type)
                return rule;
        return nullptr;
    }

    size_t RuleCount() { return std::size(Rules); }
    size_t EncounterCount() { return std::size(Encounters); }

    float CasterAreaRadius(SpellInfo const* spell)
    {
        if (!spell || spell->IsPositive())
            return 0.0f;
        float radius = 0.0f;
        for (SpellEffectInfo const& effect : spell->GetEffects())
        {
            if (!effect.IsEffect())
                continue;
            // an area of enemies centred on the caster (Arcane Explosion, Thunderclap, War Stomp, Whirlwind...)
            bool aroundCaster = effect.TargetA.GetTarget() == TARGET_SRC_CASTER || effect.TargetB.GetTarget() == TARGET_SRC_CASTER;
            bool area = effect.TargetA.GetSelectionCategory() == TARGET_SELECT_CATEGORY_AREA
                || effect.TargetB.GetSelectionCategory() == TARGET_SELECT_CATEGORY_AREA;
            if (aroundCaster && area)
                radius = std::max({ radius, effect.CalcRadius().Max, effect.CalcRadius(nullptr, SpellTargetIndex::TargetB).Max });
            // a channel that keeps hitting around the caster (Whirlwind, Inferno) through a periodic trigger
            if (effect.IsAura(SPELL_AURA_PERIODIC_TRIGGER_SPELL) && effect.TriggerSpell)
                if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell, spell->Difficulty))
                    if (triggered != spell)
                        for (SpellEffectInfo const& inner : triggered->GetEffects())
                            if ((inner.TargetA.GetTarget() == TARGET_SRC_CASTER || inner.TargetB.GetTarget() == TARGET_SRC_CASTER)
                                && (inner.TargetA.GetSelectionCategory() == TARGET_SELECT_CATEGORY_AREA
                                    || inner.TargetB.GetSelectionCategory() == TARGET_SELECT_CATEGORY_AREA))
                                radius = std::max({ radius, inner.CalcRadius().Max, inner.CalcRadius(nullptr, SpellTargetIndex::TargetB).Max });
        }
        return radius <= 40.0f ? radius : 0.0f;
    }

    bool IsCone(SpellInfo const* spell)
    {
        if (!spell || spell->IsPositive())
            return false;
        for (SpellEffectInfo const& effect : spell->GetEffects())
            if (effect.IsEffect() && (effect.TargetA.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE
                || effect.TargetB.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE))
                return true;
        return false;
    }

    bool IsUrgentInterrupt(SpellInfo const* spell)
    {
        if (!spell)
            return false;
        for (SpellEffectInfo const& effect : spell->GetEffects())
        {
            if (effect.IsEffect(SPELL_EFFECT_HEAL) || effect.IsEffect(SPELL_EFFECT_HEAL_PCT) || effect.IsEffect(SPELL_EFFECT_HEAL_MAX_HEALTH)
                || effect.IsEffect(SPELL_EFFECT_RESURRECT) || effect.IsEffect(SPELL_EFFECT_RESURRECT_NEW))
                return true;
            if (effect.IsAura(SPELL_AURA_PERIODIC_HEAL) || effect.IsAura(SPELL_AURA_MOD_FEAR) || effect.IsAura(SPELL_AURA_MOD_CONFUSE)
                || effect.IsAura(SPELL_AURA_MOD_CHARM) || effect.IsAura(SPELL_AURA_MOD_POSSESS) || effect.IsAura(SPELL_AURA_TRANSFORM)
                || effect.IsAura(SPELL_AURA_MOD_STUN))
                return true;
        }
        return false;
    }
}
