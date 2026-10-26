/*
 * Companions: plans. Instructions in several steps, the way a raid leader gives them: "attack this pack, then wait for
 * full mana, then we pull the other pack when everyone has full health and mana and the team confirmed they are ready",
 * "when we pull this pack I want Jaina to polymorph this mob". One plan per owner, carried out step by step on the world
 * thread:
 *   pull   a pack (the target's, a numbered one P1, P2... or the next one when the step starts), with crowd control
 *          assignments for that pull (the tank pulls a member nobody controls; the controlled one dies last)
 *   rest   until everyone is at that health and mana
 *   ready  until each player of the group said ready ("ready", "r", "go"...); the companions say it when they are
 *   wait   some seconds
 * Typed as an order ("plan pull p1, jaina cc e5; rest; ready; pull next") or said in plain English; the brain writes
 * the order form for what it understands.
 */

#ifndef COMPANION_PLAN_H
#define COMPANION_PLAN_H

#include "ObjectGuid.h"
#include "Position.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class Player;

namespace Companions
{
    struct Pack
    {
        std::vector<ObjectGuid> Members;
        Position Center;
        float Distance = 0.0f;      // from the player it was found around
    };

    class Plans
    {
    public:
        static Plans* instance();

        // hostile creatures around a player that are not fighting yet, grouped in packs, nearest first
        static std::vector<Pack> FindPacks(Player* around, float range);
        // for the brain: the packs around, numbered P1, P2... (the numbers orders use), and the plan going on
        std::string PackLines(Player* owner);
        // "p2" -> that pack (numbered by the last PackLines or FindPacks for this owner)
        bool ResolvePack(Player* owner, std::string const& word, Pack& pack);

        // "plan <steps>" (the order form): true when it understood at least one step
        bool StartOrder(Player* commander, std::vector<Player*> const& bots, std::string const& steps, bool quiet);
        // plain English with several steps or a crowd control on the pull; false when it is not one
        bool StartNatural(Player* commander, std::vector<Player*> const& bots, std::string const& text, bool quiet);
        void Cancel(ObjectGuid owner, bool announce);
        bool HasPlan(ObjectGuid owner) const { return _plans.count(owner) != 0; }
        std::string Describe(ObjectGuid owner) const;

        bool OnChat(Player* player, std::string const& message);    // "ready", "cancel plan": true when it was for a plan
        void Update(uint32 diff);   // world thread

    private:
        struct Step
        {
            enum class Kind : uint8 { Pull, Rest, Ready, Wait };
            Kind Type = Kind::Pull;
            ObjectGuid Target;
            std::vector<ObjectGuid> PackMembers;
            bool NextPack = false;          // the nearest pack when the step starts
            std::vector<std::pair<ObjectGuid, ObjectGuid>> Control;   // bot, enemy
            uint8 Health = 100;
            uint8 Mana = 100;
            uint32 Ms = 0;
        };
        struct Plan
        {
            ObjectGuid Owner;
            std::vector<Step> Steps;
            size_t Current = 0;
            bool StepStarted = false;
            uint32 StepMs = 0;
            uint32 QuietMs = 0;
            uint32 RefreshMs = 0;
            bool Engaged = false;
            bool Released = false;
            std::unordered_set<ObjectGuid> Ready;
        };

        bool Start(Player* commander, std::vector<Step> steps, bool quiet);
        bool ParseStep(Player* commander, std::vector<Player*> const& bots, std::string const& text, std::vector<Step>& steps);
        Player* BotFor(std::vector<Player*> const& bots, std::string const& word) const;    // a name or a class ("the mage")
        void StartStep(Plan& plan, Player* owner);
        bool StepDone(Plan& plan, Player* owner, uint32 diff);
        std::string StepText(Step const& step, Player* owner) const;
        void Announce(Player* owner, std::string const& text, Player* speaker = nullptr);

        std::unordered_map<ObjectGuid, Plan> _plans;
        std::unordered_map<ObjectGuid, std::vector<Pack>> _packs;   // per owner, as last numbered
        uint32 _timer = 0;
    };
}

#define sPlans Companions::Plans::instance()

#endif
