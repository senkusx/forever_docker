/*
 * Companions: bridge to the AI brain workers
 */

#include "CompanionLLM.h"
#include "CompanionTactics.h"
#include "AsyncCallbackProcessor.h"
#include "CellImpl.h"
#include "CompanionAI.h"
#include "CompanionCommands.h"
#include "CompanionConfig.h"
#include "CompanionMechanics.h"
#include "CompanionMgr.h"
#include "CompanionPlan.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryCallback.h"
#include "StringFormat.h"
#include "SpellInfo.h"
#include "Spell.h"
#include <deque>
#include <sstream>
#include <unordered_map>

namespace Companions::CompanionLLM
{
    namespace
    {
        QueryCallbackProcessor _callbacks;
        uint32 _pollTimer = 0;
        bool _pollInFlight = false;
        uint64 _lastActionId = 0;      // the "consumed" update is asynchronous: never take an action twice
        uint32 _msTime = 0;
        std::unordered_map<ObjectGuid, uint32> _lastEventMs;       // bot or owner -> last event (flood guard)
        std::unordered_map<std::string, uint32> _lastCalloutMs;    // "bot:kind" / "owner#kind" -> last callout
        std::unordered_map<ObjectGuid, uint32> _lastPartyCallout;  // owner -> last callout of the party

        // enemies of a fight numbered for the brain (E1, E2...), per owner, stable while the fight lasts
        std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> _enemyRefs;
        struct TacticsState { uint32 LastMs = 0; uint32 CombatSinceMs = 0; size_t Enemies = 0; };
        std::unordered_map<ObjectGuid, TacticsState> _tactics;


        char const* CreatureTypeName(uint32 type)
        {
            switch (type)
            {
                case CREATURE_TYPE_BEAST:      return "beast";
                case CREATURE_TYPE_DRAGONKIN:  return "dragonkin";
                case CREATURE_TYPE_DEMON:      return "demon";
                case CREATURE_TYPE_ELEMENTAL:  return "elemental";
                case CREATURE_TYPE_GIANT:      return "giant";
                case CREATURE_TYPE_UNDEAD:     return "undead";
                case CREATURE_TYPE_HUMANOID:   return "humanoid";
                case CREATURE_TYPE_MECHANICAL: return "mechanical";
                default:                       return "creature";
            }
        }

        // what a bot can do for tactics, from the spells it knows
        std::string Abilities(Player* bot)
        {
            static char const* const useful[] = { "polymorph", "shackle undead", "banish", "hibernate", "scare beast", "gouge",
                "kick", "pummel", "shield bash", "counterspell", "earth shock", "hammer of justice", "bash", "taunt", "growl",
                "challenging shout", "challenging roar" };
            CompanionAI* ai = sCompanionMgr->GetAI(bot);
            std::string out;
            for (char const* spell : useful)
                if (ai && ai->Knows(spell))
                    out += (out.empty() ? "" : ", ") + std::string(spell);
            return out;
        }

        struct ChatLine { uint32 At; std::string Speaker; std::string Text; };
        std::unordered_map<ObjectGuid, std::deque<ChatLine>> _history;   // group -> recent party chat

        // answers waiting for their turn (bots do not all speak in the same millisecond)
        struct Scheduled { uint32 Due; ObjectGuid Bot; std::string Channel; std::string Target; std::string Say; std::string Commands; };
        std::vector<Scheduled> _scheduled;

        char const* ClassName(uint8 classId)
        {
            switch (classId)
            {
                case CLASS_WARRIOR: return "Warrior";
                case CLASS_PALADIN: return "Paladin";
                case CLASS_HUNTER:  return "Hunter";
                case CLASS_ROGUE:   return "Rogue";
                case CLASS_PRIEST:  return "Priest";
                case CLASS_SHAMAN:  return "Shaman";
                case CLASS_MAGE:    return "Mage";
                case CLASS_WARLOCK: return "Warlock";
                case CLASS_DRUID:   return "Druid";
                default:            return "Adventurer";
            }
        }

        char const* RaceName(uint8 race)
        {
            if (ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race))
                if (char const* name = entry->Name[LOCALE_enUS])
                    return name;
            return "unknown";
        }

        std::string AreaName(uint32 areaId)
        {
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
                if (char const* name = area->AreaName[LOCALE_enUS])
                    return name;
            return "somewhere";
        }

