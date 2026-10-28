/*
 * Companions: bot sessions, login/logout, staying with the owner
 */

#include "CompanionMgr.h"
#include "CompanionTactics.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CompanionAI.h"
#include "CompanionAmbient.h"
#include "CompanionPersonality.h"
#include "CompanionPlan.h"
#include "CompanionConfig.h"
#include "CompanionFactory.h"
#include "CompanionLLM.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Spell.h"
#include "Containers.h"
#include "Corpse.h"
#include "GroupMgr.h"
#include "Log.h"
#include "LoginQueryHolder.h"
#include "Map.h"
#include "MovementPackets.h"
#include "ObjectAccessor.h"
#include "PartyPackets.h"
#include "RealmList.h"
#include "Player.h"
#include "Random.h"
#include "ItemTemplate.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <sstream>
#include <limits>

namespace Companions
{
    CompanionMgr* CompanionMgr::instance()
    {
        static CompanionMgr instance;
        return &instance;
    }

    void CompanionMgr::Initialize()
    {
        BotSessionHooks::OnPlayerUpdate = &CompanionMgr::OnBotPlayerUpdate;
        LoadStoredSettings();
        if (!GetConfig().Enable)
            return;

        EnsureTables();
        _botAccountId = GetOrCreateCreationAccount();
        LoadRecords();
        sCompanionFactory->Initialize();
        Tactics::Load();
        CompanionLLM::Initialize();
        sCompanionAmbient->Initialize();
        TC_LOG_INFO("server.worldserver", "Companions: {} bots known, companion account {}", _records.size(), _botAccountId);
    }

