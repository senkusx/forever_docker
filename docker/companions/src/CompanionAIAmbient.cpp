/*
 * Companions: the bot brain, ambient life (bots of the world, CompanionAmbient.h). Simple on purpose: grind mobs of its
 * level, walk up to quest givers, wander; in cities the auction house, the bank, the inn, flight masters and trainers.
 */

#include "CompanionAI.h"
#include "CellImpl.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "GridDefines.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include <cmath>

namespace Companions
{
    namespace
    {
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

        std::list<Creature*> CreaturesAround(Player* bot, float range)
        {
            std::list<Creature*> creatures;
            Trinity::AllWorldObjectsInRange check(bot, range);
            Trinity::CreatureListSearcher<Trinity::AllWorldObjectsInRange> searcher(bot, creatures, check);
            Cell::VisitGridObjects(bot, searcher, range);
            return creatures;
        }

        // a real player close to it: its mob, its quest, not ours to take
        bool NearRealPlayer(Creature const* creature, float range)
        {
            for (MapReference const& ref : creature->GetMap()->GetPlayers())
                if (Player* other = ref.GetSource())
                    if (!other->GetSession()->IsBot() && other->IsAlive() && other->GetDistance(creature) < range)
                        return true;
            return false;
        }
    }

    void CompanionAI::SetAmbient(Position const& home, bool city)
    {
        _ambient = true;
        _ambientCity = city;
        _ambientHome = home;
        _activity = Activity::None;
        _activityUntil = GameTime::Now() + Milliseconds(urand(1000, 5000));
        _aggression = Aggression::Assist;
        _stance = Stance::Follow;
    }

    void CompanionAI::BecomeCompanion(ObjectGuid owner)
    {
        _ambient = false;
        _ambientCity = false;
        _owner = owner;
        _activity = Activity::None;
        _forcedTarget.Clear();
        _pulling = false;
        if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
        SetRole(Role::Auto);
        _movement = MoveMode::None;
    }

    std::string CompanionAI::AmbientLine() const
    {
        static char const* const names[] = { "deciding", "grinding", "visiting", "wandering", "sitting" };
        std::string out = std::string(_ambientCity ? "city, " : "world, ") + names[uint8(_activity)];
        if (!_activityTarget.IsEmpty())
            if (Unit* unit = ObjectAccessor::GetUnit(*_bot, _activityTarget))
                out += " " + unit->GetName();
        out += ", " + std::to_string(uint32(_bot->GetExactDist2d(&_ambientHome))) + " yards from home";
        return out;
    }

    // out of combat: rest, buff, then one activity after the other
    void CompanionAI::UpdateAmbient()
    {
        if (_eating)
        {
            bool full = HealthPct() >= 95.0f && (_bot->GetMaxPower(POWER_MANA) == 0 || ManaPct() >= 95.0f);
            bool resting = _bot->HasAura([](Aura const* aura) { return IsFoodOrDrink(aura); });
            if (!full && resting)
                return;
            _eating = false;
            if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
                _bot->SetStandState(UNIT_STAND_STATE_STAND);
        }
        if (UpdateHealing(false) || UpdateLooting() || UpdateEatDrink() || UpdateBuffs(false) || _class->OutOfCombat())
            return;

        TimePoint const now = GameTime::Now();
        switch (_activity)
        {
            case Activity::Visit:
            case Activity::Wander:
            {
                if (_bot->isMoving())
                    return;
                if (!_activityArrived)
                {
                    _activityArrived = true;
                    _activityUntil = now + Milliseconds(_activityDwell);
                    if (Unit* npc = _activityTarget.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*_bot, _activityTarget))
                    {
                        _bot->SetFacingToObject(npc);
                        _bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
                    }
                }
                if (now < _activityUntil)
                    return;
                break;
            }
            case Activity::Sit:
                if (now < _activityUntil)
                    return;
                _bot->SetStandState(UNIT_STAND_STATE_STAND);
                break;
            case Activity::Grind:
                // the kill is over (combat ended) or the target was taken: next thing
                break;
            case Activity::None:
                if (now < _activityUntil)
                    return;
                break;
        }

