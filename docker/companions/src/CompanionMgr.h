/*
 * Companions: owns the bot sessions (world thread), logs bots in and out, keeps them with their owner.
 */

#ifndef COMPANION_MGR_H
#define COMPANION_MGR_H

#include "AsyncCallbackProcessor.h"
#include "CompanionDefines.h"
#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"
#include "QueryHolder.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class WorldSession;

namespace Companions
{
    class CompanionAI;

    // A row of the companion_bots table
    struct BotRecord
    {
        ObjectGuid Guid;
        ObjectGuid Owner;
        Role BotRole = Role::Auto;
        bool Active = false;        // logged in with its owner (restored at the owner's next login)
        bool Created = false;       // made by ".bot create" (companion account), else the owner's alt
        bool UseLLM = true;
        std::string Personality;
        std::string Spec;           // talent tabs in spending order, e.g. "2,0" (empty = from the role)
        uint8 GearQuality = 0;      // highest item quality (0 = Companions.Gear.MaxQuality)
        Aggression AggressionMode = Aggression::Assist;
        Stance StanceMode = Stance::Follow;
        uint32 TemplateId = 0;      // web panel template the bot was made from
        uint32 LLMProvider = 0;     // AI provider answering for it (0 = any)
    };

    class CompanionMgr
    {
    public:
        static CompanionMgr* instance();

        void Initialize();          // world startup: tables, accounts, records
        void Update(uint32 diff);   // world thread, after the maps
        void Shutdown();

        // ---- owner actions (world thread) ----
        // log a bot in for the owner: a companion created for him, or (AllowAlts) one of his own characters
        bool Summon(Player* owner, ObjectGuid botGuid, std::string& error);
        void Dismiss(ObjectGuid botGuid, bool keepActive = false);
        void DismissAll(Player* owner, bool keepActive);
        void OnOwnerLogin(Player* owner);
        void OnOwnerLogout(Player* owner);
        void OnOwnerLevelChanged(Player* owner);
        void QueuePrepare(ObjectGuid botGuid, uint8 level, bool regear, bool respec = false);   // re-level/train/gear on the world thread
        bool DeleteCompanion(ObjectGuid owner, ObjectGuid botGuid, std::string& error);

        // ---- bot records ----
        void AddRecord(BotRecord const& record);
        void DeleteRecord(ObjectGuid botGuid);
        BotRecord* GetRecord(ObjectGuid botGuid);
        std::vector<BotRecord const*> GetRecordsOf(ObjectGuid owner) const;
        void SaveRecord(BotRecord const& record);

        // ---- online bots ----
        bool IsBot(Player const* player) const;
        bool IsBotGuid(ObjectGuid guid) const { return _ais.count(guid) != 0; }
        CompanionAI* GetAI(ObjectGuid botGuid) const;
        CompanionAI* GetAI(Player const* bot) const;
        std::vector<Player*> GetBotsOf(ObjectGuid owner) const;   // online and in the world
        uint32 CountBotsOf(ObjectGuid owner) const;               // online or logging in
        uint32 CountAll() const { return uint32(_ais.size() + _pendingLogins.size()); }   // whole server
        bool IsLoggingIn(ObjectGuid botGuid) const { return _pendingLogins.count(botGuid) != 0; }
        void ArrangeFormation(ObjectGuid owner);

        // ---- shared combat knowledge (map thread, single map updater thread) ----
        // a heal on its way: other healers count it before choosing their target
        void AddIncomingHeal(ObjectGuid target, uint32 amount, uint32 castTimeMs);
        uint32 GetIncomingHeal(ObjectGuid target) const;
        // only one bot interrupts a given cast
        bool CanInterrupt(ObjectGuid caster, ObjectGuid bot) const;
        void CommitInterrupt(ObjectGuid caster, ObjectGuid bot);
        // casts of the group aimed at this player or its corpse (a resurrection): checked before a bot revives, teleports or
        // leaves, so no cast loses its target halfway
        static bool IsTargetOfGroupCast(Player* target);
        static void InterruptGroupCastsOn(Player* target);
        // an enemy a bot keeps crowd controlled: the others do not hit it
        void ClaimCrowdControl(ObjectGuid enemy, ObjectGuid bot, uint32 durationMs);
        bool IsCrowdControlClaimed(ObjectGuid enemy, ObjectGuid notBy) const;

        // Player::Update hook (map thread)
        static void OnBotPlayerUpdate(Player* player, uint32 diff);

        // test harness (".bot test", admins only): a headless stand-in owner, to check bots without a game client
        bool LoginPuppet(ObjectGuid guid, std::string& error);
        void LogoutPuppet(ObjectGuid guid);
        Player* GetPuppet(std::string const& name) const;

        uint32 GetBotAccountId() const { return _botAccountId; }
        uint32 GetOrCreateCreationAccount();
        static void EnsureColumn(char const* table, char const* column, char const* definition);

    private:
        CompanionMgr() = default;

        void LoadRecords();
        void EnsureTables();
        WorldSession* CreateBotSession(uint32 accountId, Player* owner);
        static void AckTeleports(Player* player);
        static void EnsureSeesWorld(Player* player);
        void FinishLogin(WorldSession* session, ObjectGuid botGuid, ObjectGuid ownerGuid);
        void ProcessLogouts();
        void UpdateBot(Player* bot, CompanionAI* ai, uint32 diff);
        void JoinOwnerGroup(Player* bot, Player* owner);
        void TeleportToOwner(Player* bot, Player* owner);

        struct PendingLogin
        {
            WorldSession* Session = nullptr;
            ObjectGuid Owner;
        };

        uint32 _botAccountId = 0;
        std::unordered_map<ObjectGuid, BotRecord> _records;
        std::unordered_map<ObjectGuid, WorldSession*> _sessions;               // logged in bots
        std::unordered_map<ObjectGuid, std::unique_ptr<CompanionAI>> _ais;
        std::unordered_map<ObjectGuid, PendingLogin> _pendingLogins;
        std::vector<std::pair<ObjectGuid, bool>> _logoutQueue;                  // bot, keep active
        std::vector<std::pair<ObjectGuid, uint32>> _restoreQueue;               // owner, delay ms
        struct PendingPrepare { ObjectGuid Bot; uint8 Level; bool Regear; bool Respec; };
        std::vector<PendingPrepare> _prepareQueue;
        AsyncCallbackProcessor<SQLQueryHolderCallback> _loginCallbacks;
        std::unordered_map<ObjectGuid, uint32> _ownerAwayTimers;                // bot -> ms the owner has been on another map
        struct IncomingHeal { uint32 Amount; uint32 Expire; };
        std::unordered_multimap<ObjectGuid, IncomingHeal> _incomingHeals;
        std::unordered_map<ObjectGuid, std::pair<ObjectGuid, uint32>> _interruptClaims;
        std::unordered_map<ObjectGuid, std::pair<ObjectGuid, uint32>> _ccClaims;
        std::unordered_map<ObjectGuid, WorldSession*> _puppets;
        uint32 _msTime = 0;
        bool _shuttingDown = false;
    };
}

#define sCompanionMgr Companions::CompanionMgr::instance()

#endif
