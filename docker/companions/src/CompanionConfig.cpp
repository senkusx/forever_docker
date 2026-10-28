/*
 * Companions: configuration
 */

#include "CompanionConfig.h"
#include "CompanionDefines.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "SharedDefines.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>

namespace Companions
{
    static Config _config;
    static std::map<std::string, std::string> _stored;     // overrides saved by the web panel

    namespace
    {
        // settings the web panel may change; key, type, bounds, and where the value lives
        struct Setting
        {
            char const* Key;
            char Type;
            float Min;
            float Max;
            std::function<void*(Config&)> Field;
        };

        std::vector<Setting> const& Settings()
        {
            static std::vector<Setting> const settings =
            {
                { "MaxPerPlayer",      'i', 1, 39,  [](Config& c) -> void* { return &c.MaxPerPlayer; } },
                { "MaxTotal",          'i', 1, 500, [](Config& c) -> void* { return &c.MaxTotal; } },
                { "RaidSize",          'i', 5, 40,  [](Config& c) -> void* { return &c.RaidSize; } },
                { "FollowMinDistance", 'f', 1, 15,  [](Config& c) -> void* { return &c.FollowMinDistance; } },
                { "FollowMaxDistance", 'f', 2, 25,  [](Config& c) -> void* { return &c.FollowMaxDistance; } },
                { "GearMaxQuality",    'i', 2, 4,   [](Config& c) -> void* { return &c.GearMaxQuality; } },
                { "SyncLevel",         'b', 0, 1,   [](Config& c) -> void* { return &c.SyncLevel; } },
                { "AutoGear",          'b', 0, 1,   [](Config& c) -> void* { return &c.AutoGear; } },
                { "FreeReagents",      'b', 0, 1,   [](Config& c) -> void* { return &c.FreeReagents; } },
                { "FreeFood",          'b', 0, 1,   [](Config& c) -> void* { return &c.FreeFood; } },
                { "AutoRevive",        'b', 0, 1,   [](Config& c) -> void* { return &c.AutoRevive; } },
                { "AutoReviveDelay",   'i', 5, 600, [](Config& c) -> void* { return &c.AutoReviveDelay; } },
                { "TeleportDistance",  'f', 30, 300, [](Config& c) -> void* { return &c.TeleportDistance; } },
                { "MountLevel",        'i', 1, 60,  [](Config& c) -> void* { return &c.MountLevel; } },
                { "AnnounceInParty",   'b', 0, 1,   [](Config& c) -> void* { return &c.AnnounceInParty; } },
                { "Callouts",          'b', 0, 1,   [](Config& c) -> void* { return &c.Callouts; } },
                { "RestoreOnLogin",    'b', 0, 1,   [](Config& c) -> void* { return &c.RestoreOnLogin; } },
                { "AllowAlts",         'b', 0, 1,   [](Config& c) -> void* { return &c.AllowAlts; } },
                { "LLM.Enable",        'b', 0, 1,   [](Config& c) -> void* { return &c.LLMEnable; } },
                { "LLM.DefaultOn",     'b', 0, 1,   [](Config& c) -> void* { return &c.LLMDefaultOn; } },
                { "LLM.PartyChat",     'b', 0, 1,   [](Config& c) -> void* { return &c.LLMPartyChat; } },
                { "LLM.Callouts",      'b', 0, 1,   [](Config& c) -> void* { return &c.LLMCallouts; } },
                { "LLM.Banter",        'b', 0, 1,   [](Config& c) -> void* { return &c.LLMBanter; } },
                { "LLM.Reactions",     'i', 0, 100, [](Config& c) -> void* { return &c.LLMReactions; } },
                { "LLM.MaxAge",        'i', 10, 300, [](Config& c) -> void* { return &c.LLMMaxAge; } },
                { "LLM.Tactics",       'b', 0, 1,   [](Config& c) -> void* { return &c.LLMTactics; } },
                { "LLM.TacticsInterval", 'i', 5, 60, [](Config& c) -> void* { return &c.LLMTacticsInterval; } },
                { "Raid20.Enable",     'b', 0, 1,   [](Config& c) -> void* { return &c.Raid20Enable; } },
                { "Raid20.Health",     'f', 0.1f, 1, [](Config& c) -> void* { return &c.Raid20Health; } },
                { "Raid20.Damage",     'f', 0.1f, 1, [](Config& c) -> void* { return &c.Raid20Damage; } },
                { "Ambient.Enable",    'b', 0, 1,   [](Config& c) -> void* { return &c.AmbientEnable; } },
                { "Ambient.Density",   'i', 0, 30,  [](Config& c) -> void* { return &c.AmbientDensity; } },
                { "Ambient.CityDensity", 'i', 0, 40, [](Config& c) -> void* { return &c.AmbientCityDensity; } },
                { "Ambient.MaxTotal",  'i', 0, 300, [](Config& c) -> void* { return &c.AmbientMaxTotal; } },
                { "Ambient.Radius",    'f', 60, 300, [](Config& c) -> void* { return &c.AmbientRadius; } },
                { "Ambient.PoolSize",  'i', 20, 1000, [](Config& c) -> void* { return &c.AmbientPoolSize; } },
                { "Ambient.GearQuality", 'i', 1, 4, [](Config& c) -> void* { return &c.AmbientGearQuality; } },
                { "Ambient.CompanionChance", 'i', 0, 100, [](Config& c) -> void* { return &c.AmbientCompanionChance; } },
            };
            return settings;
        }

