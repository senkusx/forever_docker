/*
 * Companions: the bot brain. One CompanionAI per logged-in bot, updated from Player::Update (map thread).
 */

#ifndef COMPANION_AI_H
#define COMPANION_AI_H

#include "CompanionDefines.h"
#include "Duration.h"
#include "GameTime.h"
#include "ObjectGuid.h"
#include "Optional.h"
#include "Position.h"
#include "SharedDefines.h"
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Creature;
class Group;
class Item;
struct ItemTemplate;
struct Loot;
class Player;
class SpellInfo;
class Unit;

namespace Companions
{
    class CompanionAI;

    // The bot's spells by (lower case, enUS) name, lowest rank first
    class SpellBook
    {
    public:
        void Rebuild(Player* bot);
        uint32 Best(std::string_view name) const;                       // highest known rank, 0 if unknown
        std::vector<uint32> const* Ranks(std::string_view name) const;  // all known ranks, lowest first
        bool Has(std::string_view name) const { return Best(name) != 0; }
        size_t Size() const { return _ranks.size(); }
        std::vector<std::string> Names() const;

    private:
        std::unordered_map<std::string, std::vector<uint32>> _ranks;
    };

    // Allies and enemies the bot considers this tick (same map only)
    struct GroupView
    {
        std::vector<Player*> Members;   // includes the bot and the owner when on the same map
        Player* Owner = nullptr;
        Player* Tank = nullptr;         // the bot with the tank role, else the owner
        std::vector<Unit*> Enemies;     // hostile units in combat with a member
    };

    // what a player told a bot about one of its abilities ("use more frost nova, adds keep reaching the healer")
    struct SpellPref
    {
        std::string Spell;          // lower case, as the spell book names it
        bool Avoid = false;         // never cast it on its own
        uint8 BelowPct = 0;         // prefer it only below this much health (0 = whenever it helps)
        std::string Reason;
    };

    enum class HealUrgency : uint8
    {
        TopUp,      // out of combat or light damage: efficient heals
        Normal,
        Emergency,  // about to die: fastest heal
    };

    // Class specific decisions. Every method returns true when it cast or otherwise used the bot's turn.
    class ClassAI
    {
    public:
        explicit ClassAI(CompanionAI& ai) : _ai(ai) { }
        virtual ~ClassAI() = default;

        virtual Role DefaultRole() const = 0;
        virtual bool CanTank() const { return false; }
        virtual bool CanHeal() const { return false; }
        // distance kept from the target for ranged roles (0 = melee)
        virtual float RangedDistance() const { return 25.0f; }

        virtual bool SelfBuffs(bool /*inCombat*/) { return false; }
        virtual bool GroupBuffs(Player* /*member*/) { return false; }
        virtual bool Heal(Unit* /*target*/, HealUrgency /*urgency*/, uint32 /*deficit*/) { return false; }
        virtual bool GroupHeal(uint32 /*injuredCount*/) { return false; }
        virtual bool Dispel(Unit* /*target*/) { return false; }
        virtual bool Resurrect(Player* /*dead*/) { return false; }
        virtual bool Interrupt(Unit* /*caster*/) { return false; }
        virtual bool Taunt(Unit* /*target*/) { return false; }
        virtual bool AoeTaunt() { return false; }                       // several enemies loose around the tank
        virtual bool CrowdControl(Unit* /*target*/) { return false; }
        virtual bool CanCrowdControl(Unit const* /*target*/) const { return false; }
        virtual bool Emergency() { return false; }      // self preservation in combat
        virtual bool Pull(Unit* target) { return Combat(target); }
        virtual bool Combat(Unit* target) = 0;          // damage / threat rotation
        virtual bool Pet(Unit* /*target*/) { return false; }
        virtual bool OutOfCombat() { return false; }    // forms, pets, totems, imbues...
        // the buffs this class gives this member (asked again with "rebuff", even when they are still up)
        virtual std::vector<std::string_view> OwnBuffs(Player* /*member*/) { return { }; }
        virtual void OnRoleChanged() { }

    protected:
        CompanionAI& _ai;
    };

    std::unique_ptr<ClassAI> CreateClassAI(CompanionAI& ai, uint8 classId);

    class CompanionAI
    {
    public:
        CompanionAI(Player* bot, ObjectGuid owner, Role role);
        ~CompanionAI();

        CompanionAI(CompanionAI const&) = delete;
        CompanionAI& operator=(CompanionAI const&) = delete;

