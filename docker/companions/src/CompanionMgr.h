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
        // what players told it about its abilities: "prefer|frost nova|0|adds reach the healer;avoid|arcane explosion|0|"
        std::string Prefs;
        uint8 LootMode = 0;         // 0 what it can use, 1 everything, 2 nothing
        bool UsePotions = true;     // potions and healthstones from its bags when low
        bool AutoCC = true;         // controls an extra add of a pack on its own
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
        // teams (characters.companion_teams, made in the web panel): the owner's companions summoned together; the ones not in
        // the team leave first so the team has room
        struct Team { uint32 Id = 0; std::string Name; std::vector<ObjectGuid> Bots; };
        std::vector<Team> GetTeamsOf(ObjectGuid owner) const;      // from the database (world thread, a small query)
        void SummonTeam(Player* owner, Team const& team, uint32& summoned, uint32& dismissed, std::vector<std::string>& errors);

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
        uint32 CountAll() const;                                  // companions on the whole server (ambient bots not counted)
        uint32 CountAmbient() const;                              // ambient bots online or logging in

        // ---- ambient bots (CompanionAmbient.h) ----
        // log a character of the pool in at that spot, at least that level, living on its own there; borrowed: a player's
        // companion not out with its player, about town as it is (no new level, gear or name)
        bool LoginAmbient(ObjectGuid botGuid, WorldLocation const& where, uint32 zoneId, uint8 level, bool city, bool borrowed = false);
        // a companion (made by the companions, not a player's own character) that is not out and not online, of that side
        ObjectGuid PickIdleCompanion(::Team team) const;
        bool IsLeaving(ObjectGuid botGuid) const;       // dismissed, logs out on the next update
        std::vector<Player*> GetPuppets() const;
        bool IsLoggingIn(ObjectGuid botGuid) const { return _pendingLogins.count(botGuid) != 0; }
        void ArrangeFormation(ObjectGuid owner);

        // ---- shared combat knowledge (map thread, single map updater thread) ----
        // a heal on its way: other healers count it before choosing their target
        void AddIncomingHeal(ObjectGuid target, uint32 amount, uint32 castTimeMs);
        uint32 GetIncomingHeal(ObjectGuid target) const;
        void RemoveIncomingHeal(ObjectGuid target, uint32 amount);     // a heal cancelled before it landed
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
        // several tanks: the loose enemy one of them went for is its own for a moment (no two run for the same one)
        void ClaimTank(ObjectGuid enemy, ObjectGuid bot, uint32 durationMs);
        bool IsTankClaimed(ObjectGuid enemy, ObjectGuid notBy) const;
        // an area taunt (Challenging Shout, Roar): one tank of a group at a time, the others keep theirs for the next crisis
        bool IsAoeTauntClaimed(ObjectGuid group) const
        {
            auto itr = _aoeTauntClaims.find(group);
            return itr != _aoeTauntClaims.end() && itr->second > _msTime;
        }
        void ClaimAoeTaunt(ObjectGuid group, uint32 durationMs) { _aoeTauntClaims[group] = _msTime + durationMs; }
        // a stun lock: one stun at a time on the same enemy
        void ClaimStun(ObjectGuid enemy, ObjectGuid bot, uint32 durationMs) { _stunClaims[enemy] = { bot, _msTime + durationMs }; }
        bool IsStunClaimed(ObjectGuid enemy, ObjectGuid notBy) const
        {
            auto itr = _stunClaims.find(enemy);
            return itr != _stunClaims.end() && itr->second.first != notBy && itr->second.second > _msTime;
        }

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
        void FinishAmbientLogin(WorldSession* session, ObjectGuid botGuid, Position const& home, uint8 level, bool city, bool borrowed);
        void UpdateAmbientBot(Player* bot, CompanionAI* ai);
        bool AdoptAmbient(Player* bot, CompanionAI* ai, Player* inviter, std::string& why);   // invited: a companion now
        void ProcessLogouts();
        void UpdateBot(Player* bot, CompanionAI* ai, uint32 diff);
        void JoinOwnerGroup(Player* bot, Player* owner);
        void TeleportToOwner(Player* bot, Player* owner);

        struct PendingLogin
        {
            WorldSession* Session = nullptr;
            ObjectGuid Owner;
            bool Ambient = false;
        };

        uint32 _botAccountId = 0;
        std::unordered_map<ObjectGuid, BotRecord> _records;
        std::unordered_map<ObjectGuid, WorldSession*> _sessions;               // logged in bots
        std::unordered_map<ObjectGuid, std::unique_ptr<CompanionAI>> _ais;
        std::unordered_map<ObjectGuid, PendingLogin> _pendingLogins;
        std::vector<std::pair<ObjectGuid, bool>> _logoutQueue;                  // bot, keep active
        std::unordered_map<ObjectGuid, ObjectGuid> _resummons;                  // borrowed bot -> owner who wants it: logs in at him after its logout
        std::vector<std::pair<ObjectGuid, uint32>> _restoreQueue;               // owner, delay ms
        struct PendingPrepare { ObjectGuid Bot; uint8 Level; bool Regear; bool Respec; };
        std::vector<PendingPrepare> _prepareQueue;
        AsyncCallbackProcessor<SQLQueryHolderCallback> _loginCallbacks;
        std::unordered_map<ObjectGuid, uint32> _ownerAwayTimers;                // bot -> ms the owner has been on another map
        struct IncomingHeal { uint32 Amount; uint32 Expire; };
        std::unordered_multimap<ObjectGuid, IncomingHeal> _incomingHeals;
        std::unordered_map<ObjectGuid, std::pair<ObjectGuid, uint32>> _interruptClaims;
        std::unordered_map<ObjectGuid, std::pair<ObjectGuid, uint32>> _ccClaims;
        std::unordered_map<ObjectGuid, std::pair<ObjectGuid, uint32>> _tankClaims;
        std::unordered_map<ObjectGuid, std::pair<ObjectGuid, uint32>> _stunClaims;
        std::unordered_map<ObjectGuid, uint32> _aoeTauntClaims;
        std::unordered_map<ObjectGuid, WorldSession*> _puppets;
        uint32 _msTime = 0;
        bool _shuttingDown = false;
    };
}

#define sCompanionMgr Companions::CompanionMgr::instance()

#endif