        Setting const* FindSetting(std::string const& key)
        {
            for (Setting const& setting : Settings())
                if (key == setting.Key)
                    return &setting;
            return nullptr;
        }

        std::string ValueOf(Setting const& setting, Config& config)
        {
            void* field = setting.Field(config);
            std::ostringstream ss;
            switch (setting.Type)
            {
                case 'b': ss << (*static_cast<bool*>(field) ? 1 : 0); break;
                case 'i': ss << *static_cast<uint32*>(field); break;
                default:  ss << *static_cast<float*>(field); break;
            }
            return ss.str();
        }

        bool Apply(Setting const& setting, Config& config, std::string const& text, std::string& error)
        {
            char* end = nullptr;
            float value = std::strtof(text.c_str(), &end);
            if (text.empty() || (end && *end))
            {
                error = "not a number";
                return false;
            }
            if (value < setting.Min || value > setting.Max)
            {
                error = "out of range";
                return false;
            }
            void* field = setting.Field(config);
            switch (setting.Type)
            {
                case 'b': *static_cast<bool*>(field) = value != 0.0f; break;
                case 'i': *static_cast<uint32*>(field) = uint32(value); break;
                default:  *static_cast<float*>(field) = value; break;
            }
            return true;
        }

        void Normalize(Config& c)
        {
            if (c.FollowMaxDistance < c.FollowMinDistance + 1.0f)
                c.FollowMaxDistance = c.FollowMinDistance + 1.0f;
            c.MaxPerPlayer = std::min(c.MaxPerPlayer, c.RaidSize - 1);
        }
    }

    Config const& GetConfig()
    {
        return _config;
    }

