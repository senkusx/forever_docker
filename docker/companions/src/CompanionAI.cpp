/*
 * Companions: the bot brain
 */

#include "CompanionAI.h"
#include "CellImpl.h"
#include "ChatPackets.h"
#include "CompanionConfig.h"
#include "CompanionFactory.h"
#include "CompanionLLM.h"
#include "CompanionMechanics.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "GameObject.h"
#include "DynamicObject.h"
#include "AreaTrigger.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Loot.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementDefines.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "QuestDef.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace Companions
{
    namespace
    {
        char const* EnName(SpellInfo const* info)
        {
            if (!info || !info->SpellName)
                return nullptr;
            char const* name = info->SpellName->Str[LOCALE_enUS];
            return (name && *name) ? name : nullptr;
        }

        // case-insensitive compare against a lower case name
        bool NameIs(SpellInfo const* info, std::string_view lowerName)
        {
            char const* name = EnName(info);
            if (!name)
                return false;
            size_t len = std::char_traits<char>::length(name);
            if (len != lowerName.size())
                return false;
            for (size_t i = 0; i < len; ++i)
                if (char(std::tolower(static_cast<unsigned char>(name[i]))) != lowerName[i])
                    return false;
            return true;
        }

        uint32 Elapsed(TimePoint since)
        {
            return uint32(std::chrono::duration_cast<Milliseconds>(GameTime::Now() - since).count());
        }

        bool IsFoodOrDrink(Aura const* aura)
        {
            switch (aura->GetSpellInfo()->GetSpellSpecific())
            {
                case SPELL_SPECIFIC_FOOD:
                case SPELL_SPECIFIC_DRINK:
                case SPELL_SPECIFIC_FOOD_AND_DRINK:
                    return true;
                default:
                    return false;
            }
        }

        bool UsesMana(Player const* bot)
        {
            return bot->GetMaxPower(POWER_MANA) > 0;
        }

        // with the caster's healing power; overTime false: the direct part only
        uint32 EstimateHeal(SpellInfo const* info, Unit* caster, Unit* target, bool overTime = true)
        {
            uint32 total = 0;
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                if (effect.IsEffect(SPELL_EFFECT_HEAL))
                    total += uint32(std::max(0, caster->SpellHealingBonusDone(target, info, std::max(0, effect.CalcValueAsInt(caster)), HEAL, effect)));
                else if (overTime && effect.IsAura(SPELL_AURA_PERIODIC_HEAL) && effect.ApplyAuraPeriod)
                {
                    int32 ticks = info->GetMaxDuration() > 0 ? info->GetMaxDuration() / int32(effect.ApplyAuraPeriod) : 1;
                    int32 tick = caster->SpellHealingBonusDone(target, info, std::max(0, effect.CalcValueAsInt(caster)), DOT, effect);
                    total += uint32(std::max(0, tick) * std::max(1, ticks));
                }
            }
            return total;
        }
    }

    // ------------------------------------------------------------------ SpellBook

    void SpellBook::Rebuild(Player* bot)
    {
        _ranks.clear();
        for (auto const& [spellId, playerSpell] : bot->GetSpellMap())
        {
            if (playerSpell.state == PLAYERSPELL_REMOVED || playerSpell.disabled)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
            if (!info || info->IsPassive())
                continue;
            char const* name = EnName(info);
            if (!name)
                continue;
            _ranks[ToLower(name)].push_back(spellId);
        }

        for (auto& [name, ids] : _ranks)
        {
            std::sort(ids.begin(), ids.end(), [](uint32 a, uint32 b)
            {
                SpellInfo const* ia = sSpellMgr->GetSpellInfo(a, DIFFICULTY_NONE);
                SpellInfo const* ib = sSpellMgr->GetSpellInfo(b, DIFFICULTY_NONE);
                uint8 ra = sSpellMgr->GetSpellRank(a);
                uint8 rb = sSpellMgr->GetSpellRank(b);
                if (ra != rb)
                    return ra < rb;
                uint32 la = std::max(ia->SpellLevel, ia->BaseLevel);
                uint32 lb = std::max(ib->SpellLevel, ib->BaseLevel);
                if (la != lb)
                    return la < lb;
                return a < b;
            });
        }
    }

    uint32 SpellBook::Best(std::string_view name) const
    {
        auto itr = _ranks.find(std::string(name));
        if (itr == _ranks.end() || itr->second.empty())
            return 0;
        return itr->second.back();
    }

    std::vector<std::string> SpellBook::Names() const
    {
        std::vector<std::string> names;
        for (auto const& [name, ids] : _ranks)
            names.push_back(name);
        std::sort(names.begin(), names.end());
        return names;
    }

    std::vector<uint32> const* SpellBook::Ranks(std::string_view name) const
    {
        auto itr = _ranks.find(std::string(name));
        return itr != _ranks.end() ? &itr->second : nullptr;
    }

    // ------------------------------------------------------------------ CompanionAI

    CompanionAI::CompanionAI(Player* bot, ObjectGuid owner, Role role) : _bot(bot), _owner(owner), _role(role)
    {
        _class = CreateClassAI(*this, bot->GetClass());
        _combatStart = GameTime::Now();
        _lastCastTime = GameTime::Now();
        _stayPosition = bot->GetPosition();
        _formationSide = roll_chance(50) ? 1.0f : -1.0f;
    }

    CompanionAI::~CompanionAI() = default;

    Role CompanionAI::GetEffectiveRole() const
    {
        if (_role != Role::Auto)
            return _role;
        return _class ? _class->DefaultRole() : DpsRoleForClass(_bot->GetClass());
    }

    void CompanionAI::SetRole(Role role)
    {
        _role = role;
        _movement = MoveMode::None;
        if (_class)
            _class->OnRoleChanged();
    }

    void CompanionAI::SetStance(Stance stance)
    {
        _stance = stance;
        _movement = MoveMode::None;
        if (stance == Stance::Stay)
            _stayPosition = _bot->GetPosition();
    }

    void CompanionAI::OrderAttack(ObjectGuid target, bool pull)
    {
        _forcedTarget = target;
        _pulling = pull;
        _holdFire = 0;
        if (_aggression == Aggression::Passive)
            _aggression = Aggression::Assist;
        _movement = MoveMode::None;
    }

    void CompanionAI::OrderStop()
    {
        _forcedTarget.Clear();
        _pulling = false;
        _holdFire = 4000;
        DisengageAll();
    }

    void CompanionAI::OrderComeHere()
    {
        _forcedTarget.Clear();
        _pulling = false;
        _stance = Stance::Follow;
        DisengageAll();
        _movement = MoveMode::None;
    }

    void CompanionAI::OrderRetreat()
    {
        OrderComeHere();
        _holdFire = 6000;
    }

    // an ally called for help: done before everything else, like a player reacting to party chat
    bool CompanionAI::UpdateCallouts()
    {
        TimePoint now = GameTime::Now();
        if (!_healTarget.IsEmpty())
        {
            Unit* target = now < _healUntil ? ObjectAccessor::GetUnit(*_bot, _healTarget) : nullptr;
            if (!target || !target->IsAlive() || !_class->CanHeal())
                _healTarget.Clear();
            else if (target->GetHealth() < target->GetMaxHealth())
            {
                if (_bot->GetDistance(target) > 36.0f || !_bot->IsWithinLOSInMap(target))
                {
                    _bot->GetMotionMaster()->MovePoint(0, target->GetPosition());
                    _movement = MoveMode::Point;
                    return true;
                }
                uint64 missing = target->GetMaxHealth() - target->GetHealth();
                if (_class->Heal(target, HealUrgency::Emergency, uint32(std::min<uint64>(missing, std::numeric_limits<uint32>::max()))))
                {
                    _healTarget.Clear();
                    return true;
                }
            }
            else
                _healTarget.Clear();
        }
        if (!_dispelTarget.IsEmpty())
        {
            Unit* target = now < _dispelUntil ? ObjectAccessor::GetUnit(*_bot, _dispelTarget) : nullptr;
            if (!target || !DispellableMask(target))
                _dispelTarget.Clear();
            else if (_class->Dispel(target))
            {
                _dispelTarget.Clear();
                return true;
            }
        }
        if (!_protectTarget.IsEmpty() && now >= _protectUntil)
            _protectTarget.Clear();
        return false;
    }

    void CompanionAI::OnRevived()
    {
        _movement = MoveMode::None;
        _deadTimer = 0;
        _spellsDirty = true;
    }

    Unit* CompanionAI::GetCurrentTarget() const
    {
        return _bot->GetVictim();
    }

    void CompanionAI::Update(uint32 diff)
    {
        if (!_bot->IsInWorld() || _bot->IsBeingTeleported() || _bot->GetSession()->PlayerLogout())
            return;

        _thinkTimer += diff;
        if (_thinkTimer < GetConfig().UpdateInterval)
            return;

        uint32 elapsed = _thinkTimer;
        _thinkTimer = 0;
        Think(elapsed);
    }

    void CompanionAI::Think(uint32 diff)
    {
        if (_spellsDirty)
        {
            _spells.Rebuild(_bot);
            _spellsDirty = false;
        }

        _slowTimer += diff;
        bool const slowTick = _slowTimer >= 1000;
        if (slowTick)
            _slowTimer = 0;

        if (_holdFire)
            _holdFire = _holdFire > diff ? _holdFire - diff : 0;

        if (!_bot->IsAlive())
        {
            HandleDeath(diff);
            return;
        }

        HandleRequests();
        BuildView();
        UpdateTrade();

        bool const combat = !_view.Enemies.empty();
        if (combat && !_inCombat)
        {
            _inCombat = true;
            _combatStart = GameTime::Now();
            _eating = false;
            if (_bot->IsMounted())
                _bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
                _bot->SetStandState(UNIT_STAND_STATE_STAND);
        }
        else if (!combat && _inCombat)
        {
            _inCombat = false;
            _forcedTarget.Clear();
            _pulling = false;
            DisengageAll();
        }

        if (slowTick && GetConfig().FreeReagents)
        {
            _reagentTimer += 1000;
            if (_reagentTimer >= 30000)
            {
                _reagentTimer = 0;
                sCompanionFactory->RestockConsumables(_bot);
            }
        }

        // standing in fire (or any hostile ground effect): out first, even in the middle of a cast, like a player who sees the
        // floor light up
        if (AvoidGroundEffects())
            return;
        // boss mechanics: out of a cast, a breath's front, a debuffed friend's way; spread, polarity, cover
        if (combat && UpdateMechanics())
            return;

        // charging (Charge, Intercept, Feral Charge) or thrown back: any new movement order would fight the effect and stutter
        if (_bot->HasUnitState(UNIT_STATE_CHARGING) || _bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == EFFECT_MOTION_TYPE)
        {
            _chargeSettleUntil = GameTime::Now() + Milliseconds(400);
            return;
        }

        // casting or channeling: let it finish (melee keeps swinging on its own); a resurrection whose target came back
        // or left stops, like a player who sees it
        if (Spell* spell = _bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (spell->GetSpellInfo()->HasEffect(SPELL_EFFECT_RESURRECT) || spell->GetSpellInfo()->HasEffect(SPELL_EFFECT_RESURRECT_WITH_AURA))
            {
                ObjectGuid aimed = spell->m_targets.GetObjectTargetGUID();
                WorldObject* object = aimed.IsEmpty() ? nullptr : ObjectAccessor::GetWorldObject(*_bot, aimed);
                Unit* unit = object ? object->ToUnit() : nullptr;
                if (!object || (unit && unit->IsAlive()))
                    _bot->InterruptSpell(CURRENT_GENERIC_SPELL);
            }
        CancelNeedlessHeal();
        if (_bot->IsNonMeleeSpellCast(false, false, true))
        {
            if (Unit* victim = _bot->GetVictim())
                if (!_bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
                    _bot->SetInFront(victim);
            return;
        }

        if (UpdatePendingCast())
            return;

        if (slowTick)
        {
            if (!_ambient)
            {
                NoticeSituation();
                CompanionLLM::ConsiderTactics(_bot);
            }
            HandleLoot();
        }

        if (UpdateCallouts())
            return;

        if (combat)
            UpdateCombat();
        else if (_ambient)
            UpdateAmbient();
        else
            UpdateIdle();

        if (slowTick && !combat)
        {
            // stuck or left far behind: join the owner like a real player would after a short run
            Player* owner = _view.Owner;
            if (owner && _stance == Stance::Follow && !_bot->IsBeingTeleported() && !owner->IsBeingTeleported())
            {
                float slotAngle, slotWidth, slotMin, slotMax;
                FormationSlot(false, slotAngle, slotWidth, slotMin, slotMax);
                float dist = _bot->GetDistance(owner);
                if (dist > GetConfig().TeleportDistance)
                    _stuckTimer = 100000;
                else if (dist > std::max(20.0f, slotMax + 8.0f) && std::fabs(dist - _lastOwnerDist) < 1.0f && !owner->isMoving())
                    _stuckTimer += 1000;
                else
                    _stuckTimer = 0;
                _lastOwnerDist = dist;

                if (_stuckTimer >= 6000)
                {
                    _stuckTimer = 0;
                    Position pos = owner->GetPosition();
                    owner->MovePosition(pos, 2.0f, float(M_PI) + frand(-0.6f, 0.6f));
                    CompanionMgr::InterruptGroupCastsOn(_bot);
                    _bot->NearTeleportTo(pos);
                    _movement = MoveMode::None;
                }
            }
        }
    }

    void CompanionAI::Callout(char const* kind, std::string const& detail, std::string const& canned)
    {
        CompanionLLM::OnCallout(_bot, kind, detail, canned);
    }

    void CompanionAI::NoticeSituation()
    {
        Config const& config = GetConfig();
        if (!config.Callouts && !(config.LLMEnable && config.LLMCallouts))
            return;
        if (!_view.Owner || !_bot->GetGroup())
            return;

        bool const combat = !_view.Enemies.empty();
        Role const role = GetEffectiveRole();

        if (UsesMana(_bot))
        {
            float mana = ManaPct();
            if (combat && !_saidOom && mana < (role == Role::Healer ? 20.0f : 12.0f))
            {
                _saidOom = true;
                Callout("oom", "you are almost out of mana (" + std::to_string(uint32(mana)) + "%) in the middle of a fight",
                    role == Role::Healer ? "I'm out of mana, careful!" : "Out of mana.");
            }
            else if (mana > 50.0f)
                _saidOom = false;
        }

        float health = HealthPct();
        if (combat && !_saidLowHealth && health < 30.0f)
        {
            _saidLowHealth = true;
            Callout("low_health", "you are at " + std::to_string(uint32(health)) + "% health and still fighting",
                role == Role::Healer ? "I'm going down, someone help!" : "Need heals!");
        }
        else if (health > 60.0f)
            _saidLowHealth = false;

        if (combat)
        {
            // a healer with something hitting it calls for the tank
            if (role != Role::Tank)
            {
                Unit* attacker = nullptr;
                for (Unit* enemy : _view.Enemies)
                    if (enemy->GetVictim() == _bot)
                        attacker = enemy;
                if (attacker && role == Role::Healer && !_saidAggro)
                {
                    _saidAggro = true;
                    Callout("aggro", attacker->GetName() + " is attacking you, the healer", attacker->GetName() + " is on me!");
                }
                else if (!attacker)
                    _saidAggro = false;
            }

            // more enemies than the fight started with
            if (_enemyCount && _view.Enemies.size() >= _enemyCount + 2)
            {
                std::string names;
                for (size_t i = _enemyCount; i < _view.Enemies.size() && i < _enemyCount + 3; ++i)
                    names += (names.empty() ? "" : ", ") + _view.Enemies[i]->GetName();
                Callout("adds", std::to_string(_view.Enemies.size() - _enemyCount) + " more enemies joined the fight: " + names, "Adds incoming!");
            }
            _enemyCount = std::max(_enemyCount, _view.Enemies.size());

            // somebody of the party just died
            for (Player* member : _view.Members)
            {
                if (member == _bot || member->IsAlive())
                    continue;
                if (std::find(_deadSeen.begin(), _deadSeen.end(), member->GetGUID()) != _deadSeen.end())
                    continue;
                _deadSeen.push_back(member->GetGUID());
                Callout("member_down", member->GetName() + " just died in the fight", member->GetName() + " is down!");
            }
        }
        else
        {
            _enemyCount = 0;
            _saidAggro = false;
            _deadSeen.erase(std::remove_if(_deadSeen.begin(), _deadSeen.end(), [this](ObjectGuid guid)
            {
                Player* member = ObjectAccessor::GetPlayer(*_bot, guid);
                return !member || member->IsAlive();
            }), _deadSeen.end());
        }

        // the moon (crowd control) target broke free
        if (Group* group = _bot->GetGroup())
        {
            ObjectGuid moon = group->GetTargetIcon(ICON_MOON);
            Unit* target = moon.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*_bot, moon);
            if (!target || !target->IsAlive())
                _moonControlled = false;
            else if (target->HasBreakableByDamageCrowdControlAura())
                _moonControlled = true;
            else if (_moonControlled && target->GetVictim())
            {
                _moonControlled = false;
                Callout("cc_broken", target->GetName() + " (moon, crowd controlled) broke free and attacks " + target->GetVictim()->GetName(),
                    "Moon is loose!");
            }
        }

        // out of a fight, every few seconds: a rare or an elite worth mentioning
        if (!combat && ++_noticeTimer >= 5)
        {
            _noticeTimer = 0;
            std::list<Creature*> nearby;
            _bot->GetCreatureListWithOptionsInGrid(nearby, 45.0f, { .IsAlive = FindCreatureAliveState::Alive });
            for (Creature* creature : nearby)
            {
                if (creature->IsInCombat() || !_bot->IsValidAttackTarget(creature) || !creature->IsVisible())
                    continue;
                // worth a word: a rare, or an elite that is a threat at this level (not the grey ones of an old elite zone)
                bool rare = creature->HasClassification(CreatureClassifications::Rare) || creature->HasClassification(CreatureClassifications::RareElite);
                bool elite = !_bot->GetMap()->IsDungeon() && (creature->IsElite() || creature->isWorldBoss())
                    && creature->GetLevel() + 2 >= _bot->GetLevel();
                rare = rare && creature->GetLevel() + 10 >= _bot->GetLevel();
                if (!rare && !elite)
                    continue;
                if (std::find(_spotted.begin(), _spotted.end(), creature->GetGUID()) != _spotted.end())
                    continue;
                if (_spotted.size() >= 32)
                    _spotted.erase(_spotted.begin());
                _spotted.push_back(creature->GetGUID());
                Callout(rare ? "rare" : "elite", std::string(rare ? "a rare" : "an elite") + " enemy is nearby: " + creature->GetName() + ", level "
                    + std::to_string(creature->GetLevel()) + ", " + std::to_string(uint32(_bot->GetDistance(creature))) + " yards away",
                    rare ? "Rare spotted: " + creature->GetName() + "!" : "Careful, " + creature->GetName() + " is elite.");
                break;
            }
        }
    }

    // group invites, ready checks, quest shares: answered like an attentive player
    void CompanionAI::HandleRequests()
    {
        if (!_bot->GetPlayerSharingQuest().IsEmpty())
        {
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(_bot->GetSharedQuestID()))
                if (_bot->CanTakeQuest(quest, false) && _bot->CanAddQuest(quest, false))
                    _bot->AddQuestAndCheckCompletion(quest, nullptr);
            _bot->ClearQuestSharingInfo();
        }

        if (Group* group = _bot->GetGroup())
        {
            if (group->IsReadyCheckStarted())
            {
                for (Group::MemberSlot const& slot : group->GetMemberSlots())
                {
                    if (slot.guid == _bot->GetGUID())
                    {
                        if (!slot.readyChecked)
                            group->SetMemberReadyCheck(_bot->GetGUID(), true);
                        break;
                    }
                }
            }
        }
    }

    void CompanionAI::HandleDeath(uint32 diff)
    {
        _inCombat = false;
        _movement = MoveMode::None;

        // a resurrection was cast on us: accept it
        if (_bot->IsResurrectRequested())
        {
            _bot->ResurrectUsingRequestData();
            OnRevived();
            return;
        }

        // an ambient bot that died is taken out of the world by CompanionAmbient
        if (!GetConfig().AutoRevive || _ambient)
            return;

        Player* owner = ObjectAccessor::GetPlayer(*_bot, _owner);
        if (!owner || !owner->IsAlive() || owner->IsInCombat())
        {
            _deadTimer = 0;
            return;
        }

        _deadTimer += diff;

        // give a resurrecter in the group time to come and cast
        uint32 delay = GetConfig().AutoReviveDelay * IN_MILLISECONDS;
        if (Group* group = _bot->GetGroup())
        {
            for (GroupReference const& ref : group->GetMembers())
            {
                Player* member = ref.GetSource();
                if (!member || member == _bot || !member->IsAlive() || member->GetMap() != _bot->GetMap())
                    continue;
                if (CompanionAI* ai = sCompanionMgr->GetAI(member))
                    if (ai->GetClassAI() && ai->GetClassAI()->CanHeal())
                    {
                        delay *= 2;
                        break;
                    }
            }
        }

        // someone is resurrecting this bot right now: wait for it
        if (_deadTimer < delay || CompanionMgr::IsTargetOfGroupCast(_bot))
            return;

        _deadTimer = 0;
        CompanionMgr::InterruptGroupCastsOn(_bot);
        _bot->ResurrectPlayer(0.5f);
        _bot->SpawnCorpseBones();
        Position pos = owner->GetPosition();
        owner->MovePosition(pos, 2.0f, float(M_PI) + frand(-0.6f, 0.6f));
        _bot->NearTeleportTo(pos);
        OnRevived();
    }

    void CompanionAI::BuildView()
    {
        _view.Members.clear();
        _view.Enemies.clear();
        _view.Owner = ObjectAccessor::GetPlayer(*_bot, _owner);
        _view.Tank = nullptr;

        if (Group* group = _bot->GetGroup())
        {
            for (GroupReference const& ref : group->GetMembers())
                if (Player* member = ref.GetSource())
                    if (member->IsInWorld() && member->GetMap() == _bot->GetMap() && !member->IsBeingTeleported())
                        _view.Members.push_back(member);
        }
        else
        {
            _view.Members.push_back(_bot);
            if (_view.Owner)
                _view.Members.push_back(_view.Owner);
        }

        for (Player* member : _view.Members)
        {
            if (CompanionAI* ai = sCompanionMgr->GetAI(member))
            {
                if (ai->GetEffectiveRole() == Role::Tank && member->IsAlive())
                {
                    _view.Tank = member;
                    break;
                }
            }
        }
        if (!_view.Tank && _view.Owner && _view.Owner->IsAlive())
            _view.Tank = _view.Owner;

        if (_holdFire)
            return;

        auto addEnemy = [this](Unit* unit, float maxDistance = 60.0f)
        {
            if (!IsValidEnemy(unit, maxDistance))
                return;
            // a mind controlled friend is crowd controlled (MechanicSpells), never fought
            if (unit->GetTypeId() == TYPEID_PLAYER && std::find(_view.Members.begin(), _view.Members.end(), unit) != _view.Members.end())
                return;
            if (std::find(_view.Enemies.begin(), _view.Enemies.end(), unit) == _view.Enemies.end())
                _view.Enemies.push_back(unit);
        };

        for (Player* member : _view.Members)
        {
            if (!member->IsAlive())
                continue;
            for (Unit* attacker : member->getAttackers())
                addEnemy(attacker);
            for (auto const& [guid, ref] : member->GetCombatManager().GetPvECombatRefs())
                addEnemy(ref->GetOther(member));
            if (Pet* pet = member->GetPet())
                for (Unit* attacker : pet->getAttackers())
                    addEnemy(attacker);
        }

        // the owner starting a fight counts, even before the first hit lands
        if (Player* owner = _view.Owner)
            if (Unit* victim = owner->GetVictim())
                if (owner->IsInCombat() || owner->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                    addEnemy(victim);

        if (!_forcedTarget.IsEmpty())
        {
            Unit* forced = ObjectAccessor::GetUnit(*_bot, _forcedTarget);
            if (forced && IsValidEnemy(forced, 100.0f))
                addEnemy(forced, 100.0f);
            else
            {
                _forcedTarget.Clear();
                _pulling = false;
            }
        }

        if (_aggression == Aggression::Aggressive && _view.Enemies.empty())
        {
            std::list<Unit*> nearby;
            Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(_bot, _bot, 20.0f);
            Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(_bot, nearby, check);
            Cell::VisitAllObjects(_bot, searcher, 20.0f);
            for (Unit* unit : nearby)
                if (unit->IsHostileTo(_bot) && !unit->IsCritter() && !unit->IsTotem() && _bot->IsWithinLOSInMap(unit))
                    addEnemy(unit);
        }
    }

    bool CompanionAI::IsValidEnemy(Unit* unit, float maxDistance) const
    {
        if (!unit || !unit->IsInWorld() || !unit->IsAlive() || unit->GetMap() != _bot->GetMap())
            return false;
        if (!unit->isTargetableForAttack() || !_bot->IsValidAttackTarget(unit))
            return false;
        if (Creature const* creature = unit->ToCreature())
            if (creature->IsEvadingAttacks() || creature->IsTrigger())
                return false;
        return _bot->GetDistance(unit) < maxDistance;
    }

    bool CompanionAI::IsCrowdControlTarget(Unit const* unit) const
    {
        if (Group* group = _bot->GetGroup())
            if (group->GetTargetIcon(ICON_MOON) == unit->GetGUID())
                return true;
        if (unit->HasBreakableByDamageCrowdControlAura())
            return true;
        return sCompanionMgr->IsCrowdControlClaimed(unit->GetGUID(), _bot->GetGUID());
    }

    void CompanionAI::OrderCrowdControl(ObjectGuid target)
    {
        _ccTarget = target;
        _ccCasts = 0;
        _ccSince = GameTime::Now();
        _ccUntil = _ccSince + Seconds(40);
        sCompanionMgr->ClaimCrowdControl(target, _bot->GetGUID(), 5000);   // time to cast it
    }

    bool CompanionAI::IsAssignedCrowdControl(Unit const* unit) const
    {
        // a few seconds to cast it: after that, an enemy nobody controls is fair game again
        return _ccTarget == unit->GetGUID() && GameTime::Now() < _ccUntil && Elapsed(_ccSince) < 5000;
    }

    // someone of the group (players, bots, pets) is hitting it: no point controlling it
    bool CompanionAI::AttackedByGroup(Unit const* unit) const
    {
        for (Player* member : _view.Members)
        {
            if (member == _bot)
                continue;
            if (member->GetVictim() == unit)
                return true;
            if (Unit* pet = member->GetGuardianPet())
                if (pet->GetVictim() == unit)
                    return true;
        }
        return false;
    }

    bool CompanionAI::Bursting() const
    {
        return IsBossFight() || GameTime::Now() < _burstUntil;
    }

    std::string CompanionAI::RecentCasts() const
    {
        std::string out;
        for (auto itr = _recentCasts.rbegin(); itr != _recentCasts.rend(); ++itr)   // newest first
            out += (out.empty() ? "" : ", ") + itr->second + " (" + std::to_string(Elapsed(itr->first) / 1000) + "s)";
        return out;
    }

    Unit* CompanionAI::SelectTarget()
    {
        _waitingFor.Clear();
        if (!_forcedTarget.IsEmpty())
            if (Unit* forced = ObjectAccessor::GetUnit(*_bot, _forcedTarget))
                if (IsValidEnemy(forced, 100.0f))
                    return forced;

        auto usable = [this](Unit* unit) { return unit && IsValidEnemy(unit) && !IsCrowdControlTarget(unit) && IsMechanicTarget(unit); };

        if (!_protectTarget.IsEmpty())
            for (Unit* enemy : _view.Enemies)
                if (enemy->GetVictim() && enemy->GetVictim()->GetGUID() == _protectTarget && usable(enemy))
                    return enemy;

        Group* group = _bot->GetGroup();
        if (group)
        {
            for (uint8 icon : { uint8(ICON_SKULL), uint8(ICON_CROSS) })
            {
                ObjectGuid guid = group->GetTargetIcon(icon);
                if (guid.IsEmpty())
                    continue;
                Unit* marked = ObjectAccessor::GetUnit(*_bot, guid);
                if (usable(marked) && std::find(_view.Enemies.begin(), _view.Enemies.end(), marked) != _view.Enemies.end())
                    return marked;
            }
        }

        // the adds a boss fight is won by (healers, totems, larvae...)
        if (Unit* first = KillFirstTarget())
            return first;

        Unit* pick = nullptr;
        // assist the owner (he leads), else the tank
        if (Player* owner = _view.Owner)
            if (Unit* victim = owner->GetVictim())
                if (usable(victim) && std::find(_view.Enemies.begin(), _view.Enemies.end(), victim) != _view.Enemies.end())
                    pick = victim;

        if (!pick && _view.Tank && _view.Tank != _bot)
            if (Unit* victim = _view.Tank->GetVictim())
                if (usable(victim) && std::find(_view.Enemies.begin(), _view.Enemies.end(), victim) != _view.Enemies.end())
                    pick = victim;

        // keep hitting the current target
        if (!pick)
            if (Unit* current = _bot->GetVictim())
                if (usable(current) && std::find(_view.Enemies.begin(), _view.Enemies.end(), current) != _view.Enemies.end())
                    pick = current;

        // whatever hurts the group the most: lowest health first
        if (!pick)
        {
            for (Unit* enemy : _view.Enemies)
            {
                if (!usable(enemy))
                    continue;
                if (!pick || enemy->GetHealthPct() < pick->GetHealthPct())
                    pick = enemy;
            }
        }

        if (!pick)
            return nullptr;

        // a careful player lets the tank build threat first
        uint32 const sincePull = Elapsed(_combatStart);
        if (_view.Tank && _view.Tank != _bot && _view.Tank->IsAlive() && IsTank(_view.Tank) && pick->GetVictim() != _bot
            && _forcedTarget.IsEmpty() && (sincePull < 1500 || (pick->GetVictim() != _view.Tank && sincePull < 4000)))
        {
            _waitingFor = pick->GetGUID();
            return nullptr;
        }

        _waitingFor.Clear();
        return pick;
    }

    bool CompanionAI::RecoverThreat()
    {
        // the loose ones: on another player of the group, the one on a healer or on whoever is lowest first
        Unit* loose = nullptr;
        int32 looseScore = std::numeric_limits<int32>::max();
        uint32 looseNear = 0;
        for (Unit* enemy : _view.Enemies)
        {
            Player* victim = enemy->GetVictim() ? enemy->GetVictim()->ToPlayer() : nullptr;
            if (!victim || victim == _bot || IsCrowdControlTarget(enemy) || !IsValidEnemy(enemy) || _bot->GetDistance(enemy) > 30.0f)
                continue;
            if (_bot->GetDistance(enemy) < 10.0f)
                ++looseNear;
            int32 score = (IsHealer(victim) ? 0 : 100) + int32(victim->GetHealthPct()) + int32(_bot->GetDistance(enemy));
            if (score < looseScore)
            {
                looseScore = score;
                loose = enemy;
            }
        }
        if (!loose)
            return false;
        if (looseNear >= 3 && _class->AoeTaunt())
            return true;
        // the taunt now; on cooldown the target choice (loose ones first) and the rotation's big threat hits take over
        return _class->Taunt(loose);
    }

    Unit* CompanionAI::KeepUnderTank(Unit* target)
    {
        _holdingThreat = false;
        Unit* tank = _view.Tank;
        if (!tank || tank == _bot || !tank->IsAlive() || !IsTank(tank) || !_forcedTarget.IsEmpty())
            return target;
        Role const role = GetEffectiveRole();
        // the enemy pulls off the tank at 110% of its threat in melee and 130% at range: stay well below it
        float const cap = role == Role::MeleeDps ? 0.85f : 0.9f;
        auto nearTank = [&](Unit* enemy)
        {
            if (enemy->GetVictim() == _bot)
                return true;        // it turned on me: hitting it more keeps it there, the tank takes it back
            if (enemy->GetVictim() != tank || !enemy->CanHaveThreatList())
                return false;
            float const tankThreat = enemy->GetThreatManager().GetThreat(tank);
            return tankThreat > 0.0f && enemy->GetThreatManager().GetThreat(_bot) >= tankThreat * cap;
        };
        if (!nearTank(target))
            return target;
        // another one the tank holds with room under its threat (in reach for melee), the weakest first; else hold
        Unit* other = nullptr;
        for (Unit* enemy : _view.Enemies)
            if (enemy != target && IsValidEnemy(enemy) && !IsCrowdControlTarget(enemy) && IsMechanicTarget(enemy) && !nearTank(enemy)
                && (role != Role::MeleeDps || _bot->GetDistance(enemy) < 10.0f) && (!other || enemy->GetHealthPct() < other->GetHealthPct()))
                other = enemy;
        if (other)
            return other;
        _holdingThreat = true;
        _waitingFor = target->GetGUID();
        if (_bot->GetVictim())
            _bot->AttackStop();
        _bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL);     // the wand or auto shot too
        return nullptr;
    }

    bool CompanionAI::SafeToAoe(Position const& center, float radius) const
    {
        Unit* tank = _view.Tank;
        if (!tank || tank == _bot || !tank->IsAlive() || !IsTank(tank))
            return true;
        if (Elapsed(_combatStart) < 4000)
            return false;       // the tank picks the pack up first
        for (Unit* enemy : _view.Enemies)
        {
            if (enemy->GetExactDist(&center) > radius + enemy->GetCombatReach() || !enemy->CanHaveThreatList())
                continue;
            if (enemy->GetVictim() != tank)
                return false;   // one the tank does not hold: area damage would take it
            float const tankThreat = enemy->GetThreatManager().GetThreat(tank);
            if (tankThreat <= 0.0f || enemy->GetThreatManager().GetThreat(_bot) >= tankThreat * 0.7f)
                return false;
        }
        return true;
    }

    void CompanionAI::UpdatePetTaunt()
    {
        Creature* creature = GetPet();
        Pet* pet = creature ? creature->ToPet() : nullptr;
        if (!pet)
            return;
        // Growl, Torment and the like pull the enemies off a tank: on with nobody else to tank, off with a tank in the group
        bool const otherTank = _view.Tank && _view.Tank != _bot && _view.Tank->IsAlive() && IsTank(_view.Tank);
        int8 const wanted = otherTank ? 0 : 1;
        if (_petTaunt == wanted && _petTauntOf == pet->GetGUID())
            return;
        _petTaunt = wanted;
        _petTauntOf = pet->GetGUID();
        for (auto const& [spellId, petSpell] : pet->m_spells)
        {
            if (petSpell.state == PETSPELL_REMOVED || petSpell.active == ACT_PASSIVE)
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
            if (info && info->IsAutocastable() && (info->HasEffect(SPELL_EFFECT_ATTACK_ME) || info->HasAura(SPELL_AURA_MOD_TAUNT)))
                pet->ToggleAutocast(info, wanted == 1);
        }
    }

    Unit* CompanionAI::SelectTankTarget()
    {
        if (!_protectTarget.IsEmpty())
            for (Unit* enemy : _view.Enemies)
                if (enemy->GetVictim() && enemy->GetVictim()->GetGUID() == _protectTarget && !IsCrowdControlTarget(enemy))
                    return enemy;

        if (!_forcedTarget.IsEmpty())
            if (Unit* forced = ObjectAccessor::GetUnit(*_bot, _forcedTarget))
                if (IsValidEnemy(forced, 100.0f))
                    return forced;

        // enemies hitting someone else: get them back, healers first
        Unit* loose = nullptr;
        int32 looseScore = std::numeric_limits<int32>::max();
        for (Unit* enemy : _view.Enemies)
        {
            if (IsCrowdControlTarget(enemy))
                continue;
            Unit* victim = enemy->GetVictim();
            if (!victim || victim == _bot)
                continue;
            Player* victimPlayer = victim->ToPlayer();
            if (!victimPlayer && victim->GetOwnerGUID() == _bot->GetGUID())
                continue;   // my own pet tanking is fine
            int32 score = IsHealer(victim) ? 0 : 100;
            score += int32(_bot->GetDistance(enemy));
            if (score < looseScore)
            {
                looseScore = score;
                loose = enemy;
            }
        }
        if (loose)
            return loose;

        // before anyone pulls it off: the enemy on me with the thinnest lead over the next one (melee take it at 110%,
        // casters at 130%), like a tank tabbing through a pack. The current target is kept unless another is clearly worse.
        Unit* current = _bot->GetVictim();
        Unit* thin = nullptr;
        float thinLead = 1.25f;
        for (Unit* enemy : _view.Enemies)
        {
            if (enemy->GetVictim() != _bot || IsCrowdControlTarget(enemy) || !enemy->CanHaveThreatList())
                continue;
            float mine = enemy->GetThreatManager().GetThreat(_bot);
            float next = 0.0f;
            for (ThreatReference const* ref : enemy->GetThreatManager().GetUnsortedThreatList())
                if (ref->GetVictim() != _bot)
                    next = std::max(next, ref->GetThreat());
            if (next <= 0.0f)
                continue;
            float lead = mine / next;
            if (enemy == current)
                lead += 0.1f;       // no back and forth for a few points
            if (lead < thinLead)
            {
                thinLead = lead;
                thin = enemy;
            }
        }
        if (thin)
            return thin;

        if (Group* group = _bot->GetGroup())
        {
            ObjectGuid skull = group->GetTargetIcon(ICON_SKULL);
            if (!skull.IsEmpty())
                if (Unit* marked = ObjectAccessor::GetUnit(*_bot, skull))
                    if (IsValidEnemy(marked) && std::find(_view.Enemies.begin(), _view.Enemies.end(), marked) != _view.Enemies.end())
                        return marked;
        }

        if (Unit* current = _bot->GetVictim())
            if (IsValidEnemy(current) && !IsCrowdControlTarget(current)
                && std::find(_view.Enemies.begin(), _view.Enemies.end(), current) != _view.Enemies.end())
                return current;

        if (Player* owner = _view.Owner)
            if (Unit* victim = owner->GetVictim())
                if (IsValidEnemy(victim) && !IsCrowdControlTarget(victim))
                    return victim;

        Unit* nearest = nullptr;
        for (Unit* enemy : _view.Enemies)
            if (!IsCrowdControlTarget(enemy) && (!nearest || _bot->GetDistance(enemy) < _bot->GetDistance(nearest)))
                nearest = enemy;
        return nearest;
    }

    void CompanionAI::UpdateCombat()
    {
        Role role = GetEffectiveRole();

        if (_class->Emergency())
            return;
        if (UsePotion())
            return;
        // a tank's first job: an enemy that went for someone else comes back, before buffs, dispels or interrupts
        if (role == Role::Tank && RecoverThreat())
            return;

        if (role == Role::Healer || _class->CanHeal())
            if (UpdateHealing(true))
                return;

        if (UpdateDispels())
            return;

        if (MechanicSpells())
            return;

        // a battle resurrection (druid) when nobody is in danger
        if (role == Role::Healer && !InjuredAllies(60.0f, 40.0f))
            for (Player* member : _view.Members)
                if (!member->IsAlive() && !member->IsResurrectRequested() && !member->HasPlayerFlag(PLAYER_FLAGS_GHOST)
                    && _bot->GetDistance(member) < 30.0f && _class->Resurrect(member))
                    return;

        if (_class->SelfBuffs(true))
            return;

        if (_aggression == Aggression::Passive && _forcedTarget.IsEmpty())
        {
            FollowOwner();
            return;
        }

        Unit* target = role == Role::Tank ? SelectTankTarget() : SelectTarget();
        if (target && role != Role::Tank)
            target = KeepUnderTank(target);
        if (!target)
        {
            if (role == Role::Healer)
                MoveForCombat(nullptr);
            else if (Unit* waiting = role == Role::MeleeDps && !_waitingFor.IsEmpty() ? ObjectAccessor::GetUnit(*_bot, _waitingFor) : nullptr)
            {
                // melee walk up with the tank and wait behind the enemy, ready to hit when it is taken
                MotionMaster* motion = _bot->GetMotionMaster();
                if (_movement != MoveMode::Chase || _moveTarget != waiting->GetGUID())
                {
                    motion->MoveChase(waiting, ChaseRange(4.0f, 7.0f), ChaseAngle(float(M_PI), float(M_PI_4)));
                    _movement = MoveMode::Chase;
                    _moveTarget = waiting->GetGUID();
                    _moveRangeMin = 4.0f;
                    _moveRangeMax = 7.0f;
                }
            }
            else if (!_holdingThreat)
                FollowOwner();      // (holding for threat at range: it stays where it stands)
            return;
        }

        if (target->GetGUID() != _lastTarget)
        {
            _lastTarget = target->GetGUID();
            _movement = MoveMode::None;
        }

        if (UpdateInterrupts())
            return;

        if (role == Role::Tank)
        {
            // threat first: enemies on others are taken back, by taunt, or all at once when several got loose
            uint32 loose = 0;
            for (Unit* enemy : _view.Enemies)
                if (enemy->GetVictim() && enemy->GetVictim() != _bot && enemy->GetVictim()->ToPlayer() && !IsCrowdControlTarget(enemy)
                    && _bot->GetDistance(enemy) < 10.0f)
                    ++loose;
            if (loose >= 3 && _class->AoeTaunt())
                return;
            // tank swaps (Mortal Wound, marks, Shadow of Ebonroc...): the other tank takes it at the stacks the rule names, and
            // the one who handed it over does not take it back while the debuff lasts
            bool handedOver = false;
            if (Creature* boss = target->ToCreature())
                for (Mechanics::Rule const* rule : Mechanics::RulesFor(boss->GetEntry()))
                    if (rule->Type == Mechanics::Kind::TankSwap)
                    {
                        Unit* holder = target->GetVictim();
                        Aura* mine = _bot->GetAura(rule->Spell);
                        uint32 myStacks = mine ? std::max<uint32>(1, mine->GetStackAmount()) : 0;
                        if (holder && holder != _bot && IsTank(holder) && myStacks > 0)
                            handedOver = true;
                        else if (holder && holder != _bot && IsTank(holder) && myStacks == 0)
                            if (Aura* theirs = holder->GetAura(rule->Spell))
                                if (std::max<uint32>(1, theirs->GetStackAmount()) >= std::max<uint32>(1, rule->Value) && _class->Taunt(target))
                                    return;
                    }
            if (!handedOver && target->GetVictim() && target->GetVictim() != _bot && _class->Taunt(target))
                return;
        }

        // an enemy this bot was told to keep under control
        if (!_ccTarget.IsEmpty())
        {
            Unit* cc = GameTime::Now() < _ccUntil ? ObjectAccessor::GetUnit(*_bot, _ccTarget) : nullptr;
            // like a player: someone hits it, or it broke twice already, then it dies with the rest
            if (!cc || !IsValidEnemy(cc) || (!cc->HasBreakableByDamageCrowdControlAura() && (AttackedByGroup(cc) || _ccCasts >= 2)))
            {
                _ccTarget.Clear();
                _ccCasts = 0;
            }
            else if (!cc->HasBreakableByDamageCrowdControlAura() && _class->CrowdControl(cc))
            {
                ++_ccCasts;
                _ccSince = GameTime::Now();
                sCompanionMgr->ClaimCrowdControl(_ccTarget, _bot->GetGUID(), 5000);
                return;
            }
            else if (cc->HasBreakableByDamageCrowdControlAura())
            {
                _ccSince = GameTime::Now();     // still under control: the others keep away
                sCompanionMgr->ClaimCrowdControl(_ccTarget, _bot->GetGUID(), 5000);
            }
        }
        else if (AutoCrowdControl(target))
            return;

        // crowd control the moon
        if (Group* group = _bot->GetGroup())
        {
            ObjectGuid moon = group->GetTargetIcon(ICON_MOON);
            if (!moon.IsEmpty())
                if (Unit* ccTarget = ObjectAccessor::GetUnit(*_bot, moon))
                    if (IsValidEnemy(ccTarget) && !ccTarget->HasBreakableByDamageCrowdControlAura() && !AttackedByGroup(ccTarget)
                        && _class->CrowdControl(ccTarget))
                        return;
        }

        MoveForCombat(role == Role::Healer ? nullptr : target);

        // what a player asked it to use more
        if (UsePreferred(target))
            return;

        if (role == Role::Healer)
        {
            // nothing to heal: help with damage while mana is comfortable
            if (ManaPct() > 60.0f && !InjuredAllies(80.0f, 40.0f))
                _class->Combat(target);
            return;
        }

        if (_pulling && role == Role::Tank && !target->IsInCombat())
        {
            if (_class->Pull(target))
                return;
        }

        _class->Pet(target);
        _class->Combat(target);
    }

    void CompanionAI::UpdateIdle()
    {
        if (_eating)
        {
            bool full = HealthPct() >= 95.0f && (!UsesMana(_bot) || ManaPct() >= 95.0f);
            bool resting = _bot->HasAura([](Aura const* aura) { return IsFoodOrDrink(aura); });
            Player* owner = _view.Owner;
            bool ownerLeft = owner && _bot->GetDistance(owner) > 30.0f;
            if (full || !resting || ownerLeft)
            {
                _eating = false;
                _restRequested = false;
                if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
                    _bot->SetStandState(UNIT_STAND_STATE_STAND);
            }
            else
                return;
        }

        if (UpdateResurrections())
            return;
        if (UpdateHealing(false))
            return;
        if (UpdateDispels())
            return;
        if (UpdateLooting())
            return;
        if (UpdateEatDrink())
            return;
        if (UpdateBuffs(false))
            return;
        if (_class->OutOfCombat())
            return;

        UpdateMount();
        if (FollowPullingTank())
            return;
        FollowOwner();
    }

    bool CompanionAI::AutoCrowdControl(Unit* killTarget)
    {
        Role role = GetEffectiveRole();
        if (!_autoCC || role == Role::Tank || role == Role::Healer || _bot->GetClass() == CLASS_ROGUE || GameTime::Now() < _ccCheckAt)
            return false;
        _ccCheckAt = GameTime::Now() + Seconds(2);
        // adds a boss fight wants controlled (Firesworn banished, Flamewakers sheeped...) count from two
        auto ruled = [](Unit const* enemy)
        {
            Creature const* creature = enemy->ToCreature();
            return creature && Mechanics::HasRule(creature->GetEntry(), Mechanics::Kind::CrowdControlNpc);
        };
        bool const anyRuled = std::any_of(_view.Enemies.begin(), _view.Enemies.end(), ruled);
        if (_view.Enemies.size() < (anyRuled ? 2u : 3u))
            return false;

        // keep two to kill at least; never the kill target, the tank's, a marked one or one half dead
        uint32 controlled = 0;
        for (Unit* enemy : _view.Enemies)
            controlled += IsCrowdControlTarget(enemy) ? 1 : 0;
        if (_view.Enemies.size() - controlled < (anyRuled ? 2u : 3u))
            return false;
        Unit* tankTarget = _view.Tank && _view.Tank != _bot ? _view.Tank->GetVictim() : nullptr;
        Group* group = _bot->GetGroup();
        Unit* pick = nullptr;
        for (Unit* enemy : _view.Enemies)
        {
            if (enemy == killTarget || enemy == tankTarget || IsCrowdControlTarget(enemy) || AttackedByGroup(enemy) || enemy->GetHealthPct() < 90.0f
                || !_class->CanCrowdControl(enemy) || _bot->GetDistance(enemy) > 30.0f)
                continue;
            if (Creature* creature = enemy->ToCreature())
                if (creature->isWorldBoss() || creature->IsDungeonBoss())
                    continue;
            if (group && (group->GetTargetIcon(ICON_SKULL) == enemy->GetGUID() || group->GetTargetIcon(ICON_CROSS) == enemy->GetGUID()))
                continue;
            // the ones the fight names first, else the healthiest one, the one the group would reach last
            if (!pick || (ruled(enemy) && !ruled(pick)) || (ruled(enemy) == ruled(pick) && enemy->GetHealthPct() > pick->GetHealthPct()))
                pick = enemy;
        }
        if (!pick)
            return false;
        OrderCrowdControl(pick->GetGUID());
        static char const* const verbs[] = { "", "", "", "Scaring", "", "Shackling", "", "", "Sheep on", "Banishing", "", "Hibernate on" };
        uint8 classId = _bot->GetClass();
        std::string verb = classId < std::size(verbs) && *verbs[classId] ? verbs[classId] : "Controlling";
        Callout("cc", "you start to crowd control " + pick->GetName() + " to keep it out of the fight", verb + " " + pick->GetName() + "!");
        return _class->CrowdControl(pick);
    }

    bool CompanionAI::AvoidGroundEffects()
    {
        Position center;
        float radius = 0.0f;
        uint32 lasts = 8000;
        bool danger = false;

        // zones it left: forgotten when they end
        TimePoint now = GameTime::Now();
        _dangerZones.erase(std::remove_if(_dangerZones.begin(), _dangerZones.end(), [now](DangerZone const& zone) { return zone.Until <= now; }),
            _dangerZones.end());

        // persistent area spells (Rain of Fire, Blizzard, Consecration, Flamestrike...): their aura on us belongs to a dynamic
        // object; traps (fire, lava) put theirs from a game object
        for (auto const& [spellId, application] : _bot->GetAppliedAuras())
        {
            Aura const* aura = application->GetBase();
            if (application->IsPositive())
                continue;
            if (aura->GetType() == DYNOBJ_AURA_TYPE)
            {
                DynamicObject* area = aura->GetDynobjOwner();
                Unit* caster = aura->GetCaster();
                if (!area || (caster && !caster->IsHostileTo(_bot)))
                    continue;
                center = area->GetPosition();
                radius = std::max(area->GetRadius(), 3.0f);
                lasts = aura->GetDuration() > 0 ? uint32(aura->GetDuration()) : 10000;
                danger = true;
                break;
            }
            if (aura->GetCasterGUID().IsGameObject())
                if (GameObject* trap = ObjectAccessor::GetGameObject(*_bot, aura->GetCasterGUID()))
                {
                    center = trap->GetPosition();
                    radius = 5.0f;
                    danger = true;
                    break;
                }
        }
        // lava and slime: the floor itself burns
        if (!danger && InLiquid(_bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ(), true))
        {
            center = _bot->GetPosition();
            radius = 4.0f;
            lasts = 3000;
            danger = true;
        }
        // ground effects of newer content are area triggers
        if (!danger)
            for (AreaTrigger* trigger : _bot->GetInsideAreaTriggers())
            {
                Unit* caster = trigger->GetCaster();
                if (!caster || !caster->IsHostileTo(_bot))
                    continue;
                center = trigger->GetPosition();
                radius = std::max(trigger->GetMaxSearchRadius(), 3.0f);
                danger = true;
                break;
            }
        if (!danger)
            return false;
        // remember it, so that following, resting spots and chasing keep out of it while it lasts
        auto known = std::find_if(_dangerZones.begin(), _dangerZones.end(), [&](DangerZone const& zone) { return zone.Center.GetExactDist2d(center) < 1.0f; });
        TimePoint until = now + Milliseconds(std::clamp<uint32>(lasts, 2000, 30000));
        if (known != _dangerZones.end())
            known->Until = until;
        else
            _dangerZones.push_back({ center, radius, until });
        _loitering = false;
        if (now < _avoidUntil && _bot->isMoving())
            return true;    // on its way out

        // the nearest way out: straight away from the middle, else a little to either side; in sight, on the ground
        float away = center.GetExactDist2d(_bot) > 0.5f ? center.GetAbsoluteAngle(_bot)
            : (_view.Owner ? center.GetAbsoluteAngle(_view.Owner) : frand(0.0f, 2.0f * float(M_PI)));
        float const out = radius + 3.0f;
        Position dest = _bot->GetPosition();
        for (float turn : { 0.0f, 0.6f, -0.6f, 1.2f, -1.2f, 2.0f, -2.0f, float(M_PI) })
        {
            float x = center.GetPositionX() + std::cos(away + turn) * out;
            float y = center.GetPositionY() + std::sin(away + turn) * out;
            float z = _bot->GetPositionZ();
            _bot->UpdateAllowedPositionZ(x, y, z);
            if (_bot->IsWithinLOS(x, y, z + 1.5f) && std::fabs(z - _bot->GetPositionZ()) < 6.0f && !InLiquid(x, y, z, false))
            {
                dest.Relocate(x, y, z);
                break;
            }
        }
        if (_bot->IsNonMeleeSpellCast(false, false, true))
            _bot->InterruptNonMeleeSpells(false);
        _bot->GetMotionMaster()->MovePoint(0, dest);
        _movement = MoveMode::Point;
        _avoidUntil = GameTime::Now() + Milliseconds(2500);
        return true;
    }

    bool CompanionAI::InDangerZone(Position const& pos, float margin) const
    {
        for (DangerZone const& zone : _dangerZones)
            if (zone.Center.GetExactDist2d(pos) < zone.Radius + margin)
                return true;
        return false;
    }

    // the tank goes for a pull: the melee walk with it instead of waiting at the leader's side
    bool CompanionAI::FollowPullingTank()
    {
        if (GetEffectiveRole() != Role::MeleeDps || _stance == Stance::Stay || _aggression == Aggression::Passive)
            return false;
        Player* tank = _view.Tank;
        CompanionAI* tankAI = tank && tank != _bot ? sCompanionMgr->GetAI(tank) : nullptr;
        if (!tankAI || !tankAI->IsPulling())
            return false;
        Unit* target = ObjectAccessor::GetUnit(*_bot, tankAI->GetPullTarget());
        if (!target || !target->IsAlive())
            return false;
        // behind the tank, a little apart
        if (_movement != MoveMode::Follow || _moveTarget != tank->GetGUID())
        {
            _bot->GetMotionMaster()->MoveFollow(tank, frand(3.0f, 5.0f), ChaseAngle(float(M_PI) + _formationSide * frand(0.3f, 0.8f), 0.3f));
            _movement = MoveMode::Follow;
            _moveTarget = tank->GetGUID();
        }
        return true;
    }

    bool CompanionAI::UpdateHealing(bool inCombat)
    {
        Role role = GetEffectiveRole();
        bool healer = role == Role::Healer;
        if (!_class->CanHeal())
            return false;
        // Nefarian's priest call and the like: its heals would hurt
        for (auto const& [spellId, application] : _bot->GetAppliedAuras())
            if (Mechanics::FindSpellRule(spellId, Mechanics::Kind::NoHealWithAura))
                return false;

        // A healer lets small damage be: in combat it heals the tank from 85% and the others from 75%, lower as its mana runs
        // out (what it does not cast comes back as regeneration); out of combat it tops up only with mana to spare, else the
        // group eats. Health counts what is already coming (heals being cast, heals over time). Hybrids in another role only
        // save lives.
        float threshold = inCombat ? 30.0f : 60.0f;
        float tankThreshold = threshold;
        if (healer)
        {
            float const mana = ManaPct();
            if (inCombat)
            {
                threshold = mana < 25.0f ? 50.0f : (mana < 50.0f ? 65.0f : 75.0f);
                tankThreshold = threshold + 10.0f;
            }
            else
                threshold = tankThreshold = mana > 60.0f ? 80.0f : 50.0f;
        }

        Unit* best = nullptr;
        float bestPct = 101.0f;
        float bestScore = 101.0f;
        uint32 injured = 0;
        for (Player* member : _view.Members)
        {
            if (!member->IsAlive() || _bot->GetDistance(member) > 40.0f || !_bot->IsWithinLOSInMap(member))
                continue;
            uint64 health = member->GetHealth() + IncomingHealing(member);
            float pct = std::min(100.0f, float(health) * 100.0f / float(std::max<uint64>(1, member->GetMaxHealth())));
            if (pct < 75.0f && _bot->GetDistance(member) < 30.0f)
                ++injured;
            if (!healer && member != _bot && inCombat && pct > 25.0f)
                continue;
            bool const tank = IsTank(member);
            float const score = tank ? pct - 10.0f : pct;     // the tank first among the equally hurt
            if (pct < (tank ? tankThreshold : threshold) && score < bestScore)
            {
                bestScore = score;
                bestPct = pct;
                best = member;
            }
        }

        // pets of the group, lower priority
        if (!best && healer)
        {
            for (Player* member : _view.Members)
                if (Pet* pet = member->GetPet())
                    if (pet->IsAlive() && pet->GetHealthPct() < 50.0f && _bot->GetDistance(pet) < 40.0f)
                    {
                        best = pet;
                        bestPct = pet->GetHealthPct();
                        break;
                    }
        }

        if (healer && injured >= 3 && _class->GroupHeal(injured))
            return true;

        if (!best)
            return false;

        HealUrgency urgency = bestPct < 35.0f ? HealUrgency::Emergency : (inCombat && bestPct < 70.0f ? HealUrgency::Normal : HealUrgency::TopUp);
        uint64 incoming = IncomingHealing(best);
        uint64 missing = best->GetMaxHealth() > best->GetHealth() + incoming ? best->GetMaxHealth() - best->GetHealth() - incoming : 0;
        if (!missing)
            return false;

        // healers stay in range of whoever needs them
        if (_bot->GetDistance(best) > 36.0f || !_bot->IsWithinLOSInMap(best))
        {
            _bot->GetMotionMaster()->MovePoint(0, best->GetPosition());
            _movement = MoveMode::Point;
            return true;
        }

        uint32 const deficit = uint32(std::min<uint64>(missing, std::numeric_limits<uint32>::max()));
        if (HealPreferred(best, deficit))
            return true;
        return _class->Heal(best, urgency, deficit);
    }

    uint32 CompanionAI::DispellableMask(Unit* target) const
    {
        DispelChargesList list;
        target->GetDispellableAuraList(_bot, DISPEL_ALL_MASK, list);
        uint32 mask = 0;
        for (DispelableAura const& dispelable : list)
            if (Aura* aura = dispelable.GetAura())
                if (aura->GetSpellInfo()->Dispel < 32 && !Mechanics::FindSpellRule(aura->GetId(), Mechanics::Kind::NoDispel))
                    mask |= 1u << aura->GetSpellInfo()->Dispel;
        return mask;
    }

    bool CompanionAI::UpdateDispels()
    {
        // what a boss fight names first (Impending Doom, Living Bomb's friends...)
        for (Player* member : _view.Members)
        {
            if (!member->IsAlive() || _bot->GetDistance(member) > 36.0f)
                continue;
            bool urgent = false;
            for (auto const& [spellId, application] : member->GetAppliedAuras())
                if (Mechanics::FindSpellRule(spellId, Mechanics::Kind::DispelFirst))
                    urgent = true;
            if (urgent && DispellableMask(member) && _class->Dispel(member))
                return true;
        }
        for (Player* member : _view.Members)
        {
            if (!member->IsAlive() || _bot->GetDistance(member) > 36.0f)
                continue;
            if (DispellableMask(member) && _class->Dispel(member))
                return true;
        }
        return false;
    }

    bool CompanionAI::UpdateInterrupts()
    {
        std::vector<Unit*> order = _view.Enemies;
        // heals, fears and what a boss rule names go first
        auto priority = [](Unit* unit)
        {
            Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!spell)
                spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            if (!spell)
                return 0;
            if (Mechanics::FindSpellRule(spell->GetSpellInfo()->Id, Mechanics::Kind::Interrupt))
                return 2;
            return Mechanics::IsUrgentInterrupt(spell->GetSpellInfo()) ? 1 : 0;
        };
        std::stable_sort(order.begin(), order.end(), [&](Unit* a, Unit* b) { return priority(a) > priority(b); });
        if (!_interruptTarget.IsEmpty())
        {
            if (GameTime::Now() >= _interruptUntil)
                _interruptTarget.Clear();
            else
                std::stable_partition(order.begin(), order.end(), [this](Unit* u) { return u->GetGUID() == _interruptTarget; });
        }
        for (Unit* enemy : order)
        {
            if (_bot->GetDistance(enemy) > 30.0f)
                continue;
            Spell* spell = enemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!spell)
                spell = enemy->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            if (!spell)
                continue;
            if (spell->getState() != SPELL_STATE_PREPARING && spell->getState() != SPELL_STATE_CHANNELING)
                continue;
            if (!spell->GetSpellInfo()->CanBeInterrupted(_bot, enemy))
                continue;
            if (!sCompanionMgr->CanInterrupt(enemy->GetGUID(), _bot->GetGUID()))
                continue;
            if (_class->Interrupt(enemy))
            {
                sCompanionMgr->CommitInterrupt(enemy->GetGUID(), _bot->GetGUID());
                return true;
            }
        }
        return false;
    }

    bool CompanionAI::UpdateResurrections()
    {
        for (Player* member : _view.Members)
        {
            if (member == _bot || member->IsAlive() || member->IsResurrectRequested())
                continue;
            if (member->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                continue;   // released: can only come back at the corpse
            if (_bot->GetDistance(member) > 30.0f)
            {
                _bot->GetMotionMaster()->MovePoint(0, member->GetPosition());
                _movement = MoveMode::Point;
                return true;
            }
            if (_class->Resurrect(member))
                return true;
        }
        return false;
    }

    bool CompanionAI::UpdateBuffs(bool inCombat)
    {
        // asked for: cast again now, even over a buff that is still up
        while (!_rebuffQueue.empty())
        {
            Player* member = ObjectAccessor::GetPlayer(*_bot, _rebuffTarget);
            if (!member || !member->IsAlive() || _bot->GetDistance(member) > 30.0f)
            {
                _rebuffQueue.clear();
                break;
            }
            std::string spell = _rebuffQueue.front();
            _rebuffQueue.erase(_rebuffQueue.begin());
            if (Cast(spell, member))
                return true;
        }
        if (_class->SelfBuffs(inCombat))
            return true;
        if (inCombat)
            return false;
        for (Player* member : _view.Members)
            if (member->IsAlive() && _bot->GetDistance(member) < 30.0f && _class->GroupBuffs(member))
                return true;
        return false;
    }

    std::string CompanionAI::BlessingFor(ObjectGuid member) const
    {
        auto itr = _blessingFor.find(member);
        return itr != _blessingFor.end() ? itr->second : std::string();
    }

    bool CompanionAI::OrderRebuff(Player* member, std::string const& filter)
    {
        // what players call them: "int" is Arcane Intellect, "fort" Fortitude, "motw" Mark of the Wild...
        static std::pair<char const*, char const*> const aliases[] = { { "int", "intellect" }, { "ai", "intellect" }, { "fort", "fortitude" },
            { "stamina", "fortitude" }, { "motw", "mark of the wild" }, { "mark", "mark of the wild" }, { "wild", "mark of the wild" },
            { "bom", "might" }, { "bok", "kings" }, { "bow", "wisdom" }, { "salv", "salvation" }, { "spirit", "spirit" } };
        std::string wanted = ToLower(filter);
        for (auto const& [alias, full] : aliases)
            if (wanted == alias)
                wanted = full;
        std::vector<std::string> queue;
        for (std::string_view spell : _class->OwnBuffs(member))
            if (Knows(spell) && (wanted.empty() || std::string(spell).find(wanted) != std::string::npos))
                queue.emplace_back(spell);
        if (queue.empty())
            return false;
        _rebuffTarget = member->GetGUID();
        _rebuffQueue = std::move(queue);
        return true;
    }

    std::string CompanionAI::OrderEquip(std::string const& what)
    {
        ScanBags();     // what was just handed over counts as "it"
        std::string wanted = ToLower(what);
        std::vector<Item*> candidates;
        _bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
        {
            if (wanted.empty() || wanted == "it" || wanted == "that" || wanted == "last")
            {
                if (item->GetGUID() == _lastNewItem)
                    candidates.push_back(item);
            }
            else if (wanted == "upgrades" || wanted == "all" || wanted == "everything")
            {
                if (sCompanionFactory->IsUpgrade(_bot, item, GetEffectiveRole()))
                    candidates.push_back(item);
            }
            else if (ToLower(item->GetTemplate()->GetName(LOCALE_enUS)).find(wanted) != std::string::npos)
                candidates.push_back(item);
            return ItemSearchCallbackResult::Continue;
        });
        if (candidates.empty())
            return "I don't have that.";
        std::string equipped;
        for (Item* item : candidates)
        {
            std::string name = item->GetTemplate()->GetName(LOCALE_enUS);
            if (!CompanionFactory::EquipFromBags(_bot, item))
            {
                if (candidates.size() == 1)
                    return "I can't use " + name + ".";
                continue;
            }
            equipped += (equipped.empty() ? "" : ", ") + name;
        }
        // what went back to the bags (the old piece) is not "it"
        ScanBags();
        _lastNewItem.Clear();
        return equipped.empty() ? "Nothing I can use there." : "Equipped " + equipped + ".";
    }

    void CompanionAI::HandleLoot()
    {
        // a roll waiting for this bot: Need on a clear upgrade it can use, else Pass (the greed is for the players)
        std::vector<LootRoll*> const& rolls = _bot->GetLootRolls();
        if (!rolls.empty())
        {
            LootRoll* roll = rolls.front();
            uint32 itemId = roll->GetItemId();
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            // judged with the stats this drop rolled, against what it wears and carries
            LootItem const* rolled = roll->GetLootItem();
            bool need = proto && rolled && sCompanionFactory->IsUpgrade(_bot, *rolled, GetEffectiveRole());
            if (need && roll->PlayerVote(_bot, RollVote::Need))
            {
                _wantedLoot.push_back(itemId);
                Say("Need on " + std::string(proto->GetName(LOCALE_enUS)) + ", that's an upgrade for me.");
            }
            else
                roll->PlayerVote(_bot, RollVote::Pass);
            return;     // one vote per second: the next one on the next tick
        }

        // put on what it rolled Need for
        if (Item* toEquip = ScanBags(); toEquip && CompanionFactory::EquipFromBags(_bot, toEquip))
        {
            Say("Equipped " + std::string(toEquip->GetTemplate()->GetName(LOCALE_enUS)) + ".");
            ObjectGuid received = _lastNewItem;
            ScanBags();             // the old piece back in the bags is not "it"
            _lastNewItem = received;
        }
    }

    // new things in the bags: remembers the last one ("equip it"); returns one it rolled Need for
    Item* CompanionAI::ScanBags()
    {
        std::vector<ObjectGuid> seen;
        Item* wantedItem = nullptr;
        _bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* item)
        {
            seen.push_back(item->GetGUID());
            if (std::find(_knownBagItems.begin(), _knownBagItems.end(), item->GetGUID()) == _knownBagItems.end() && !_knownBagItems.empty())
                _lastNewItem = item->GetGUID();
            auto wanted = std::find(_wantedLoot.begin(), _wantedLoot.end(), item->GetEntry());
            if (wanted != _wantedLoot.end() && !wantedItem)
            {
                _wantedLoot.erase(wanted);
                wantedItem = item;
            }
            return ItemSearchCallbackResult::Continue;
        });
        if (_knownBagItems.empty() && seen.empty())
            seen.push_back(ObjectGuid::Empty);   // an empty bag also counts as seen
        _knownBagItems = std::move(seen);
        return wantedItem;
    }

    bool CompanionAI::NeedsRest() const
    {
        if (_restRequested)
            return HealthPct() < 95.0f || (UsesMana(_bot) && ManaPct() < 95.0f);
        return HealthPct() < 55.0f || (UsesMana(_bot) && ManaPct() < 40.0f);
    }

    bool CompanionAI::UpdateEatDrink()
    {
        if (!GetConfig().FreeFood || !NeedsRest() || _bot->IsMounted() || _bot->IsInWater())
            return false;

        Player* owner = _view.Owner;
        if (owner && (owner->isMoving() || _bot->GetDistance(owner) > 25.0f))
            return false;

        StopMovement();
        bool started = false;
        if (HealthPct() < 95.0f)
            if (uint32 food = sCompanionFactory->GetFoodSpell(_bot->GetLevel()))
                started |= _bot->CastSpell(_bot, food, CastSpellExtraArgs(TRIGGERED_NONE)) == SPELL_CAST_OK;
        if (UsesMana(_bot) && ManaPct() < 95.0f)
            if (uint32 drink = sCompanionFactory->GetDrinkSpell(_bot->GetLevel()))
                started |= _bot->CastSpell(_bot, drink, CastSpellExtraArgs(TRIGGERED_IGNORE_CAST_IN_PROGRESS)) == SPELL_CAST_OK;
        _eating = started;
        return started;
    }

    bool CompanionAI::UpdatePendingCast()
    {
        if (_pendingCast.empty())
            return false;

        std::string spell = std::move(_pendingCast);
        _pendingCast.clear();
        Unit* target = _pendingCastTarget.IsEmpty() ? _bot : ObjectAccessor::GetUnit(*_bot, _pendingCastTarget);
        if (!target)
            return false;
        if (!Knows(spell))
        {
            Say("I don't know " + spell + ".");
            return false;
        }
        _allowAvoided = true;     // asked for by name: that overrides "never use it"
        bool cast = Cast(spell, target);
        _allowAvoided = false;
        if (!cast)
        {
            Say("I can't cast " + spell + " right now.");
            return false;
        }
        return true;
    }

    // its own mount: the warlock's or paladin's when it knows it, else one of its race's (a gnome on a mechanostrider, a
    // dwarf on a ram), always the same one for this bot; swift (100%) when its player rides a swift one
    uint32 CompanionAI::MountFor(bool swift) const
    {
        struct ClassMount { uint8 Class; uint32 Normal; uint32 Swift; };
        static ClassMount const ClassMounts[] = { { CLASS_WARLOCK, 5784, 23161 }, { CLASS_PALADIN, 13819, 23214 } };
        for (ClassMount const& mount : ClassMounts)
            if (_bot->GetClass() == mount.Class)
            {
                if (swift && _bot->HasSpell(mount.Swift))
                    return mount.Swift;
                if (_bot->HasSpell(mount.Normal))
                    return mount.Normal;
            }

        static std::unordered_map<uint8, std::pair<std::vector<uint32>, std::vector<uint32>>> const RaceMounts = {
            { RACE_HUMAN,    { { 458, 470, 472, 6648 },          { 23227, 23228, 23229 } } },  // horses
            { RACE_ORC,      { { 580, 6653, 6654 },              { 23250, 23251, 23252 } } },  // wolves
            { RACE_DWARF,    { { 6777, 6898, 6899 },             { 23238, 23239, 23240 } } },  // rams
            { RACE_NIGHTELF, { { 8394, 10789, 10793 },           { 23219, 23221, 23338 } } },  // sabers
            { RACE_UNDEAD_PLAYER, { { 17462, 17463, 17464 },     { 17465, 23246 } } },         // skeletal horses
            { RACE_TAUREN,   { { 18989, 18990 },                 { 23247, 23248, 23249 } } },  // kodos
            { RACE_GNOME,    { { 10873, 10969, 17453, 17454 },   { 23222, 23223, 23225 } } },  // mechanostriders
            { RACE_TROLL,    { { 8395, 10796, 10799 },           { 23241, 23242, 23243 } } },  // raptors
        };
        auto itr = RaceMounts.find(_bot->GetRace());
        if (itr == RaceMounts.end())
            return 0;
        std::vector<uint32> known;
        for (uint32 spell : swift ? itr->second.second : itr->second.first)
            if (sSpellMgr->GetSpellInfo(spell, DIFFICULTY_NONE))
                known.push_back(spell);
        return known.empty() ? 0 : known[_bot->GetGUID().GetCounter() % known.size()];
    }

    void CompanionAI::UpdateMount()
    {
        Player* owner = _view.Owner;
        if (!owner)
            return;

        if (owner->IsMounted() && !_bot->IsMounted() && _bot->GetLevel() >= GetConfig().MountLevel && !_bot->IsInWater()
            && _bot->GetMap()->IsWorldMap() && _bot->IsOutdoors())
        {
            Unit::AuraEffectList const& mounts = owner->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
            if (mounts.empty())
                return;
            bool swift = false;
            for (AuraEffect const* speed : owner->GetAuraEffectsByType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED))
                swift = swift || speed->GetAmount() >= 100;
            uint32 mount = MountFor(swift);
            _bot->CastSpell(_bot, mount ? mount : mounts.front()->GetId(), CastSpellExtraArgs(TRIGGERED_FULL_MASK));
        }
        else if (!owner->IsMounted() && _bot->IsMounted())
            _bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    }

    void CompanionAI::MoveForCombat(Unit* target)
    {
        Role role = GetEffectiveRole();

        if (role == Role::Healer || !target)
        {
            // stay in healing range of the tank, out of melee
            Unit* anchor = _view.Tank && _view.Tank != _bot ? _view.Tank : _view.Owner;
            if (!anchor || anchor == _bot)
                return;
            float dist = _bot->GetDistance(anchor);
            if (_stance == Stance::Stay)
                return;
            // a boss mechanic put it where it is: it stays there unless the tank is out of reach
            if (HoldsForMechanic() && dist < 38.0f)
            {
                if (target)
                    FaceTarget(target);
                return;
            }
            if (dist > 30.0f || !_bot->IsWithinLOSInMap(anchor))
            {
                if (_movement != MoveMode::Follow || _moveTarget != anchor->GetGUID())
                {
                    _bot->GetMotionMaster()->MoveFollow(anchor, 15.0f, ChaseAngle(float(M_PI)));
                    _movement = MoveMode::Follow;
                    _moveTarget = anchor->GetGUID();
                }
            }
            else if (_movement == MoveMode::Follow && dist < 22.0f)
            {
                StopMovement();
                _movement = MoveMode::Stay;
            }
            if (target)
                FaceTarget(target);
            return;
        }

        bool melee = role == Role::Tank || role == Role::MeleeDps;
        // a hunter caught in melee swings its weapon (and its Raptor Strike goes off with the swing)
        EngageTarget(target, melee || (_bot->GetClass() == CLASS_HUNTER && InMelee(target)));

        // a ground effect still burning nearby: stay out of it (melee go in only for a target standing outside it)
        if (!_dangerZones.empty() && (!melee || InDangerZone(target->GetPosition(), 1.0f)))
        {
            if (_movement == MoveMode::Chase)
            {
                StopMovement();
                _movement = MoveMode::None;
            }
            FaceTarget(target);
            return;
        }

        // just landed from a charge: the client still shows its end, a new chase right now would make it jump
        if (GameTime::Now() < _chargeSettleUntil)
            return;

        if (_stance == Stance::Stay && _forcedTarget.IsEmpty())
            return;

        MotionMaster* motion = _bot->GetMotionMaster();
        bool chasing = motion->GetCurrentMovementGeneratorType() == CHASE_MOTION_TYPE;

        // a boss mechanic is active (a cast to stay out of, spreading, a debuffed friend): no running back in
        if (HoldsForMechanic() && (melee || _bot->GetDistance(target) <= _class->RangedDistance() + 5.0f))
        {
            if (_movement == MoveMode::Chase)
            {
                StopMovement();
                _movement = MoveMode::None;
            }
            FaceTarget(target);
            return;
        }

        if (melee)
        {
            Optional<ChaseAngle> angle;
            if (role == Role::MeleeDps && target->GetVictim() && target->GetVictim() != _bot)
                angle = ChaseAngle(float(M_PI), float(M_PI_4));
            // behind or at the flanks of a boss whose front (and tail) hurts
            if (role == Role::MeleeDps && _mechanicAngle)
                angle = ChaseAngle(*_mechanicAngle, float(M_PI) / 6.0f);
            float wantMin = 0.0f, wantMax = angle ? 1.0f : 0.0f;
            float wantAngle = _mechanicAngle && role == Role::MeleeDps ? *_mechanicAngle : -1.0f;
            if (_movement != MoveMode::Chase || !chasing || _moveTarget != target->GetGUID() || _moveRangeMin != wantMin || _moveRangeMax != wantMax
                || _moveAngle != wantAngle)
            {
                motion->MoveChase(target, {}, angle);
                _movement = MoveMode::Chase;
                _moveTarget = target->GetGUID();
                _moveRangeMin = wantMin;
                _moveRangeMax = wantMax;
                _moveAngle = wantAngle;
            }
            return;
        }

        float maxRange = _class->RangedDistance();
        // when the mob comes for us, stand and cast: running in circles helps nobody
        // casters and hunters stand well back (out of cleaves and the hunter's dead zone)
        float minRange = (target->GetVictim() == _bot) ? 0.0f : std::min(18.0f, maxRange - 6.0f);
        // out of a breath's front: at its side, well back
        Optional<ChaseAngle> angle;
        if (_mechanicAngle)
            angle = ChaseAngle(*_mechanicAngle, float(M_PI) / 4.0f);
        float wantAngle = _mechanicAngle ? *_mechanicAngle : -1.0f;
        if (_movement != MoveMode::Chase || !chasing || _moveTarget != target->GetGUID() || _moveRangeMin != minRange || _moveRangeMax != maxRange
            || _moveAngle != wantAngle)
        {
            motion->MoveChase(target, ChaseRange(minRange, minRange + 2.0f, std::max(minRange + 3.0f, maxRange - 5.0f), maxRange), angle);
            _movement = MoveMode::Chase;
            _moveTarget = target->GetGUID();
            _moveRangeMin = minRange;
            _moveRangeMax = maxRange;
            _moveAngle = wantAngle;
        }
    }

    void CompanionAI::EngageTarget(Unit* target, bool melee)
    {
        if (_bot->IsMounted())
            _bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);

        bool meleeOn = _bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
        if (_bot->GetVictim() != target || meleeOn != melee)
            _bot->Attack(target, melee);
        if (_bot->GetTarget() != target->GetGUID())
            _bot->SetSelection(target->GetGUID());
        FaceTarget(target);
    }

    void CompanionAI::FaceTarget(Unit* target)
    {
        if (!target || target == _bot)
            return;
        if (!_bot->HasInArc(float(M_PI) / 3.0f, target) && !_bot->isMoving())
            _bot->SetFacingToObject(target);
        else
            _bot->SetInFront(target);
    }

    void CompanionAI::DisengageAll()
    {
        if (_bot->GetVictim())
            _bot->AttackStop();
        _bot->SetSelection(ObjectGuid::Empty);
        if (Creature* pet = GetPet())
            if (pet->GetVictim())
                PetFollow();
        _movement = MoveMode::None;
    }

    void CompanionAI::FollowOwner()
    {
        Player* owner = _view.Owner;
        MotionMaster* motion = _bot->GetMotionMaster();

        if (_stance == Stance::Stay)
        {
            if (_movement != MoveMode::Stay)
            {
                if (_bot->GetExactDist(_stayPosition) > 3.0f)
                    motion->MovePoint(0, _stayPosition);
                else
                    StopMovement();
                _movement = MoveMode::Stay;
            }
            return;
        }

        if (!owner || owner == _bot)
            return;
        if (_bot->GetVictim())
            _bot->AttackStop();

        TimePoint now = GameTime::Now();

        bool ownerMoving = owner->isMoving() || owner->GetExactDist2d(_lastOwnerPosition) > 0.3f;
        _lastOwnerPosition = owner->GetPosition();
        if (ownerMoving)
        {
            _ownerStillSince = now;
            _loitering = false;
        }

        // travelling: each bot follows in its part of the group (tank ahead, healers and casters behind), drifting now and then
        if (ownerMoving || Elapsed(_ownerStillSince) < _loiterDelay)
        {
            bool following = _movement == MoveMode::Follow && _moveTarget == owner->GetGUID()
                && motion->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE;
            if (following && now < _scatterUntil)
                return;
            float angle, halfWidth, slotMin, slotMax;
            FormationSlot(true, angle, halfWidth, slotMin, slotMax);
            float dist = frand(slotMin, slotMax);
            _scatterUntil = now + Milliseconds(urand(5000, 12000));
            _loiterDelay = urand(400, 1800);                    // each bot notices the stop at its own pace
            motion->MoveFollow(owner, dist, ChaseAngle(angle + frand(-halfWidth, halfWidth), float(M_PI) / 10.0f));
            _movement = MoveMode::Follow;
            _moveTarget = owner->GetGUID();
            return;
        }

        // the owner waits: each bot takes a spot of its own in its part of the group, and moves again now and then
        float angle, halfWidth, slotMin, slotMax;
        FormationSlot(false, angle, halfWidth, slotMin, slotMax);
        float dist = _bot->GetExactDist2d(owner);
        bool fidget = now >= _fidgetAt;
        if (fidget)
            _fidgetAt = now + Milliseconds(urand(12000, 35000));
        if (!_loitering || dist > slotMax + 4.0f || dist < slotMin * 0.5f || (fidget && roll_chance(35)))
        {
            Position spot;
            if (PickLoiterSpot(owner, angle, halfWidth, slotMin, slotMax, spot))
            {
                _loiterSpot = spot;
                _loitering = true;
                if (_bot->GetExactDist2d(spot) > 0.8f)
                {
                    motion->MovePoint(0, spot);
                    _movement = MoveMode::Point;
                }
            }
            return;
        }

        // standing there like a player waiting: looks at the owner, sometimes around
        if (!_bot->isMoving() && now >= _faceAt)
        {
            _faceAt = now + Milliseconds(urand(4000, 15000));
            float toOwner = _bot->GetAbsoluteAngle(owner);
            _bot->SetFacingTo(roll_chance(70) ? Position::NormalizeOrientation(toOwner + frand(-0.5f, 0.5f)) : frand(0.0f, 2.0f * float(M_PI)));
        }
    }

    void CompanionAI::FormationSlot(bool travelling, float& angle, float& halfWidth, float& minDist, float& maxDist) const
    {
        Config const& config = GetConfig();
        // a bigger group stands wider
        float size = std::clamp(std::sqrt(float(sCompanionMgr->CountBotsOf(_owner)) / 4.0f), 1.0f, 2.2f);
        float near = std::max(1.0f, config.FollowMinDistance * _spread);
        float far = std::max(near + 1.0f, config.FollowMaxDistance * _spread * size);
        float const pi = float(M_PI);
        switch (GetEffectiveRole())
        {
            case Role::Tank:
                // in front, leading the way (on the move a little to the side so the leader still sees ahead)
                angle = travelling ? _formationSide * 0.5f : 0.0f;
                halfWidth = travelling ? 0.3f : 0.6f;
                minDist = near + 1.0f;
                maxDist = std::max(minDist + 1.0f, far * 0.8f);
                break;
            case Role::MeleeDps:
                // beside the leader, between the tank and the back line
                angle = _formationSide * (travelling ? 1.9f : 1.5f);
                halfWidth = 0.5f;
                minDist = near;
                maxDist = std::max(minDist + 1.0f, far * 0.7f);
                break;
            case Role::Healer:
                angle = pi;
                halfWidth = 0.8f;
                minDist = near + far * 0.3f;
                maxDist = far * 1.15f;
                break;
            default:    // ranged damage: the back line, a little wider
                angle = pi + _formationSide * 0.4f;
                halfWidth = 0.9f;
                minDist = near + far * 0.3f;
                maxDist = far * 1.2f;
                break;
        }
    }

    bool CompanionAI::PickLoiterSpot(Player* owner, float angle, float halfWidth, float minDist, float maxDist, Position& spot) const
    {
        // the spots of the other companions, so nobody stands in somebody else
        std::vector<Position> taken;
        for (Player* other : sCompanionMgr->GetBotsOf(_owner))
        {
            if (other == _bot)
                continue;
            CompanionAI* ai = sCompanionMgr->GetAI(other);
            taken.push_back(ai && ai->_loitering ? ai->_loiterSpot : other->GetPosition());
        }

        // distances from the owner's edge, like players see them
        float const reach = owner->GetCombatReach() + _bot->GetCombatReach();
        for (uint32 attempt = 0; attempt < 8; ++attempt)
        {
            Position pos = owner->GetPosition();
            // later attempts look wider when the slot is blocked
            float spread = halfWidth * (1.0f + attempt * 0.25f);
            owner->MovePosition(pos, reach + frand(minDist, maxDist), angle + frand(-spread, spread));
            if (!owner->IsWithinLOS(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 1.5f))
                continue;
            // a slope or a wall shortened the move: not a spot on the owner's toes
            if (owner->GetExactDist2d(pos) < reach + minDist * 0.75f && attempt < 7)
                continue;
            if (InDangerZone(pos, 2.0f) && attempt < 7)
                continue;
            bool crowded = std::any_of(taken.begin(), taken.end(), [&pos](Position const& p) { return p.GetExactDist2d(pos) < 2.0f; });
            if (crowded && attempt < 7)
                continue;
            spot = pos;
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ casting helpers

    bool CompanionAI::CanCastId(uint32 spellId, Unit* target) const
    {
        if (!spellId)
            return false;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!info)
            return false;
        if (!target)
            target = _bot;

        SpellHistory const* history = _bot->GetSpellHistory();
        if (!history->IsReady(info) || history->HasGlobalCooldown(info))
            return false;

        for (SpellPowerCost const& cost : info->CalcPowerCost(_bot, info->GetSchoolMask()))
            if (cost.Amount > 0 && _bot->GetPower(cost.Power) < cost.Amount)
                return false;

        if (info->CheckShapeshift(_bot->GetShapeshiftForm()) != SPELL_CAST_OK)
            return false;

        if (target != _bot)
        {
            if (!target->IsInWorld() || target->GetMap() != _bot->GetMap())
                return false;
            bool positive = info->IsPositive();
            float maxRange = info->GetMaxRange(positive, _bot);
            if (maxRange > 0.0f)
            {
                if (maxRange <= NOMINAL_MELEE_RANGE + 0.5f)
                {
                    if (!_bot->IsWithinMeleeRange(target))
                        return false;
                }
                else if (!_bot->IsWithinCombatRange(target, maxRange))
                    return false;
            }
            float minRange = info->GetMinRange(positive);
            if (minRange > 0.0f && _bot->IsWithinCombatRange(target, minRange))
                return false;
            if (!_bot->IsWithinLOSInMap(target))
                return false;
        }
        return true;
    }

    bool CompanionAI::CanCast(std::string_view name, Unit* target) const
    {
        return CanCastId(SpellIdOf(name), target);
    }

    bool CompanionAI::IsSpellReady(std::string_view name) const
    {
        uint32 id = SpellIdOf(name);
        if (!id)
            return false;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE);
        return info && _bot->GetSpellHistory()->IsReady(info);
    }

    bool CompanionAI::EnsureReagents(SpellInfo const* info)
    {
        for (size_t i = 0; i < info->Reagent.size(); ++i)
        {
            if (info->Reagent[i] <= 0 || info->ReagentCount[i] <= 0)
                continue;
            uint32 item = uint32(info->Reagent[i]);
            uint32 count = uint32(info->ReagentCount[i]);
            if (!_bot->HasItemCount(item, count))
                _bot->AddItem(item, count * 5);
        }
        for (int32 totem : info->Totem)
            if (totem > 0 && !_bot->HasItemCount(uint32(totem), 1))
                _bot->AddItem(uint32(totem), 1);
        return true;
    }

    bool CompanionAI::CastId(uint32 spellId, Unit* target)
    {
        if (!CanCastId(spellId, target))
            return false;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE);
        if (!target)
            target = _bot;
        // a player told it never to use this one
        if (!_prefs.empty() && info->SpellName && IsAvoided(ToLower(info->SpellName->Str[LOCALE_enUS])))
            return false;

        if (GetConfig().FreeReagents)
            EnsureReagents(info);

        if ((info->CalcCastTime() > 0 || info->IsChanneled()) && _bot->isMoving())
            StopMovement();

        if (target != _bot)
        {
            if (!info->IsPositive())
                FaceTarget(target);
            else
                _bot->SetInFront(target);
        }

        CastSpellExtraArgs args(TRIGGERED_NONE);
        SpellCastResult result;
        if (info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION)
        {
            Position dest = target->GetPosition();
            result = _bot->CastSpell(dest, spellId, args);
        }
        else
            result = _bot->CastSpell(target, spellId, args);

        if (result == SPELL_CAST_OK)
        {
            _lastCastTime = GameTime::Now();
            _recentCasts.emplace_back(_lastCastTime, ToLower(info->SpellName->Str[LOCALE_enUS]));
            while (_recentCasts.size() > 12)
                _recentCasts.pop_front();
            return true;
        }

        if (result == SPELL_FAILED_MOVING)
            StopMovement();
        return false;
    }

    bool CompanionAI::Cast(std::string_view name, Unit* target)
    {
        // "shoot" means the wand or the bow/gun/crossbow depending on what is equipped
        if (name == "shoot")
        {
            if (UsesWand())
                return CastId(_spells.Has("shoot") ? 5019 : 0, target) || CastId(SpellIdOf("shoot"), target);
            if (std::vector<uint32> const* ranks = _spells.Ranks("shoot"))
                for (uint32 id : *ranks)
                    if (id != 5019 && CastId(id, target))
                        return true;
            return false;
        }
        return CastId(SpellIdOf(name), target);
    }

    bool CompanionAI::CastHeal(std::string_view name, Unit* target, uint32 deficit)
    {
        std::vector<uint32> const* ranks = _spells.Ranks(name);
        if (!ranks || ranks->empty())
            return false;

        // short of mana a smaller rank does, unless the target is about to die
        uint64 const wanted = ManaPct() < 40.0f && target->GetHealthPct() >= 35.0f ? uint64(deficit) * 6 / 10 : uint64(deficit) * 8 / 10;
        uint32 chosen = 0;
        uint32 chosenAmount = 0;
        for (uint32 id : *ranks)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id, DIFFICULTY_NONE);
            if (!info || !CanCastId(id, target))
                continue;
            uint32 amount = EstimateHeal(info, _bot, target);
            chosen = id;
            chosenAmount = amount;
            if (amount >= wanted)
                break;  // the cheapest rank that does the job
        }

        if (!chosen || !CastId(chosen, target))
            return false;

        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(chosen, DIFFICULTY_NONE))
        {
            sCompanionMgr->AddIncomingHeal(target->GetGUID(), chosenAmount, info->CalcCastTime() + 500);
            if (info->CalcCastTime() > 0)
                _healCast = { target->GetGUID(), chosen, chosenAmount };
        }
        return true;
    }

    bool CompanionAI::IsSpellNamed(uint32 spellId, std::string_view name) const
    {
        return NameIs(sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE), name);
    }

    uint64 CompanionAI::IncomingHealing(Unit* target) const
    {
        uint64 total = sCompanionMgr->GetIncomingHeal(target->GetGUID());
        // heals over time: the ticks of the next 4 seconds
        for (AuraEffect const* hot : target->GetAuraEffectsByType(SPELL_AURA_PERIODIC_HEAL))
        {
            int32 const period = hot->GetPeriod();
            if (period <= 0)
                continue;
            int32 const left = hot->GetBase()->GetDuration();
            int32 const window = left < 0 ? 4000 : std::min(left, 4000);
            total += uint64(std::max<int64>(0, int64(hot->GetAmount()))) * uint64(window / period);
        }
        // a player's heal being cast on it (the companions' own are counted above)
        for (Player* member : _view.Members)
        {
            if (sCompanionMgr->GetAI(member->GetGUID()))
                continue;
            if (Spell const* spell = member->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                if (spell->m_targets.GetUnitTargetGUID() == target->GetGUID() && spell->GetSpellInfo()->HasEffect(SPELL_EFFECT_HEAL))
                    total += EstimateHeal(spell->GetSpellInfo(), member, target, false);
        }
        return total;
    }

    // a heal whose target got full meanwhile (another healer, a potion, the pull ended) stops before it costs mana; a tank's
    // heal runs on until its last half second, since healers start it ahead of the hits
    bool CompanionAI::CancelNeedlessHeal()
    {
        if (!_healCast.SpellId)
            return false;
        Spell* spell = _bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!spell || spell->GetSpellInfo()->Id != _healCast.SpellId || spell->getState() != SPELL_STATE_PREPARING)
        {
            _healCast = {};
            return false;
        }
        Unit* target = ObjectAccessor::GetUnit(*_bot, _healCast.Target);
        bool needless = !target || !target->IsAlive();
        if (!needless)
        {
            if (IsTank(target) && spell->GetRemainingCastTime() > 500)
                return false;
            uint64 others = IncomingHealing(target);
            others = others > _healCast.Amount ? others - _healCast.Amount : 0;
            uint64 const have = target->GetHealth() + others;
            uint64 const missing = target->GetMaxHealth() > have ? target->GetMaxHealth() - have : 0;
            needless = missing * 4 < _healCast.Amount;     // three quarters of it would be overheal
        }
        if (!needless)
            return false;
        _bot->InterruptSpell(CURRENT_GENERIC_SPELL, false);
        sCompanionMgr->RemoveIncomingHeal(_healCast.Target, _healCast.Amount);
        _recentCasts.emplace_back(GameTime::Now(), "cancelled heal");
        _healCast = {};
        return true;
    }

    bool CompanionAI::HasAura(Unit const* unit, std::string_view name, bool mineOnly) const
    {
        return AuraRemaining(unit, name, mineOnly) != 0;
    }

    int32 CompanionAI::AuraRemaining(Unit const* unit, std::string_view name, bool mineOnly) const
    {
        if (!unit)
            return 0;
        int32 remaining = 0;
        ObjectGuid me = _bot->GetGUID();
        unit->HasAura([&](Aura const* aura)
        {
            if (mineOnly && aura->GetCasterGUID() != me)
                return false;
            if (!NameIs(aura->GetSpellInfo(), name))
                return false;
            remaining = aura->IsPermanent() || aura->GetDuration() < 0 ? -1 : std::max(1, aura->GetDuration());
            return true;
        });
        return remaining;
    }

    uint32 CompanionAI::AuraStacks(Unit const* unit, std::string_view name, bool mineOnly) const
    {
        if (!unit)
            return 0;
        uint32 stacks = 0;
        ObjectGuid me = _bot->GetGUID();
        unit->HasAura([&](Aura const* aura)
        {
            if (mineOnly && aura->GetCasterGUID() != me)
                return false;
            if (!NameIs(aura->GetSpellInfo(), name))
                return false;
            stacks = std::max<uint32>(1, aura->GetStackAmount());
            return true;
        });
        return stacks;
    }

    // ------------------------------------------------------------------ state helpers

    float CompanionAI::HealthPct() const { return _bot->GetHealthPct(); }
    float CompanionAI::ManaPct() const { return UsesMana(_bot) ? _bot->GetPowerPct(POWER_MANA) : 100.0f; }
    int32 CompanionAI::Rage() const { return _bot->GetPower(POWER_RAGE) / 10; }
    int32 CompanionAI::Energy() const { return _bot->GetPower(POWER_ENERGY); }

    int32 CompanionAI::ComboPoints(Unit const* target) const
    {
        if (!target || _bot->GetClassicComboTarget() != target->GetGUID())
            return 0;
        return _bot->GetPower(POWER_COMBO_POINTS);
    }

    bool CompanionAI::IsBehind(Unit const* target) const
    {
        return target && !target->HasInArc(float(M_PI), _bot);
    }

    bool CompanionAI::InMelee(Unit const* target) const
    {
        return target && _bot->IsWithinMeleeRange(target);
    }

    uint32 CompanionAI::EnemiesNear(Position const& pos, float radius) const
    {
        uint32 count = 0;
        for (Unit* enemy : _view.Enemies)
            if (enemy->GetExactDist(pos) <= radius)
                ++count;
        return count;
    }

    uint32 CompanionAI::EnemiesAttacking(Unit const* unit) const
    {
        uint32 count = 0;
        for (Unit* enemy : _view.Enemies)
            if (enemy->GetVictim() == unit)
                ++count;
        return count;
    }

    bool CompanionAI::AnyCrowdControlledNear(Position const& pos, float radius) const
    {
        for (Unit* enemy : _view.Enemies)
            if (enemy->GetExactDist(pos) <= radius && IsCrowdControlTarget(enemy))
                return true;
        if (Group* group = _bot->GetGroup())
        {
            ObjectGuid moon = group->GetTargetIcon(ICON_MOON);
            if (!moon.IsEmpty())
                if (Unit* unit = ObjectAccessor::GetUnit(*_bot, moon))
                    if (unit->IsAlive() && unit->GetExactDist(pos) <= radius)
                        return true;
        }
        return false;
    }

    bool CompanionAI::IsTank(Unit const* unit) const
    {
        if (!unit)
            return false;
        if (unit == _view.Tank)
            return true;
        if (Player const* player = unit->ToPlayer())
            if (CompanionAI* ai = sCompanionMgr->GetAI(player))
                return ai->GetEffectiveRole() == Role::Tank;
        return false;
    }

    bool CompanionAI::IsHealer(Unit const* unit) const
    {
        if (Player const* player = unit ? unit->ToPlayer() : nullptr)
            if (CompanionAI* ai = sCompanionMgr->GetAI(player))
                return ai->GetEffectiveRole() == Role::Healer;
        return false;
    }

    Unit* CompanionAI::LowestAlly(float maxPct, float range, bool includePets) const
    {
        Unit* best = nullptr;
        for (Player* member : _view.Members)
        {
            if (member->IsAlive() && member->GetHealthPct() < maxPct && _bot->GetDistance(member) <= range
                && (!best || member->GetHealthPct() < best->GetHealthPct()))
                best = member;
            if (includePets)
                if (Pet* pet = member->GetPet())
                    if (pet->IsAlive() && pet->GetHealthPct() < maxPct && _bot->GetDistance(pet) <= range
                        && (!best || pet->GetHealthPct() < best->GetHealthPct()))
                        best = pet;
        }
        return best;
    }

    uint32 CompanionAI::InjuredAllies(float maxPct, float range) const
    {
        uint32 count = 0;
        for (Player* member : _view.Members)
            if (member->IsAlive() && member->GetHealthPct() < maxPct && _bot->GetDistance(member) <= range)
                ++count;
        return count;
    }

    bool CompanionAI::IsBossFight() const
    {
        for (Unit* enemy : _view.Enemies)
            if (Creature* creature = enemy->ToCreature())
                if (creature->isWorldBoss() || creature->IsDungeonBoss() || (creature->IsElite() && creature->GetLevel() >= _bot->GetLevel() + 2))
                    return true;
        return false;
    }

    bool CompanionAI::IsTargetedByEnemy() const
    {
        return EnemyAttackingMe() != nullptr;
    }

    Unit* CompanionAI::EnemyAttackingMe() const
    {
        for (Unit* enemy : _view.Enemies)
            if (enemy->GetVictim() == _bot)
                return enemy;
        return nullptr;
    }

    bool CompanionAI::IsMoving() const
    {
        return _bot->isMoving();
    }

    void CompanionAI::StopMovement()
    {
        _bot->GetMotionMaster()->Clear(MOTION_PRIORITY_NORMAL);
        _bot->StopMoving();
        _movement = MoveMode::None;
    }

    Creature* CompanionAI::GetPet() const
    {
        if (Pet* pet = _bot->GetPet())
            return pet;
        return nullptr;
    }

    bool CompanionAI::HasPet() const
    {
        Creature* pet = GetPet();
        return pet && pet->IsAlive();
    }

    void CompanionAI::PetAttack(Unit* target)
    {
        Creature* pet = GetPet();
        if (!pet || !pet->IsAlive() || !target || pet->GetVictim() == target)
            return;
        UpdatePetTaunt();
        _bot->GetSession()->HandlePetActionHelper(pet, pet->GetGUID(), COMMAND_ATTACK, ACT_COMMAND, target->GetGUID(), Position());
    }

    void CompanionAI::PetFollow()
    {
        Creature* pet = GetPet();
        if (!pet || !pet->IsAlive())
            return;
        _bot->GetSession()->HandlePetActionHelper(pet, pet->GetGUID(), COMMAND_FOLLOW, ACT_COMMAND, ObjectGuid::Empty, Position());
    }

    Item* CompanionAI::MainHand() const
    {
        return _bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    }

    bool CompanionAI::HasShield() const
    {
        Item* offhand = _bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        return offhand && offhand->GetTemplate()->GetClass() == ITEM_CLASS_ARMOR && offhand->GetTemplate()->GetSubClass() == ITEM_SUBCLASS_ARMOR_SHIELD;
    }

    bool CompanionAI::UsesWand() const
    {
        Item* ranged = _bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        return ranged && ranged->GetTemplate()->GetClass() == ITEM_CLASS_WEAPON && ranged->GetTemplate()->GetSubClass() == ITEM_SUBCLASS_WEAPON_WAND;
    }

    void CompanionAI::Say(std::string const& text)
    {
        if (text.empty())
            return;
        Group* group = _bot->GetGroup();
        if (!group)
        {
            _bot->Say(text, LANG_UNIVERSAL);
            return;
        }
        bool const leader = group->IsLeader(_bot->GetGUID());
        WorldPackets::Chat::Chat packet;
        if (group->isRaidGroup())
            packet.Initialize(leader ? CHAT_MSG_RAID_LEADER : CHAT_MSG_RAID, LANG_UNIVERSAL, _bot, nullptr, text);
        else
            packet.Initialize(leader ? CHAT_MSG_PARTY_LEADER : CHAT_MSG_PARTY, LANG_UNIVERSAL, _bot, nullptr, text);
        packet.SenderName = _bot->GetName();
        group->BroadcastPacket(packet.Write(), false, group->isRaidGroup() ? -1 : group->GetMemberGroup(_bot->GetGUID()));
        CompanionLLM::RecordChat(group, _bot->GetName(), text);
    }

    std::string CompanionAI::StatusLine() const
    {
        std::ostringstream ss;
        ss << _bot->GetName() << " (" << uint32(_bot->GetLevel()) << " " << RoleName(GetEffectiveRole()) << "): "
           << uint32(HealthPct()) << "% health";
        if (UsesMana(_bot))
            ss << ", " << uint32(ManaPct()) << "% mana";
        if (!_bot->IsAlive())
            ss << ", dead";
        else if (_inCombat)
            ss << ", fighting";
        if (_stance == Stance::Stay)
            ss << ", holding position";
        if (_aggression == Aggression::Passive)
            ss << ", passive";
        return ss.str();
    }
}
