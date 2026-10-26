#include "CompanionTactics.h"
#include "CompanionAI.h"
#include "CompanionDefines.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include <algorithm>
#include <sstream>

namespace Companions::Tactics
{
    namespace
    {
        std::vector<Boss> _bosses;

        // lower case, no apostrophes (players type "kelthuzad"), punctuation as spaces
        std::string Normalize(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (char c : text)
            {
                if (c == '\'' || c == '`')
                    continue;
                if (c == '?' || c == '!' || c == ',' || c == '.' || c == ':' || c == ';' || c == '"' || c == '(' || c == ')')
                    out += ' ';
                else
                    out += char(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        std::vector<std::string> List(std::string const& text)
        {
            std::vector<std::string> out;
            std::stringstream ss(text);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                item = Normalize(item);
                item.erase(0, item.find_first_not_of(' '));
                item.erase(item.find_last_not_of(' ') + 1);
                if (!item.empty())
                    out.push_back(item);
            }
            return out;
        }

        bool Mentions(std::string const& lower, std::initializer_list<char const*> words)
        {
            for (char const* word : words)
                if (lower.find(word) != std::string::npos)
                    return true;
            return false;
        }

        // the map of an instance the text names ("what bosses are in strat"), 0 if none
        uint32 InstanceNamed(std::string const& lower)
        {
            for (Boss const& boss : _bosses)
                for (std::string const& alias : boss.InstanceAliases)
                {
                    if (!ContainsWord(lower, alias))
                        continue;
                    // "strat" is also the strategy: Stratholme only as a place ("bosses in strat")
                    if (alias == "strat" && !ContainsWord(lower, "in strat") && !ContainsWord(lower, "of strat") && !ContainsWord(lower, "at strat"))
                        continue;
                    return boss.Map;
                }
            return 0;
        }

        // chat lines stay under 255 characters: longer text is cut at a space
        void Push(std::vector<Line>& lines, Player* bot, std::string text, uint32 delay)
        {
            while (text.size() > 250)
            {
                size_t cut = text.rfind(' ', 245);
                if (cut == std::string::npos || cut < 100)
                    cut = 245;
                lines.push_back({ bot, text.substr(0, cut), delay });
                text = text.substr(cut + 1);
                delay += 1500;
            }
            if (!text.empty())
                lines.push_back({ bot, text, delay });
        }

        Role RoleOf(Player* bot)
        {
            CompanionAI* ai = sCompanionMgr->GetAI(bot);
            return ai ? ai->GetEffectiveRole() : Role::MeleeDps;
        }
    }

    void Load()
    {
        _bosses.clear();
        QueryResult result = WorldDatabase.Query("SELECT map, sort, instance, instance_aliases, wing, boss, aliases, general, tank, healer, dps "
            "FROM companion_boss_tactics ORDER BY map, sort");
        if (!result)
        {
            TC_LOG_INFO("server.worldserver", "Companions: no boss tactics (world.companion_boss_tactics is empty)");
            return;
        }
        do
        {
            Field* fields = result->Fetch();
            Boss boss;
            boss.Map = fields[0].GetUInt32();
            boss.Sort = fields[1].GetUInt32();
            boss.Instance = fields[2].GetString();
            boss.InstanceAliases = List(fields[3].GetString());
            boss.InstanceAliases.insert(boss.InstanceAliases.begin(), Normalize(boss.Instance));
            boss.Wing = fields[4].GetString();
            boss.Name = fields[5].GetString();
            boss.Aliases = List(fields[6].GetString());
            boss.Aliases.insert(boss.Aliases.begin(), Normalize(boss.Name));
            boss.General = fields[7].GetString();
            boss.Tank = fields[8].GetString();
            boss.Healer = fields[9].GetString();
            boss.Dps = fields[10].GetString();
            _bosses.push_back(std::move(boss));
        } while (result->NextRow());

        std::vector<uint32> maps;
        for (Boss const& boss : _bosses)
            if (std::find(maps.begin(), maps.end(), boss.Map) == maps.end())
                maps.push_back(boss.Map);
        TC_LOG_INFO("server.worldserver", "Companions: {} boss tactics in {} instances", _bosses.size(), maps.size());
    }

    size_t Count() { return _bosses.size(); }

    std::vector<Boss const*> OfMap(uint32 mapId)
    {
        std::vector<Boss const*> out;
        for (Boss const& boss : _bosses)
            if (boss.Map == mapId)
                out.push_back(&boss);
        return out;
    }

    Boss const* Named(std::string const& text, uint32 mapId)
    {
        std::string const lower = Normalize(text);
        Boss const* best = nullptr;
        size_t bestLength = 0;
        bool bestHere = false;
        for (Boss const& boss : _bosses)
            for (std::string const& alias : boss.Aliases)
            {
                if (alias.size() < 2 || !ContainsWord(lower, alias))
                    continue;
                bool const here = boss.Map == mapId;
                if ((here && !bestHere) || (here == bestHere && alias.size() > bestLength))
                {
                    best = &boss;
                    bestLength = alias.size();
                    bestHere = here;
                }
            }
        return best;
    }

    Boss const* Of(Unit const* unit)
    {
        Creature const* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature)
            return nullptr;
        std::string const name = Normalize(creature->GetName());
        Boss const* elsewhere = nullptr;
        for (Boss const& boss : _bosses)
            if (boss.Aliases.front() == name)
            {
                if (boss.Map == creature->GetMapId())
                    return &boss;
                elsewhere = elsewhere ? elsewhere : &boss;
            }
        return elsewhere;
    }

