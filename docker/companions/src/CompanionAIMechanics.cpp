/*
 * Companions: the bot brain, boss mechanics (rules in CompanionMechanics.cpp)
 */

#include "CompanionAI.h"
#include "CellImpl.h"
#include "CompanionMechanics.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "GameObject.h"
#include "GridDefines.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "InstanceScript.h"
#include "StringFormat.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include <array>
#include <cmath>
#include <unordered_set>

namespace Companions
{
    using Mechanics::Kind;
    using Mechanics::Rule;

    namespace
    {
        struct Circle { Position Center; float Radius; };
        struct Band { Position Origin; float Angle; float Length; float HalfWidth; };       // in front of Origin along Angle
        struct Arc { ObjectGuid Owner; Position Apex; float Facing; float HalfAngle; float Length; };

        struct Constraints
        {
            std::vector<Circle> Circles;    // hard: casts, debuffed allies, hazards (and soft: spreading)
            std::vector<Band> Bands;
            std::vector<Arc> Arcs;          // in front of (or behind) a boss
            bool Hard = false;              // something hurts right now: worth breaking a cast for

            bool Violated(Position const& pos, ObjectGuid ignoreArcsOf) const
            {
                for (Circle const& circle : Circles)
                    if (circle.Center.GetExactDist2d(pos) < circle.Radius)
                        return true;
                for (Band const& band : Bands)
                {
                    float dx = pos.GetPositionX() - band.Origin.GetPositionX();
                    float dy = pos.GetPositionY() - band.Origin.GetPositionY();
                    float along = dx * std::cos(band.Angle) + dy * std::sin(band.Angle);
                    float across = std::fabs(-dx * std::sin(band.Angle) + dy * std::cos(band.Angle));
                    if (along > -3.0f && along < band.Length && across < band.HalfWidth)
                        return true;
                }
                for (Arc const& arc : Arcs)
                {
                    if (arc.Owner == ignoreArcsOf)
                        continue;
                    float dist = arc.Apex.GetExactDist2d(pos);
                    if (dist > arc.Length || dist < 0.5f)
                        continue;
                    float angle = Position::NormalizeOrientation(arc.Apex.GetAbsoluteAngle(pos) - arc.Facing);
                    if (angle > float(M_PI))
                        angle -= 2.0f * float(M_PI);
                    if (std::fabs(angle) < arc.HalfAngle)
                        return true;
                }
                return false;
            }

            // how deep inside: yards into circles, a flat cost for bands and arcs (0 = safe)
            float Depth(Position const& pos, ObjectGuid ignoreArcsOf) const
            {
                float depth = 0.0f;
                for (Circle const& circle : Circles)
                    depth += std::max(0.0f, circle.Radius - circle.Center.GetExactDist2d(pos));
                Constraints rest;
                rest.Bands = Bands;
                rest.Arcs = Arcs;
                if (rest.Violated(pos, ignoreArcsOf))
                    depth += 15.0f;
                return depth;
            }

            bool Empty() const { return Circles.empty() && Bands.empty() && Arcs.empty(); }
        };

        Spell* CurrentCast(Unit* unit)
        {
            Spell* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!spell)
                spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            if (!spell || (spell->getState() != SPELL_STATE_PREPARING && spell->getState() != SPELL_STATE_CHANNELING))
                return nullptr;
            return spell;
        }

        // bosses seen casting a cone (breath, cleave): their front is avoided from then on, by every bot
        std::unordered_set<uint32> ConeCasters;

        bool IsBig(Creature const* creature)
        {
            return creature && (creature->IsElite() || creature->isWorldBoss() || creature->IsDungeonBoss());
        }

