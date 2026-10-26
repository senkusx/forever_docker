/*
 * Companions: configuration. Defaults come from worldserver.conf ("Companions.*", or TC_COMPANIONS_* environment
 * variables); the settings the web panel changes are stored in characters.companion_settings and win over the file.
 */

#ifndef COMPANION_CONFIG_H
#define COMPANION_CONFIG_H

#include "Define.h"
#include <string>
#include <vector>

namespace Companions
{
    struct Config
    {
        bool Enable = true;
        uint32 MaxPerPlayer = 4;            // companions one character may have with him (1-39)
        uint32 MaxTotal = 39;               // companions in the game at once on the whole server
        uint32 RaidSize = 20;               // largest group companions fill (owner and other players included)
        std::string AccountName = "COMPANIONS";
        bool AllowAlts = true;              // ".bot add <name>" also works with the owner's own characters
        bool SyncLevel = true;              // companion characters follow the owner's level
        bool AutoGear = true;               // re-gear companion characters when their level changes
        uint32 GearMaxQuality = 3;          // 2 = green, 3 = blue, 4 = epic
        bool FreeReagents = true;           // keep reagents, ammo, totems and soul shards stocked
        bool FreeFood = true;               // eat and drink without items
        bool AutoRevive = true;             // revive next to the owner when nobody can resurrect
        uint32 AutoReviveDelay = 30;        // seconds out of combat before reviving alone
        float TeleportDistance = 90.0f;     // out of combat, a bot farther than this joins the owner
        uint32 UpdateInterval = 100;        // ms between two AI decisions
        bool RestoreOnLogin = true;         // the owner's active bots come back at login
        uint32 MountLevel = 40;
        bool AnnounceInParty = true;        // bots acknowledge orders in party chat
        float FollowMinDistance = 2.0f;     // out of combat the bots spread around the owner between these
        float FollowMaxDistance = 9.0f;     // distances, each at its own spot, moving now and then
        bool Callouts = true;               // bots without a brain call out the essentials (out of mana, adds...)

        bool LLMEnable = false;             // the brain answers chat and gives orders (Claude, Ollama, OpenAI-compatible)
        bool LLMDefaultOn = true;           // new bots use the brain when it is enabled
        bool LLMPartyChat = true;           // every party line that is not an order goes to the brain (callouts, talk)
        bool LLMCallouts = true;            // the brain words the bots' situational callouts
        bool LLMBanter = false;             // bots comment on events (level up, boss kill)
        uint32 LLMPollInterval = 1000;      // ms
        uint32 LLMMaxAge = 45;              // seconds after which an unanswered line is dropped
        bool LLMTactics = true;             // in fights the brain assigns kill order, crowd control, interrupts, cooldowns
        uint32 LLMTacticsInterval = 15;     // seconds between two tactical reviews of the same fight

        bool Raid20Enable = true;           // the 40-player raids are tuned for 20 (see CompanionScaling.h)
        float Raid20Health = 0.5f;          // share of their creatures' health (applies when they spawn)
        float Raid20Damage = 0.8f;          // share of the damage they deal to players and pets

        bool AmbientEnable = true;          // random bots around players in the open world (CompanionAmbient.h)
        uint32 AmbientDensity = 5;          // ambient bots around each player in the open world
        uint32 AmbientCityDensity = 15;     // ... in a capital city
        uint32 AmbientMaxTotal = 60;        // ambient bots on the whole server
        float AmbientRadius = 150.0f;       // yards around a player they count in (and appear in)
        uint32 AmbientPoolSize = 200;       // characters kept for them (reused, recycled when too high in level)
        uint32 AmbientGearQuality = 2;      // highest item quality they wear (2 green, 3 blue)
    };

    // one setting the web panel can read and change
    struct SettingInfo
    {
        std::string Key;
        std::string Value;
        char Type;      // 'b' bool, 'i' integer, 'f' decimal
        float Min;
        float Max;
    };

    Config const& GetConfig();
    void LoadConfig();                                  // worldserver.conf, then the stored overrides
    void LoadStoredSettings();                          // characters.companion_settings (world startup)
    std::vector<SettingInfo> ListSettings();
    bool ChangeSetting(std::string const& key, std::string const& value, std::string& error);
    bool ResetSetting(std::string const& key);         // back to worldserver.conf
    bool IsStored(std::string const& key);              // changed in the web panel
}

#endif
