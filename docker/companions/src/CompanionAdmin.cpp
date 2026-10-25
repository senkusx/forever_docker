/*
 * Companions: ".bot admin" commands for the web panel (console / SOAP, administrators only).
 * Every answer line starts with "COMP|" so the website can parse it; values never contain '|'.
 */

#include "CompanionAdmin.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CompanionAI.h"
#include "CompanionCommands.h"
#include "CompanionConfig.h"
#include "CompanionFactory.h"
#include "CompanionMgr.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "RealmList.h"
#include "Realm.h"
#include "WorldSession.h"
#include <sstream>

using namespace Trinity::ChatCommands;

namespace Companions
{
    namespace
    {
        constexpr rbac::RBACPermissions PERM_ADMIN = rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN;

        ObjectGuid PlayerGuid(uint64 low) { return ObjectGuid::Create<HighGuid::Player>(low); }

        std::string Clean(std::string text)
        {
            for (char& c : text)
                if (c == '|' || c == '\n' || c == '\r')
                    c = ' ';
            return text;
        }

        void Error(ChatHandler* handler, std::string const& message)
        {
            handler->SendSysMessage("COMP|ERROR|" + Clean(message));
        }

        void Ok(ChatHandler* handler, std::string const& detail = "")
        {
            handler->SendSysMessage("COMP|OK|" + Clean(detail));
        }

        char const* AreaName(uint32 areaId)
        {
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
                if (char const* name = area->AreaName[LOCALE_enUS])
                    return name;
            return "";
        }

        // a bot that belongs to this owner (any known companion record)
        BotRecord* OwnedRecord(ChatHandler* handler, uint64 owner, uint64 bot)
        {
            BotRecord* record = sCompanionMgr->GetRecord(PlayerGuid(bot));
            if (!record || (owner && record->Owner != PlayerGuid(owner)))
            {
                Error(handler, "unknown companion");
                return nullptr;
            }
            return record;
        }
    }

    // .bot admin info
    static bool HandleInfo(ChatHandler* handler)
    {
        Config const& c = GetConfig();
        std::ostringstream ss;
        ss << "COMP|INFO|" << c.Enable << "|" << c.LLMEnable << "|" << c.MaxPerPlayer << "|" << c.GearMaxQuality << "|" << uint32(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
           << "|" << c.MaxTotal << "|" << c.RaidSize << "|" << sCompanionMgr->CountAll();
        handler->SendSysMessage(ss.str());
        return true;
    }

    // .bot admin live <owner>: owner state, then one line per online bot
    static bool HandleLive(ChatHandler* handler, uint64 owner)
    {
        ObjectGuid ownerGuid = PlayerGuid(owner);
        Player* player = ObjectAccessor::FindPlayer(ownerGuid);
        std::ostringstream o;
        o << "COMP|OWNER|" << owner << "|" << (player ? 1 : 0) << "|" << (player ? uint32(player->GetLevel()) : 0) << "|" << (player ? AreaName(player->GetZoneId()) : "")
          << "|" << sCompanionMgr->CountBotsOf(ownerGuid);
        handler->SendSysMessage(o.str());

        for (Player* bot : sCompanionMgr->GetBotsOf(ownerGuid))
        {
            CompanionAI* ai = sCompanionMgr->GetAI(bot);
            if (!ai)
                continue;
            char const* state = !bot->IsAlive() ? "dead" : (bot->IsInCombat() ? "combat" : "idle");
            uint32 itemLevel = 0, items = 0;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    itemLevel += item->GetItemLevel(bot);
                    ++items;
                }
            std::ostringstream b;
            b << "COMP|LIVE|" << bot->GetGUID().GetCounter() << "|" << uint32(bot->GetHealthPct()) << "|"
              << (bot->GetMaxPower(POWER_MANA) > 0 ? int32(bot->GetPowerPct(POWER_MANA)) : -1) << "|" << state << "|"
              << uint32(ai->GetEffectiveRole()) << "|" << uint32(ai->GetAggression()) << "|" << uint32(ai->GetStance()) << "|"
              << (items ? itemLevel / items : 0) << "|" << ai->Spells().Size() << "|" << AreaName(bot->GetZoneId()) << "|"
              << uint32(bot->GetLevel()) << "|" << Clean(sCompanionFactory->DescribeTalents(bot));
            handler->SendSysMessage(b.str());
        }
        for (BotRecord const* record : sCompanionMgr->GetRecordsOf(ownerGuid))
            if (sCompanionMgr->IsLoggingIn(record->Guid))
                handler->SendSysMessage("COMP|LOADING|" + std::to_string(record->Guid.GetCounter()));
        return true;
    }

