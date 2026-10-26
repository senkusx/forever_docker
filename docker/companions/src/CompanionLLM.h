/*
 * Companions: bridge to the AI brain (the "brain" workers: Claude, Ollama or any OpenAI-compatible server, on this
 * machine or remote). The worldserver writes what the bots hear into companion_llm_events with a snapshot of the
 * situation; a worker answers in companion_llm_actions with what the bots say and the orders they carry out (the
 * same orders players type). The game never waits for the brain: the server AI keeps fighting meanwhile.
 */

#ifndef COMPANION_LLM_H
#define COMPANION_LLM_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Group;
class Player;
class Unit;

namespace Companions::CompanionLLM
{
    void EnsureTables();
    void Initialize();
    void Update(uint32 diff);   // world thread

    bool IsEnabledFor(Player const* bot);
    // a whisper to one bot; returns false when the brain is off for it
    bool OnChatToBot(Player* bot, Player* speaker, std::string const& channel, std::string const& message);
    // a party line that is not an order: one event for all the brain-linked bots of this owner in the group
    bool OnGroupChat(Player* owner, Player* speaker, std::vector<Player*> const& bots, std::string const& message, Player* addressed);
    // something the bot noticed (out of mana, adds...): worded by the brain, or the canned line when it has none
    void OnCallout(Player* bot, std::string const& kind, std::string const& detail, std::string const& canned);
    // banter: "levelup", "boss"... (only with Companions.LLM.Banter = 1)
    void OnEvent(Player* bot, std::string const& kind, std::string const& detail);
    // party chat as the bots hear it (players and bots), for the brain's context
    void RecordChat(Group const* group, std::string const& speaker, std::string const& text);
    std::string BuildContext(Player* bot, std::vector<Player*> const& voiced);
    // in fights: the brain reviews the situation now and then and assigns kill order, crowd control, interrupts...
    void ConsiderTactics(Player* bot);
    // "e3": an enemy as the brain numbers them for this owner's group
    bool IsEnemyRef(std::string const& word);
    Unit* ResolveEnemy(Player* commander, std::string const& word);
    uint32 RefOf(ObjectGuid owner, ObjectGuid enemy);   // the number of that enemy for this owner (E<n>)
}

#endif