        std::string Escape(std::string text)
        {
            CharacterDatabase.EscapeString(text);
            return text;
        }

        char const* IconName(uint8 icon)
        {
            static char const* const names[] = { "star", "circle", "diamond", "triangle", "moon", "square", "cross", "skull" };
            return icon < 8 ? names[icon] : "?";
        }

        BotRecord const* RecordOf(Player const* bot) { return sCompanionMgr->GetRecord(bot->GetGUID()); }

        uint32 ProviderOf(Player const* bot)
        {
            BotRecord const* record = RecordOf(bot);
            return record ? record->LLMProvider : 0;
        }

        std::string MemberLine(Player* member, Player* owner)
        {
            std::ostringstream ss;
            ss << member->GetName() << " (" << uint32(member->GetLevel()) << " " << ClassName(member->GetClass());
            if (CompanionAI* ai = sCompanionMgr->GetAI(member))
                ss << ", companion, " << RoleName(ai->GetEffectiveRole());
            else if (owner && member == owner)
                ss << ", the leader you follow";
            else
                ss << ", player";
            if (!member->IsAlive())
                ss << ", DEAD";
            else
            {
                ss << ", " << uint32(member->GetHealthPct()) << "% health";
                if (member->GetMaxPower(POWER_MANA) > 0)
                    ss << ", " << uint32(member->GetPowerPct(POWER_MANA)) << "% mana";
                if (member->IsInCombat())
                    ss << ", fighting";
            }
            ss << ")";
            return ss.str();
        }

        void Insert(Player* owner, Player* mainBot, std::string const& kind, std::string const& channel, std::string const& speaker,
            std::string const& message, std::vector<Player*> const& voiced, uint32 provider, Player* addressed = nullptr)
        {
            std::ostringstream bots;
            for (Player* bot : voiced)
            {
                CompanionAI* ai = sCompanionMgr->GetAI(bot);
                bots << bot->GetGUID().GetCounter() << "|" << bot->GetName() << "|" << ClassName(bot->GetClass()) << "|"
                     << (ai ? RoleName(ai->GetEffectiveRole()) : "dps") << "|" << Abilities(bot) << "\n";
            }
            BotRecord const* record = RecordOf(mainBot);
            std::string context = BuildContext(mainBot, voiced) + Tactics::BrainContext(mainBot, message);
            if (addressed)
                context += "spoken to: " + addressed->GetName() + "\n";
            CharacterDatabase.PExecute("INSERT INTO companion_llm_events (bot_guid, owner_guid, kind, channel, speaker, message, context, bots, provider_id) "
                "VALUES ({}, {}, '{}', '{}', '{}', '{}', '{}', '{}', {})",
                mainBot->GetGUID().GetCounter(), (owner ? owner->GetGUID() : (record ? record->Owner : ObjectGuid::Empty)).GetCounter(),
                Escape(kind), Escape(channel), Escape(speaker), Escape(message.substr(0, 500)), Escape(context),
                Escape(bots.str()), provider);
        }

        void Perform(Scheduled const& action)
        {
            CompanionAI* ai = sCompanionMgr->GetAI(action.Bot);
            Player* bot = ai ? ai->GetBot() : nullptr;
            if (!bot || !bot->IsInWorld())
                return;

            bool const aloud = action.Channel == "say" || action.Channel == "yell";
            if (!action.Say.empty())
            {
                std::string say = action.Say.substr(0, 250);
                Player* receiver = action.Target.empty() ? nullptr : ObjectAccessor::FindPlayerByName(action.Target);
                Language const tongue = bot->GetTeam() == ALLIANCE ? LANG_COMMON : LANG_ORCISH;
                if (action.Channel == "whisper" && receiver)
                    bot->Whisper(say, LANG_UNIVERSAL, receiver);
                else if (action.Channel == "say")
                    bot->Say(say, tongue);
                else if (action.Channel == "yell")
                    bot->Yell(say, tongue);
                else
                    ai->Say(say);
            }
            // orders come through party chat and whispers only
            if (aloud)
                return;

            // orders: one per line, the same words a player types in party chat
            Player* owner = ObjectAccessor::FindPlayer(ai->GetOwnerGUID());
            std::istringstream lines(action.Commands);
            std::string line;
            uint32 count = 0;
            while (owner && std::getline(lines, line) && ++count <= 5)
                if (!line.empty())
                    Commands::ExecuteOrder(owner, { bot }, line, true);
        }