    // .bot admin create <owner> <class> <race> <gender> <role> <quality> <llm> <aggression> <spec|-> <template> <looks|-> <name|->
    static bool HandleCreate(ChatHandler* handler, uint64 owner, uint8 classId, uint8 race, uint8 gender, std::string roleText, uint8 quality,
        uint8 llm, uint8 aggression, std::string spec, uint32 templateId, std::string looksText, std::string name)
    {
        if (!GetConfig().Enable)
        {
            Error(handler, "companions are disabled");
            return true;
        }
        ObjectGuid ownerGuid = PlayerGuid(owner);
        CharacterCacheEntry const* ownerCache = sCharacterCache->GetCharacterCacheByGuid(ownerGuid);
        if (!ownerCache)
        {
            Error(handler, "unknown owner");
            return true;
        }

        Role role = Role::Auto;
        if (!ParseRole(roleText, role, classId))
        {
            Error(handler, "bad role");
            return true;
        }
        if (spec == "-")
            spec.clear();
        for (char c : spec)
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != ',')
            {
                Error(handler, "bad spec");
                return true;
            }

        std::vector<std::pair<uint32, uint32>> looks;
        if (looksText != "-")
        {
            std::istringstream ss(looksText);
            std::string pair;
            while (std::getline(ss, pair, ','))
            {
                size_t colon = pair.find(':');
                if (colon == std::string::npos)
                    continue;
                looks.emplace_back(uint32(std::strtoul(pair.substr(0, colon).c_str(), nullptr, 10)),
                    uint32(std::strtoul(pair.substr(colon + 1).c_str(), nullptr, 10)));
            }
        }
        if (name == "-")
            name = sCompanionFactory->RandomFreeName();   // chosen now so the panel can show it right away

        Player* ownerPlayer = ObjectAccessor::FindPlayer(ownerGuid);
        uint32 build = ownerPlayer ? ownerPlayer->GetSession()->GetClientBuild() : 0;
        if (!build)
            if (std::shared_ptr<Realm const> realm = sRealmList->GetCurrentRealm())
                build = realm->Build;

        std::string error;
        ObjectGuid guid = sCompanionFactory->CreateCharacter(Player::TeamForRace(ownerCache->Race), build, classId, race, gender, name, looks, error,
            [ownerGuid](ObjectGuid botGuid, bool saved)
            {
                // joins right away when the owner is in the game
                if (!saved)
                    return;
                if (Player* currentOwner = ObjectAccessor::FindPlayer(ownerGuid))
                {
                    std::string summonError;
                    sCompanionMgr->Summon(currentOwner, botGuid, summonError);
                }
            });
        if (guid.IsEmpty())
        {
            Error(handler, error);
            return true;
        }

        BotRecord record;
        record.Guid = guid;
        record.Owner = ownerGuid;
        record.BotRole = role;
        record.Created = true;
        record.UseLLM = llm != 0;
        record.Spec = spec;
        record.GearQuality = std::min<uint8>(quality, ITEM_QUALITY_LEGENDARY);
        record.AggressionMode = Aggression(std::min<uint8>(aggression, uint8(Aggression::Aggressive)));
        record.TemplateId = templateId;
        sCompanionMgr->AddRecord(record);