        void Update(uint32 diff);

        // ---- orders (called from commands on the world thread, or from the AI itself) ----
        void SetStance(Stance stance);
        void SetAggression(Aggression aggression) { _aggression = aggression; }
        void SetRole(Role role);
        void OrderAttack(ObjectGuid target, bool pull);
        void OrderStop();
        void OrderComeHere();
        void OrderRest() { _restRequested = true; }
        void OrderHeal(ObjectGuid target) { _healTarget = target; _healUntil = GameTime::Now() + Seconds(8); }
        void OrderDispel(ObjectGuid target) { _dispelTarget = target; _dispelUntil = GameTime::Now() + Seconds(8); }
        void OrderProtect(ObjectGuid target) { _protectTarget = target; _protectUntil = GameTime::Now() + Seconds(25); }
        void OrderRetreat();
        void OrderCast(std::string spell, ObjectGuid target) { _pendingCast = std::move(spell); _pendingCastTarget = target; }
        // tactics (players or the AI brain): keep this enemy crowd controlled, interrupt this caster, use cooldowns now
        void OrderCrowdControl(ObjectGuid target);
        void ReleaseCrowdControl() { _ccTarget.Clear(); _ccCasts = 0; }
        void OrderInterrupt(ObjectGuid target) { _interruptTarget = target; _interruptUntil = GameTime::Now() + Seconds(30); }
        void OrderBurst() { _burstUntil = GameTime::Now() + Seconds(20); }
        // buffs asked for: a blessing of choice for someone ("salvation on me"), or the buffs again now ("rebuff me intellect")
        void SetBlessingFor(ObjectGuid member, std::string kind) { _blessingFor[member] = std::move(kind); }
        std::string BlessingFor(ObjectGuid member) const;
        bool OrderRebuff(Player* member, std::string const& filter);   // false: none of its buffs matches
        // loot: equip something from its bags ("it" = the last item it received, "upgrades" = all that are better)
        std::string OrderEquip(std::string const& what);
        void SetFollowSlot(uint32 slot, uint32 count) { _followSlot = slot; _followCount = count; _movement = MoveMode::None; }
        void SetSpread(float spread) { _spread = spread; _loitering = false; _movement = MoveMode::None; }
        void RefreshSpells() { _spellsDirty = true; }

        // ---- state ----
        Player* GetBot() const { return _bot; }
        ObjectGuid GetOwnerGUID() const { return _owner; }
        Role GetRole() const { return _role; }
        Role GetEffectiveRole() const;
        Stance GetStance() const { return _stance; }
        Aggression GetAggression() const { return _aggression; }
        ClassAI* GetClassAI() const { return _class.get(); }
        SpellBook const& Spells() const { return _spells; }
        GroupView const& View() const { return _view; }
        Unit* GetCurrentTarget() const;
        std::string StatusLine() const;
        bool IsPulling() const { return _pulling && !_forcedTarget.IsEmpty(); }
        ObjectGuid GetPullTarget() const { return _pulling ? _forcedTarget : ObjectGuid::Empty; }
        // an enemy this bot was told to crowd control: the others leave it alone while it does
        bool IsAssignedCrowdControl(Unit const* unit) const;
        bool AttackedByGroup(Unit const* unit) const;
        std::string RecentCasts() const;    // test harness: what it did lately
        std::string MechanicsLine() const;  // test harness: the boss mechanics it follows right now

        // ---- helpers for ClassAI ----
        uint32 SpellIdOf(std::string_view name) const { return _spells.Best(name); }
        bool Knows(std::string_view name) const { return _spells.Has(name); }
        // cast the best known rank; target nullptr = self
        bool Cast(std::string_view name, Unit* target = nullptr);
        bool CastId(uint32 spellId, Unit* target = nullptr);
        bool CanCast(std::string_view name, Unit* target = nullptr) const;
        bool CanCastId(uint32 spellId, Unit* target = nullptr) const;
        // heal with the cheapest rank that covers the deficit
        bool CastHeal(std::string_view name, Unit* target, uint32 deficit);
        bool HasAura(Unit const* unit, std::string_view name, bool mineOnly = false) const;
        int32 AuraRemaining(Unit const* unit, std::string_view name, bool mineOnly = false) const;   // ms, 0 = absent, -1 = permanent
        uint32 AuraStacks(Unit const* unit, std::string_view name, bool mineOnly = false) const;
        bool IsSpellReady(std::string_view name) const;
        uint32 DispellableMask(Unit* target) const;    // 1 << DispelType of harmful auras we could remove

