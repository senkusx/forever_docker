/*
 * Companions: personalities. Every companion gets one (a voice, an attitude, a quirk) unless its player wrote one; the
 * brain speaks in it, and the short answers the bots give without the brain ("Following.") are said in its voice too.
 */

#ifndef COMPANION_PERSONALITY_H
#define COMPANION_PERSONALITY_H

#include "Define.h"
#include <string>

class Player;

namespace Companions::Personality
{
    // a random one that fits the race and class a little
    std::string Random(uint8 race, uint8 classId);
    // a canned answer in the bot's voice: kind = follow, stay, come, rest, focus, burst, cc, interrupt, ok, prefer, avoid,
    // thanks; {x} in the lines is replaced by detail
    std::string Line(Player* bot, std::string const& kind, std::string const& detail = "");
}

#endif