        _activity = Activity::None;
        _activityTarget.Clear();
        uint32 roll = urand(0, 99);
        if (_ambientCity)
        {
            if (roll < 65 && AmbientVisit())
                return;
            if (roll < 80)
            {
                _activity = Activity::Sit;
                _activityUntil = now + Seconds(urand(15, 60));
                _bot->SetStandState(UNIT_STAND_STATE_SIT);
                return;
            }
            AmbientWander(25.0f);
            return;
        }
        if (roll < 70 && AmbientGrind())
            return;
        if (roll < 85 && AmbientVisit())
            return;
        AmbientWander(40.0f);
    }

    // a mob of its level nobody else is busy with
    bool CompanionAI::AmbientGrind()
    {
        uint8 const level = _bot->GetLevel();
        Creature* best = nullptr;
        for (Creature* creature : CreaturesAround(_bot, 45.0f))
        {
            if (!creature->IsAlive() || creature->IsInCombat() || creature->IsPet() || creature->IsTotem() || creature->IsTrigger()
                || creature->IsCritter() || creature->IsCivilian() || creature->IsElite() || creature->isWorldBoss() || creature->IsEvadingAttacks())
                continue;
            if (creature->GetLevel() + 4 < level || creature->GetLevel() > level + 2)
                continue;
            if (!_bot->IsValidAttackTarget(creature) || creature->GetExactDist2d(&_ambientHome) > 120.0f || !_bot->IsWithinLOSInMap(creature))
                continue;
            if (NearRealPlayer(creature, 35.0f))
                continue;
            if (!best || _bot->GetDistance(creature) < _bot->GetDistance(best))
                best = creature;
        }
        if (!best)
            return false;
        _activity = Activity::Grind;
        _activityTarget = best->GetGUID();
        if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
        OrderAttack(best->GetGUID(), false);
        _ambientHome = best->GetPosition();     // hunting grounds move with the hunt
        return true;
    }

    // a quest giver to talk to; in cities the auction house, the bank, an inn, a flight master, a trainer, a vendor
    bool CompanionAI::AmbientVisit()
    {
        NPCFlags const wanted = _ambientCity
            ? NPCFlags(UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_BANKER | UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_FLIGHTMASTER
                | UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_QUESTGIVER)
            : NPCFlags(UNIT_NPC_FLAG_QUESTGIVER | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_INNKEEPER);
        std::vector<Creature*> choices;
        for (Creature* creature : CreaturesAround(_bot, _ambientCity ? 120.0f : 90.0f))
        {
            if (!creature->IsAlive() || !creature->HasNpcFlag(wanted) || creature->IsHostileTo(_bot) || creature->isMoving())
                continue;
            if (creature->GetExactDist2d(&_ambientHome) > 150.0f || std::fabs(creature->GetPositionZ() - _bot->GetPositionZ()) > 15.0f)
                continue;
            // the auction house and the bank are where a crowd stands
            uint32 weight = creature->HasNpcFlag(NPCFlags(UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_BANKER)) ? 3 : 1;
            for (uint32 i = 0; i < weight; ++i)
                choices.push_back(creature);
        }
        if (choices.empty())
            return false;
        Creature* npc = choices[urand(0, choices.size() - 1)];

        // a spot in front of it, a little to the side, so a crowd does not stand in one point
        float angle = npc->GetOrientation() + frand(-0.9f, 0.9f);
        float dist = npc->GetCombatReach() + frand(1.5f, 4.0f);
        float x = npc->GetPositionX() + std::cos(angle) * dist;
        float y = npc->GetPositionY() + std::sin(angle) * dist;
        float z = npc->GetPositionZ();
        if (!Trinity::IsValidMapCoord(x, y))
            return false;
        _bot->UpdateAllowedPositionZ(x, y, z);
        if (std::fabs(z - npc->GetPositionZ()) > 3.0f)
        {
            x = npc->GetPositionX();
            y = npc->GetPositionY();
            z = npc->GetPositionZ();
        }
        _activity = Activity::Visit;
        _activityTarget = npc->GetGUID();
        _activitySpot.Relocate(x, y, z);
        _activityArrived = false;
        _activityDwell = _ambientCity ? urand(20000, 90000) : urand(6000, 20000);
        if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
        _bot->GetMotionMaster()->MovePoint(0, _activitySpot);
        _movement = MoveMode::Point;
        return true;
    }

    bool CompanionAI::AmbientWander(float range)
    {
        for (uint32 attempt = 0; attempt < 6; ++attempt)
        {
            float angle = frand(0.0f, 2.0f * float(M_PI));
            float dist = frand(range * 0.3f, range);
            float x = _ambientHome.GetPositionX() + std::cos(angle) * dist;
            float y = _ambientHome.GetPositionY() + std::sin(angle) * dist;
            float z = _bot->GetPositionZ();
            if (!Trinity::IsValidMapCoord(x, y))
                continue;
            _bot->UpdateAllowedPositionZ(x, y, z);
            if (std::fabs(z - _bot->GetPositionZ()) > 10.0f || InLiquid(x, y, z, false) || !_bot->IsWithinLOS(x, y, z + 1.5f))
                continue;
            _activity = Activity::Wander;
            _activityTarget.Clear();
            _activitySpot.Relocate(x, y, z);
            _activityArrived = false;
            _activityDwell = urand(3000, 15000);
            if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
                _bot->SetStandState(UNIT_STAND_STATE_STAND);
            _bot->GetMotionMaster()->MovePoint(0, _activitySpot);
            _movement = MoveMode::Point;
            return true;
        }
        _activity = Activity::None;
        _activityUntil = GameTime::Now() + Seconds(urand(5, 15));
        return false;
    }
}