        bool MeleeSchool(Player const* bot, Role role)
        {
            return role == Role::Tank || role == Role::MeleeDps || bot->GetClass() == CLASS_HUNTER;
        }
    }

    uint8 CompanionAI::RoleBit() const
    {
        switch (GetEffectiveRole())
        {
            case Role::Tank:     return Mechanics::ROLE_BIT_TANK;
            case Role::Healer:   return Mechanics::ROLE_BIT_HEALER;
            case Role::MeleeDps: return Mechanics::ROLE_BIT_MELEE;
            default:             return Mechanics::ROLE_BIT_RANGED;
        }
    }

    bool CompanionAI::UpdateMechanics()
    {
        _mechanicAngle.reset();
        _mechanicDebug.clear();
        if (_view.Enemies.empty())
            return false;
        TimePoint const now = GameTime::Now();
        uint8 const roleBit = RoleBit();
        Role const role = GetEffectiveRole();
        bool const melee = role == Role::Tank || role == Role::MeleeDps;

        // Threatening Gaze and the like: not a step, not a cast, not a swing until it ends
        for (auto const& [spellId, application] : _bot->GetAppliedAuras())
            if (Mechanics::FindSpellRule(spellId, Kind::FreezeWithDebuff))
            {
                if (_bot->GetVictim())
                    _bot->AttackStop();
                if (_bot->IsNonMeleeSpellCast(false))
                    _bot->InterruptNonMeleeSpells(false);
                if (_bot->isMoving())
                    StopMovement();
                _movement = MoveMode::None;
                _mechanicHold = now + Milliseconds(500);
                return true;
            }

        // hazards that are creatures of their own (void zones, tornadoes, clouds): looked up now and then in instances
        if (now >= _mechanicScanAt)
        {
            _mechanicScanAt = now + Seconds(1);
            _hazards.clear();
            if (_bot->GetMap()->IsDungeon())
            {
                std::list<Unit*> nearby;
                Trinity::AnyUnitInObjectRangeCheck check(_bot, 40.0f);
                Trinity::UnitListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(_bot, nearby, check);
                Cell::VisitAllObjects(_bot, searcher, 40.0f);
                for (Unit* unit : nearby)
                    if (Creature* creature = unit->ToCreature())
                        if (creature->IsAlive() && Mechanics::HasRule(creature->GetEntry(), Kind::AvoidNpc))
                            _hazards.push_back(creature->GetGUID());
            }
        }

        Constraints avoid;
        Unit* hideFrom = nullptr;
        uint32 hideObject = 0;
        Optional<Position> spot;            // a place to be (polarity)
        bool runner = false;
        float runRadius = 0.0f;
        float spreadRadius = 0.0f;
        Unit* target = _bot->GetVictim();

        for (Unit* enemy : _view.Enemies)
        {
            Creature* creature = enemy->ToCreature();
            uint32 const entry = creature ? creature->GetEntry() : 0;
            Spell* spell = CurrentCast(enemy);
            SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;

            if (info)
            {
                bool ruled = false;
                for (Rule const* rule : Mechanics::RulesForSpell(info->Id))
                {
                    if (rule->Type != Kind::AvoidCast && rule->Type != Kind::AvoidLine && rule->Type != Kind::HideBehind)
                        continue;
                    ruled = true;
                    if (!(rule->Roles & roleBit))
                        continue;
                    if (rule->Type == Kind::AvoidCast)
                    {
                        avoid.Circles.push_back({ enemy->GetPosition(), rule->Radius + 2.0f });
                        HoldAwayFrom(enemy, rule->Value, rule->Radius + 2.0f);
                    }
                    else if (rule->Type == Kind::AvoidLine)
                        avoid.Bands.push_back({ enemy->GetPosition(), enemy->GetOrientation(), rule->Radius, float(rule->Value) / 2.0f + 2.0f });
                    else
                    {
                        hideFrom = enemy;
                        hideObject = rule->Value;
                    }
                    avoid.Hard = true;
                }
                // nobody wrote a rule: a cast or channel hitting all around the caster is left by everyone but the tank
                if (!ruled && roleBit != Mechanics::ROLE_BIT_TANK && (info->IsChanneled() || info->CalcCastTime() >= 1000))
                {
                    float radius = Mechanics::CasterAreaRadius(info);
                    if (radius > 0.0f && radius <= 20.0f)
                    {
                        avoid.Circles.push_back({ enemy->GetPosition(), radius + 2.0f });
                        avoid.Hard = true;
                    }
                }
                if (IsBig(creature) && Mechanics::IsCone(info))
                    ConeCasters.insert(entry);
            }

            // a rooted enemy (Frost Nova, Entangling Roots) still hits whoever stands at its side: casters and healers next
            // to one step out of its reach (right away when it is hitting them) instead of standing there taking it
            if (!melee && enemy->HasUnitState(UNIT_STATE_ROOT) && !enemy->HasUnitState(UNIT_STATE_STUNNED) && !IsCrowdControlTarget(enemy))
            {
                float reach = enemy->GetCombatReach() + _bot->GetCombatReach() + 4.0f;
                if (_bot->GetClass() == CLASS_HUNTER)
                    reach = std::max(reach, 9.0f);      // and out of the dead zone of the bow
                if (_bot->GetExactDist2d(enemy) < reach + 6.0f)
                {
                    avoid.Circles.push_back({ enemy->GetPosition(), reach });
                    if (enemy->GetVictim() == _bot)
                        avoid.Hard = true;
                }
            }

            for (Rule const* rule : Mechanics::RulesFor(entry))
            {
                if (!entry || !(rule->Roles & roleBit))
                    continue;
                switch (rule->Type)
                {
                    case Kind::AvoidCast:
                        // the same rule as an aura on the boss (Inferno, Earthquake as auras)
                        if (rule->Spell && enemy->HasAura(rule->Spell) && (!info || info->Id != rule->Spell))
                        {
                            avoid.Circles.push_back({ enemy->GetPosition(), rule->Radius + 2.0f });
                            avoid.Hard = true;
                            HoldAwayFrom(enemy, rule->Value, rule->Radius + 2.0f);
                        }
                        break;
                    case Kind::Spread:
                        spreadRadius = std::max(spreadRadius, rule->Radius);
                        break;
                    case Kind::Polarity:
                    {
                        float side = _bot->HasAura(rule->Spell) ? 1.0f : (_bot->HasAura(rule->Value) ? -1.0f : 0.0f);
                        if (side != 0.0f)
                        {
                            float angle = enemy->GetOrientation() + side * float(M_PI_2);
                            float dist = melee ? enemy->GetCombatReach() + 1.0f : 18.0f;
                            spot = Position(enemy->GetPositionX() + std::cos(angle) * dist, enemy->GetPositionY() + std::sin(angle) * dist,
                                enemy->GetPositionZ());
                        }
                        break;
                    }
                    default:
                        break;
                }
            }

            // Onyxia's Deep Breath shows no cast (the script casts it at once): she hangs still in the air without a target while
            // she takes the breath (5 s), then crosses the room at three times her speed, and its fire reaches 30 yards to either
            // side of her path (twelve blasts of 30 yards): everyone keeps out of that band
            uint32 constexpr NPC_ONYXIA = 10184, SPELL_ONYXIA_HOVER = 17131;
            if (entry == NPC_ONYXIA && enemy->HasAura(SPELL_ONYXIA_HOVER)
                && ((enemy->movespline->Finalized() && enemy->GetTarget().IsEmpty()) || enemy->GetSpeedRate(MOVE_RUN) > 2.0f))
            {
                avoid.Bands.push_back({ enemy->GetPosition(), enemy->GetOrientation(), 110.0f, 33.0f });
                avoid.Hard = true;
            }

            // in front of a breath or a cleave (by rule, or seen casting a cone): only its tank stands there
            if (IsBig(creature) && roleBit != Mechanics::ROLE_BIT_TANK && enemy->GetVictim() != _bot)
            {
                bool sides = Mechanics::HasRule(entry, Kind::AvoidSides);
                if (sides || Mechanics::HasRule(entry, Kind::AvoidFront) || ConeCasters.count(entry))
                {
                    // as far as the rule says it reaches (a dragon's breath: 45 yards, where the raid's healers stood)
                    float frontReach = enemy->GetCombatReach() + 25.0f;
                    for (Rule const* rule : Mechanics::RulesFor(entry))
                        if ((rule->Type == Kind::AvoidSides || rule->Type == Kind::AvoidFront) && rule->Radius > 0.0f)
                            frontReach = std::max(frontReach, enemy->GetCombatReach() + rule->Radius);
                    avoid.Arcs.push_back({ enemy->GetGUID(), enemy->GetPosition(), enemy->GetOrientation(), float(M_PI) / 3.0f, frontReach });
                    if (sides)
                        avoid.Arcs.push_back({ enemy->GetGUID(), enemy->GetPosition(), enemy->GetOrientation() + float(M_PI), float(M_PI) / 4.0f,
                            enemy->GetCombatReach() + 12.0f });
                    // flanks: half the group left, half right (by character, so it stays the same all fight)
                    if (enemy == target)
                        _mechanicAngle = sides ? ((_bot->GetGUID().GetCounter() & 1) ? float(M_PI_2) : float(M_PI) * 1.5f) : float(M_PI);
                }
            }
        }

        // casters still kept away from after their cast or aura ended (a rule's hold time)
        for (auto itr = _castAvoid.begin(); itr != _castAvoid.end();)
        {
            Unit* caster = GameTime::Now() < itr->second.first ? ObjectAccessor::GetUnit(*_bot, itr->first) : nullptr;
            if (!caster || !caster->IsAlive())
            {
                itr = _castAvoid.erase(itr);
                continue;
            }
            avoid.Circles.push_back({ caster->GetPosition(), itr->second.second });
            avoid.Hard = true;
            ++itr;
        }

        // debuffs that hurt the others (Living Bomb, Burning Adrenaline, Conflagration): the one who has it leaves everyone, and
        // the raid around the boss stays where it is: the others keep away from him only when he cannot go (confused, rooted,
        // a tank who keeps the boss where it is). (All of them stepping away from every carrier kept a raid's tanks and melee
        // off Baron Geddon most of the fight, and stepping away in its last seconds sent the casters into his Inferno.)
        auto holdsBoss = [this](Player const* member)
        {
            if (!IsTank(member))
                return false;
            return std::any_of(_view.Enemies.begin(), _view.Enemies.end(), [member](Unit* enemy) { return enemy->GetVictim() == member && IsBossUnit(enemy); });
        };
        for (Player* member : _view.Members)
        {
            if (!member->IsAlive())
                continue;
            for (auto const& [spellId, application] : member->GetAppliedAuras())
                if (Rule const* rule = Mechanics::FindSpellRule(spellId, Kind::RunWithDebuff))
                {
                    bool const keepsBoss = holdsBoss(member);
                    if (member == _bot)
                    {
                        if (!keepsBoss)
                        {
                            runner = true;
                            runRadius = std::max(runRadius, rule->Radius);
                        }
                        continue;
                    }
                    bool const stuck = keepsBoss || member->HasUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_FLEEING);
                    if (stuck)
                    {
                        avoid.Circles.push_back({ member->GetPosition(), rule->Radius });
                        avoid.Hard = true;
                    }
                }
        }
        if (runner)
        {
            avoid.Hard = true;
            for (Player* member : _view.Members)
                if (member != _bot && member->IsAlive())
                    avoid.Circles.push_back({ member->GetPosition(), runRadius });
        }
        else if (spreadRadius > 0.0f)
            for (Player* member : _view.Members)
                if (member != _bot && member->IsAlive() && !IsTank(member))
                    avoid.Circles.push_back({ member->GetPosition(), spreadRadius });

        for (ObjectGuid const& guid : _hazards)
            if (Creature* hazard = ObjectAccessor::GetCreature(*_bot, guid))
                if (hazard->IsAlive())
                    for (Rule const* rule : Mechanics::RulesFor(hazard->GetEntry()))
                        if (rule->Type == Kind::AvoidNpc && (rule->Roles & roleBit))
                        {
                            avoid.Circles.push_back({ hazard->GetPosition(), rule->Radius + hazard->GetCombatReach() });
                            avoid.Hard = true;
                        }

        // a breath to hide from: behind the nearest ice block (or whatever the rule names), away from the caster
        if (hideFrom && hideObject)
        {
            std::list<GameObject*> objects;
            Trinity::AllGameObjectsWithEntryInRange check(_bot, hideObject, 60.0f);
            Trinity::GameObjectListSearcher<Trinity::AllGameObjectsWithEntryInRange> searcher(_bot, objects, check);
            Cell::VisitGridObjects(_bot, searcher, 60.0f);
            GameObject* cover = nullptr;
            for (GameObject* object : objects)
                if (!cover || _bot->GetDistance(object) < _bot->GetDistance(cover))
                    cover = object;
            if (cover)
            {
                float angle = hideFrom->GetAbsoluteAngle(cover);
                spot = Position(cover->GetPositionX() + std::cos(angle) * 2.5f, cover->GetPositionY() + std::sin(angle) * 2.5f, cover->GetPositionZ());
            }
        }

        // an enemy after a caster or healer (one that can still move): to the tank, who takes it back. Not a boss: the tank
        // taunts it off, and a raid's healers running into it gathered the whole raid in Baron Geddon's Inferno
        if (!spot && !melee && !runner)
            if (Player* tank = _view.Tank; tank && tank != _bot && tank->IsAlive() && IsTank(tank) && _bot->GetExactDist2d(tank) > 8.0f)
                for (Unit* enemy : _view.Enemies)
                {
                    if (enemy->GetVictim() != _bot || enemy->HasUnitState(UNIT_STATE_ROOT) || IsCrowdControlTarget(enemy) || IsBossUnit(enemy))
                        continue;
                    float const angle = tank->GetAbsoluteAngle(_bot);
                    Position near(tank->GetPositionX() + std::cos(angle) * 3.0f, tank->GetPositionY() + std::sin(angle) * 3.0f, tank->GetPositionZ());
                    if (!InDangerZone(near, 1.0f) && !avoid.Violated(near, ObjectGuid::Empty))
                        spot = near;
                    break;
                }

        // no chasing into it: melee (and tanks) only when their target stands inside a danger, the others while any is around
        bool targetInside = false;
        if (target)
            for (Circle const& circle : avoid.Circles)
                if (circle.Center.GetExactDist2d(target) < circle.Radius)
                    targetInside = true;
        bool const anyDanger = !avoid.Circles.empty() || !avoid.Bands.empty();
        bool const hold = spot.has_value() || (melee ? targetInside : anyDanger);
        if (!anyDanger && !spot && avoid.Arcs.empty())
            return false;
        if (hold)
            _mechanicHold = now + Milliseconds(400);

        if (now < _mechanicMoveUntil && _bot->isMoving())
            return true;    // on its way

        Position const here = _bot->GetPosition();
        if (!_bot->IsInWorld() || !Trinity::IsValidMapCoord(here.GetPositionX(), here.GetPositionY(), here.GetPositionZ()))
        {
            _mechanicDebug = "not on a valid spot";
            return false;
        }
        Position dest;
        bool move = false;
        if (spot && Trinity::IsValidMapCoord(spot->GetPositionX(), spot->GetPositionY()))
        {
            if (here.GetExactDist2d(*spot) > 2.5f)
            {
                dest = *spot;
                float z = dest.GetPositionZ();
                _bot->UpdateAllowedPositionZ(dest.GetPositionX(), dest.GetPositionY(), z);
                dest.m_positionZ = z;
                move = true;
            }
        }
        else
        {
            // the melee get behind or beside their own target by chasing (MoveForCombat), not here
            ObjectGuid const ownArc = melee && target ? target->GetGUID() : ObjectGuid::Empty;
            if (!avoid.Violated(here, ownArc) && !InDangerZone(here, 0.5f))
                return false;
            // a cast in progress is finished first unless the floor is about to hurt
            if (!avoid.Hard && _bot->IsNonMeleeSpellCast(false, false, true))
                return false;

            // the safest reachable spot; where nothing is fully safe (a bomb in a crowded room), the least bad one
            Player const* tank = _view.Tank && _view.Tank != _bot ? _view.Tank : nullptr;
            float const depthHere = avoid.Depth(here, ownArc);
            // a bomb carrier heads away from the middle of the group (the others are leaving the same danger as it is)
            Position centre;
            uint32 others = 0;
            for (Player* member : _view.Members)
                if (member != _bot && member->IsAlive())
                {
                    centre.m_positionX += member->GetPositionX();
                    centre.m_positionY += member->GetPositionY();
                    ++others;
                }
            if (others)
            {
                centre.m_positionX /= float(others);
                centre.m_positionY /= float(others);
            }
            float bestCost = std::numeric_limits<float>::max();
            uint32 rejectZ = 0, rejectDepth = 0, rejectSight = 0;
            float shallowest = 999.0f;
            static constexpr std::array<float, 6> RunRings = { 8.0f, 15.0f, 22.0f, 30.0f, 38.0f, 46.0f };
            static constexpr std::array<float, 6> StepRings = { 4.0f, 7.0f, 11.0f, 16.0f, 22.0f, 30.0f };
            for (float dist : runner ? RunRings : StepRings)
                for (uint32 i = 0; i < 16; ++i)
                {
                    float angle = float(i) * float(M_PI) / 8.0f;
                    float x = here.GetPositionX() + std::cos(angle) * dist;
                    float y = here.GetPositionY() + std::sin(angle) * dist;
                    float z = here.GetPositionZ();
                    if (!Trinity::IsValidMapCoord(x, y))
                        continue;
                    _bot->UpdateAllowedPositionZ(x, y, z);
                    Position candidate(x, y, z);
                    if (std::fabs(z - here.GetPositionZ()) > 8.0f || InDangerZone(candidate, 1.0f) || InLiquid(x, y, z, false))
                    {
                        ++rejectZ;
                        continue;
                    }
                    float depth = avoid.Depth(candidate, ownArc);
                    shallowest = std::min(shallowest, depth);
                    if (depth >= depthHere - 0.5f)
                    {
                        ++rejectDepth;
                        continue;
                    }
                    if (!_bot->IsWithinLOS(x, y, z + 1.5f))
                    {
                        ++rejectSight;
                        continue;
                    }
                    float cost = dist + depth * 10.0f;
                    if (runner && others)
                        cost -= centre.GetExactDist2d(candidate) * 2.0f;
                    if (target && !runner)
                    {
                        float reach = target->GetExactDist2d(x, y);
                        if (melee && reach > target->GetCombatReach() + 4.0f)
                            cost += (reach - target->GetCombatReach() - 4.0f) * 0.7f;
                        else if (!melee && reach > 30.0f)
                            cost += (reach - 30.0f) * 2.0f;
                    }
                    if (role == Role::Healer && tank && tank->GetExactDist2d(x, y) > 35.0f)
                        cost += 25.0f;
                    if (cost < bestCost)
                    {
                        bestCost = cost;
                        dest = candidate;
                        move = true;
                    }
                }
            _mechanicDebug = "no spot: depth " + std::to_string(int32(depthHere)) + ", rejected height " + std::to_string(rejectZ)
                + ", deeper " + std::to_string(rejectDepth) + ", sight " + std::to_string(rejectSight)
                + ", shallowest " + std::to_string(int32(shallowest)) + ", circles " + std::to_string(avoid.Circles.size()) + ", arcs "
                + std::to_string(avoid.Arcs.size()) + ", bands " + std::to_string(avoid.Bands.size());
        }
        if (!move)
            return false;
        _mechanicDebug.clear();

        if (avoid.Hard && _bot->IsNonMeleeSpellCast(false, false, true))
            _bot->InterruptNonMeleeSpells(false);
        _bot->GetMotionMaster()->MovePoint(0, dest);
        _movement = MoveMode::Point;
        _loitering = false;
        _mechanicMoveUntil = now + Milliseconds(2500);
        _mechanicHold = now + Milliseconds(800);
        return true;
    }

    std::string CompanionAI::MechanicsLine() const
    {
        std::string out = HoldsForMechanic() ? "holding" : "free";
        if (!_stunDebug.empty())
            out += ", stun lock: " + _stunDebug;
        if (_mechanicAngle)
            out += ", chase angle " + std::to_string(int32(*_mechanicAngle * 180.0f / float(M_PI)));
        if (GameTime::Now() < _mechanicMoveUntil && _bot->isMoving())
            out += ", moving for a mechanic";
        for (auto const& [spellId, application] : _bot->GetAppliedAuras())
            if (!Mechanics::RulesForSpell(spellId).empty())
                out += ", has " + std::to_string(spellId);
        if (!_hazards.empty())
            out += ", hazards " + std::to_string(_hazards.size());
        if (!_mechanicDebug.empty())
            out += ", " + _mechanicDebug;
        return out;
    }

    bool CompanionAI::InLiquid(float x, float y, float z, bool harmfulOnly) const
    {
        map_liquidHeaderTypeFlags types = harmfulOnly ? (map_liquidHeaderTypeFlags::Magma | map_liquidHeaderTypeFlags::Slime)
                                                      : map_liquidHeaderTypeFlags::AllLiquids;
        ZLiquidStatus status = _bot->GetMap()->GetLiquidStatus(_bot->GetPhaseShift(), x, y, z, types, nullptr, _bot->GetCollisionHeight());
        return (status & MAP_LIQUID_STATUS_IN_CONTACT) != 0;
    }

    // a cast or aura to keep away from for some seconds from its start, whatever becomes of it (counted once per start)
    void CompanionAI::HoldAwayFrom(Unit* caster, uint32 seconds, float radius)
    {
        if (seconds)
            _castAvoid.try_emplace(caster->GetGUID(), GameTime::Now() + Seconds(seconds), radius);
    }

    bool CompanionAI::IsMechanicTarget(Unit const* unit) const
    {
        if (!unit)
            return false;
        uint8 const roleBit = RoleBit();
        bool const melee = MeleeSchool(_bot, GetEffectiveRole());
        if (Creature const* creature = unit->ToCreature())
            for (Rule const* rule : Mechanics::RulesFor(creature->GetEntry()))
                if (rule->Type == Kind::DontAttack && (rule->Roles & roleBit))
                    return false;
        // reflecting or shielded: who it hurts stops
        for (auto const& [spellId, application] : unit->GetAppliedAuras())
            if (Rule const* rule = Mechanics::FindSpellRule(spellId, Kind::StopOnAura))
                if (((rule->Value & 1) && melee) || ((rule->Value & 2) && !melee))
                    return false;
        // immune to what this bot deals (Twin Emperors and anything else that is)
        if (melee ? unit->IsImmunedToDamage(SPELL_SCHOOL_MASK_NORMAL) : unit->IsImmunedToDamage(SPELL_SCHOOL_MASK_MAGIC))
            return false;
        return true;
    }

    Unit* CompanionAI::KillFirstTarget() const
    {
        uint8 const roleBit = RoleBit();
        Unit* best = nullptr;
        uint32 bestValue = 0;
        for (Unit* enemy : _view.Enemies)
        {
            Creature* creature = enemy->ToCreature();
            if (!creature || IsCrowdControlTarget(enemy) || !IsMechanicTarget(enemy) || _bot->GetDistance(enemy) > 40.0f)
                continue;
            for (Rule const* rule : Mechanics::RulesFor(creature->GetEntry()))
                if (rule->Type == Kind::KillFirst && (rule->Roles & roleBit)
                    && (rule->Value > bestValue || (rule->Value == bestValue && best && enemy->GetHealthPct() < best->GetHealthPct())))
                {
                    best = enemy;
                    bestValue = rule->Value;
                }
        }
        return best;
    }

    bool CompanionAI::MechanicSpells()
    {
        uint8 const classId = _bot->GetClass();
        bool fears = false;
        for (Unit* enemy : _view.Enemies)
            if (Creature* creature = enemy->ToCreature())
                if (Mechanics::HasRule(creature->GetEntry(), Kind::Fear))
                    fears = true;

        if (fears)
        {
            Player* tank = _view.Tank;
            // priests keep Fear Ward on the tank, shamans a Tremor Totem down, warriors break the fear themselves
            if (classId == CLASS_PRIEST && tank && tank->IsAlive() && GameTime::Now() >= _fearWardAt && _bot->GetDistance(tank) < 30.0f
                && !HasAura(tank, "fear ward") && Cast("fear ward", tank))
            {
                _fearWardAt = GameTime::Now() + Seconds(5);
                return true;
            }
            if (classId == CLASS_SHAMAN && Knows("tremor totem"))
            {
                bool placed = false;
                for (uint8 slot = SUMMON_SLOT_TOTEM; slot <= SUMMON_SLOT_TOTEM_4; ++slot)
                    if (Creature* totem = ObjectAccessor::GetCreature(*_bot, _bot->m_SummonSlot[slot]))
                        if (totem->IsAlive() && _bot->GetDistance(totem) < 25.0f && totem->GetName().find("Tremor") != std::string::npos)
                            placed = true;
                if (!placed && Cast("tremor totem"))
                    return true;
            }
            if (classId == CLASS_WARRIOR && _bot->HasAuraType(SPELL_AURA_MOD_FEAR) && Cast("berserker rage"))
                return true;
        }

        for (Unit* enemy : _view.Enemies)
        {
            if (_bot->GetDistance(enemy) > 35.0f)
                continue;
            Creature* creature = enemy->ToCreature();
            for (auto const& [spellId, application] : enemy->GetAppliedAuras())
            {
                SpellInfo const* info = application->GetBase()->GetSpellInfo();
                // Frenzy: Tranquilizing Shot (by rule, or any enrage the shot removes)
                if (classId == CLASS_HUNTER && (Mechanics::FindSpellRule(spellId, Kind::Tranquilize) || info->Dispel == DISPEL_ENRAGE)
                    && creature && Cast("tranquilizing shot", enemy))
                    return true;
                // magic on the enemy that must go (Deaden Magic, Renew)
                if (Mechanics::FindSpellRule(spellId, Kind::Purge))
                {
                    if (classId == CLASS_PRIEST && Cast("dispel magic", enemy))
                        return true;
                    if (classId == CLASS_SHAMAN && Cast("purge", enemy))
                        return true;
                }
            }
        }

        // mana kept low: the Obsidian Destroyers, Eradicators and Nullifiers of Ahn'Qiraj drain it from the raid and blast
        // everyone at full mana
        if (classId == CLASS_PRIEST || classId == CLASS_WARLOCK || classId == CLASS_HUNTER)
            for (Unit* enemy : _view.Enemies)
            {
                Creature* creature = enemy->ToCreature();
                if (!creature || !Mechanics::HasRule(creature->GetEntry(), Kind::DrainMana) || !enemy->GetMaxPower(POWER_MANA)
                    || enemy->GetPowerPct(POWER_MANA) < 30.0f || _bot->GetDistance(enemy) > 30.0f)
                    continue;
                if ((classId == CLASS_PRIEST && Cast("mana burn", enemy)) || (classId == CLASS_WARLOCK && Cast("drain mana", enemy))
                    || (classId == CLASS_HUNTER && !HasAura(enemy, "viper sting") && Cast("viper sting", enemy)))
                    return true;
            }

        // stun locks (Gurubashi Axe Thrower...): whoever has a stun keeps it stunned in turn, one stun at a time (claimed), never
        // while it is stunned already or immune to stuns (its own Axe Flurry makes it so)
        for (Unit* enemy : _view.Enemies)
        {
            Creature* creature = enemy->ToCreature();
            if (!creature || !Mechanics::HasRule(creature->GetEntry(), Kind::StunLock) || _bot->GetDistance(enemy) > 30.0f)
                continue;
            // the next stun lands as the last one ends (it casts the moment it is free): not before 0.7 s are left
            int32 stunLeft = 0;
            for (AuraEffect const* stun : enemy->GetAuraEffectsByType(SPELL_AURA_MOD_STUN))
                stunLeft = std::max(stunLeft, stun->GetBase()->GetDuration());
            if (stunLeft > 700 || sCompanionMgr->IsStunClaimed(enemy->GetGUID(), _bot->GetGUID()))
            {
                _stunDebug = stunLeft > 700 ? "stunned" : "claimed";
                continue;
            }
            // its deadly channel makes it immune to stuns (Axe Flurry: a state immunity to stun auras) and cannot be
            // interrupted either: polymorph, fear or a disorient stop it
            bool const immune = enemy->HasAuraTypeWithMiscvalue(SPELL_AURA_MECHANIC_IMMUNITY, MECHANIC_STUN)
                || enemy->HasAuraTypeWithMiscvalue(SPELL_AURA_STATE_IMMUNITY, SPELL_AURA_MOD_STUN);
            bool const casting = enemy->GetCurrentSpell(CURRENT_CHANNELED_SPELL) || enemy->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (!immune && _bot->GetDistance(enemy) <= 20.0f && _class->Stun(enemy))
            {
                sCompanionMgr->ClaimStun(enemy->GetGUID(), _bot->GetGUID(), 2000);
                return true;
            }
            if ((immune || casting) && _class->BreakCast(enemy))
            {
                sCompanionMgr->ClaimStun(enemy->GetGUID(), _bot->GetGUID(), 1500);
                _stunDebug = "broke its cast";
                return true;
            }
            _stunDebug = std::string(immune ? "immune" : "open") + (casting ? ", casting" : "") + ": nothing to use (" + _lastCastFailure + ")";
        }

        // a group member under the enemy's control: crowd control him, nobody kills him
        for (Player* member : _view.Members)
        {
            if (member == _bot || !member->IsAlive() || !member->IsCharmed() || member->HasBreakableByDamageCrowdControlAura())
                continue;
            Unit* charmer = member->GetCharmer();
            if (!charmer || !charmer->IsHostileTo(_bot) || _bot->GetDistance(member) > 30.0f)
                continue;
            if (_class->CanCrowdControl(member) && _class->CrowdControl(member))
                return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ Blackwing Lair: Razorgore the Untamed

    namespace
    {
        uint32 constexpr MAP_BLACKWING_LAIR = 469;
        uint32 constexpr NPC_RAZORGORE = 12435;
        uint32 constexpr GO_ORB_OF_DOMINATION = 177808;
        uint32 constexpr GO_BLACK_DRAGON_EGG = 177807;
        uint32 constexpr SPELL_DESTROY_EGG = 19873;
        uint32 constexpr SPELL_POSSESS = 19832;
        uint32 constexpr SPELL_MIND_EXHAUSTION = 23958;
        uint32 constexpr DATA_EGGS = 12;            // the instance script's egg state: DONE once all 30 are broken
        uint32 constexpr DATA_EGGS_BROKEN = 13;     // how many are broken
        uint32 constexpr EGGS_LAST = 27;            // from here the main tank controls him: he turns on the last one who did
    }

    bool Mechanics::Untouchable(Unit const* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_UNIT || unit->GetEntry() != NPC_RAZORGORE || unit->GetMapId() != MAP_BLACKWING_LAIR)
            return false;
        InstanceScript const* instance = unit->GetInstanceScript();
        return instance && instance->GetData(DATA_EGGS) != DONE;
    }

    namespace
    {
        // who takes the orb next: the owner's companions in the group view, alive and without Mind Exhaustion (all agree)
        Player* NextOrbController(GroupView const& view, ObjectGuid owner, uint32 broken)
        {
            Player* best = nullptr;
            int32 bestRank = 0;
            for (Player* member : view.Members)
            {
                CompanionAI const* ai = sCompanionMgr->GetAI(member);
                if (!ai || ai->GetOwnerGUID() != owner || !member->IsAlive() || member->HasAura(SPELL_MIND_EXHAUSTION))
                    continue;
                Role const role = ai->GetEffectiveRole();
                int32 rank = role == Role::RangedDps ? 1 : role == Role::MeleeDps ? 2 : role == Role::Tank ? 3 : 4;
                if (broken >= EGGS_LAST && member == view.MainTank)
                    rank = 0;
                if (!best || rank < bestRank || (rank == bestRank && member->GetGUID() < best->GetGUID()))
                {
                    best = member;
                    bestRank = rank;
                }
            }
            return best;
        }
    }

    std::string CompanionAI::OrbLine()
    {
        Creature* razorgore = _bot->FindNearestCreature(NPC_RAZORGORE, 200.0f);
        GameObject* orb = _bot->FindNearestGameObject(GO_ORB_OF_DOMINATION, 200.0f);
        if (!razorgore)
            return "no Razorgore";
        uint32 const broken = razorgore->GetInstanceScript() ? razorgore->GetInstanceScript()->GetData(DATA_EGGS_BROKEN) : 0;
        Player* next = NextOrbController(_view, _owner, broken);
        Unit* charmer = razorgore->GetCharmer();
        return Trinity::StringFormat("Razorgore {} {}, untouchable {}, controlled by {}, eggs broken {}, next {}, orb {:.1f} yards{}",
            razorgore->IsInCombat() ? "fighting" : "idle", razorgore->IsInEvadeMode() ? "evading" : "", uint32(Mechanics::Untouchable(razorgore)),
            charmer ? charmer->GetName() : "nobody", broken, next ? next->GetName() : "nobody", orb ? _bot->GetExactDist2d(orb) : -1.0f,
            _bot->HasAura(SPELL_MIND_EXHAUSTION) ? ", exhausted" : "");
    }

    // Razorgore's phase one: his eggs break only under his own Destroy Egg, while someone controls him through the Orb of
    // Domination, and his death before the last one blows up the whole raid. One companion at a time takes the orb (damage
    // dealers first; the main tank for the last eggs, so that Razorgore turns on a tank), walks him from egg to egg and
    // breaks them; when its control ends the next one without Mind Exhaustion takes over. A player who takes the orb keeps it.
    bool CompanionAI::UpdateOrbControl()
    {
        if (_bot->GetMapId() != MAP_BLACKWING_LAIR || _ambient)
            return false;
        Creature* razorgore = _bot->FindNearestCreature(NPC_RAZORGORE, 200.0f);
        if (!razorgore || !Mechanics::Untouchable(razorgore) || !razorgore->IsInCombat())
            return false;
        uint32 const broken = razorgore->GetInstanceScript()->GetData(DATA_EGGS_BROKEN);

        auto nextController = [&]() { return NextOrbController(_view, _owner, broken); };

        // controlling him: its own body keeps still (moving or casting would end the control), he goes from egg to egg
        if (_bot->GetCharmedGUID() == razorgore->GetGUID())
        {
            Player* next = nextController();
            if (broken >= EGGS_LAST && next && next != _bot && next == _view.MainTank)
            {
                razorgore->RemoveAurasDueToSpell(SPELL_POSSESS);    // the main tank takes him for the last eggs
                return true;
            }
            if (razorgore->HasUnitState(UNIT_STATE_CASTING))
                return true;
            std::list<GameObject*> eggs;
            razorgore->GetGameObjectListWithEntryInGrid(eggs, GO_BLACK_DRAGON_EGG, 150.0f);
            GameObject* egg = nullptr;
            for (GameObject* candidate : eggs)
                if (candidate->isSpawned() && (!egg || razorgore->GetExactDist2d(candidate) < razorgore->GetExactDist2d(egg)))
                    egg = candidate;
            if (!egg)
                return true;
            if (razorgore->GetExactDist2d(egg) > 3.0f)
            {
                if (_orbEgg != egg->GetGUID() || !razorgore->isMoving())
                {
                    razorgore->GetMotionMaster()->MovePoint(0, egg->GetPosition());
                    _orbEgg = egg->GetGUID();
                }
                return true;
            }
            if (razorgore->isMoving())
                razorgore->StopMoving();
            razorgore->CastSpell(egg, SPELL_DESTROY_EGG, false);
            return true;
        }

        // someone controls him already: the others fight the adds
        if (!razorgore->GetCharmerGUID().IsEmpty() || nextController() != _bot)
            return false;
        GameObject* orb = _bot->FindNearestGameObject(GO_ORB_OF_DOMINATION, 200.0f);
        if (!orb)
            return false;
        if (_bot->GetExactDist2d(orb) > 3.0f)
        {
            if (!_bot->isMoving())
            {
                _bot->GetMotionMaster()->MovePoint(0, orb->GetPosition());
                _movement = MoveMode::Point;
            }
            return true;
        }
        StopMovement();
        orb->Use(_bot);
        return true;
    }
}
