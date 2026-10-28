/*
 * Companions: what the bots know of the dungeon and raid bosses, by role (world.companion_boss_tactics, written from
 * docker/companions/tactics/*.json by scripts/gen-tactics.py). Asked in party chat ("what's the strat for Baron?", "how do
 * we do Gandling", "tactics" with the boss targeted), the companions tell it, each its own part; the brain gets it too.
 */

#ifndef COMPANION_TACTICS_H
#define COMPANION_TACTICS_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Player;
class Unit;

namespace Companions::Tactics
{
    struct Boss
    {
        uint32 Map = 0;
        uint32 Sort = 0;                    // the order the group meets them
        bool Trash = false;                 // dangerous trash: answered when named or targeted, not in the lists of bosses
        std::string Instance;
        std::vector<std::string> InstanceAliases;
        std::string Wing;
        std::string Name;
        std::vector<std::string> Aliases;   // lower case without apostrophes: the name first, then what players call it
        std::string General, Tank, Healer, Dps;
    };

    void Load();
    size_t Count();
    std::vector<Boss const*> OfMap(uint32 mapId);         // the bosses (no trash), in order
    // a boss (or trash) the text names, this instance's first
    Boss const* Named(std::string const& text, uint32 mapId);
    Boss const* Of(Unit const* unit);
    // "tactics", "strat", "how do we do", "what bosses"...
    bool AsksTactics(std::string const& text);

    // kept by GUID: the plain answer may be said seconds later (after the brain), when a bot may have left
    struct Line
    {
        ObjectGuid Bot;
        std::string Name;
        std::string Text;
        uint32 DelayMs;
    };
    // the boss a question is about: the one named, else the asker's target, else one the companions fight
    Boss const* Resolve(Player* asker, std::vector<Player*> const& bots, std::string const& text);
    // one line for the brain: the boss's fight and each role's part
    std::string BrainLine(Boss const& boss);

    // what the companions answer (empty: not a boss question they can answer); named: the one asked, it tells all of it
    std::vector<Line> Answer(Player* asker, std::vector<Player*> const& bots, Player* named, std::string const& text);
    // says them (in party chat, or whispered to the asker)
    void Tell(std::vector<Line> const& lines, Player* whisperTo = nullptr);
    // for the brain: the instance's bosses, and the tactics of the ones named in the message or fought now
    std::string BrainContext(Player* bot, std::string const& message);
}

#endif