    void LoadConfig()
    {
        Config c;
        c.Enable          = sConfigMgr->GetBoolDefault("Companions.Enable", true, true);
        c.MaxPerPlayer    = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.MaxPerPlayer", 4, true), 1, 39);
        c.MaxTotal        = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.MaxTotal", 39, true), 1, 500);
        c.RaidSize        = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.RaidSize", 20, true), 5, 40);
        c.AccountName     = sConfigMgr->GetStringDefault("Companions.AccountName", "COMPANIONS", true);
        c.AllowAlts       = sConfigMgr->GetBoolDefault("Companions.AllowAlts", true, true);
        c.SyncLevel       = sConfigMgr->GetBoolDefault("Companions.SyncLevel", true, true);
        c.AutoGear        = sConfigMgr->GetBoolDefault("Companions.AutoGear", true, true);
        c.GearMaxQuality  = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.Gear.MaxQuality", 3, true), 1, 5);
        c.FreeReagents    = sConfigMgr->GetBoolDefault("Companions.FreeReagents", true, true);
        c.FreeFood        = sConfigMgr->GetBoolDefault("Companions.FreeFood", true, true);
        c.AutoRevive      = sConfigMgr->GetBoolDefault("Companions.AutoRevive", true, true);
        c.AutoReviveDelay = std::max<uint32>(sConfigMgr->GetIntDefault("Companions.AutoRevive.Delay", 30, true), 5);
        c.TeleportDistance = std::max(sConfigMgr->GetFloatDefault("Companions.TeleportDistance", 90.0f, true), 30.0f);
        c.UpdateInterval  = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.UpdateInterval", 100, true), 50, 1000);
        c.RestoreOnLogin  = sConfigMgr->GetBoolDefault("Companions.RestoreOnLogin", true, true);
        c.MountLevel      = sConfigMgr->GetIntDefault("Companions.MountLevel", 40, true);
        c.AnnounceInParty = sConfigMgr->GetBoolDefault("Companions.AnnounceInParty", true, true);
        c.FollowMinDistance = std::clamp(sConfigMgr->GetFloatDefault("Companions.Follow.MinDistance", 2.0f, true), 1.0f, 15.0f);
        c.FollowMaxDistance = std::clamp(sConfigMgr->GetFloatDefault("Companions.Follow.MaxDistance", 9.0f, true), 2.0f, 25.0f);
        c.Callouts        = sConfigMgr->GetBoolDefault("Companions.Callouts", true, true);
        c.LLMEnable       = sConfigMgr->GetBoolDefault("Companions.LLM.Enable", false, true);
        c.LLMDefaultOn    = sConfigMgr->GetBoolDefault("Companions.LLM.DefaultOn", true, true);
        c.LLMPartyChat    = sConfigMgr->GetBoolDefault("Companions.LLM.PartyChat", true, true);
        c.LLMCallouts     = sConfigMgr->GetBoolDefault("Companions.LLM.Callouts", true, true);
        c.LLMBanter       = sConfigMgr->GetBoolDefault("Companions.LLM.Banter", false, true);
        c.LLMReactions    = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.LLM.Reactions", 35, true), 0, 100);
        c.LLMPollInterval = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.LLM.PollInterval", 1000, true), 250, 10000);
        c.LLMMaxAge       = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.LLM.MaxAge", 45, true), 10, 300);
        c.LLMTactics      = sConfigMgr->GetBoolDefault("Companions.LLM.Tactics", true, true);
        c.LLMTacticsInterval = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.LLM.TacticsInterval", 15, true), 5, 60);
        c.Raid20Enable    = sConfigMgr->GetBoolDefault("Companions.Raid20.Enable", true, true);
        c.Raid20Health    = std::clamp(sConfigMgr->GetFloatDefault("Companions.Raid20.Health", 0.5f, true), 0.1f, 1.0f);
        c.Raid20Damage    = std::clamp(sConfigMgr->GetFloatDefault("Companions.Raid20.Damage", 0.8f, true), 0.1f, 1.0f);
        c.AmbientEnable   = sConfigMgr->GetBoolDefault("Companions.Ambient.Enable", true, true);
        c.AmbientDensity  = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.Ambient.Density", 5, true), 0, 30);
        c.AmbientCityDensity = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.Ambient.CityDensity", 15, true), 0, 40);
        c.AmbientMaxTotal = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.Ambient.MaxTotal", 60, true), 0, 300);
        c.AmbientRadius   = std::clamp(sConfigMgr->GetFloatDefault("Companions.Ambient.Radius", 150.0f, true), 60.0f, 300.0f);
        c.AmbientPoolSize = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.Ambient.PoolSize", 200, true), 20, 1000);
        c.AmbientGearQuality = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.Ambient.GearQuality", 2, true), 1, 4);
        c.AmbientCompanionChance = std::clamp<uint32>(sConfigMgr->GetIntDefault("Companions.Ambient.CompanionChance", 20, true), 0, 100);

        // what the web panel saved wins over the file
        for (auto const& [key, value] : _stored)
            if (Setting const* setting = FindSetting(key))
            {
                std::string error;
                Apply(*setting, c, value, error);
            }
        Normalize(c);
        _config = c;

        TC_LOG_INFO("server.worldserver", "Companions: {} (max {} per player, {} on the server, groups up to {}, brain {})",
            c.Enable ? "enabled" : "disabled", c.MaxPerPlayer, c.MaxTotal, c.RaidSize, c.LLMEnable ? "on" : "off");
    }