        float HealthPct() const;
        float ManaPct() const;
        int32 Rage() const;         // 0..100
        int32 Energy() const;
        int32 ComboPoints(Unit const* target) const;
        bool IsBehind(Unit const* target) const;
        bool InMelee(Unit const* target) const;
        uint32 EnemiesNear(Position const& pos, float radius) const;           // engaged enemies around a point
        uint32 EnemiesAttacking(Unit const* unit) const;
        bool AnyCrowdControlledNear(Position const& pos, float radius) const;  // AoE would break a CC
        bool IsTank(Unit const* unit) const;
        bool IsHealer(Unit const* unit) const;
        Unit* LowestAlly(float maxPct, float range, bool includePets = false) const;
        uint32 InjuredAllies(float maxPct, float range) const;
        bool IsBossFight() const;
        bool Bursting() const;              // boss fight or ordered: offensive cooldowns now
        bool IsTargetedByEnemy() const;
        Unit* EnemyAttackingMe() const;
        bool IsMoving() const;
        void StopMovement();
        bool HasPet() const;
        Creature* GetPet() const;
        void PetAttack(Unit* target);
        void PetFollow();
        Item* MainHand() const;
        bool HasShield() const;
        bool UsesWand() const;
        void Say(std::string const& text);  // party chat (or say when alone)

        void OnOwnerTeleported() { _movement = MoveMode::None; }

        // ambient bots (CompanionAmbient.h): no owner, they live their own life around home (grinding, questing, the
        // auction house...); invited to a group they become a companion of whoever invited them
        void SetAmbient(Position const& home, bool city);
        bool IsAmbient() const { return _ambient; }
        bool IsAmbientCity() const { return _ambientCity; }
        bool AmbientFinished() const { return _ambientFinished; }  // its errand is done: it may leave the world
        void BecomeCompanion(ObjectGuid owner);
        std::string AmbientLine() const;    // test harness: what it is doing

        // ---- spell preferences (CompanionAIPrefs.cpp) ----
        void LoadPrefs(std::string const& text);
        std::string SavePrefs() const;
        std::vector<SpellPref> const& Prefs() const { return _prefs; }
        bool SetPref(std::string const& spell, bool avoid, uint8 belowPct, std::string const& reason);
        bool ClearPref(std::string const& spell);
        void ClearPrefs() { _prefs.clear(); }
        std::string MatchSpell(std::string const& text) const;     // the longest spell it knows named in the text
        bool IsAvoided(std::string_view name) const;
        std::string PrefsLine() const;      // for the brain and "prefs": what it was told
        // behaviour settings of its record
        void SetUsePotions(bool on) { _usePotions = on; }
        void SetAutoCC(bool on) { _autoCC = on; }
        void SetLootMode(uint8 mode) { _lootMode = mode; }
        uint8 GetLootMode() const { return _lootMode; }
        // ---- items (CompanionAIItems.cpp) ----
        void UpdateTrade();                 // answers trade windows: opens, gives what was asked, accepts
        bool OrderGive(Player* to, std::string const& what, std::string& reply);   // "give me your potions"
        bool UpdateLooting();               // walks to its corpses and takes what it can use
        bool UsePotion();                   // in combat: a potion or healthstone when low
        void OnTradeFinished();             // thanks the trader, tells what it will do with it
        void OnRevived();

    private:
        enum class MoveMode : uint8 { None, Follow, Chase, Stay, Point };