    bool AsksTactics(std::string const& text)
    {
        std::string const lower = Normalize(text);
        return Mentions(lower, { "tactic", "strat", "mechanic", "how do we", "how do i", "how should", "how to ", "how does",
            "how do you", "what do we do", "what do i do", "what should i", "what should we", "what does", "explain", "what bosses",
            "which bosses", "boss list", "bosses here", "bosses in", "whats the plan", "what is the plan", "plan for", "tips for",
            "what about" });
    }

    std::vector<Line> Answer(Player* asker, std::vector<Player*> const& bots, Player* named, std::string const& text)
    {
        std::vector<Line> lines;
        if (bots.empty() || !asker)
            return lines;
        std::string const lower = Normalize(text);
        uint32 const map = asker->GetMapId();

        // the boss named, else the one the asker targets, else one the companions fight
        Boss const* boss = Named(lower, map);
        if (!boss)
            boss = Of(asker->GetSelectedUnit());
        if (!boss)
            for (Player* bot : bots)
                if (CompanionAI* ai = sCompanionMgr->GetAI(bot); ai && !boss)
                    for (Unit* enemy : ai->View().Enemies)
                        if (!boss)
                            boss = Of(enemy);

        // the tank explains, unless someone was asked
        Player* first = named;
        if (!first)
            for (Player* bot : bots)
                if (!first && RoleOf(bot) == Role::Tank)
                    first = bot;
        if (!first)
            first = bots.front();

        if (!boss)
        {
            if (!Mentions(lower, { "boss", "tactic", "strat", "mechanic" }))
                return lines;
            // the bosses of the instance named, or of the one we are in
            uint32 const instanceMap = InstanceNamed(lower);
            std::vector<Boss const*> list = instanceMap ? OfMap(instanceMap) : asker->GetMap()->Instanceable() ? OfMap(map) : std::vector<Boss const*>();
            if (list.empty())
            {
                Push(lines, first, "Which boss? Say its name, or target it and ask again.", 0);
                return lines;
            }
            std::string names;
            std::string wing;
            for (Boss const* entry : list)
            {
                if (!entry->Wing.empty() && entry->Wing != wing)
                {
                    wing = entry->Wing;
                    names += (names.empty() ? "" : "; ") + wing + ": ";
                }
                else if (!names.empty())
                    names += ", ";
                names += entry->Name;
            }
            Push(lines, first, list.front()->Instance + " bosses - " + names + ". Ask me about one by name.", 0);
            return lines;
        }

        Push(lines, first, boss->Name + (boss->Map != map ? " (" + boss->Instance + ")" : "") + ": " + boss->General, 0);
        struct Part { Role Wanted; char const* Label; std::string const* Text; };
        Part const parts[] = {
            { Role::Tank, "Tank", &boss->Tank },
            { Role::Healer, "Healers", &boss->Healer },
            { Role::MeleeDps, "DPS", &boss->Dps },
        };
        uint32 delay = 2000;
        for (Part const& part : parts)
        {
            if (part.Text->empty())
                continue;
            Player* who = named;
            if (!who)
                for (Player* bot : bots)
                {
                    Role role = RoleOf(bot);
                    if (role == part.Wanted || (part.Wanted == Role::MeleeDps && role == Role::RangedDps))
                    {
                        who = bot;
                        break;
                    }
                }
            Push(lines, who ? who : first, std::string(part.Label) + ": " + *part.Text, delay);
            delay += 2500;
        }
        return lines;
    }

    void Tell(std::vector<Line> const& lines, Player* whisperTo)
    {
        for (Line const& line : lines)
            if (CompanionAI* ai = sCompanionMgr->GetAI(line.Bot))
                ai->SayLater(line.Text, line.DelayMs, whisperTo ? whisperTo->GetGUID() : ObjectGuid::Empty);
    }

    std::string BrainContext(Player* bot, std::string const& message)
    {
        std::ostringstream ss;
        uint32 const map = bot->GetMapId();
        std::vector<Boss const*> here = OfMap(map);
        if (!here.empty() && bot->GetMap()->Instanceable())
        {
            ss << "bosses of " << here.front()->Instance << " (in the order met):";
            for (Boss const* boss : here)
                ss << " " << boss->Name << (boss == here.back() ? "" : ",");
            ss << "\n";
        }

        std::vector<Boss const*> told;
        if (Boss const* boss = Named(message, map))
            told.push_back(boss);
        if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
            for (Unit* enemy : ai->View().Enemies)
                if (Boss const* boss = Of(enemy); boss && std::find(told.begin(), told.end(), boss) == told.end())
                    told.push_back(boss);
        for (Boss const* boss : told)
        {
            ss << "tactics, " << boss->Name << " (" << boss->Instance << "): " << boss->General;
            if (!boss->Tank.empty())
                ss << " Tank: " << boss->Tank;
            if (!boss->Healer.empty())
                ss << " Healers: " << boss->Healer;
            if (!boss->Dps.empty())
                ss << " DPS: " << boss->Dps;
            ss << "\n";
        }
        return ss.str();
    }
}