    void LoadStoredSettings()
    {
        CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS `companion_settings` (`key` VARCHAR(48) NOT NULL, `value` VARCHAR(64) NOT NULL, "
            "PRIMARY KEY (`key`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Companions: settings changed in the web panel'");
        _stored.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT `key`, `value` FROM companion_settings"))
            do
                _stored[result->Fetch()[0].GetString()] = result->Fetch()[1].GetString();
            while (result->NextRow());
        LoadConfig();
    }

    std::vector<SettingInfo> ListSettings()
    {
        std::vector<SettingInfo> list;
        for (Setting const& setting : Settings())
            list.push_back({ setting.Key, ValueOf(setting, _config), setting.Type, setting.Min, setting.Max });
        return list;
    }

    bool ChangeSetting(std::string const& key, std::string const& value, std::string& error)
    {
        Setting const* setting = FindSetting(key);
        if (!setting)
        {
            error = "unknown setting";
            return false;
        }
        Config changed = _config;
        if (!Apply(*setting, changed, value, error))
            return false;
        Normalize(changed);
        _config = changed;

        std::string stored = ValueOf(*setting, _config);
        _stored[key] = stored;
        std::string escapedKey = key;
        CharacterDatabase.EscapeString(escapedKey);
        CharacterDatabase.PExecute("REPLACE INTO companion_settings (`key`, `value`) VALUES ('{}', '{}')", escapedKey, stored);
        TC_LOG_INFO("server.worldserver", "Companions: setting {} = {}", key, stored);
        return true;
    }

    bool ResetSetting(std::string const& key)
    {
        if (!FindSetting(key) || !_stored.erase(key))
            return false;
        std::string escapedKey = key;
        CharacterDatabase.EscapeString(escapedKey);
        CharacterDatabase.PExecute("DELETE FROM companion_settings WHERE `key` = '{}'", escapedKey);
        LoadConfig();
        return true;
    }

    bool IsStored(std::string const& key)
    {
        return _stored.count(key) != 0;
    }

    Role DpsRoleForClass(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_HUNTER:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
            case CLASS_PRIEST:
                return Role::RangedDps;
            default:
                return Role::MeleeDps;
        }
    }

    bool ParseRole(std::string_view text, Role& role, uint8 classId)
    {
        std::string t = ToLower(text);
        if (t == "tank")
            role = Role::Tank;
        else if (t == "heal" || t == "healer" || t == "heals")
            role = Role::Healer;
        else if (t == "dps" || t == "damage")
            role = DpsRoleForClass(classId);
        else if (t == "melee")
            role = Role::MeleeDps;
        else if (t == "ranged" || t == "caster")
            role = Role::RangedDps;
        else if (t == "auto")
            role = Role::Auto;
        else
            return false;
        return true;
    }

    std::string ToLower(std::string_view text)
    {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return out;
    }

    std::string Utf8Prefix(std::string_view text, size_t maxBytes)
    {
        if (text.size() <= maxBytes)
            return std::string(text);
        size_t end = maxBytes;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            --end;      // text[end] continues a character that starts before it: cut before that start
        return std::string(text.substr(0, end));
    }

    bool ContainsWord(std::string_view text, std::string_view word)
    {
        if (word.empty())
            return false;
        auto letter = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '\''; };
        for (size_t pos = text.find(word); pos != std::string_view::npos; pos = text.find(word, pos + 1))
        {
            size_t const end = pos + word.size();
            if ((pos == 0 || !letter(text[pos - 1])) && (end >= text.size() || !letter(text[end])))
                return true;
        }
        return false;
    }
}