        std::string botName;
        sCharacterCache->GetCharacterNameByGuid(guid, botName);
        if (botName.empty())
            botName = name;
        handler->SendSysMessage("COMP|CREATED|" + std::to_string(guid.GetCounter()) + "|" + botName);
        return true;
    }

    static bool HandleSummon(ChatHandler* handler, uint64 owner, uint64 bot)
    {
        if (!OwnedRecord(handler, owner, bot))
            return true;
        Player* ownerPlayer = ObjectAccessor::FindPlayer(PlayerGuid(owner));
        if (!ownerPlayer)
        {
            Error(handler, "the owner is not in the game");
            return true;
        }
        std::string error;
        if (sCompanionMgr->Summon(ownerPlayer, PlayerGuid(bot), error))
            Ok(handler);
        else
            Error(handler, error);
        return true;
    }

    static bool HandleDismiss(ChatHandler* handler, uint64 owner, uint64 bot)
    {
        if (!OwnedRecord(handler, owner, bot))
            return true;
        sCompanionMgr->Dismiss(PlayerGuid(bot), false);
        Ok(handler);
        return true;
    }

    // .bot admin set <owner> <bot> <role|llm|quality|spec|aggression|stance|personality|name> <value>
    static bool HandleSet(ChatHandler* handler, uint64 owner, uint64 bot, std::string key, Tail value)
    {
        BotRecord* record = OwnedRecord(handler, owner, bot);
        if (!record)
            return true;
        ObjectGuid guid = PlayerGuid(bot);
        CompanionAI* ai = sCompanionMgr->GetAI(guid);
        std::string text = Clean(std::string(value));
        CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);

        if (key == "role")
        {
            Role role;
            if (!cache || !ParseRole(text, role, cache->Class))
            {
                Error(handler, "bad role");
                return true;
            }
            record->BotRole = role;
            if (ai)
            {
                ai->SetRole(role);
                sCompanionMgr->QueuePrepare(guid, 0, true, true);
            }
        }
        else if (key == "llm")
            record->UseLLM = text == "1" || text == "on";
        else if (key == "quality")
        {
            record->GearQuality = uint8(std::min<unsigned long>(std::strtoul(text.c_str(), nullptr, 10), ITEM_QUALITY_LEGENDARY));
            if (ai)
                sCompanionMgr->QueuePrepare(guid, 0, true);
        }
        else if (key == "spec")
        {
            if (text == "-")
                text.clear();
            for (char c : text)
                if (!std::isdigit(static_cast<unsigned char>(c)) && c != ',')
                {
                    Error(handler, "bad spec");
                    return true;
                }
            record->Spec = text;
            if (ai)
                sCompanionMgr->QueuePrepare(guid, 0, false, true);
        }
        else if (key == "aggression")
        {
            record->AggressionMode = Aggression(std::min<unsigned long>(std::strtoul(text.c_str(), nullptr, 10), uint32(Aggression::Aggressive)));
            if (ai)
                ai->SetAggression(record->AggressionMode);
        }
        else if (key == "stance")
        {
            record->StanceMode = text == "1" ? Stance::Stay : Stance::Follow;
            if (ai)
                ai->SetStance(record->StanceMode);
        }
        else if (key == "personality")
            record->Personality = text.substr(0, 250);
        else if (key == "name")
        {
            if (ai || sCompanionMgr->IsLoggingIn(guid))
            {
                Error(handler, "dismiss the companion before renaming it");
                return true;
            }
            if (!record->Created)
            {
                Error(handler, "only companions made for you can be renamed here");
                return true;
            }
            std::string name = text;
            if (!normalizePlayerName(name) || ObjectMgr::CheckPlayerName(name, LOCALE_enUS, true) != CHAR_NAME_SUCCESS || sObjectMgr->IsReservedName(name))
            {
                Error(handler, "that name is not allowed");
                return true;
            }
            if (sCharacterCache->GetCharacterCacheByName(name))
            {
                Error(handler, "that name is taken");
                return true;
            }
            std::string escaped = name;
            CharacterDatabase.EscapeString(escaped);
            CharacterDatabase.PExecute("UPDATE characters SET name = '{}' WHERE guid = {}", escaped, bot);
            sCharacterCache->UpdateCharacterData(guid, name);
        }
        else if (key == "template")
            record->TemplateId = uint32(std::strtoul(text.c_str(), nullptr, 10));
        else if (key == "provider")
            record->LLMProvider = uint32(std::strtoul(text.c_str(), nullptr, 10));
        else
        {
            Error(handler, "unknown setting");
            return true;
        }
        sCompanionMgr->SaveRecord(*record);
        Ok(handler);
        return true;
    }

    // .bot admin order <owner> <bot|0 = all> <order>
    static bool HandleOrder(ChatHandler* handler, uint64 owner, uint64 bot, Tail text)
    {
        Player* ownerPlayer = ObjectAccessor::FindPlayer(PlayerGuid(owner));
        if (!ownerPlayer)
        {
            Error(handler, "the owner is not in the game");
            return true;
        }
        std::vector<Player*> bots = sCompanionMgr->GetBotsOf(ownerPlayer->GetGUID());
        if (bot)
            std::erase_if(bots, [bot](Player* p) { return p->GetGUID().GetCounter() != bot; });
        if (bots.empty())
        {
            Error(handler, "no companion online");
            return true;
        }
        if (Commands::ExecuteOrder(ownerPlayer, bots, std::string(text), false))
            Ok(handler);
        else
            Error(handler, "unknown order");
        return true;
    }

    static bool HandleRegear(ChatHandler* handler, uint64 owner, uint64 bot)
    {
        if (!OwnedRecord(handler, owner, bot))
            return true;
        if (!sCompanionMgr->GetAI(PlayerGuid(bot)))
        {
            Error(handler, "summon the companion first");
            return true;
        }
        sCompanionMgr->QueuePrepare(PlayerGuid(bot), 0, true);
        Ok(handler);
        return true;
    }

    static bool HandleRespec(ChatHandler* handler, uint64 owner, uint64 bot)
    {
        if (!OwnedRecord(handler, owner, bot))
            return true;
        if (!sCompanionMgr->GetAI(PlayerGuid(bot)))
        {
            Error(handler, "summon the companion first");
            return true;
        }
        sCompanionMgr->QueuePrepare(PlayerGuid(bot), 0, false, true);
        Ok(handler);
        return true;
    }

    static bool HandleDelete(ChatHandler* handler, uint64 owner, uint64 bot)
    {
        std::string error;
        if (sCompanionMgr->DeleteCompanion(PlayerGuid(owner), PlayerGuid(bot), error))
            Ok(handler);
        else
            Error(handler, error);
        return true;
    }

    // .bot admin looks <race> <gender>: customization options and the choices that need no unlock
    static bool HandleLooks(ChatHandler* handler, uint8 race, uint8 gender)
    {
        std::vector<ChrCustomizationOptionEntry const*> const* options = sDB2Manager.GetCustomiztionOptions(race, gender);
        if (!options)
        {
            Error(handler, "no customizations for that race and gender");
            return true;
        }
        std::vector<ChrCustomizationOptionEntry const*> sorted(options->begin(), options->end());
        std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->SortIndex != b->SortIndex ? a->SortIndex < b->SortIndex : a->ID < b->ID; });
        for (ChrCustomizationOptionEntry const* option : sorted)
        {
            std::vector<ChrCustomizationChoiceEntry const*> const* choices = sDB2Manager.GetCustomiztionChoices(option->ID);
            if (!choices || choices->empty())
                continue;
            char const* optionName = option->Name[LOCALE_enUS];
            handler->SendSysMessage("COMP|OPT|" + std::to_string(option->ID) + "|" + Clean(optionName ? optionName : ""));
            uint32 index = 0;
            for (ChrCustomizationChoiceEntry const* choice : *choices)
            {
                if (!CompanionFactory::IsChoiceAvailable(choice, race))
                    continue;
                ++index;
                char const* choiceName = choice->Name[LOCALE_enUS];
                handler->SendSysMessage("COMP|CHOICE|" + std::to_string(option->ID) + "|" + std::to_string(choice->ID) + "|"
                    + Clean(choiceName && *choiceName ? choiceName : std::to_string(index)));
            }
        }
        return true;
    }

    static bool HandleRandomName(ChatHandler* handler)
    {
        std::string name = sCompanionFactory->RandomFreeName();
        if (name.empty())
            Error(handler, "no free name found");
        else
            handler->SendSysMessage("COMP|NAME|" + name);
        return true;
    }

    // .bot admin tabs <class>: the talent tabs (index, talents, first talents)
    static bool HandleTabs(ChatHandler* handler, uint8 classId)
    {
        std::vector<std::pair<uint32, std::string>> tabs = sCompanionFactory->DescribeTabs(classId);
        for (uint32 i = 0; i < tabs.size(); ++i)
            handler->SendSysMessage("COMP|TAB|" + std::to_string(i) + "|" + std::to_string(tabs[i].first) + "|" + Clean(tabs[i].second));
        return true;
    }

    // .bot admin races <owner>: classic races of the owner's faction for each class
    static bool HandleRaces(ChatHandler* handler, uint64 owner)
    {
        CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(PlayerGuid(owner));
        if (!cache)
        {
            Error(handler, "unknown owner");
            return true;
        }
        Team team = Player::TeamForRace(cache->Race);
        for (uint8 classId = CLASS_WARRIOR; classId <= CLASS_DRUID; ++classId)
        {
            std::vector<uint8> races = sCompanionFactory->RacesFor(team, classId);
            if (races.empty())
                continue;
            std::string list;
            for (uint8 race : races)
                list += (list.empty() ? "" : ",") + std::to_string(race);
            handler->SendSysMessage("COMP|RACES|" + std::to_string(classId) + "|" + list);
        }
        return true;
    }

    // .bot admin builds: COMP|BUILD|class|tree|a/b/c|role, the meta build of every talent tree
    static bool HandleBuilds(ChatHandler* handler)
    {
        for (uint8 classId : { CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID })
            for (uint32 tree = 0; tree < 3; ++tree)
                if (CompanionFactory::MetaBuild const* meta = CompanionFactory::MetaBuildFor(classId, tree))
                {
                    std::ostringstream ss;
                    ss << "COMP|BUILD|" << uint32(classId) << "|" << tree << "|" << meta->Points[0] << "/" << meta->Points[1] << "/" << meta->Points[2]
                       << "|" << uint32(meta->ForRole);
                    handler->SendSysMessage(ss.str());
                }
        return true;
    }

    // .bot admin items <id,id,...>: COMP|ITEMS|<the ids this server knows> (data imports check against it)
    static bool HandleItems(ChatHandler* handler, std::string list)
    {
        std::string known;
        std::istringstream ids(list);
        std::string id;
        while (std::getline(ids, id, ','))
            if (!id.empty() && std::isdigit(static_cast<unsigned char>(id[0])) && sObjectMgr->GetItemTemplate(uint32(std::stoul(id))))
                known += (known.empty() ? "" : ",") + id;
        handler->SendSysMessage("COMP|ITEMS|" + known);
        return true;
    }

    // .bot admin config list: COMP|SET|key|value|type|min|max|stored
    static bool HandleConfigList(ChatHandler* handler)
    {
        for (SettingInfo const& setting : ListSettings())
        {
            std::ostringstream ss;
            ss << "COMP|SET|" << setting.Key << "|" << setting.Value << "|" << setting.Type << "|" << setting.Min << "|" << setting.Max << "|"
               << (IsStored(setting.Key) ? 1 : 0);
            handler->SendSysMessage(ss.str());
        }
        return true;
    }

    // .bot admin config set <key> <value>
    static bool HandleConfigSet(ChatHandler* handler, std::string key, std::string value)
    {
        std::string error;
        if (!ChangeSetting(key, value, error))
            Error(handler, error);
        else
            Ok(handler);
        return true;
    }

    // .bot admin config reset <key>: back to the worldserver.conf value
    static bool HandleConfigReset(ChatHandler* handler, std::string key)
    {
        ResetSetting(key);
        Ok(handler);
        return true;
    }

    std::span<ChatCommandBuilder const> AdminCommandTable()
    {
        static ChatCommandTable configTable =
        {
            { "list",  HandleConfigList,  PERM_ADMIN, Console::Yes },
            { "set",   HandleConfigSet,   PERM_ADMIN, Console::Yes },
            { "reset", HandleConfigReset, PERM_ADMIN, Console::Yes },
        };
        static ChatCommandTable table =
        {
            { "info",       HandleInfo,       PERM_ADMIN, Console::Yes },
            { "live",       HandleLive,       PERM_ADMIN, Console::Yes },
            { "create",     HandleCreate,     PERM_ADMIN, Console::Yes },
            { "summon",     HandleSummon,     PERM_ADMIN, Console::Yes },
            { "dismiss",    HandleDismiss,    PERM_ADMIN, Console::Yes },
            { "set",        HandleSet,        PERM_ADMIN, Console::Yes },
            { "order",      HandleOrder,      PERM_ADMIN, Console::Yes },
            { "regear",     HandleRegear,     PERM_ADMIN, Console::Yes },
            { "respec",     HandleRespec,     PERM_ADMIN, Console::Yes },
            { "delete",     HandleDelete,     PERM_ADMIN, Console::Yes },
            { "looks",      HandleLooks,      PERM_ADMIN, Console::Yes },
            { "randomname", HandleRandomName, PERM_ADMIN, Console::Yes },
            { "tabs",       HandleTabs,       PERM_ADMIN, Console::Yes },
            { "races",      HandleRaces,      PERM_ADMIN, Console::Yes },
            { "config",     configTable },
            { "builds",     HandleBuilds,     PERM_ADMIN, Console::Yes },
            { "items",      HandleItems,      PERM_ADMIN, Console::Yes },
        };
        return table;
    }
}