        void Receive(QueryResult result)
        {
            _pollInFlight = false;
            if (!result)
                return;

            std::ostringstream ids;
            bool first = true;
            do
            {
                Field* fields = result->Fetch();
                Scheduled action;
                ids << (first ? "" : ",") << fields[0].GetUInt64();
                _lastActionId = std::max(_lastActionId, fields[0].GetUInt64());
                first = false;
                action.Bot = ObjectGuid::Create<HighGuid::Player>(fields[1].GetUInt64());
                action.Channel = fields[2].GetString();
                action.Target = fields[3].GetString();
                action.Say = fields[4].GetString();
                action.Commands = fields[5].GetString();
                action.Due = _msTime + std::min<uint32>(fields[6].GetUInt32(), 8000);
                _scheduled.push_back(std::move(action));
            } while (result->NextRow());

            CharacterDatabase.PExecute("UPDATE companion_llm_actions SET consumed = 1 WHERE id IN ({})", ids.str());
        }

        bool Flooding(ObjectGuid key, uint32 minMs)
        {
            uint32& last = _lastEventMs[key];
            if (last && _msTime - last < minMs)
                return true;
            last = _msTime;
            return false;
        }
    }

    void EnsureTables()
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `companion_llm_events` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`bot_guid` BIGINT UNSIGNED NOT NULL,"
            "`owner_guid` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
            "`channel` VARCHAR(16) NOT NULL DEFAULT 'party',"
            "`speaker` VARCHAR(64) NOT NULL DEFAULT '',"
            "`message` TEXT NOT NULL,"
            "`context` TEXT NOT NULL,"
            "`created` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`status` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`id`), KEY `status` (`status`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Companions: what the bots hear, for the AI brain'");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `companion_llm_actions` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`event_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,"
            "`bot_guid` BIGINT UNSIGNED NOT NULL,"
            "`channel` VARCHAR(16) NOT NULL DEFAULT 'party',"
            "`target` VARCHAR(64) NOT NULL DEFAULT '',"
            "`say` TEXT NOT NULL,"
            "`commands` TEXT NOT NULL,"
            "`created` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`consumed` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`id`), KEY `consumed` (`consumed`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Companions: answers of the AI brain'");
        // added with providers and remote workers
        CompanionMgr::EnsureColumn("companion_llm_events", "kind", "VARCHAR(16) NOT NULL DEFAULT 'chat'");
        CompanionMgr::EnsureColumn("companion_llm_events", "bots", "TEXT NULL");
        CompanionMgr::EnsureColumn("companion_llm_events", "provider_id", "INT UNSIGNED NOT NULL DEFAULT 0");
        CompanionMgr::EnsureColumn("companion_llm_events", "claimed_by", "VARCHAR(64) NOT NULL DEFAULT ''");
        CompanionMgr::EnsureColumn("companion_llm_events", "claimed_at", "TIMESTAMP NULL DEFAULT NULL");
        CompanionMgr::EnsureColumn("companion_llm_actions", "delay_ms", "INT UNSIGNED NOT NULL DEFAULT 0");

        // the AI providers and the remote workers, edited in the web panel and read by the brain workers
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `companion_llm_providers` ("
            "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`name` VARCHAR(64) NOT NULL,"
            "`kind` VARCHAR(16) NOT NULL COMMENT 'anthropic, ollama, openai',"
            "`endpoint` VARCHAR(255) NOT NULL DEFAULT '',"
            "`model` VARCHAR(128) NOT NULL,"
            "`api_key` VARCHAR(255) NOT NULL DEFAULT '',"
            "`weight` INT UNSIGNED NOT NULL DEFAULT 1,"
            "`max_concurrent` INT UNSIGNED NOT NULL DEFAULT 2,"
            "`timeout_s` INT UNSIGNED NOT NULL DEFAULT 30,"
            "`options` TEXT NULL,"
            "`enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`worker_id` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = the brain next to the server',"
            "`answered` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`failed` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`avg_ms` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`last_ok` TIMESTAMP NULL DEFAULT NULL,"
            "`last_error` VARCHAR(255) NOT NULL DEFAULT '',"
            "PRIMARY KEY (`id`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Companions: AI providers of the brain'");
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `companion_llm_workers` ("
            "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`name` VARCHAR(64) NOT NULL,"
            "`token_hash` CHAR(64) NOT NULL,"
            "`last_seen` TIMESTAMP NULL DEFAULT NULL,"
            "`info` VARCHAR(255) NOT NULL DEFAULT '',"
            "PRIMARY KEY (`id`), UNIQUE KEY `token` (`token_hash`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Companions: remote brain workers'");
    }

    void Initialize()
    {
        // what was said while the server was down is stale
        CharacterDatabase.DirectExecute("UPDATE companion_llm_events SET status = 3 WHERE status < 2");
        CharacterDatabase.DirectExecute("UPDATE companion_llm_actions SET consumed = 1 WHERE consumed = 0");
    }

    bool IsEnabledFor(Player const* bot)
    {
        if (!GetConfig().LLMEnable || !bot)
            return false;
        BotRecord const* record = RecordOf(bot);
        return record && record->UseLLM;
    }

    void Update(uint32 diff)
    {
        _msTime += diff;

        // answers whose turn has come
        for (auto itr = _scheduled.begin(); itr != _scheduled.end();)
        {
            if (itr->Due <= _msTime)
            {
                Scheduled action = std::move(*itr);
                itr = _scheduled.erase(itr);
                Perform(action);
            }
            else
                ++itr;
        }

        if (!GetConfig().LLMEnable)
            return;
        _callbacks.ProcessReadyCallbacks();
        _pollTimer += diff;
        if (_pollTimer < GetConfig().LLMPollInterval || _pollInFlight)
            return;
        _pollTimer = 0;
        _pollInFlight = true;
        _callbacks.AddCallback(CharacterDatabase.AsyncQuery(Trinity::StringFormat("SELECT id, bot_guid, channel, target, say, commands, delay_ms "
            "FROM companion_llm_actions WHERE consumed = 0 AND id > {} ORDER BY id LIMIT 40", _lastActionId).c_str()).WithCallback(&Receive));

        // nobody answered in time: the moment has passed
        static uint32 expireTimer = 0;
        expireTimer += GetConfig().LLMPollInterval;
        if (expireTimer >= 10000)
        {
            expireTimer = 0;
            CharacterDatabase.PExecute("UPDATE companion_llm_events SET status = 3 WHERE status = 0 AND created < NOW() - INTERVAL {} SECOND",
                GetConfig().LLMMaxAge);
            // a worker that took a line and vanished: someone else may answer it
            CharacterDatabase.Execute("UPDATE companion_llm_events SET status = 0, claimed_by = '' WHERE status = 1 AND claimed_at < NOW() - INTERVAL 90 SECOND");
        }
    }

    bool OnChatToBot(Player* bot, Player* speaker, std::string const& channel, std::string const& message)
    {
        if (!IsEnabledFor(bot))
            return false;
        if (Flooding(bot->GetGUID(), 1500))
            return true;
        Player* owner = nullptr;
        if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
            owner = ObjectAccessor::FindPlayer(ai->GetOwnerGUID());
        Insert(owner, bot, "chat", channel, speaker ? speaker->GetName() : "", message, { bot }, ProviderOf(bot));
        return true;
    }

    bool OnGroupChat(Player* owner, Player* speaker, std::vector<Player*> const& bots, std::string const& message, Player* addressed)
    {
        Config const& config = GetConfig();
        if (!config.LLMEnable || !config.LLMPartyChat)
            return false;
        std::vector<Player*> voiced;
        for (Player* bot : bots)
            if (IsEnabledFor(bot))
                voiced.push_back(bot);
        if (voiced.empty())
            return false;
        if (Flooding(speaker->GetGUID(), 1200))
            return true;
        // the addressed bot leads (and its provider answers); else the first one
        Player* main = voiced.front();
        if (addressed && std::find(voiced.begin(), voiced.end(), addressed) != voiced.end())
            main = addressed;
        Insert(owner, main, "group", "party", speaker->GetName(), message, voiced, ProviderOf(main), addressed);
        return true;
    }

    bool OnOpenChat(Player* speaker, std::vector<Player*> const& bots, std::string const& channel, std::string const& message, Player* addressed)
    {
        if (!GetConfig().LLMEnable)
            return false;
        std::vector<Player*> voiced;
        if (addressed && IsEnabledFor(addressed))
            voiced.push_back(addressed);
        for (Player* bot : bots)
            if (bot != addressed && voiced.size() < 4 && IsEnabledFor(bot))
                voiced.push_back(bot);
        if (voiced.empty())
            return false;
        if (Flooding(speaker->GetGUID(), 1500))
            return true;
        Player* main = voiced.front();
        CompanionAI* ai = sCompanionMgr->GetAI(main);
        Insert(ai ? ObjectAccessor::FindPlayer(ai->GetOwnerGUID()) : nullptr, main, "group", channel, speaker->GetName(), message, voiced,
            ProviderOf(main), addressed);
        return true;
    }

    void OnCallout(Player* bot, std::string const& kind, std::string const& detail, std::string const& canned)
    {
        Config const& config = GetConfig();
        // the same callout from the same bot at most every 30 s; the party hears at most one callout every 5 s
        uint32& last = _lastCalloutMs[std::to_string(bot->GetGUID().GetCounter()) + ":" + kind];
        if (last && _msTime - last < 30000)
            return;
        CompanionAI* ai = sCompanionMgr->GetAI(bot);
        ObjectGuid owner = ai ? ai->GetOwnerGUID() : ObjectGuid::Empty;
        uint32& partyLast = _lastPartyCallout[owner];
        if (partyLast && _msTime - partyLast < 5000)
            return;
        // one bot says it for the party: the same kind of callout again from another bot only after a while
        uint32& kindLast = _lastCalloutMs[std::to_string(owner.GetCounter()) + "#" + kind];
        uint32 const kindDelay = kind == "rare" || kind == "elite" ? 60000 : kind == "member_down" ? 3000 : 15000;
        if (kindLast && _msTime - kindLast < kindDelay)
            return;
        last = _msTime;
        partyLast = _msTime;
        kindLast = _msTime;

        if (config.LLMCallouts && IsEnabledFor(bot))
        {
            Insert(ObjectAccessor::FindPlayer(owner), bot, "callout", "party", kind, detail, { bot }, ProviderOf(bot));
            return;
        }
        if (config.Callouts && ai && !canned.empty())
            ai->Say(canned);
    }

    void OnEvent(Player* bot, std::string const& kind, std::string const& detail)
    {
        if (!GetConfig().LLMBanter || !IsEnabledFor(bot))
            return;
        if (Flooding(bot->GetGUID(), 30000))
            return;
        Insert(nullptr, bot, "event", "party", kind, detail, { bot }, ProviderOf(bot));
    }

    uint32 RefOf(ObjectGuid owner, ObjectGuid enemy)
    {
        std::vector<ObjectGuid>& refs = _enemyRefs[owner];
        auto itr = std::find(refs.begin(), refs.end(), enemy);
        if (itr != refs.end())
            return uint32(itr - refs.begin()) + 1;
        refs.push_back(enemy);
        return uint32(refs.size());
    }

    bool IsEnemyRef(std::string const& word)
    {
        return word.size() >= 2 && word.size() <= 3 && (word[0] == 'e' || word[0] == 'E')
            && std::all_of(word.begin() + 1, word.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    }

    Unit* ResolveEnemy(Player* commander, std::string const& word)
    {
        if (!commander || !IsEnemyRef(word))
            return nullptr;
        auto itr = _enemyRefs.find(commander->GetGUID());
        uint32 index = uint32(std::strtoul(word.c_str() + 1, nullptr, 10));
        if (itr == _enemyRefs.end() || !index || index > itr->second.size())
            return nullptr;
        return ObjectAccessor::GetUnit(*commander, itr->second[index - 1]);
    }

    void ConsiderTactics(Player* bot)
    {
        Config const& config = GetConfig();
        if (!config.LLMEnable || !config.LLMTactics || !IsEnabledFor(bot))
            return;
        CompanionAI* ai = sCompanionMgr->GetAI(bot);
        if (!ai)
            return;
        ObjectGuid ownerGuid = ai->GetOwnerGUID();
        TacticsState& state = _tactics[ownerGuid];
        GroupView const& view = ai->View();
        if (view.Enemies.empty())
        {
            state.CombatSinceMs = 0;
            state.Enemies = 0;
            return;
        }
        if (!state.CombatSinceMs)
            state.CombatSinceMs = _msTime;
        if (_msTime - state.CombatSinceMs < 1500)
            return;     // the pull settles first

        // worth a plan: a pack (3+), two with an elite among them, or a boss; one mob needs no raid leader
        bool elite = false, boss = false;
        for (Unit* enemy : view.Enemies)
            if (Creature* creature = enemy->ToCreature())
            {
                elite = elite || creature->IsElite();
                boss = boss || creature->isWorldBoss() || creature->IsDungeonBoss();
            }
        if (!boss && view.Enemies.size() < 3 && !(elite && view.Enemies.size() >= 2))
            return;

        bool firstLook = state.LastMs < state.CombatSinceMs || !state.LastMs;
        bool adds = view.Enemies.size() >= state.Enemies + 2 && _msTime - state.LastMs > 5000;
        if (!firstLook && !adds && _msTime - state.LastMs < config.LLMTacticsInterval * 1000)
            return;
        state.LastMs = _msTime;
        state.Enemies = view.Enemies.size();

        std::vector<Player*> voiced;
        for (Player* member : sCompanionMgr->GetBotsOf(ownerGuid))
            if (IsEnabledFor(member))
                voiced.push_back(member);
        if (voiced.empty())
            return;
        Insert(ObjectAccessor::FindPlayer(ownerGuid), bot, "tactics", "party", "tactics",
            std::to_string(view.Enemies.size()) + (firstLook ? " enemies, the fight just started" : " enemies, the fight goes on"), voiced, ProviderOf(bot));
    }

    void RecordChat(Group const* group, std::string const& speaker, std::string const& text)
    {
        if (!group || !GetConfig().LLMEnable)
            return;
        std::deque<ChatLine>& lines = _history[group->GetGUID()];
        lines.push_back({ _msTime, speaker, text.substr(0, 200) });
        while (lines.size() > 14)
            lines.pop_front();
    }

    std::string BuildContext(Player* bot, std::vector<Player*> const& voiced)
    {
        std::ostringstream ss;
        CompanionAI* ai = sCompanionMgr->GetAI(bot);
        Player* owner = ai ? ObjectAccessor::FindPlayer(ai->GetOwnerGUID()) : nullptr;

        ss << "companions you speak for:\n";
        for (Player* member : voiced)
        {
            CompanionAI* memberAI = sCompanionMgr->GetAI(member);
            BotRecord const* record = RecordOf(member);
            ss << "- " << member->GetName() << ": level " << uint32(member->GetLevel()) << " " << RaceName(member->GetRace()) << " "
               << ClassName(member->GetClass());
            if (memberAI)
                ss << ", " << RoleName(memberAI->GetEffectiveRole()) << ", " << (memberAI->GetStance() == Stance::Stay ? "holding position" : "following");
            ss << ", " << uint32(member->GetHealthPct()) << "% health";
            if (member->GetMaxPower(POWER_MANA) > 0)
                ss << ", " << uint32(member->GetPowerPct(POWER_MANA)) << "% mana";
            if (!member->IsAlive())
                ss << ", dead";
            if (Unit* target = member->GetVictim())
                ss << ", attacking " << target->GetName();
            if (record && !record->Personality.empty())
                ss << ". Personality: " << record->Personality;
            if (memberAI && !memberAI->PrefsLine().empty())
                ss << ". Told about its abilities: " << memberAI->PrefsLine();
            ss << "\n";
        }

        if (owner)
            ss << "leader: " << owner->GetName() << "\n";
        ss << "place: " << AreaName(bot->GetZoneId());
        if (bot->GetAreaId() != bot->GetZoneId())
            ss << " (" << AreaName(bot->GetAreaId()) << ")";
        if (bot->GetMap()->IsDungeon())
            ss << ", inside a dungeon";
        if (bot->IsMounted())
            ss << ", mounted";
        ss << "\n";

        Group* group = bot->GetGroup();
        if (group)
        {
            // the companions above are not repeated
            std::string others;
            for (GroupReference const& ref : group->GetMembers())
                if (Player* member = ref.GetSource())
                    if (std::find(voiced.begin(), voiced.end(), member) == voiced.end())
                        others += " " + MemberLine(member, owner) + ";";
            ss << (group->isRaidGroup() ? "raid, " : "party, ") << group->GetMembersCount() << " members; the others:" << others << "\n";

            std::string marks;
            for (uint8 icon = 0; icon < 8; ++icon)
            {
                ObjectGuid marked = group->GetTargetIcon(icon);
                if (marked.IsEmpty())
                    continue;
                if (Unit* unit = ObjectAccessor::GetUnit(*bot, marked))
                    if (unit->IsAlive())
                        marks += std::string(" ") + IconName(icon) + " = " + unit->GetName() + ";";
            }
            if (!marks.empty())
                ss << "raid markers:" << marks << "\n";
        }

        if (ai)
        {
            GroupView const& view = ai->View();
            ObjectGuid ownerGuid = ai->GetOwnerGUID();
            if (view.Enemies.empty())
                _enemyRefs.erase(ownerGuid);
            else
            {
                // numbered for orders ("kill E2", "cc E3"): type for crowd control, what they cast for interrupts
                ss << "enemies in the fight:\n";
                uint32 shown = 0;
                for (Unit* enemy : view.Enemies)
                {
                    if (++shown > 10)
                        break;
                    ss << "  E" << RefOf(ownerGuid, enemy->GetGUID()) << " " << enemy->GetName() << " (level " << uint32(enemy->GetLevel());
                    if (Creature* creature = enemy->ToCreature())
                    {
                        if (creature->isWorldBoss() || creature->IsDungeonBoss())
                            ss << ", boss";
                        else if (creature->IsElite())
                            ss << ", elite";
                    }
                    ss << ", " << CreatureTypeName(enemy->GetCreatureType()) << ", " << uint32(enemy->GetHealthPct()) << "%";
                    if (Unit* victim = enemy->GetVictim())
                        ss << ", on " << victim->GetName();
                    Spell* casting = enemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                    if (!casting)
                        casting = enemy->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                    if (casting && casting->GetSpellInfo()->SpellName)
                        ss << ", casting " << casting->GetSpellInfo()->SpellName->Str[LOCALE_enUS];
                    if (enemy->HasBreakableByDamageCrowdControlAura())
                        ss << ", crowd controlled";
                    if (group)
                        for (uint8 icon = 0; icon < 8; ++icon)
                            if (group->GetTargetIcon(icon) == enemy->GetGUID())
                                ss << ", marked " << IconName(icon);
                    ss << ")\n";
                }

                // what the bots know of the encounter (CompanionMechanics.cpp): the brain's tactics build on it
                std::vector<uint32> told;
                for (Unit* enemy : view.Enemies)
                    if (Creature* creature = enemy->ToCreature())
                        if (Mechanics::Encounter const* encounter = Mechanics::EncounterFor(creature->GetEntry()))
                            if (std::find(told.begin(), told.end(), encounter->Entry) == told.end())
                            {
                                told.push_back(encounter->Entry);
                                ss << "boss tactics, " << encounter->Name << ": " << encounter->Note << "\n";
                            }
            }
        }

        // packs not fighting yet (numbered for plans: "plan pull p2"), and the plan going on
        if (owner)
            ss << sPlans->PackLines(owner);

        // what a player would notice around: elites and rares not yet fighting
        {
            std::list<Unit*> nearby;
            Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, 50.0f);
            Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, nearby, check);
            Cell::VisitAllObjects(bot, searcher, 50.0f);
            std::string notable;
            uint32 shown = 0;
            for (Unit* unit : nearby)
            {
                Creature* creature = unit->ToCreature();
                if (!creature || creature->IsInCombat() || !bot->IsValidAttackTarget(creature))
                    continue;
                bool rare = creature->HasClassification(CreatureClassifications::Rare) || creature->HasClassification(CreatureClassifications::RareElite);
                bool elite = (creature->IsElite() || creature->isWorldBoss()) && creature->GetLevel() + 2 >= bot->GetLevel();
                if (!(rare && creature->GetLevel() + 10 >= bot->GetLevel()) && !elite)
                    continue;
                if (++shown > 5)
                    break;
                notable += " " + creature->GetName() + " (level " + std::to_string(creature->GetLevel()) + (rare ? ", rare" : ", elite")
                    + ", " + std::to_string(uint32(bot->GetDistance(creature))) + " yards);";
            }
            if (!notable.empty())
                ss << "notable enemies nearby, not fighting:" << notable << "\n";
        }

        if (group)
        {
            auto itr = _history.find(group->GetGUID());
            if (itr != _history.end() && !itr->second.empty())
            {
                ss << "recent party chat:\n";
                for (ChatLine const& line : itr->second)
                    ss << "  [" << (_msTime - line.At) / 1000 << "s ago] " << line.Speaker << ": " << line.Text << "\n";
            }
        }
        return ss.str();
    }
}