        void Think(uint32 diff);
        void HandleRequests();
        void HandleDeath(uint32 diff);
        void BuildView();
        void UpdateCombat();
        void UpdateIdle();
        bool UpdateHealing(bool inCombat);
        bool UpdateDispels();
        bool UpdateInterrupts();
        bool UpdateResurrections();
        bool UpdateBuffs(bool inCombat);
        bool UpdateEatDrink();
        bool UpdatePendingCast();
        void UpdateMount();
        Unit* SelectTarget();
        Unit* SelectTankTarget();
        bool IsValidEnemy(Unit* unit, float maxDistance = 60.0f) const;   // ordered targets reach further
        bool IsCrowdControlTarget(Unit const* unit) const;
        void MoveForCombat(Unit* target);
        void FollowOwner();
        bool PickLoiterSpot(Player* owner, float angle, float halfWidth, float minDist, float maxDist, Position& spot) const;
        // where this bot stands around the owner: tank in front, melee beside, healers and casters behind
        void FormationSlot(bool travelling, float& angle, float& halfWidth, float& minDist, float& maxDist) const;
        bool FollowPullingTank();
        bool AvoidGroundEffects();      // out of fire, poison clouds and other hostile ground effects
        // boss mechanics (CompanionMechanics.h): out of casts, debuffs, fronts and lines; spreading, polarity, hiding
        bool UpdateMechanics();
        bool MechanicSpells();                          // Fear Ward, Tranquilizing Shot, Purge, crowd control the mind controlled
        bool IsMechanicTarget(Unit const* unit) const;  // false: a rule says this bot leaves it alone (immune, reflecting...)
        Unit* KillFirstTarget() const;
        bool HoldsForMechanic() const { return GameTime::Now() < _mechanicHold; }
        uint8 RoleBit() const;
        bool InDangerZone(Position const& pos, float margin) const;
        bool InLiquid(float x, float y, float z, bool harmfulOnly) const;   // lava and slime (or any liquid) at that spot
        void HandleLoot();              // roll Need on upgrades, Pass on the rest; put on what it won
        Item* ScanBags();
        bool AutoCrowdControl(Unit* killTarget);   // a pack: control one add, like a mage sheeps without being asked
        void EngageTarget(Unit* target, bool melee);
        void FaceTarget(Unit* target);
        void DisengageAll();
        bool EnsureReagents(SpellInfo const* info);
        bool NeedsRest() const;
        void UpdateAmbient();
        bool UsePreferred(Unit* target);    // the abilities it was asked to use more
        bool HealPreferred(Unit* target, uint32 deficit);
        bool WantsLoot(ItemTemplate const* proto) const;
        bool WantsSomething(Loot const* loot) const;
        void LootCorpse(Creature* corpse);
        bool AmbientGrind();
        bool AmbientVisit();
        bool AmbientWander(float range);
        // its job for this life (the world: hunting one kind of mob, questing; cities: trading, crafting, errands, the inn)
        void ChooseJob();
        bool JobHunt();
        bool JobQuest();
        bool JobCity();
        bool ChooseHuntingGround(Position const& around, float range);
        bool GoTo(Creature* npc, uint32 dwellMs);
        Creature* FindNpc(uint32 npcFlags, float range, std::function<bool(Creature*)> const& filter = nullptr) const;

        Player* _bot;
        ObjectGuid _owner;
        Role _role;
        Stance _stance = Stance::Follow;
        Aggression _aggression = Aggression::Assist;
        std::unique_ptr<ClassAI> _class;
        SpellBook _spells;
        bool _spellsDirty = true;
        GroupView _view;

        uint32 _thinkTimer = 0;
        uint32 _slowTimer = 0;          // ~1 s housekeeping
        uint32 _followSlot = 0;
        uint32 _followCount = 1;
        MoveMode _movement = MoveMode::None;
        ObjectGuid _moveTarget;
        float _moveRangeMin = 0.0f;
        float _moveRangeMax = 0.0f;
        float _moveAngle = -1.0f;       // chase angle asked for (-1: none)
        Position _stayPosition;

