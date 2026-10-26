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
#include "CompanionMechanics.h"
#include "CompanionMgr.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Bag.h"
#include "CreatureData.h"
#include "Item.h"
#include "Loot.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "RealmList.h"
#include "Realm.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
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

    // .bot admin create <owner> <class> <race> <gender> <role> <quality> <llm> <aggression> <spec|-> <template> <looks|-> <name|-> [surname]
    static bool HandleCreate(ChatHandler* handler, uint64 owner, uint8 classId, uint8 race, uint8 gender, std::string roleText, uint8 quality,
        uint8 llm, uint8 aggression, std::string spec, uint32 templateId, std::string looksText, std::string name, Tail surnameText)
    {
        std::string surname = Clean(std::string(surnameText));
        if (!CompanionFactory::ValidSurname(surname))
        {
            Error(handler, "bad surname");
            return true;
        }
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
        // race and gender now, so a random name is one of that people (and the panel can show it right away)
        if (!race)
        {
            std::vector<uint8> races = sCompanionFactory->RacesFor(Player::TeamForRace(ownerCache->Race), classId);
            if (!races.empty())
                race = races[urand(0, races.size() - 1)];
        }
        if (gender > GENDER_FEMALE)
            gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
        if (name == "-")
        {
            name = sCompanionFactory->RandomFreeName(race, gender);
            if (surname.empty())
                surname = CompanionFactory::RandomSurname(race);
        }

        Player* ownerPlayer = ObjectAccessor::FindPlayer(ownerGuid);
        uint32 build = ownerPlayer ? ownerPlayer->GetSession()->GetClientBuild() : 0;
        if (!build)
            if (std::shared_ptr<Realm const> realm = sRealmList->GetCurrentRealm())
                build = realm->Build;

        std::string error;
        ObjectGuid guid = sCompanionFactory->CreateCharacter(Player::TeamForRace(ownerCache->Race), build, classId, race, gender, name, looks, error,
            [ownerGuid, surname](ObjectGuid botGuid, bool saved)
            {
                // joins right away when the owner is in the game
                if (!saved)
                    return;
                if (!surname.empty())
                    CompanionFactory::SetSurname(botGuid, surname);
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
        else if (key == "surname")
        {
            std::string surname = text == "-" ? std::string() : text;
            if (!CompanionFactory::ValidSurname(surname))
            {
                Error(handler, "bad surname");
                return true;
            }
            CompanionFactory::SetSurname(guid, surname);
        }
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
        else if (key == "loot")
        {
            record->LootMode = uint8(std::min<unsigned long>(std::strtoul(text.c_str(), nullptr, 10), 2));
            if (ai)
                ai->SetLootMode(record->LootMode);
        }
        else if (key == "potions")
        {
            record->UsePotions = text == "1" || text == "on";
            if (ai)
                ai->SetUsePotions(record->UsePotions);
        }
        else if (key == "autocc")
        {
            record->AutoCC = text == "1" || text == "on";
            if (ai)
                ai->SetAutoCC(record->AutoCC);
        }
        else if (key == "prefer" || key == "avoid" || key == "allow")
        {
            // "prefer frost nova", "avoid arcane explosion", "allow rend", "allow *" (all of them)
            if (!ai)
            {
                Error(handler, "summon the companion first");
                return true;
            }
            if (key == "allow" && text == "*")
                ai->ClearPrefs();
            else if (key == "allow")
                ai->ClearPref(ToLower(text));
            else if (!ai->SetPref(ToLower(text), key == "avoid", 0, "set in the web panel"))
            {
                Error(handler, "it does not know that spell");
                return true;
            }
            record->Prefs = ai->SavePrefs();
        }
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

    // .bot admin randomname [race] [gender]: COMP|NAME|first|surname
    static bool HandleRandomName(ChatHandler* handler, Optional<uint8> race, Optional<uint8> gender)
    {
        uint8 r = race.value_or(0);
        if (!r)
        {
            static uint8 const classicRaces[] = { RACE_HUMAN, RACE_DWARF, RACE_NIGHTELF, RACE_GNOME, RACE_ORC, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_TROLL };
            r = classicRaces[urand(0, std::size(classicRaces) - 1)];
        }
        std::string name = sCompanionFactory->RandomFreeName(r, gender.value_or(2));
        if (name.empty())
            Error(handler, "no free name found");
        else
            handler->SendSysMessage("COMP|NAME|" + name + "|" + CompanionFactory::RandomSurname(r));
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

    // .bot admin loot <creature entry> [kills]: what the creature's loot list gives over that many kills (data imports
    // check against it). COMP|LOOT|entry|lootid|kills, then COMP|DROP|item|name|quality|quest|times per item
    static bool HandleLoot(ChatHandler* handler, uint32 entry, Optional<uint32> kills)
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        CreatureDifficulty const* difficulty = creature ? creature->GetDifficulty(DIFFICULTY_NONE) : nullptr;
        LootTemplate const* loot = difficulty ? LootTemplates_Creature.GetLootFor(difficulty->LootID) : nullptr;
        if (!loot)
        {
            handler->SendSysMessage("COMP|ERROR|no loot list");
            return true;
        }
        uint32 rolls = std::clamp<uint32>(kills.value_or(100), 1, 10000);
        std::map<uint32, std::pair<bool, uint32>> drops;     // item -> quest item, times
        for (uint32 i = 0; i < rolls; ++i)
        {
            Loot corpse(nullptr, ObjectGuid::Empty, LOOT_CORPSE, nullptr);
            loot->Process(corpse, true, LOOT_MODE_DEFAULT, 0);
            for (LootItem const& item : corpse.items)
            {
                auto& drop = drops[item.itemid];
                drop.first = item.needs_quest;
                ++drop.second;
            }
        }
        handler->SendSysMessage(Trinity::StringFormat("COMP|LOOT|{}|{}|{}", entry, difficulty->LootID, rolls));
        for (auto const& [item, drop] : drops)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item))
                handler->SendSysMessage(Trinity::StringFormat("COMP|DROP|{}|{}|{}|{}|{}", item, Clean(proto->GetName(LOCALE_enUS)),
                    proto->GetQuality(), drop.first ? 1 : 0, drop.second));
        return true;
    }

    // .bot admin inspect <owner> <bot>: everything about one companion for the web panel
    //   COMP|INSPECT|online|level|class|race|role|health|maxhealth|power|maxpower|money|loot|potions|autocc
    //   COMP|STAT|key|value   COMP|GEAR|slot|item|entry|name|quality|itemlevel
    //   COMP|BAG|bag|slot|item|entry|name|quality|count|equippable|upgrade|bound
    //   COMP|TALENT|tab|name|rank|max   COMP|SPELL|name   COMP|PREF|kind|spell|below|reason
    static bool HandleInspect(ChatHandler* handler, uint64 owner, uint64 bot)
    {
        BotRecord* record = OwnedRecord(handler, owner, bot);
        if (!record)
            return true;
        ObjectGuid guid = PlayerGuid(bot);
        CompanionAI* ai = sCompanionMgr->GetAI(guid);
        Player* player = ai ? ai->GetBot() : nullptr;
        auto itemLine = [](char const* kind, std::string const& where, ObjectGuid::LowType low, ItemTemplate const* proto, uint32 count,
            std::string const& extra) -> std::string
        {
            std::ostringstream ss;
            ss << "COMP|" << kind << "|" << where << "|" << low << "|" << proto->GetId() << "|" << Clean(proto->GetName(LOCALE_enUS)) << "|"
               << proto->GetQuality() << "|" << proto->GetBaseItemLevel() << "|" << count << extra;
            return ss.str();
        };

        if (!player)
        {
            // not in the game: what its bags hold, from the database
            CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);
            std::ostringstream head;
            head << "COMP|INSPECT|0|" << (cache ? uint32(cache->Level) : 0) << "|" << (cache ? uint32(cache->Class) : 0) << "|"
                 << (cache ? uint32(cache->Race) : 0) << "|" << uint32(record->BotRole) << "|0|0|0|0|0|" << uint32(record->LootMode) << "|"
                 << uint32(record->UsePotions) << "|" << uint32(record->AutoCC);
            handler->SendSysMessage(head.str());
            std::unordered_map<ObjectGuid::LowType, uint32> bagSlots;
            if (QueryResult result = CharacterDatabase.PQuery("SELECT ci.bag, ci.slot, ci.item, ii.itemEntry, ii.count FROM character_inventory ci "
                "JOIN item_instance ii ON ii.guid = ci.item WHERE ci.guid = {} ORDER BY ci.bag, ci.slot", bot))
            {
                std::vector<std::array<uint64, 5>> rows;
                do
                {
                    Field* f = result->Fetch();
                    rows.push_back({ f[0].GetUInt64(), f[1].GetUInt8(), f[2].GetUInt64(), f[3].GetUInt32(), f[4].GetUInt32() });
                } while (result->NextRow());
                for (auto const& row : rows)
                    if (row[0] == 0 && row[1] >= INVENTORY_SLOT_BAG_START && row[1] < INVENTORY_SLOT_BAG_END)
                        bagSlots[row[2]] = uint32(row[1]);
                for (auto const& row : rows)
                {
                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(uint32(row[3]));
                    if (!proto)
                        continue;
                    if (row[0] == 0 && row[1] < EQUIPMENT_SLOT_END)
                        handler->SendSysMessage(itemLine("GEAR", std::to_string(row[1]), row[2], proto, 1, ""));
                    else if (row[0] == 0 && row[1] >= INVENTORY_SLOT_ITEM_START && row[1] < INVENTORY_SLOT_ITEM_END)
                        handler->SendSysMessage(itemLine("BAG", "255|" + std::to_string(row[1]), row[2], proto, uint32(row[4]), "|0|0|0"));
                    else if (row[0] != 0)
                        handler->SendSysMessage(itemLine("BAG", std::to_string(bagSlots[row[0]]) + "|" + std::to_string(row[1]), row[2], proto,
                            uint32(row[4]), "|0|0|0"));
                }
            }
        }
        else
        {
            Powers power = player->GetPowerType();
            std::ostringstream head;
            head << "COMP|INSPECT|1|" << uint32(player->GetLevel()) << "|" << uint32(player->GetClass()) << "|" << uint32(player->GetRace()) << "|"
                 << uint32(ai->GetEffectiveRole()) << "|" << player->GetHealth() << "|" << player->GetMaxHealth() << "|" << player->GetPower(power)
                 << "|" << player->GetMaxPower(power) << "|" << player->GetMoney() << "|" << uint32(record->LootMode) << "|"
                 << uint32(record->UsePotions) << "|" << uint32(record->AutoCC);
            handler->SendSysMessage(head.str());

            auto stat = [&](char const* key, float value)
            {
                std::ostringstream ss;
                ss.setf(std::ios::fixed);
                ss.precision(value == std::floor(value) ? 0 : 2);
                ss << "COMP|STAT|" << key << "|" << value;
                handler->SendSysMessage(ss.str());
            };
            stat("strength", player->GetStat(STAT_STRENGTH));
            stat("agility", player->GetStat(STAT_AGILITY));
            stat("stamina", player->GetStat(STAT_STAMINA));
            stat("intellect", player->GetStat(STAT_INTELLECT));
            stat("spirit", player->GetStat(STAT_SPIRIT));
            stat("armor", float(player->GetArmor()));
            stat("attack_power", player->GetTotalAttackPowerValue(BASE_ATTACK));
            stat("ranged_attack_power", player->GetTotalAttackPowerValue(RANGED_ATTACK));
            stat("spell_power", float(player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC)));
            stat("healing_power", float(player->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_ALL)));
            stat("melee_crit", *player->m_activePlayerData->CritPercentage);
            stat("ranged_crit", *player->m_activePlayerData->RangedCritPercentage);
            stat("dodge", *player->m_activePlayerData->DodgePercentage);
            stat("parry", *player->m_activePlayerData->ParryPercentage);
            stat("block", *player->m_activePlayerData->BlockPercentage);
            stat("fire_resistance", float(player->GetResistance(SPELL_SCHOOL_FIRE)));
            stat("nature_resistance", float(player->GetResistance(SPELL_SCHOOL_NATURE)));
            stat("frost_resistance", float(player->GetResistance(SPELL_SCHOOL_FROST)));
            stat("shadow_resistance", float(player->GetResistance(SPELL_SCHOOL_SHADOW)));
            stat("arcane_resistance", float(player->GetResistance(SPELL_SCHOOL_ARCANE)));

            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    if (ItemTemplate const* proto = item->GetTemplate())
                        handler->SendSysMessage(itemLine("GEAR", std::to_string(slot), item->GetGUID().GetCounter(), proto, 1, ""));
            auto bagItem = [&](Item* item, uint8 bag, uint8 slot)
            {
                ItemTemplate const* proto = item->GetTemplate();
                if (!proto)
                    return;
                bool equippable = proto->GetInventoryType() != INVTYPE_NON_EQUIP && player->CanUseItem(proto) == EQUIP_ERR_OK;
                bool upgrade = equippable && sCompanionFactory->IsUpgrade(player, proto, ai->GetEffectiveRole());
                handler->SendSysMessage(itemLine("BAG", std::to_string(bag) + "|" + std::to_string(slot), item->GetGUID().GetCounter(), proto,
                    item->GetCount(), "|" + std::to_string(equippable) + "|" + std::to_string(upgrade) + "|" + std::to_string(item->IsSoulBound())));
            };
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    bagItem(item, INVENTORY_SLOT_BAG_0, slot);
            for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
                if (Bag* container = player->GetBagByPos(bag))
                    for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                        if (Item* item = container->GetItemByPos(uint8(slot)))
                            bagItem(item, bag, uint8(slot));

            for (CompanionFactory::TalentLine const& talent : sCompanionFactory->ListTalents(player))
                handler->SendSysMessage("COMP|TALENT|" + std::to_string(talent.Tab) + "|" + Clean(talent.Name) + "|" + std::to_string(talent.Rank)
                    + "|" + std::to_string(talent.MaxRank));
            // the abilities a player sees in the spell book (no passives, no hidden helper spells)
            for (std::string const& name : ai->Spells().Names())
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(ai->SpellIdOf(name), DIFFICULTY_NONE);
                if (!info || info->IsPassive() || info->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY_SPELLBOOK_AURA_ICON_COMBAT_LOG) || name.rfind("activate ", 0) == 0
                    || name == "attack" || name == "attacking" || name == "closing" || name == "opening")
                    continue;
                handler->SendSysMessage("COMP|SPELL|" + Clean(name));
            }
        }

        // what it was told about its spells (from the record when it is not in the game)
        std::string prefs = ai ? ai->SavePrefs() : record->Prefs;
        std::istringstream in(prefs);
        std::string entry;
        while (std::getline(in, entry, ';'))
            if (!entry.empty())
                handler->SendSysMessage("COMP|PREF|" + entry);
        return true;
    }

    // .bot admin item <owner> <bot> <equip|unequip|destroy> <item guid|slot>
    static bool HandleItem(ChatHandler* handler, uint64 owner, uint64 bot, std::string action, uint64 what)
    {
        if (!OwnedRecord(handler, owner, bot))
            return true;
        CompanionAI* ai = sCompanionMgr->GetAI(PlayerGuid(bot));
        Player* player = ai ? ai->GetBot() : nullptr;
        if (!player || player->IsInCombat())
        {
            Error(handler, player ? "it is fighting" : "summon the companion first");
            return true;
        }
        if (action == "unequip")
        {
            Item* item = what < EQUIPMENT_SLOT_END ? player->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(what)) : nullptr;
            ItemPosCountVec dest;
            if (!item || player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
            {
                Error(handler, item ? "no room in its bags" : "nothing there");
                return true;
            }
            player->RemoveItem(INVENTORY_SLOT_BAG_0, uint8(what), true);
            player->StoreItem(dest, item, true);
        }
        else
        {
            Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(what));
            if (!item || item->IsEquipped())
            {
                Error(handler, "no such item in its bags");
                return true;
            }
            if (action == "equip")
            {
                if (!CompanionFactory::EquipFromBags(player, item))
                {
                    Error(handler, "it cannot wear that");
                    return true;
                }
            }
            else if (action == "destroy")
                player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            else
            {
                Error(handler, "unknown action");
                return true;
            }
        }
        player->SaveToDB();
        Ok(handler);
        return true;
    }

    // .bot admin team <owner> <team id>: the team comes, the others leave; COMP|TEAM|summoned|dismissed, COMP|TEAMERROR|text
    static bool HandleTeam(ChatHandler* handler, uint64 owner, uint32 teamId)
    {
        Player* player = ObjectAccessor::FindPlayer(PlayerGuid(owner));
        if (!player)
        {
            Error(handler, "the character must be in the game");
            return true;
        }
        for (CompanionMgr::Team const& team : sCompanionMgr->GetTeamsOf(player->GetGUID()))
        {
            if (team.Id != teamId)
                continue;
            uint32 summoned = 0, dismissed = 0;
            std::vector<std::string> errors;
            sCompanionMgr->SummonTeam(player, team, summoned, dismissed, errors);
            handler->SendSysMessage("COMP|TEAM|" + std::to_string(summoned) + "|" + std::to_string(dismissed));
            for (std::string const& error : errors)
                handler->SendSysMessage("COMP|TEAMERROR|" + Clean(error));
            return true;
        }
        Error(handler, "no such team");
        return true;
    }

    // .bot admin mechanics [entry]: COMP|MECH|rules|encounters, then a creature's rules and note
    static bool HandleMechanics(ChatHandler* handler, Optional<uint32> entry)
    {
        handler->SendSysMessage("COMP|MECH|" + std::to_string(Mechanics::RuleCount()) + "|" + std::to_string(Mechanics::EncounterCount()));
        if (!entry)
            return true;
        for (Mechanics::Rule const* rule : Mechanics::RulesFor(*entry))
        {
            std::ostringstream ss;
            ss << "COMP|MECHRULE|" << uint32(rule->Type) << "|" << rule->Spell << "|" << rule->Radius << "|" << rule->Value << "|" << uint32(rule->Roles);
            handler->SendSysMessage(ss.str());
        }
        if (Mechanics::Encounter const* encounter = Mechanics::EncounterFor(*entry))
            handler->SendSysMessage(std::string("COMP|MECHNOTE|") + encounter->Name + "|" + encounter->Note);
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
            { "loot",       HandleLoot,       PERM_ADMIN, Console::Yes },
            { "mechanics",  HandleMechanics,  PERM_ADMIN, Console::Yes },
            { "inspect",    HandleInspect,    PERM_ADMIN, Console::Yes },
            { "team",       HandleTeam,       PERM_ADMIN, Console::Yes },
            { "item",       HandleItem,       PERM_ADMIN, Console::Yes },
        };
        return table;
    }
}
