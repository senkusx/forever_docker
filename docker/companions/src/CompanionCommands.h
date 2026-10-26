/*
 * Companions: orders given in party chat, whispers, ".bot order" or by the LLM brain
 */

#ifndef COMPANION_COMMANDS_H
#define COMPANION_COMMANDS_H

#include <string>
#include <vector>

class Group;
class Player;

namespace Companions::Commands
{
    // returns false when the text is not an order (it may then go to the LLM brain)
    bool ExecuteOrder(Player* commander, std::vector<Player*> const& bots, std::string const& text, bool quiet);
    std::string OrdersHelp();
    // what a player says about an ability ("use more frost nova", "stop using arcane explosion"): kept by the bots that know it
    bool SpeechPreference(Player* commander, std::vector<Player*> const& bots, std::string const& message, bool quiet);
    // a party or raid line from a player: orders for the companions, else talk for the AI brain
    void OnPartyChat(Player* speaker, std::string const& msg, Group* group);
}

#endif
