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
#include <unordered_map>

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

        // what a crafter calls out in town (its level decides what it can make)
        std::string CrafterLine(std::string const& trade, uint8 level)
        {
            bool high = level >= 50;
            std::vector<char const*> lines;
            if (trade == "enchanting")
                lines = high ? std::vector<char const*>{ "Enchanter LF work! Crusader, Fiery, +stats on chest and bracers. Your mats, tips welcome.",
                                                         "Enchanting services here, bring your mats and I'll make that weapon shine." }
                             : std::vector<char const*>{ "Enchanter LF work, cheap bracer and chest enchants, your mats.", "Need an enchant? Bring mats, tips welcome." };
            else if (trade == "tailoring")
                lines = high ? std::vector<char const*>{ "Tailor here, Runecloth Bags and robes made to order. PST.", "WTS Mooncloth crafts, bring your cloth!" }
                             : std::vector<char const*>{ "Tailor LF work, bags and robes, your cloth.", "Making linen and wool bags, cheap!" };
            else if (trade == "blacksmithing")
                lines = { "Blacksmith LF work: weapons, armor, sharpening stones. Bring the bars!", "Smithing on request, your ore, my anvil." };
            else if (trade == "leatherworking")
                lines = { "Leatherworker here: armor kits and leather gear, your mats.", "Need armor kits? Leatherworker LF work." };
            else if (trade == "alchemy")
                lines = high ? std::vector<char const*>{ "WTS Major Healing and Mana potions, flasks on request.", "Alchemist here, transmutes and elixirs, PST." }
                             : std::vector<char const*>{ "Selling healing potions, cheap!", "Alchemist LF work, bring herbs." };
            else if (trade == "engineering")
                lines = { "Engineer here, scopes, bombs and goggles. PST!", "Need a scope on that gun? Engineer LF work." };
            else if (trade == "cooking")
                lines = { "Cook LF work! Bring the meat, I bring the spices.", "Selling food that actually tastes good. PST." };
            else
                lines = { "Selling bandages, cheap!", "First aid here, bandages for your next dungeon run." };
            return lines[urand(0, lines.size() - 1)];
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
        static char const* const jobs[] = { "no job", "hunter", "quester", "trader", "crafter", "shopper", "idler" };
        std::string out = std::string(_ambientCity ? "city, " : "world, ") + jobs[uint8(_job)] + (_jobTrade.empty() ? "" : " (" + _jobTrade + ")")
            + ", " + names[uint8(_activity)];
        if (_jobEntry)
            out += ", hunts " + std::to_string(_jobEntry) + " (" + std::to_string(_jobKills) + (_jobGoal ? "/" + std::to_string(_jobGoal) : "") + ")";
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
        // a crafter calls out its trade now and then while it stands there
        if (_job == Job::Crafter && _activity != Activity::None && now >= _jobAdvertAt)
        {
            _jobAdvertAt = now + Seconds(urand(120, 260));
            _bot->Say(CrafterLine(_jobTrade, _bot->GetLevel()), LANG_UNIVERSAL);
        }
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
                if (Unit* prey = _activityTarget.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*_bot, _activityTarget); prey && !prey->IsAlive())
                    ++_jobKills;
                break;
            case Activity::None:
                if (now < _activityUntil)
                    return;
                break;
        }

        _activity = Activity::None;
        _activityTarget.Clear();
        if (_job == Job::None)
            ChooseJob();
        bool acted = false;
        switch (_job)
        {
            case Job::Hunter:  acted = JobHunt(); break;
            case Job::Quester: acted = JobQuest(); break;
            default:           acted = JobCity(); break;
        }
        // nothing to do this moment: a short pause, then again
        if (!acted)
        {
            _activity = Activity::None;
            _activityUntil = now + Seconds(urand(4, 10));
        }
    }

    void CompanionAI::ChooseJob()
    {
        uint32 roll = urand(0, 99);
        if (_ambientCity)
        {
            _job = roll < 25 ? Job::Trader : roll < 45 ? Job::Crafter : roll < 85 ? Job::Shopper : Job::Idler;
            if (_job == Job::Crafter)
            {
                static char const* const trades[] = { "enchanting", "tailoring", "blacksmithing", "leatherworking", "alchemy", "engineering",
                    "cooking", "first aid" };
                _jobTrade = trades[urand(0, std::size(trades) - 1)];
                _jobAdvertAt = GameTime::Now() + Seconds(urand(10, 60));
            }
            if (_job == Job::Shopper)
            {
                // two or three errands, then off by flight master
                static uint32 const stops[] = { UNIT_NPC_FLAG_VENDOR, UNIT_NPC_FLAG_TRAINER, UNIT_NPC_FLAG_BANKER, UNIT_NPC_FLAG_AUCTIONEER,
                    UNIT_NPC_FLAG_INNKEEPER, UNIT_NPC_FLAG_VENDOR };
                uint32 count = urand(2, 3);
                for (uint32 i = 0; i < count; ++i)
                    _jobErrands.push_back(stops[urand(0, std::size(stops) - 1)]);
                _jobErrands.push_back(UNIT_NPC_FLAG_FLIGHTMASTER);
            }
            return;
        }
        _job = roll < 55 ? Job::Hunter : Job::Quester;
    }

    // the kind of mob around it that is most common at its level: its hunting ground for this life
    bool CompanionAI::ChooseHuntingGround(Position const& around, float range)
    {
        uint8 const level = _bot->GetLevel();
        std::unordered_map<uint32, std::vector<Creature*>> byEntry;
        for (Creature* creature : CreaturesAround(_bot, range))
        {
            if (!creature->IsAlive() || creature->IsPet() || creature->IsTotem() || creature->IsTrigger() || creature->IsCritter()
                || creature->IsCivilian() || creature->IsElite() || creature->isWorldBoss() || creature->GetExactDist2d(&around) > range)
                continue;
            if (creature->GetLevel() + 4 < level || creature->GetLevel() > level + 2 || !_bot->IsValidAttackTarget(creature))
                continue;
            byEntry[creature->GetEntry()].push_back(creature);
        }
        std::vector<Creature*> const* best = nullptr;
        for (auto const& [entry, list] : byEntry)
            if (!best || list.size() > best->size() || (list.size() == best->size() && urand(0, 1)))
                best = &list;
        if (!best)
            return false;
        float x = 0.0f, y = 0.0f;
        for (Creature* creature : *best)
        {
            x += creature->GetPositionX();
            y += creature->GetPositionY();
        }
        _jobEntry = best->front()->GetEntry();
        _jobCenter.Relocate(x / float(best->size()), y / float(best->size()), best->front()->GetPositionZ());
        return true;
    }

    // one kind of mob in one place, again and again (they respawn): what a player grinding a spot does
    bool CompanionAI::JobHunt()
    {
        if (!_jobEntry && !ChooseHuntingGround(_ambientHome, 70.0f))
        {
            // nothing of its level around: it takes a quest instead, or walks a little
            if (_job == Job::Hunter && FindNpc(UNIT_NPC_FLAG_QUESTGIVER, 150.0f))
            {
                _job = Job::Quester;
                return JobQuest();
            }
            return AmbientWander(40.0f);
        }
        Creature* best = nullptr;
        for (Creature* creature : CreaturesAround(_bot, 70.0f))
        {
            if (creature->GetEntry() != _jobEntry || !creature->IsAlive() || creature->IsInCombat() || creature->GetExactDist2d(&_jobCenter) > 60.0f)
                continue;
            if (!_bot->IsValidAttackTarget(creature) || NearRealPlayer(creature, 35.0f) || !_bot->IsWithinLOSInMap(creature))
                continue;
            if (!best || _bot->GetDistance(creature) < _bot->GetDistance(best))
                best = creature;
        }
        if (best)
        {
            _activity = Activity::Grind;
            _activityTarget = best->GetGUID();
            if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
                _bot->SetStandState(UNIT_STAND_STATE_STAND);
            OrderAttack(best->GetGUID(), false);
            return true;
        }
        // all down for now: it waits on its spot for them to come back
        _ambientHome = _jobCenter;
        return AmbientWander(18.0f);
    }

    // a quest: talk to the giver, hunt the mobs around, come back, again
    bool CompanionAI::JobQuest()
    {
        Creature* giver = _jobNpc.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*_bot, _jobNpc);
        if (!giver)
        {
            giver = FindNpc(UNIT_NPC_FLAG_QUESTGIVER, 150.0f);
            if (!giver)
            {
                _job = Job::Hunter;
                return JobHunt();
            }
            _jobNpc = giver->GetGUID();
            _jobStage = 0;
        }
        switch (_jobStage)
        {
            case 0:     // to the quest giver
                _jobStage = 1;
                return GoTo(giver, urand(6000, 15000));
            case 1:     // a quest taken: the mobs around for it
                _jobEntry = 0;
                if (!ChooseHuntingGround(giver->GetPosition(), 110.0f))
                {
                    _jobStage = 0;
                    _jobNpc.Clear();    // nothing for it here: another giver
                    return AmbientWander(40.0f);
                }
                _jobKills = 0;
                _jobGoal = urand(4, 9);
                _jobStage = 2;
                [[fallthrough]];
            default:    // hunting until the quest is done, then back
                if (_jobKills >= _jobGoal)
                {
                    _jobStage = 0;
                    return JobQuest();
                }
                return JobHunt();
        }
    }

    // cities: the auction house trader, the crafter calling out its trade, the shopper with errands, the one at the inn
    bool CompanionAI::JobCity()
    {
        switch (_job)
        {
            case Job::Trader:
            {
                bool bank = urand(0, 99) < 25;
                Creature* npc = FindNpc(bank ? UNIT_NPC_FLAG_BANKER : UNIT_NPC_FLAG_AUCTIONEER, 150.0f);
                if (!npc && bank)
                    npc = FindNpc(UNIT_NPC_FLAG_AUCTIONEER, 150.0f);
                return npc ? GoTo(npc, bank ? urand(20000, 40000) : urand(60000, 150000)) : AmbientWander(25.0f);
            }
            case Job::Crafter:
            {
                // next to its trainer if the city has one, else where the crowd is
                std::string trade = _jobTrade;
                Creature* npc = FindNpc(UNIT_NPC_FLAG_TRAINER, 150.0f, [&trade](Creature* c)
                {
                    std::string sub = c->GetCreatureTemplate()->SubName;
                    for (char& ch : sub)
                        ch = char(std::tolower(static_cast<unsigned char>(ch)));
                    return sub.find(trade) != std::string::npos;
                });
                if (!npc)
                    npc = FindNpc(urand(0, 1) ? UNIT_NPC_FLAG_AUCTIONEER : UNIT_NPC_FLAG_BANKER, 150.0f);
                return npc ? GoTo(npc, urand(120000, 240000)) : AmbientWander(20.0f);
            }
            case Job::Shopper:
            {
                if (_jobStage >= _jobErrands.size())
                {
                    _ambientFinished = true;    // errands done: it flies off (leaves the world)
                    return false;
                }
                uint32 flags = _jobErrands[_jobStage++];
                uint8 classId = _bot->GetClass();
                Creature* npc = FindNpc(flags, 160.0f, [flags, classId](Creature* c)
                {
                    // a trainer of its own class
                    return flags != UNIT_NPC_FLAG_TRAINER || c->GetCreatureTemplate()->trainer_class == classId;
                });
                return npc ? GoTo(npc, flags == UNIT_NPC_FLAG_FLIGHTMASTER ? urand(4000, 8000) : urand(15000, 45000)) : true;
            }
            default:    // the inn: a seat near the innkeeper
            {
                if (_jobStage == 0)
                {
                    _jobStage = 1;
                    if (Creature* inn = FindNpc(UNIT_NPC_FLAG_INNKEEPER, 150.0f))
                        return GoTo(inn, urand(3000, 8000));
                }
                _jobStage = urand(0, 99) < 15 ? 0 : 1;
                _activity = Activity::Sit;
                _activityUntil = GameTime::Now() + Seconds(urand(60, 200));
                _bot->SetStandState(UNIT_STAND_STATE_SIT);
                return true;
            }
        }
    }

    Creature* CompanionAI::FindNpc(uint32 npcFlags, float range, std::function<bool(Creature*)> const& filter) const
    {
        std::vector<Creature*> found;
        for (Creature* creature : CreaturesAround(_bot, range))
            if (creature->IsAlive() && creature->HasNpcFlag(NPCFlags(npcFlags)) && !creature->IsHostileTo(_bot)
                && std::fabs(creature->GetPositionZ() - _bot->GetPositionZ()) < 25.0f && (!filter || filter(creature)))
                found.push_back(creature);
        if (found.empty())
            return nullptr;
        // one of the closest: not always the same one
        std::sort(found.begin(), found.end(), [this](Creature* a, Creature* b) { return _bot->GetDistance(a) < _bot->GetDistance(b); });
        return found[urand(0, std::min<size_t>(found.size(), 3) - 1)];
    }

    // to a spot in front of an NPC, a little to the side (a crowd does not stand in one point), and stay a while
    bool CompanionAI::GoTo(Creature* npc, uint32 dwellMs)
    {
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
        _activityDwell = dwellMs;
        if (_bot->GetStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
        _bot->GetMotionMaster()->MovePoint(0, _activitySpot);
        _movement = MoveMode::Point;
        return true;
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
