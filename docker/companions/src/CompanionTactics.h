/*
 * Companions: what the bots know of the dungeon and raid bosses, by role (world.companion_boss_tactics, written from
 * docker/companions/tactics/*.json by scripts/gen-tactics.py). Asked in party chat ("what's the strat for Baron?", "how do
 * we do Gandling", "tactics" with the boss targeted), the companions tell it, each its own part; the brain gets it too.
 */

#ifndef COMPANION_TACTICS_H
#define COMPANION_TACTICS_H

#include "Define.h"
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
        std::string Instance;
        std::vector<std::string> InstanceAliases;
        std::string Wing;
        std::string Name;
        std::vector<std::string> Aliases;   // lower case without apostrophes: the name first, then what players call it
        std::string General, Tank, Healer, Dps;
    };

    void Load();
    size_t Count();
    std::vector<Boss const*> OfMap(uint32 mapId);
    // a boss the text names, this instance's first
    Boss const* Named(std::string const& text, uint32 mapId);
    Boss const* Of(Unit const* unit);
    // "tactics", "strat", "how do we do", "what bosses"...
    bool AsksTactics(std::string const& text);

    struct Line
    {
        Player* Bot;
        std::string Text;
        uint32 DelayMs;
    };
    // what the companions answer (empty: not a boss question they can answer); named: the one asked, it tells all of it
    std::vector<Line> Answer(Player* asker, std::vector<Player*> const& bots, Player* named, std::string const& text);
    // says them (in party chat, or whispered to the asker)
    void Tell(std::vector<Line> const& lines, Player* whisperTo = nullptr);
    // for the brain: the instance's bosses, and the tactics of the ones named in the message or fought now
    std::string BrainContext(Player* bot, std::string const& message);
}

#endif