    void CompanionMgr::EnsureTables()
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `companion_bots` ("
            "`guid` BIGINT UNSIGNED NOT NULL,"
            "`owner_guid` BIGINT UNSIGNED NOT NULL,"
            "`role` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`active` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`created` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`llm` TINYINT UNSIGNED NOT NULL DEFAULT 1,"
            "`personality` VARCHAR(255) NOT NULL DEFAULT '',"
            "PRIMARY KEY (`guid`), KEY `owner` (`owner_guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Companions: player bots and their owners'");
        // added with the web panel
        EnsureColumn("companion_bots", "spec", "VARCHAR(16) NOT NULL DEFAULT ''");
        EnsureColumn("companion_bots", "gear_quality", "TINYINT UNSIGNED NOT NULL DEFAULT 0");
        EnsureColumn("companion_bots", "aggression", "TINYINT UNSIGNED NOT NULL DEFAULT 0");
        EnsureColumn("companion_bots", "stance", "TINYINT UNSIGNED NOT NULL DEFAULT 0");
        EnsureColumn("companion_bots", "template_id", "INT UNSIGNED NOT NULL DEFAULT 0");
        // added with the AI providers (0 = any)
        EnsureColumn("companion_bots", "llm_provider", "INT UNSIGNED NOT NULL DEFAULT 0");
        // added with spell preferences, looting and potions
        EnsureColumn("companion_bots", "prefs", "VARCHAR(1024) NOT NULL DEFAULT ''");
        EnsureColumn("companion_bots", "loot_mode", "TINYINT UNSIGNED NOT NULL DEFAULT 0");
        EnsureColumn("companion_bots", "potions", "TINYINT UNSIGNED NOT NULL DEFAULT 1");
        EnsureColumn("companion_bots", "auto_cc", "TINYINT UNSIGNED NOT NULL DEFAULT 1");
        // teams of companions summoned together (the web panel makes them; the same definition as web/app/companions.py)
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `companion_teams` ("
            "`id` INT UNSIGNED NOT NULL AUTO_INCREMENT, `owner_guid` BIGINT UNSIGNED NOT NULL, `name` VARCHAR(32) NOT NULL, "
            "`bots` VARCHAR(512) NOT NULL DEFAULT '', "
            "`updated` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, "
            "PRIMARY KEY (`id`), UNIQUE KEY `owner_name` (`owner_guid`, `name`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COMMENT='Companions: teams of companions summoned together'");
        CompanionLLM::EnsureTables();
    }

    void CompanionMgr::EnsureColumn(char const* table, char const* column, char const* definition)
    {
        QueryResult result = CharacterDatabase.PQuery("SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() "
            "AND table_name = '{}' AND column_name = '{}'", table, column);
        if (!result)
            CharacterDatabase.DirectExecute(Trinity::StringFormat("ALTER TABLE `{}` ADD COLUMN `{}` {}", table, column, definition).c_str());
    }

    uint32 CompanionMgr::GetOrCreateCreationAccount()
    {
        if (_botAccountId)
            return _botAccountId;

        std::string name = GetConfig().AccountName;
        uint32 id = AccountMgr::GetId(name);
        if (!id)
        {
            // nobody logs into this account: a long random password
            std::string password;
            for (uint32 i = 0; i < 16; ++i)
                password += char('a' + urand(0, 25));
            AccountOpResult result = sAccountMgr->CreateAccount(name, password);
            if (result != AccountOpResult::AOR_OK)
            {
                TC_LOG_ERROR("server.worldserver", "Companions: cannot create the account {} (error {})", name, uint32(result));
                return 0;
            }
            id = AccountMgr::GetId(name);
        }
        _botAccountId = id;
        return id;
    }

    void CompanionMgr::LoadRecords()
    {
        _records.clear();
        QueryResult result = CharacterDatabase.Query("SELECT b.guid, b.owner_guid, b.role, b.active, b.created, b.llm, b.personality, "
            "b.spec, b.gear_quality, b.aggression, b.stance, b.template_id, b.llm_provider, b.prefs, b.loot_mode, b.potions, b.auto_cc FROM companion_bots b JOIN characters c ON c.guid = b.guid JOIN characters o ON o.guid = b.owner_guid WHERE c.deleteDate IS NULL");
        if (!result)
            return;
        do
        {
            Field* fields = result->Fetch();
            BotRecord record;
            record.Guid = ObjectGuid::Create<HighGuid::Player>(fields[0].GetUInt64());
            record.Owner = ObjectGuid::Create<HighGuid::Player>(fields[1].GetUInt64());
            record.BotRole = Role(std::min<uint8>(fields[2].GetUInt8(), uint8(Role::RangedDps)));
            record.Active = fields[3].GetBool();
            record.Created = fields[4].GetBool();
            record.UseLLM = fields[5].GetBool();
            record.Personality = fields[6].GetString();
            record.Spec = fields[7].GetString();
            record.GearQuality = std::min<uint8>(fields[8].GetUInt8(), ITEM_QUALITY_LEGENDARY);
            record.AggressionMode = Aggression(std::min<uint8>(fields[9].GetUInt8(), uint8(Aggression::Aggressive)));
            record.StanceMode = Stance(std::min<uint8>(fields[10].GetUInt8(), uint8(Stance::Stay)));
            record.TemplateId = fields[11].GetUInt32();
            record.LLMProvider = fields[12].GetUInt32();
            record.Prefs = fields[13].GetString();
            record.LootMode = std::min<uint8>(fields[14].GetUInt8(), 2);
            record.UsePotions = fields[15].GetBool();
            record.AutoCC = fields[16].GetBool();
            _records[record.Guid] = record;
        } while (result->NextRow());
    }

    void CompanionMgr::SaveRecord(BotRecord const& record)
    {
        std::string personality = record.Personality;
        CharacterDatabase.EscapeString(personality);
        std::string spec = record.Spec;
        CharacterDatabase.EscapeString(spec);
        std::string prefs = record.Prefs;
        CharacterDatabase.EscapeString(prefs);
        CharacterDatabase.PExecute("REPLACE INTO companion_bots (guid, owner_guid, role, active, created, llm, personality, spec, gear_quality, aggression, stance, template_id, llm_provider, "
            "prefs, loot_mode, potions, auto_cc) VALUES ({}, {}, {}, {}, {}, {}, '{}', '{}', {}, {}, {}, {}, {}, '{}', {}, {}, {})",
            record.Guid.GetCounter(), record.Owner.GetCounter(), uint32(record.BotRole), uint32(record.Active), uint32(record.Created),
            uint32(record.UseLLM), personality, spec, uint32(record.GearQuality), uint32(record.AggressionMode), uint32(record.StanceMode), record.TemplateId, record.LLMProvider,
            prefs, uint32(record.LootMode), uint32(record.UsePotions), uint32(record.AutoCC));
    }

    void CompanionMgr::AddRecord(BotRecord const& record)
    {
        _records[record.Guid] = record;
        SaveRecord(record);
    }

    void CompanionMgr::DeleteRecord(ObjectGuid botGuid)
    {
        _records.erase(botGuid);
        CharacterDatabase.PExecute("DELETE FROM companion_bots WHERE guid = {}", botGuid.GetCounter());
    }

    BotRecord* CompanionMgr::GetRecord(ObjectGuid botGuid)
    {
        auto itr = _records.find(botGuid);
        return itr != _records.end() ? &itr->second : nullptr;
    }

    std::vector<BotRecord const*> CompanionMgr::GetRecordsOf(ObjectGuid owner) const
    {
        std::vector<BotRecord const*> records;
        for (auto const& [guid, record] : _records)
            if (record.Owner == owner)
                records.push_back(&record);
        return records;
    }

    bool CompanionMgr::IsBot(Player const* player) const
    {
        return player && player->GetSession() && player->GetSession()->IsBot();
    }

    CompanionAI* CompanionMgr::GetAI(ObjectGuid botGuid) const
    {
        auto itr = _ais.find(botGuid);
        return itr != _ais.end() ? itr->second.get() : nullptr;
    }

    CompanionAI* CompanionMgr::GetAI(Player const* bot) const
    {
        return bot ? GetAI(bot->GetGUID()) : nullptr;
    }

    std::vector<Player*> CompanionMgr::GetBotsOf(ObjectGuid owner) const
    {
        std::vector<Player*> bots;
        for (auto const& [guid, ai] : _ais)
            if (ai->GetOwnerGUID() == owner)
                if (Player* bot = ai->GetBot())
                    bots.push_back(bot);
        std::sort(bots.begin(), bots.end(), [](Player* a, Player* b) { return a->GetGUID() < b->GetGUID(); });
        return bots;
    }

    uint32 CompanionMgr::CountBotsOf(ObjectGuid owner) const
    {
        uint32 count = 0;
        for (auto const& [guid, ai] : _ais)
            if (ai->GetOwnerGUID() == owner)
                ++count;
        for (auto const& [guid, pending] : _pendingLogins)
            if (pending.Owner == owner)
                ++count;
        return count;
    }

    void CompanionMgr::ArrangeFormation(ObjectGuid owner)
    {
        std::vector<Player*> bots = GetBotsOf(owner);
        for (uint32 i = 0; i < bots.size(); ++i)
            if (CompanionAI* ai = GetAI(bots[i]))
                ai->SetFollowSlot(i, uint32(bots.size()));
    }

    WorldSession* CompanionMgr::CreateBotSession(uint32 accountId, Player* owner)
    {
        uint32 build = 0;
        ClientBuild::VariantId variant = { };
        if (owner)
        {
            build = owner->GetSession()->GetClientBuild();
            variant = owner->GetSession()->GetClientBuildVariant();
        }
        else if (std::shared_ptr<Realm const> realm = sRealmList->GetCurrentRealm())
            build = realm->Build;
        WorldSession* session = new WorldSession(accountId, "companion", 0, std::string(), nullptr, SEC_PLAYER,
            uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, std::string("Wn64"), Minutes(0), build, variant, LOCALE_enUS, 0, false);
        session->SetBot(true);
        session->m_timeOutTime = std::numeric_limits<time_t>::max();
        session->LoadPermissions();
        return session;
    }

    bool CompanionMgr::Summon(Player* owner, ObjectGuid botGuid, std::string& error)
    {
        Config const& config = GetConfig();
        if (!config.Enable)
        {
            error = "Companions are disabled on this server.";
            return false;
        }
        if (botGuid == owner->GetGUID())
        {
            error = "You cannot summon yourself.";
            return false;
        }
        // its companion about town (the living world borrowed it): it leaves the city and logs in at its player
        if (CompanionAI* about = GetAI(botGuid); about && about->IsAmbient())
        {
            BotRecord const* owned = GetRecord(botGuid);
            if (!owned || owned->Owner != owner->GetGUID())
            {
                error = "That companion belongs to someone else.";
                return false;
            }
            _resummons[botGuid] = owner->GetGUID();
            Dismiss(botGuid);
            return true;
        }
        if (IsBotGuid(botGuid) || IsLoggingIn(botGuid))
        {
            error = "That companion is already with you.";
            return false;
        }
        if (ObjectAccessor::FindConnectedPlayer(botGuid))
        {
            error = "That character is online.";
            return false;
        }

        CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(botGuid);
        if (!cache || cache->IsDeleted)
        {
            error = "No such character.";
            return false;
        }

        BotRecord* record = GetRecord(botGuid);
        if (record && record->Owner != owner->GetGUID())
        {
            error = "That companion belongs to someone else.";
            return false;
        }
        if (!record)
        {
            if (!config.AllowAlts || cache->AccountId != owner->GetSession()->GetAccountId())
            {
                error = "You can only summon your companions or your own characters.";
                return false;
            }
            BotRecord alt;
            alt.Guid = botGuid;
            alt.Owner = owner->GetGUID();
            alt.Created = false;
            alt.UseLLM = config.LLMDefaultOn;
            AddRecord(alt);
            record = GetRecord(botGuid);
        }

        if (Player::TeamForRace(cache->Race) != owner->GetTeam())
        {
            error = "That character is of the other faction.";
            return false;
        }
        if (CountBotsOf(owner->GetGUID()) >= config.MaxPerPlayer)
        {
            error = "You already have the maximum number of companions (" + std::to_string(config.MaxPerPlayer) + ").";
            return false;
        }
        if (CountAll() >= config.MaxTotal)
        {
            error = "The server has reached its companion limit (" + std::to_string(config.MaxTotal) + " in the game).";
            return false;
        }
        if (Group* group = owner->GetGroup())
        {
            // the bots of this owner still logging in count, they are not members yet
            uint32 members = group->GetMembersCount();
            for (auto const& [guid, pending] : _pendingLogins)
                if (pending.Owner == owner->GetGUID() && !group->IsMember(guid))
                    ++members;
            if (!group->IsMember(botGuid) && members >= config.RaidSize)
            {
                error = "Your group is full (" + std::to_string(config.RaidSize) + " players).";
                return false;
            }
        }

        WorldSession* session = CreateBotSession(cache->AccountId, owner);
        std::shared_ptr<LoginQueryHolder> holder = std::make_shared<LoginQueryHolder>(cache->AccountId, botGuid);
        if (!holder->Initialize())
        {
            delete session;
            error = "The character could not be loaded.";
            return false;
        }

        _pendingLogins[botGuid] = { session, owner->GetGUID() };
        ObjectGuid ownerGuid = owner->GetGUID();
        _loginCallbacks.AddCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete([this, botGuid, ownerGuid](SQLQueryHolderBase const& result)
        {
            auto itr = _pendingLogins.find(botGuid);
            if (itr == _pendingLogins.end())
                return;
            WorldSession* pendingSession = itr->second.Session;
            _pendingLogins.erase(itr);
            if (_shuttingDown)
            {
                delete pendingSession;
                return;
            }
            pendingSession->HandlePlayerLogin(static_cast<LoginQueryHolder const&>(result));
            FinishLogin(pendingSession, botGuid, ownerGuid);
        });
        return true;
    }

    uint32 CompanionMgr::CountAll() const
    {
        return uint32(_ais.size() + _pendingLogins.size()) - CountAmbient();
    }

    uint32 CompanionMgr::CountAmbient() const
    {
        uint32 count = 0;
        for (auto const& [guid, ai] : _ais)
            count += ai->IsAmbient() ? 1 : 0;
        for (auto const& [guid, pending] : _pendingLogins)
            count += pending.Ambient ? 1 : 0;
        return count;
    }

    std::vector<Player*> CompanionMgr::GetPuppets() const
    {
        std::vector<Player*> puppets;
        for (auto const& [guid, session] : _puppets)
            if (Player* puppet = session->GetPlayer())
                puppets.push_back(puppet);
        return puppets;
    }

    bool CompanionMgr::LoginAmbient(ObjectGuid botGuid, WorldLocation const& where, uint32 zoneId, uint8 level, bool city, bool borrowed)
    {
        if (_shuttingDown || IsBotGuid(botGuid) || IsLoggingIn(botGuid) || ObjectAccessor::FindConnectedPlayer(botGuid))
            return false;
        CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(botGuid);
        if (!cache || cache->IsDeleted)
            return false;

        // it logs in where it is wanted (no far teleport after the login): written before the login loads the character
        CharacterDatabase.DirectExecute(Trinity::StringFormat("UPDATE characters SET map = {}, instance_id = 0, position_x = {}, position_y = {}, "
            "position_z = {}, orientation = {}, zone = {}, transguid = 0, trans_x = 0, trans_y = 0, trans_z = 0, trans_o = 0 WHERE guid = {}",
            where.GetMapId(), where.GetPositionX(), where.GetPositionY(), where.GetPositionZ(), where.GetOrientation(), zoneId,
            botGuid.GetCounter()).c_str());

        WorldSession* session = CreateBotSession(cache->AccountId, nullptr);
        std::shared_ptr<LoginQueryHolder> holder = std::make_shared<LoginQueryHolder>(cache->AccountId, botGuid);
        if (!holder->Initialize())
        {
            delete session;
            return false;
        }
        _pendingLogins[botGuid] = { session, ObjectGuid::Empty, true };
        Position home = where.GetPosition();
        _loginCallbacks.AddCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete([this, botGuid, home, level, city, borrowed](SQLQueryHolderBase const& result)
        {
            auto itr = _pendingLogins.find(botGuid);
            if (itr == _pendingLogins.end())
                return;
            WorldSession* pendingSession = itr->second.Session;
            _pendingLogins.erase(itr);
            if (_shuttingDown)
            {
                delete pendingSession;
                return;
            }
            pendingSession->HandlePlayerLogin(static_cast<LoginQueryHolder const&>(result));
            FinishAmbientLogin(pendingSession, botGuid, home, level, city, borrowed);
        });
        return true;
    }

    void CompanionMgr::FinishAmbientLogin(WorldSession* session, ObjectGuid botGuid, Position const& home, uint8 level, bool city, bool borrowed)
    {
        Player* bot = session->GetPlayer();
        if (!bot)
        {
            TC_LOG_ERROR("server.worldserver", "Companions: ambient login of {} failed", botGuid.ToString());
            delete session;
            return;
        }
        EnsureSeesWorld(bot);
        _sessions[botGuid] = session;
        auto ai = std::make_unique<CompanionAI>(bot, ObjectGuid::Empty, DpsRoleForClass(bot->GetClass()));
        ai->SetAmbient(home, city);
        ai->SetLootMode(1);     // alone in the world: it takes everything off its kills
        _ais[botGuid] = std::move(ai);
        // left the world dead last time: back on its feet
        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
        }
        bot->SetPassOnGroupLoot(true);
        // a player's companion keeps its level, gear and name; the world's own adventurers get theirs
        if (!borrowed)
        {
            sCompanionFactory->Prepare(bot, level, DpsRoleForClass(bot->GetClass()), true);
            // they carry a family name, like Forever's players
            if (sCharacterCache->GetCharacterSurnameByGuid(botGuid).empty())
                CompanionFactory::SetSurname(botGuid, CompanionFactory::RandomSurname(bot->GetRace()));
        }
        TC_LOG_DEBUG("server.worldserver", "Companions: ambient {} (level {}) in zone {}", bot->GetName(), uint32(bot->GetLevel()), bot->GetZoneId());
    }

    void CompanionMgr::UpdateAmbientBot(Player* bot, CompanionAI* ai)
    {
        // invited to a group: it joins whoever invited it, as a companion of theirs
        Group* invite = bot->GetGroupInvite();
        if (!invite)
            return;
        Player* inviter = ObjectAccessor::FindConnectedPlayer(invite->GetLeaderGUID());
        // somebody's companion about town: it goes with its own player only, as his companion
        if (BotRecord const* owned = GetRecord(bot->GetGUID()))
        {
            bot->UninviteFromGroup();
            if (inviter && inviter->GetGUID() == owned->Owner)
            {
                _resummons[bot->GetGUID()] = inviter->GetGUID();
                Dismiss(bot->GetGUID());
            }
            else if (inviter && !IsBotGuid(inviter->GetGUID()))
            {
                std::string ownerName;
                sCharacterCache->GetCharacterNameByGuid(owned->Owner, ownerName);
                bot->Whisper("Thanks, but I'm " + (ownerName.empty() ? std::string("someone else") : ownerName) + "'s companion.",
                    LANG_UNIVERSAL, inviter);
            }
            return;
        }
        std::string why;
        if (inviter && !IsBotGuid(inviter->GetGUID()) && AdoptAmbient(bot, ai, inviter, why))
        {
            WorldPackets::Party::PartyInviteResponse response{ WorldPacket(CMSG_PARTY_INVITE_RESPONSE) };
            response.Accept = true;
            bot->GetSession()->HandlePartyInviteResponseOpcode(response);
            ai->Say("Sure, count me in!");
            return;
        }
        bot->UninviteFromGroup();
        if (inviter && !why.empty())
            bot->Whisper(why, LANG_UNIVERSAL, inviter);
    }

    bool CompanionMgr::AdoptAmbient(Player* bot, CompanionAI* ai, Player* inviter, std::string& why)
    {
        Config const& config = GetConfig();
        if (!config.Enable)
            return false;
        if (bot->GetTeam() != inviter->GetTeam())
            return false;
        if (CountBotsOf(inviter->GetGUID()) >= config.MaxPerPlayer)
        {
            why = "Thanks, but you already have enough companions with you.";
            return false;
        }
        if (CountAll() >= config.MaxTotal)
        {
            why = "Sorry, not right now.";
            return false;
        }
        BotRecord record;
        record.Guid = bot->GetGUID();
        record.Owner = inviter->GetGUID();
        record.Created = true;
        record.Active = true;
        record.UseLLM = config.LLMDefaultOn;
        record.Personality = Personality::Random(bot->GetRace(), bot->GetClass());
        AddRecord(record);
        ai->BecomeCompanion(inviter->GetGUID());
        sCompanionAmbient->Forget(bot->GetGUID());
        // a companion keeps up with its owner from now on, gear of a companion included
        if (config.SyncLevel && inviter->GetLevel() > bot->GetLevel())
            QueuePrepare(bot->GetGUID(), inviter->GetLevel(), config.AutoGear);
        else if (config.AutoGear)
            QueuePrepare(bot->GetGUID(), 0, true);
        TC_LOG_INFO("server.worldserver", "Companions: {} (ambient) joined {} as a companion", bot->GetName(), inviter->GetName());
        return true;
    }

    void CompanionMgr::FinishLogin(WorldSession* session, ObjectGuid botGuid, ObjectGuid ownerGuid)
    {
        Player* bot = session->GetPlayer();
        if (!bot)
        {
            TC_LOG_ERROR("server.worldserver", "Companions: login of {} failed", botGuid.ToString());
            delete session;
            return;
        }

        EnsureSeesWorld(bot);
        BotRecord* record = GetRecord(botGuid);
        Role role = record ? record->BotRole : Role::Auto;
        _sessions[botGuid] = session;
        _ais[botGuid] = std::make_unique<CompanionAI>(bot, ownerGuid, role);
        if (record)
        {
            _ais[botGuid]->SetAggression(record->AggressionMode);
            _ais[botGuid]->SetStance(record->StanceMode);
            _ais[botGuid]->LoadPrefs(record->Prefs);
            // a voice of its own when its player wrote none
            if (record->Personality.empty())
            {
                record->Personality = Personality::Random(bot->GetRace(), bot->GetClass());
                SaveRecord(*record);
            }
            _ais[botGuid]->SetLootMode(record->LootMode);
            _ais[botGuid]->SetUsePotions(record->UsePotions);
            _ais[botGuid]->SetAutoCC(record->AutoCC);
        }
        bot->SetPassOnGroupLoot(false);    // the AI answers each roll: Need on its upgrades, Pass on the rest
        if (record)
        {
            record->Active = true;
            SaveRecord(*record);
        }

        Player* owner = ObjectAccessor::FindConnectedPlayer(ownerGuid);    // also while he zones (in no map for a moment)
        if (!owner)
        {
            Dismiss(botGuid, true);
            return;
        }

        Config const& config = GetConfig();
        bool created = record && record->Created;
        uint8 level = (created && config.SyncLevel) ? owner->GetLevel() : 0;
        bool naked = !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_CHEST) || !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        bool regear = created && config.AutoGear && (naked || level > bot->GetLevel());
        sCompanionFactory->Prepare(bot, level, role, regear);

        JoinOwnerGroup(bot, owner);
        TeleportToOwner(bot, owner);
        ArrangeFormation(ownerGuid);
        TC_LOG_INFO("server.worldserver", "Companions: {} joined {}", bot->GetName(), owner->GetName());
    }

    std::vector<CompanionMgr::Team> CompanionMgr::GetTeamsOf(ObjectGuid owner) const
    {
        std::vector<Team> teams;
        if (QueryResult result = CharacterDatabase.PQuery("SELECT id, name, bots FROM companion_teams WHERE owner_guid = {} ORDER BY name",
            owner.GetCounter()))
        {
            do
            {
                Field* fields = result->Fetch();
                Team team;
                team.Id = fields[0].GetUInt32();
                team.Name = fields[1].GetString();
                std::istringstream bots(fields[2].GetString());
                std::string id;
                while (std::getline(bots, id, ','))
                    if (!id.empty() && std::isdigit(static_cast<unsigned char>(id[0])))
                    {
                        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(std::strtoull(id.c_str(), nullptr, 10));
                        auto record = _records.find(guid);
                        if (record != _records.end() && record->second.Owner == owner)
                            team.Bots.push_back(guid);
                    }
                teams.push_back(std::move(team));
            } while (result->NextRow());
        }
        return teams;
    }

    void CompanionMgr::SummonTeam(Player* owner, Team const& team, uint32& summoned, uint32& dismissed, std::vector<std::string>& errors)
    {
        // the others first, right now (their places in the group and the companion count are free for the team)
        for (Player* bot : GetBotsOf(owner->GetGUID()))
            if (std::find(team.Bots.begin(), team.Bots.end(), bot->GetGUID()) == team.Bots.end())
            {
                Dismiss(bot->GetGUID(), false);
                ++dismissed;
            }
        for (auto const& [guid, pending] : _pendingLogins)
            if (pending.Owner == owner->GetGUID() && std::find(team.Bots.begin(), team.Bots.end(), guid) == team.Bots.end())
            {
                Dismiss(guid, false);
                ++dismissed;
            }
        ProcessLogouts();
        for (ObjectGuid const& guid : team.Bots)
        {
            if (IsBotGuid(guid) || IsLoggingIn(guid))
                continue;   // already with it
            std::string error;
            if (Summon(owner, guid, error))
                ++summoned;
            else
            {
                std::string name;
                sCharacterCache->GetCharacterNameByGuid(guid, name);
                errors.push_back(name + ": " + error);
            }
        }
    }

    void CompanionMgr::Dismiss(ObjectGuid botGuid, bool keepActive)
    {
        if (std::find_if(_logoutQueue.begin(), _logoutQueue.end(), [&](auto const& p) { return p.first == botGuid; }) == _logoutQueue.end())
            _logoutQueue.emplace_back(botGuid, keepActive);
    }

    void CompanionMgr::DismissAll(Player* owner, bool keepActive)
    {
        for (auto const& [guid, ai] : _ais)
            if (ai->GetOwnerGUID() == owner->GetGUID())
                Dismiss(guid, keepActive);
        for (auto const& [guid, pending] : _pendingLogins)
            if (pending.Owner == owner->GetGUID())
                Dismiss(guid, keepActive);
    }

    void CompanionMgr::ProcessLogouts()
    {
        std::vector<std::pair<ObjectGuid, bool>> queue;
        queue.swap(_logoutQueue);
        for (auto const& [guid, keepActive] : queue)
        {
            if (BotRecord* record = GetRecord(guid))
            {
                record->Active = keepActive;
                SaveRecord(*record);
            }

            // still loading: the login callback drops it
            auto pending = _pendingLogins.find(guid);
            if (pending != _pendingLogins.end())
            {
                delete pending->second.Session;
                _pendingLogins.erase(pending);
                continue;
            }

            auto itr = _sessions.find(guid);
            if (itr == _sessions.end())
                continue;
            WorldSession* session = itr->second;
            _sessions.erase(itr);

            ObjectGuid owner;
            if (CompanionAI* ai = GetAI(guid))
                owner = ai->GetOwnerGUID();
            _ais.erase(guid);

            if (Player* bot = session->GetPlayer())
            {
                // finish a transfer first: logging out in the middle of one loses the player
                while (bot->IsBeingTeleportedFar())
                    session->HandleMoveWorldportAck();
                InterruptGroupCastsOn(bot);
                if (bot->GetGroup())
                    bot->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);
                TC_LOG_INFO("server.worldserver", "Companions: {} leaves", bot->GetName());
            }
            session->LogoutPlayer(true);
            delete session;

            if (!owner.IsEmpty())
                ArrangeFormation(owner);
        }

        // borrowed ones their player asked for: now that they left the city, they log in at him
        for (auto const& [guid, keepActive] : queue)
        {
            auto wanted = _resummons.find(guid);
            if (wanted == _resummons.end())
                continue;
            ObjectGuid const ownerGuid = wanted->second;
            _resummons.erase(wanted);
            std::string error;
            if (Player* owner = ObjectAccessor::FindConnectedPlayer(ownerGuid); owner && !Summon(owner, guid, error))
                ChatHandler(owner->GetSession()).SendSysMessage("Companions: " + error);
        }
    }

    bool CompanionMgr::IsLeaving(ObjectGuid botGuid) const
    {
        return std::any_of(_logoutQueue.begin(), _logoutQueue.end(), [&](auto const& entry) { return entry.first == botGuid; });
    }

    ObjectGuid CompanionMgr::PickIdleCompanion(::Team team) const
    {
        std::vector<ObjectGuid> idle;
        for (auto const& [guid, record] : _records)
        {
            // never a player's own character (he may log in with it), never one that is out, coming or leaving
            if (!record.Created || record.Active || IsBotGuid(guid) || IsLoggingIn(guid) || IsLeaving(guid)
                || ObjectAccessor::FindConnectedPlayer(guid))
                continue;
            CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);
            if (!cache || cache->IsDeleted || cache->Level < 10 || Player::TeamForRace(cache->Race) != team)
                continue;
            idle.push_back(guid);
        }
        return idle.empty() ? ObjectGuid::Empty : Trinity::Containers::SelectRandomContainerElement(idle);
    }

    void CompanionMgr::JoinOwnerGroup(Player* bot, Player* owner)
    {
        Group* group = owner->GetGroup();
        if (group && bot->GetGroup() == group)
            return;

        if (bot->GetGroup())
            bot->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);
        if (bot->GetGroupInvite())
            bot->UninviteFromGroup();

        group = owner->GetGroup();
        if (!group)
        {
            group = new Group();
            if (!group->Create(owner))
            {
                delete group;
                return;
            }
            sGroupMgr->AddGroup(group);
        }

        // companions left in the group by a crash (offline members): they make room
        if (group->IsFull() || (!group->isRaidGroup() && group->GetMembersCount() >= MAX_GROUP_SIZE))
        {
            std::vector<ObjectGuid> stale;
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                if (slot.guid != bot->GetGUID() && !ObjectAccessor::FindConnectedPlayer(slot.guid))
                    if (BotRecord const* record = GetRecord(slot.guid); record && record->Owner == owner->GetGUID())
                        stale.push_back(slot.guid);
            for (ObjectGuid guid : stale)
            {
                group = owner->GetGroup();
                if (!group)
                    break;
                group->RemoveMember(guid, GROUP_REMOVEMETHOD_DEFAULT);
            }
            group = owner->GetGroup();
            if (!group)
            {
                group = new Group();
                if (!group->Create(owner))
                {
                    delete group;
                    return;
                }
                sGroupMgr->AddGroup(group);
            }
        }

        if (group->GetMembersCount() >= GetConfig().RaidSize)
            return;
        // a full party becomes a raid (only its leader can do that)
        if (!group->isRaidGroup() && group->GetMembersCount() >= MAX_GROUP_SIZE)
        {
            if (!group->IsLeader(owner->GetGUID()))
                return;
            group->ConvertToRaid();
        }
        if (group->IsFull())
            return;

        if (group->AddMember(bot))
            group->BroadcastGroupUpdate();
    }

    void CompanionMgr::TeleportToOwner(Player* bot, Player* owner)
    {
        if (!owner->IsInWorld() || owner->IsBeingTeleported() || bot->IsBeingTeleported())
            return;
        if (bot->GetMap() == owner->GetMap() && bot->GetDistance(owner) < 30.0f)
            return;

        InterruptGroupCastsOn(bot);
        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(0.5f);
            bot->SpawnCorpseBones();
        }

        Position pos = owner->GetPosition();
        owner->MovePosition(pos, 2.0f, float(M_PI) + frand(-0.7f, 0.7f));
        if (bot->GetMap() == owner->GetMap())
            bot->NearTeleportTo(pos);
        else
            bot->TeleportTo(owner->GetMapId(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), owner->GetOrientation(),
                TELE_TO_NONE, owner->GetInstanceId());
        if (CompanionAI* ai = GetAI(bot))
            ai->OnOwnerTeleported();
    }

    void CompanionMgr::OnOwnerLogin(Player* owner)
    {
        if (!GetConfig().Enable || !GetConfig().RestoreOnLogin)
            return;
        for (BotRecord const* record : GetRecordsOf(owner->GetGUID()))
        {
            if (record->Active)
            {
                _restoreQueue.emplace_back(owner->GetGUID(), 3000);
                break;
            }
        }
    }

    void CompanionMgr::OnOwnerLogout(Player* owner)
    {
        DismissAll(owner, true);
    }

    void CompanionMgr::OnOwnerLevelChanged(Player* owner)
    {
        if (!GetConfig().SyncLevel)
            return;
        for (Player* bot : GetBotsOf(owner->GetGUID()))
            if (BotRecord* record = GetRecord(bot->GetGUID()))
                if (record->Created && bot->GetLevel() < owner->GetLevel())
                    QueuePrepare(bot->GetGUID(), owner->GetLevel(), GetConfig().AutoGear);
    }

    void CompanionMgr::QueuePrepare(ObjectGuid botGuid, uint8 level, bool regear, bool respec)
    {
        _prepareQueue.push_back({ botGuid, level, regear, respec });
    }

    bool CompanionMgr::DeleteCompanion(ObjectGuid owner, ObjectGuid botGuid, std::string& error)
    {
        BotRecord* record = GetRecord(botGuid);
        if (!record || record->Owner != owner)
        {
            error = "That is not one of your companions.";
            return false;
        }
        if (!record->Created)
        {
            // an alt: only forget it, never delete a real character
            DeleteRecord(botGuid);
            Dismiss(botGuid, false);
            return true;
        }
        if (IsBotGuid(botGuid) || IsLoggingIn(botGuid))
        {
            error = "Dismiss it first.";
            return false;
        }
        uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(botGuid);
        std::string name;
        sCharacterCache->GetCharacterNameByGuid(botGuid, name);
        Player::DeleteFromDB(botGuid, accountId, true, true);
        sCharacterCache->DeleteCharacterCacheEntry(botGuid, name);
        DeleteRecord(botGuid);
        return true;
    }

    // ------------------------------------------------------------------ updates

    void CompanionMgr::Update(uint32 diff)
    {
        if (!GetConfig().Enable)
            return;

        _msTime += diff;
        sCompanionFactory->Update();
        _loginCallbacks.ProcessReadyCallbacks();

        for (auto itr = _restoreQueue.begin(); itr != _restoreQueue.end();)
        {
            if (itr->second > diff)
            {
                itr->second -= diff;
                ++itr;
                continue;
            }
            ObjectGuid ownerGuid = itr->first;
            itr = _restoreQueue.erase(itr);
            Player* owner = ObjectAccessor::FindPlayer(ownerGuid);
            if (!owner)
                continue;
            for (BotRecord const* record : GetRecordsOf(ownerGuid))
            {
                if (!record->Active)
                    continue;
                std::string error;
                if (!Summon(owner, record->Guid, error))
                    TC_LOG_DEBUG("server.worldserver", "Companions: restoring {} failed: {}", record->Guid.ToString(), error);
            }
        }

        ProcessLogouts();
        sCompanionAmbient->Update(diff);
        sPlans->Update(diff);

        std::vector<PendingPrepare> prepares;
        prepares.swap(_prepareQueue);
        for (PendingPrepare const& prepare : prepares)
        {
            CompanionAI* ai = GetAI(prepare.Bot);
            Player* bot = ai ? ai->GetBot() : nullptr;
            if (bot && bot->IsInWorld() && !bot->IsBeingTeleported() && !bot->IsInCombat())
            {
                if (prepare.Respec)
                    sCompanionFactory->Respec(bot, ai->GetEffectiveRole());
                sCompanionFactory->Prepare(bot, prepare.Level, ai->GetRole(), prepare.Regear);
            }
            else if (bot)
                _prepareQueue.push_back(prepare);   // try again when it is out of combat
        }

        for (auto const& [guid, ai] : _ais)
            if (Player* bot = ai->GetBot())
                UpdateBot(bot, ai.get(), diff);
        for (auto const& [guid, session] : _puppets)
            if (Player* puppet = session->GetPlayer())
            {
                AckTeleports(puppet);
                if (puppet->IsInWorld() && !puppet->IsBeingTeleported())
                    EnsureSeesWorld(puppet);
            }

        // forget old heal predictions and interrupt claims
        for (auto itr = _incomingHeals.begin(); itr != _incomingHeals.end();)
            itr = itr->second.Expire <= _msTime ? _incomingHeals.erase(itr) : std::next(itr);
        for (auto itr = _interruptClaims.begin(); itr != _interruptClaims.end();)
            itr = itr->second.second <= _msTime ? _interruptClaims.erase(itr) : std::next(itr);
        for (auto itr = _ccClaims.begin(); itr != _ccClaims.end();)
            itr = itr->second.second <= _msTime ? _ccClaims.erase(itr) : std::next(itr);
        for (auto itr = _tankClaims.begin(); itr != _tankClaims.end();)
            itr = itr->second.second <= _msTime ? _tankClaims.erase(itr) : std::next(itr);
        for (auto itr = _stunClaims.begin(); itr != _stunClaims.end();)
            itr = itr->second.second <= _msTime ? _stunClaims.erase(itr) : std::next(itr);

        CompanionLLM::Update(diff);
    }

    // a client's first time sync answers let the player see the world (Player::CanNeverSee): a headless player sets it itself
    void CompanionMgr::EnsureSeesWorld(Player* player)
    {
        if (player->HasPlayerLocalFlag(PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME))
            return;
        player->SetPlayerLocalFlag(PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME);
        player->SetTransportServerTime(0);
        if (player->IsInWorld())
            player->UpdateObjectVisibility(false);
    }

    // a client acknowledges teleports: a headless player does it itself
    void CompanionMgr::AckTeleports(Player* player)
    {
        WorldSession* session = player->GetSession();
        switch (player->GetTeleportState())
        {
            case TeleportState::WaitingForTeleportAck:
            {
                WorldPackets::Movement::MoveTeleportAck ack{ WorldPacket(CMSG_MOVE_TELEPORT_ACK) };
                ack.MoverGUID = player->GetGUID();
                session->HandleMoveTeleportAck(ack);
                break;
            }
            case TeleportState::WaitingForSuspendTokenResponse:
            case TeleportState::WaitingForWorldPortAck:
                player->SetTeleportState(TeleportState::WaitingForWorldPortAck);
                session->HandleMoveWorldportAck();
                break;
            default:
                break;
        }
    }

    void CompanionMgr::UpdateBot(Player* bot, CompanionAI* ai, uint32 diff)
    {
        WorldSession* session = bot->GetSession();

        TeleportState state = bot->GetTeleportState();
        if (state == TeleportState::WaitingForTeleportAck || state == TeleportState::WaitingForSuspendTokenResponse || state == TeleportState::WaitingForWorldPortAck)
        {
            AckTeleports(bot);
            ai->OnOwnerTeleported();
            return;
        }

        if (!bot->IsInWorld() || bot->IsBeingTeleported())
            return;
        EnsureSeesWorld(bot);

        if (ai->IsAmbient())
        {
            UpdateAmbientBot(bot, ai);
            return;
        }

        // connected, not "in world": between two maps (a loading screen into a dungeon) the owner is in no map, and the
        // bots wait for him instead of leaving
        Player* owner = ObjectAccessor::FindConnectedPlayer(ai->GetOwnerGUID());
        if (!owner)
        {
            Dismiss(bot->GetGUID(), true);
            return;
        }
        if (!owner->IsInWorld() || owner->IsBeingTeleported())
            return;

        // invited by the owner's group (e.g. /invite from the client): accept
        if (Group* invite = bot->GetGroupInvite())
        {
            if (invite == owner->GetGroup() || invite->GetLeaderGUID() == owner->GetGUID())
            {
                WorldPackets::Party::PartyInviteResponse response{ WorldPacket(CMSG_PARTY_INVITE_RESPONSE) };
                response.Accept = true;
                session->HandlePartyInviteResponseOpcode(response);
            }
            else
                bot->UninviteFromGroup();
        }

        if (!IsLeaving(bot->GetGUID()) && (!bot->GetGroup() || bot->GetGroup() != owner->GetGroup()))
            JoinOwnerGroup(bot, owner);

        // the owner changed map (zoned into a dungeon, took a boat, hearthed): follow him there
        if (bot->GetMap() != owner->GetMap())
        {
            uint32& timer = _ownerAwayTimers[bot->GetGUID()];
            timer += diff;
            if (timer >= 2000)
            {
                timer = 0;
                TeleportToOwner(bot, owner);
            }
        }
        else
            _ownerAwayTimers.erase(bot->GetGUID());
    }

    void CompanionMgr::OnBotPlayerUpdate(Player* player, uint32 diff)
    {
        if (CompanionAI* ai = instance()->GetAI(player->GetGUID()))
            ai->Update(diff);
    }

    void CompanionMgr::AddIncomingHeal(ObjectGuid target, uint32 amount, uint32 castTimeMs)
    {
        _incomingHeals.emplace(target, IncomingHeal{ amount, _msTime + castTimeMs + 300 });
    }

    uint32 CompanionMgr::GetIncomingHeal(ObjectGuid target) const
    {
        uint32 total = 0;
        auto range = _incomingHeals.equal_range(target);
        for (auto itr = range.first; itr != range.second; ++itr)
            if (itr->second.Expire > _msTime)
                total += itr->second.Amount;
        return total;
    }

    void CompanionMgr::RemoveIncomingHeal(ObjectGuid target, uint32 amount)
    {
        auto range = _incomingHeals.equal_range(target);
        for (auto itr = range.first; itr != range.second; ++itr)
            if (itr->second.Amount == amount && itr->second.Expire > _msTime)
            {
                _incomingHeals.erase(itr);
                return;
            }
    }

    bool CompanionMgr::CanInterrupt(ObjectGuid caster, ObjectGuid bot) const
    {
        auto itr = _interruptClaims.find(caster);
        return itr == _interruptClaims.end() || itr->second.first == bot || itr->second.second <= _msTime;
    }

    namespace
    {
        // the spell of a group member aimed at this player or at its corpse
        template <typename Fn>
        void ForGroupCastsOn(Player* target, Fn&& fn)
        {
            Group* group = target->GetGroup();
            if (!group)
                return;
            ObjectGuid corpse;
            if (Corpse* body = target->GetCorpse())
                corpse = body->GetGUID();
            for (GroupReference const& ref : group->GetMembers())
            {
                Player* member = ref.GetSource();
                if (!member || member == target || !member->IsInWorld())
                    continue;
                for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
                    if (Spell* spell = member->GetCurrentSpell(type))
                    {
                        ObjectGuid aimed = spell->m_targets.GetObjectTargetGUID();
                        if (aimed == target->GetGUID() || (!corpse.IsEmpty() && aimed == corpse) || spell->m_targets.GetUnitTargetGUID() == target->GetGUID())
                            fn(member, type);
                    }
            }
        }
    }

    bool CompanionMgr::IsTargetOfGroupCast(Player* target)
    {
        bool found = false;
        ForGroupCastsOn(target, [&found](Player*, CurrentSpellTypes) { found = true; });
        return found;
    }

    void CompanionMgr::InterruptGroupCastsOn(Player* target)
    {
        ForGroupCastsOn(target, [](Player* member, CurrentSpellTypes type) { member->InterruptSpell(type); });
    }

    void CompanionMgr::ClaimCrowdControl(ObjectGuid enemy, ObjectGuid bot, uint32 durationMs)
    {
        _ccClaims[enemy] = { bot, _msTime + durationMs };
    }

    bool CompanionMgr::IsCrowdControlClaimed(ObjectGuid enemy, ObjectGuid notBy) const
    {
        auto itr = _ccClaims.find(enemy);
        return itr != _ccClaims.end() && itr->second.first != notBy && itr->second.second > _msTime;
    }

    void CompanionMgr::ClaimTank(ObjectGuid enemy, ObjectGuid bot, uint32 durationMs)
    {
        _tankClaims[enemy] = { bot, _msTime + durationMs };
    }

    bool CompanionMgr::IsTankClaimed(ObjectGuid enemy, ObjectGuid notBy) const
    {
        auto itr = _tankClaims.find(enemy);
        return itr != _tankClaims.end() && itr->second.first != notBy && itr->second.second > _msTime;
    }

    void CompanionMgr::CommitInterrupt(ObjectGuid caster, ObjectGuid bot)
    {
        _interruptClaims[caster] = { bot, _msTime + 1500 };
    }

    bool CompanionMgr::LoginPuppet(ObjectGuid guid, std::string& error)
    {
        if (ObjectAccessor::FindConnectedPlayer(guid) || _puppets.count(guid))
        {
            error = "already online";
            return false;
        }
        CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);
        if (!cache)
        {
            error = "no such character";
            return false;
        }
        WorldSession* session = CreateBotSession(cache->AccountId, nullptr);
        std::shared_ptr<LoginQueryHolder> holder = std::make_shared<LoginQueryHolder>(cache->AccountId, guid);
        if (!holder->Initialize())
        {
            delete session;
            error = "cannot load";
            return false;
        }
        _puppets[guid] = session;
        _loginCallbacks.AddCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete([this, guid, session](SQLQueryHolderBase const& result)
        {
            auto itr = _puppets.find(guid);
            if (itr == _puppets.end() || itr->second != session)
                return;
            session->HandlePlayerLogin(static_cast<LoginQueryHolder const&>(result));
            if (!session->GetPlayer())
            {
                _puppets.erase(guid);
                delete session;
                TC_LOG_ERROR("server.worldserver", "Companions: puppet {} failed to log in", guid.ToString());
                return;
            }
            TC_LOG_INFO("server.worldserver", "Companions: puppet owner {} is in the world", session->GetPlayer()->GetName());
        });
        return true;
    }

    void CompanionMgr::LogoutPuppet(ObjectGuid guid)
    {
        auto itr = _puppets.find(guid);
        if (itr == _puppets.end())
            return;
        WorldSession* session = itr->second;
        _puppets.erase(itr);
        if (Player* puppet = session->GetPlayer())
        {
            DismissAll(puppet, true);
            ProcessLogouts();
            while (puppet->IsBeingTeleportedFar())
                session->HandleMoveWorldportAck();
        }
        session->LogoutPlayer(true);
        delete session;
    }

    Player* CompanionMgr::GetPuppet(std::string const& name) const
    {
        std::string lower = ToLower(name);
        for (auto const& [guid, session] : _puppets)
            if (Player* puppet = session->GetPlayer())
                if (ToLower(puppet->GetName()) == lower)
                    return puppet;
        return nullptr;
    }

    void CompanionMgr::Shutdown()
    {
        _shuttingDown = true;
        _ais.clear();
        for (auto const& [guid, session] : _sessions)
        {
            if (Player* bot = session->GetPlayer())
                while (bot->IsBeingTeleportedFar())
                    session->HandleMoveWorldportAck();
            session->LogoutPlayer(true);
            delete session;
        }
        _sessions.clear();
        for (auto const& [guid, session] : _puppets)
        {
            if (Player* puppet = session->GetPlayer())
                while (puppet->IsBeingTeleportedFar())
                    session->HandleMoveWorldportAck();
            session->LogoutPlayer(true);
            delete session;
        }
        _puppets.clear();
        for (auto const& [guid, pending] : _pendingLogins)
            delete pending.Session;
        _pendingLogins.clear();
        BotSessionHooks::OnPlayerUpdate = nullptr;
    }
}