        ObjectGuid _forcedTarget;       // ordered "attack"/"pull"
        bool _pulling = false;
        ObjectGuid _lastTarget;
        TimePoint _combatStart;
        bool _inCombat = false;
        TimePoint _lastCastTime;
        uint32 _deadTimer = 0;
        bool _restRequested = false;
        bool _eating = false;
        uint32 _restTimer = 0;
        std::string _pendingCast;
        ObjectGuid _pendingCastTarget;
        uint32 _stuckTimer = 0;
        float _lastOwnerDist = 0.0f;
        uint32 _reagentTimer = 0;
        uint32 _holdFire = 0;           // "stop": ms before attacking again
        // loose following: a spot of its own around the owner instead of a fixed formation
        float _spread = 1.0f;           // "spread" / "stack" orders scale the radius
        bool _loitering = false;
        Position _loiterSpot;
        Position _lastOwnerPosition;
        TimePoint _ownerStillSince = { };
        TimePoint _scatterUntil = { };
        TimePoint _fidgetAt = { };
        TimePoint _faceAt = { };
        uint32 _loiterDelay = 1000;
        // callouts from players: heal / dispel / protect someone right now
        ObjectGuid _healTarget;
        TimePoint _healUntil = { };
        ObjectGuid _dispelTarget;
        TimePoint _dispelUntil = { };
        ObjectGuid _protectTarget;
        TimePoint _protectUntil = { };
        bool UpdateCallouts();
        // tactics
        ObjectGuid _ccTarget;
        TimePoint _ccSince = { };
        TimePoint _ccUntil = { };
        ObjectGuid _interruptTarget;
        TimePoint _interruptUntil = { };
        TimePoint _burstUntil = { };
        ObjectGuid _waitingFor;         // melee waiting for the tank to take this enemy
        TimePoint _ccCheckAt = { };
        TimePoint _chargeSettleUntil = { };
        TimePoint _avoidUntil = { };
        struct DangerZone { Position Center; float Radius; TimePoint Until; };
        std::vector<DangerZone> _dangerZones;   // ground effects it stepped out of, while they last
        TimePoint _mechanicHold = { };  // a mechanic is active: no chasing into it
        TimePoint _mechanicMoveUntil = { };
        TimePoint _mechanicScanAt = { };
        Optional<float> _mechanicAngle; // chase angle around the target a mechanic asks for (behind, flanks)
        std::vector<ObjectGuid> _hazards;   // creatures to keep away from (void zones, tornadoes, clouds)
        TimePoint _fearWardAt = { };
        std::string _mechanicDebug;     // test harness: why the last search found no spot
        // ambient life
        enum class Activity : uint8 { None, Grind, Visit, Wander, Sit };
        bool _ambient = false;
        bool _ambientCity = false;
        Position _ambientHome;
        Activity _activity = Activity::None;
        TimePoint _activityUntil = { };
        bool _activityArrived = false;
        Position _activitySpot;
        ObjectGuid _activityTarget;
        uint32 _activityDwell = 0;      // ms to stay once there
        enum class Job : uint8 { None, Hunter, Quester, Trader, Crafter, Shopper, Idler };
        Job _job = Job::None;
        uint32 _jobEntry = 0;           // the mob it hunts
        Position _jobCenter;            // where it hunts
        ObjectGuid _jobNpc;             // its quest giver, its auctioneer...
        uint32 _jobKills = 0;
        uint32 _jobGoal = 0;
        uint8 _jobStage = 0;
        std::string _jobTrade;          // a crafter's profession
        TimePoint _jobAdvertAt = { };
        std::vector<uint32> _jobErrands;    // a shopper's list (npc flags of each stop)
        bool _ambientFinished = false;
        // preferences and behaviour settings
        std::vector<SpellPref> _prefs;
        std::unordered_map<std::string, TimePoint> _prefTried;
        bool _allowAvoided = false;     // an explicit "cast <spell>" order overrides "avoid"
        bool _usePotions = true;
        bool _autoCC = true;
        uint8 _lootMode = 0;
        TimePoint _potionAt = { };
        // trading and looting
        ObjectGuid _giveTo;             // a trade it opened to hand something over
        std::string _giveWhat;
        TimePoint _giveUntil = { };
        bool _giveFilled = false;
        ObjectGuid _tradeBegun;         // the trade window it opened for that player
        ObjectGuid _tradeDone;          // accepted: thanks once the trade is over
        ObjectGuid _lootCorpse;
        TimePoint _lootUntil = { };
        TimePoint _lootScanAt = { };
        std::vector<ObjectGuid> _looted;
        uint32 _ccCasts = 0;            // casts on the current assignment: two that do not hold and it gives up
        float _formationSide = 1.0f;    // melee: left or right of the owner
        std::deque<std::pair<TimePoint, std::string>> _recentCasts;
        std::unordered_map<ObjectGuid, std::string> _blessingFor;
        ObjectGuid _rebuffTarget;
        std::vector<std::string> _rebuffQueue;
        std::vector<uint32> _wantedLoot;            // won on Need: equipped as soon as it is in the bags
        std::vector<ObjectGuid> _knownBagItems;
        ObjectGuid _lastNewItem;
        // situational callouts to the party, like players make them (out of mana, adds, a rare...)
        void NoticeSituation();
        void Callout(char const* kind, std::string const& detail, std::string const& canned);
        uint32 _noticeTimer = 0;
        bool _saidOom = false;
        bool _saidLowHealth = false;
        bool _saidAggro = false;
        bool _moonControlled = false;
        size_t _enemyCount = 0;
        std::vector<ObjectGuid> _deadSeen;
        std::vector<ObjectGuid> _spotted;
    };
}

#endif
