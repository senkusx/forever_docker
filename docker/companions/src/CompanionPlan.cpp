#include "CompanionPlan.h"
#include "CellImpl.h"
#include "CompanionAI.h"
#include "CompanionDefines.h"
#include "CompanionLLM.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include <algorithm>
#include <numeric>
#include <sstream>

namespace Companions
{
    namespace
    {
        std::string Lower(std::string text)
        {
            for (char& c : text)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::vector<std::string> Words(std::string const& text)
        {
            std::vector<std::string> words;
            std::string word;
            for (char c : text)
            {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '\'' || c == '#')
                    word += c;
                else if (!word.empty())
                {
                    words.push_back(word);
                    word.clear();
                }
            }
            if (!word.empty())
                words.push_back(word);
            return words;
        }

        bool Has(std::string const& text, std::initializer_list<char const*> needles)
        {
            for (char const* needle : needles)
            {
                std::string n = needle;
                size_t at = text.find(n);
                while (at != std::string::npos)
                {
                    bool startOk = at == 0 || !std::isalpha(static_cast<unsigned char>(text[at - 1]));
                    bool endOk = at + n.size() >= text.size() || !std::isalpha(static_cast<unsigned char>(text[at + n.size()]));
                    if (startOk && endOk)
                        return true;
                    at = text.find(n, at + 1);
                }
            }
            return false;
        }

        int32 Icon(std::string const& word)
        {
            static char const* const names[] = { "star", "circle", "diamond", "triangle", "moon", "square", "cross", "skull" };
            for (int32 i = 0; i < 8; ++i)
                if (word == names[i])
                    return i;
            return -1;
        }

        Unit* CommanderTarget(Player* commander)
        {
            Unit* target = commander->GetSelectedUnit();
            if (target && target != commander && commander->IsValidAttackTarget(target))
                return target;
            return commander->GetVictim();
        }

        // "target", "this", "it", a marker, "e3"
        Unit* UnitRef(Player* commander, std::string const& word)
        {
            int32 icon = Icon(word);
            if (icon >= 0)
            {
                Group* group = commander->GetGroup();
                ObjectGuid marked = group ? group->GetTargetIcon(uint8(icon)) : ObjectGuid::Empty;
                return marked.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*commander, marked);
            }
            if (CompanionLLM::IsEnemyRef(word))
                return CompanionLLM::ResolveEnemy(commander, word);
            return CommanderTarget(commander);
        }

        uint8 ClassWord(std::string const& word)
        {
            static std::pair<char const*, uint8> const classes[] = { { "warrior", CLASS_WARRIOR }, { "paladin", CLASS_PALADIN },
                { "hunter", CLASS_HUNTER }, { "rogue", CLASS_ROGUE }, { "priest", CLASS_PRIEST }, { "shaman", CLASS_SHAMAN },
                { "mage", CLASS_MAGE }, { "warlock", CLASS_WARLOCK }, { "lock", CLASS_WARLOCK }, { "druid", CLASS_DRUID } };
            for (auto const& [name, id] : classes)
                if (word == name || word == std::string(name) + "s")
                    return id;
            return 0;
        }

        bool ReadyWord(std::string const& text)
        {
            static char const* const words[] = { "ready", "r", "rdy", "go", "im ready", "i'm ready", "ready!", "ready to go", "lets go",
                "let's go", "yes", "yep", "y", "ok", "ready now", "all set", "set" };
            std::string trimmed = Lower(text);
            while (!trimmed.empty() && (trimmed.back() == '!' || trimmed.back() == '.' || trimmed.back() == ' '))
                trimmed.pop_back();
            for (char const* word : words)
                if (trimmed == word)
                    return true;
            return false;
        }

        char const* const CcVerbs[] = { "polymorph", "poly", "sheep", "cc", "crowd control", "shackle", "banish", "hibernate", "sap",
            "trap", "scare", "control" };
    }

    Plans* Plans::instance()
    {
        static Plans instance;
        return &instance;
    }

    // ------------------------------------------------------------------ packs

    std::vector<Pack> Plans::FindPacks(Player* around, float range)
    {
        std::list<Creature*> found;
        Trinity::AllWorldObjectsInRange check(around, range);
        Trinity::CreatureListSearcher<Trinity::AllWorldObjectsInRange> searcher(around, found, check);
        Cell::VisitGridObjects(around, searcher, range);
        std::vector<Creature*> mobs;
        for (Creature* creature : found)
            if (creature->IsAlive() && !creature->IsInCombat() && !creature->IsPet() && !creature->IsTotem() && !creature->IsTrigger()
                && !creature->IsCritter() && !creature->IsCivilian() && !creature->IsEvadingAttacks() && creature->IsHostileTo(around)
                && around->IsValidAttackTarget(creature))
                mobs.push_back(creature);

        // close together (or of the same formation): one pack
        std::vector<size_t> parent(mobs.size());
        std::iota(parent.begin(), parent.end(), 0);
        std::function<size_t(size_t)> root = [&](size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
        for (size_t i = 0; i < mobs.size(); ++i)
            for (size_t j = i + 1; j < mobs.size(); ++j)
                if (mobs[i]->GetDistance(mobs[j]) < 10.0f || (mobs[i]->GetFormation() && mobs[i]->GetFormation() == mobs[j]->GetFormation()))
                    parent[root(i)] = root(j);

        std::unordered_map<size_t, Pack> byRoot;
        for (size_t i = 0; i < mobs.size(); ++i)
        {
            Pack& pack = byRoot[root(i)];
            pack.Members.push_back(mobs[i]->GetGUID());
            float dist = around->GetDistance(mobs[i]);
            if (pack.Members.size() == 1 || dist < pack.Distance)
            {
                pack.Distance = dist;
                pack.Center = mobs[i]->GetPosition();
            }
        }
        std::vector<Pack> packs;
        for (auto& [r, pack] : byRoot)
            packs.push_back(std::move(pack));
        std::sort(packs.begin(), packs.end(), [](Pack const& a, Pack const& b) { return a.Distance < b.Distance; });
        if (packs.size() > 6)
            packs.resize(6);
        return packs;
    }

    std::string Plans::PackLines(Player* owner)
    {
        std::vector<Pack> packs = FindPacks(owner, 70.0f);
        _packs[owner->GetGUID()] = packs;
        std::ostringstream ss;
        if (!packs.empty())
        {
            ss << "packs nearby, not fighting yet:\n";
            for (size_t i = 0; i < packs.size(); ++i)
            {
                ss << "  P" << (i + 1) << " (" << uint32(packs[i].Distance) << " yards):";
                for (ObjectGuid const& guid : packs[i].Members)
                    if (Creature* mob = ObjectAccessor::GetCreature(*owner, guid))
                        ss << " " << mob->GetName() << " (level " << uint32(mob->GetLevel()) << ", " << (mob->IsElite() ? "elite " : "")
                           << "E" << CompanionLLM::RefOf(owner->GetGUID(), guid) << ");";
                ss << "\n";
            }
        }
        if (HasPlan(owner->GetGUID()))
            ss << "plan going on: " << Describe(owner->GetGUID()) << "\n";
        return ss.str();
    }

    bool Plans::ResolvePack(Player* owner, std::string const& word, Pack& pack)
    {
        if (word.size() < 2 || (word[0] != 'p' && word[0] != 'P') || !std::isdigit(static_cast<unsigned char>(word[1])))
            return false;
        size_t index = std::strtoul(word.c_str() + 1, nullptr, 10);
        auto itr = _packs.find(owner->GetGUID());
        if (itr == _packs.end())
        {
            _packs[owner->GetGUID()] = FindPacks(owner, 70.0f);
            itr = _packs.find(owner->GetGUID());
        }
        if (index < 1 || index > itr->second.size())
            return false;
        pack = itr->second[index - 1];
        return true;
    }

    // ------------------------------------------------------------------ parsing

    Player* Plans::BotFor(std::vector<Player*> const& bots, std::string const& word) const
    {
        for (Player* bot : bots)
            if (Lower(bot->GetName()) == word)
                return bot;
        if (uint8 classId = ClassWord(word))
            for (Player* bot : bots)
                if (bot->GetClass() == classId)
                    return bot;
        return nullptr;
    }

    // one step of the order form: "pull p1, jaina cc e5" | "pull next" | "rest" | "rest 80" | "ready" | "wait 10"
    bool Plans::ParseStep(Player* commander, std::vector<Player*> const& bots, std::string const& text, std::vector<Step>& steps)
    {
        std::vector<std::string> words = Words(Lower(text));
        if (words.empty())
            return false;
        std::string const& verb = words[0];
        if (verb == "pull" || verb == "attack" || verb == "kill" || verb == "clear")
        {
            Step step;
            step.Type = Step::Kind::Pull;
            std::string ref = words.size() > 1 ? words[1] : "target";
            Pack pack;
            if (ref == "next" || ref == "other" || ref == "another")
                step.NextPack = true;
            else if (ResolvePack(commander, ref, pack))
                step.PackMembers = pack.Members;
            else if (Unit* unit = UnitRef(commander, ref))
                step.Target = unit->GetGUID();
            else
                step.NextPack = true;
            // "<bot> cc <ref>" clauses
            for (size_t i = 2; i + 2 < words.size() + 1; ++i)
            {
                if (i + 1 >= words.size())
                    break;
                bool ccVerb = std::find_if(std::begin(CcVerbs), std::end(CcVerbs), [&](char const* v) { return words[i] == v; }) != std::end(CcVerbs);
                if (!ccVerb)
                    continue;
                Player* bot = BotFor(bots, words[i - 1]);
                Unit* enemy = UnitRef(commander, words[i + 1]);
                if (bot && enemy)
                    step.Control.emplace_back(bot->GetGUID(), enemy->GetGUID());
            }
            steps.push_back(step);
            return true;
        }
        if (verb == "rest" || verb == "drink" || verb == "eat" || verb == "regen")
        {
            Step step;
            step.Type = Step::Kind::Rest;
            if (words.size() > 1 && std::isdigit(static_cast<unsigned char>(words[1][0])))
                step.Health = step.Mana = uint8(std::clamp<unsigned long>(std::strtoul(words[1].c_str(), nullptr, 10), 10, 100));
            steps.push_back(step);
            return true;
        }
        if (verb == "ready")
        {
            steps.push_back({ Step::Kind::Ready });
            return true;
        }
        if (verb == "wait" && words.size() > 1 && std::isdigit(static_cast<unsigned char>(words[1][0])))
        {
            Step step;
            step.Type = Step::Kind::Wait;
            step.Ms = uint32(std::clamp<unsigned long>(std::strtoul(words[1].c_str(), nullptr, 10), 1, 300)) * 1000;
            steps.push_back(step);
            return true;
        }
        return false;
    }

    bool Plans::StartOrder(Player* commander, std::vector<Player*> const& bots, std::string const& text, bool quiet)
    {
        std::string normalized = text;
        for (std::string sep : { std::string(" then "), std::string(" > "), std::string("->") })
            for (size_t at = normalized.find(sep); at != std::string::npos; at = normalized.find(sep))
                normalized.replace(at, sep.size(), ";");
        std::vector<Step> steps;
        std::istringstream in(normalized);
        std::string part;
        while (std::getline(in, part, ';'))
            ParseStep(commander, bots, part, steps);
        return Start(commander, std::move(steps), quiet);
    }

    // plain English: "attack this pack, then wait for full mana and then we attack the other pack when we have full health
    // and mana and the team confirmed they're ready", "when we pull this pack I want the mage to polymorph this mob"
    bool Plans::StartNatural(Player* commander, std::vector<Player*> const& bots, std::string const& text, bool quiet)
    {
        std::string lower = " " + Lower(text) + " ";
        static char const* const pullVerbs[] = { "attack", "pull", "kill", "clear", "take on", "fight", "engage", "go for", "hit" };
        bool hasPull = false;
        for (char const* v : pullVerbs)
            hasPull = hasPull || Has(lower, { v });
        bool hasCc = false;
        for (char const* v : CcVerbs)
            hasCc = hasCc || Has(lower, { v });
        bool sequence = Has(lower, { "then", "after that", "afterwards", "once", "when", "until", "before" });
        if (!hasPull || !(sequence || hasCc))
            return false;

        // clauses, in order
        for (std::string sep : { std::string(" and then "), std::string(", then "), std::string(" then "), std::string(" after that "),
                std::string(" afterwards "), std::string(";"), std::string(". ") })
            for (size_t at = lower.find(sep); at != std::string::npos; at = lower.find(sep))
                lower.replace(at, sep.size(), "\n");

        std::vector<Step> steps;
        std::vector<std::pair<ObjectGuid, ObjectGuid>> pendingControl;
        std::istringstream in(lower);
        std::string clause;
        while (std::getline(in, clause))
        {
            clause = " " + clause + " ";
            bool pull = false;
            for (char const* v : pullVerbs)
                pull = pull || Has(clause, { v });
            bool rest = Has(clause, { "full mana", "full health", "drink", "rest", "regen", "recover", "mana up", "eat" });
            bool ready = Has(clause, { "ready" });
            uint32 seconds = 0;
            {
                std::vector<std::string> w = Words(clause);
                for (size_t i = 0; i + 1 < w.size(); ++i)
                    if (w[i] == "wait" && std::isdigit(static_cast<unsigned char>(w[i + 1][0])))
                        seconds = std::strtoul(w[i + 1].c_str(), nullptr, 10);
            }

            // crowd control: "<name|the mage> ... polymorph|sheep|cc ... <this mob|target|marker|e#>"
            std::vector<std::string> w = Words(clause);
            for (size_t i = 0; i < w.size(); ++i)
            {
                bool ccVerb = std::find_if(std::begin(CcVerbs), std::end(CcVerbs), [&](char const* v) { return w[i] == v; }) != std::end(CcVerbs);
                if (!ccVerb)
                    continue;
                Player* bot = nullptr;
                for (size_t j = 0; j < i && !bot; ++j)
                    bot = BotFor(bots, w[j]);
                if (!bot)
                    for (Player* candidate : bots)
                        if (CompanionAI* ai = sCompanionMgr->GetAI(candidate); ai && ai->GetClassAI() && !bot)
                            if (Unit* target = CommanderTarget(commander); target && ai->GetClassAI()->CanCrowdControl(target))
                                bot = candidate;
                Unit* enemy = nullptr;
                for (size_t j = i + 1; j < w.size() && !enemy; ++j)
                    if (Icon(w[j]) >= 0 || CompanionLLM::IsEnemyRef(w[j]))
                        enemy = UnitRef(commander, w[j]);
                if (!enemy)
                    enemy = CommanderTarget(commander);
                if (bot && enemy)
                    pendingControl.emplace_back(bot->GetGUID(), enemy->GetGUID());
            }

            // conditions before the pull they gate ("we attack the other pack when we have full mana and everyone is ready")
            if (rest)
            {
                Step step;
                step.Type = Step::Kind::Rest;
                bool health = Has(clause, { "health", "hp" });
                bool mana = Has(clause, { "mana" });
                step.Health = (health || !mana) ? 100 : 0;
                step.Mana = (mana || !health) ? 100 : 0;
                if (!steps.empty() && steps.back().Type == Step::Kind::Rest)
                {
                    steps.back().Health = std::max(steps.back().Health, step.Health);
                    steps.back().Mana = std::max(steps.back().Mana, step.Mana);
                }
                else
                    steps.push_back(step);
            }
            if (ready)
                steps.push_back({ Step::Kind::Ready });
            if (seconds)
            {
                Step step;
                step.Type = Step::Kind::Wait;
                step.Ms = std::min<uint32>(seconds, 300) * 1000;
                steps.push_back(step);
            }
            if (pull)
            {
                Step step;
                step.Type = Step::Kind::Pull;
                std::vector<std::string> cw = Words(clause);
                Pack pack;
                bool done = false;
                for (std::string const& word : cw)
                {
                    if (word == "other" || word == "next" || word == "another" || word == "second")
                    {
                        step.NextPack = true;
                        done = true;
                        break;
                    }
                    if (ResolvePack(commander, word, pack))
                    {
                        step.PackMembers = pack.Members;
                        done = true;
                        break;
                    }
                    if (Icon(word) >= 0 || CompanionLLM::IsEnemyRef(word))
                        if (Unit* unit = UnitRef(commander, word))
                        {
                            step.Target = unit->GetGUID();
                            done = true;
                            break;
                        }
                }
                if (!done)
                {
                    if (Unit* target = CommanderTarget(commander))
                        step.Target = target->GetGUID();
                    else
                        step.NextPack = true;
                }
                step.Control = std::move(pendingControl);
                pendingControl.clear();
                steps.push_back(step);
            }
        }
        // a crowd control said after its pull belongs to that pull
        if (!pendingControl.empty())
            for (auto itr = steps.rbegin(); itr != steps.rend(); ++itr)
                if (itr->Type == Step::Kind::Pull)
                {
                    itr->Control.insert(itr->Control.end(), pendingControl.begin(), pendingControl.end());
                    pendingControl.clear();
                    break;
                }

        bool controlled = std::any_of(steps.begin(), steps.end(), [](Step const& s) { return !s.Control.empty(); });
        if (steps.size() < 2 && !controlled)
            return false;
        return Start(commander, std::move(steps), quiet);
    }

    bool Plans::Start(Player* commander, std::vector<Step> steps, bool quiet)
    {
        if (steps.empty())
            return false;
        ObjectGuid owner = commander->GetGUID();
        // the first pull's pack is the one targeted now
        for (Step& step : steps)
            if (step.Type == Step::Kind::Pull && step.PackMembers.empty() && !step.Target.IsEmpty())
            {
                for (Pack const& pack : FindPacks(commander, 80.0f))
                    if (std::find(pack.Members.begin(), pack.Members.end(), step.Target) != pack.Members.end())
                        step.PackMembers = pack.Members;
                if (step.PackMembers.empty())
                    step.PackMembers.push_back(step.Target);
            }
        Plan plan;
        plan.Owner = owner;
        plan.Steps = std::move(steps);
        _plans[owner] = std::move(plan);
        if (!quiet)
            Announce(commander, "Plan: " + Describe(owner) + ".");
        return true;
    }

    void Plans::Cancel(ObjectGuid owner, bool announce)
    {
        auto itr = _plans.find(owner);
        if (itr == _plans.end())
            return;
        _plans.erase(itr);
        if (announce)
            if (Player* player = ObjectAccessor::FindConnectedPlayer(owner))
                Announce(player, "Plan cancelled.");
    }

    std::string Plans::StepText(Step const& step, Player* owner) const
    {
        auto name = [&](ObjectGuid guid) -> std::string
        {
            if (owner)
                if (Unit* unit = ObjectAccessor::GetUnit(*owner, guid))
                    return unit->GetName();
            return "it";
        };
        switch (step.Type)
        {
            case Step::Kind::Pull:
            {
                std::string text;
                if (step.NextPack)
                    text = "pull the next pack";
                else
                {
                    ObjectGuid first = !step.Target.IsEmpty() ? step.Target : (step.PackMembers.empty() ? ObjectGuid::Empty : step.PackMembers.front());
                    text = "pull " + name(first);
                    if (step.PackMembers.size() > 1)
                        text += "'s pack (" + std::to_string(step.PackMembers.size()) + ")";
                }
                for (auto const& [bot, enemy] : step.Control)
                    text += ", " + name(bot) + " controls " + name(enemy);
                return text;
            }
            case Step::Kind::Rest:
                if (step.Health && step.Mana)
                    return step.Health == 100 && step.Mana == 100 ? "rest to full health and mana" : "rest to " + std::to_string(step.Health) + "%";
                return step.Mana ? "drink to full mana" : "eat to full health";
            case Step::Kind::Ready:
                return "wait until everyone says ready";
            case Step::Kind::Wait:
                return "wait " + std::to_string(step.Ms / 1000) + " seconds";
        }
        return "";
    }

    std::string Plans::Describe(ObjectGuid owner) const
    {
        auto itr = _plans.find(owner);
        if (itr == _plans.end())
            return "no plan";
        Player* player = ObjectAccessor::FindConnectedPlayer(owner);
        std::string out;
        for (size_t i = itr->second.Current; i < itr->second.Steps.size(); ++i)
            out += (out.empty() ? "" : ", then ") + StepText(itr->second.Steps[i], player);
        return out;
    }

    void Plans::Announce(Player* owner, std::string const& text, Player* speaker)
    {
        if (!speaker)
        {
            std::vector<Player*> bots = sCompanionMgr->GetBotsOf(owner->GetGUID());
            if (bots.empty())
                return;
            speaker = bots.front();
        }
        if (CompanionAI* ai = sCompanionMgr->GetAI(speaker))
            ai->Say(text);
    }

    // ------------------------------------------------------------------ carrying it out

    bool Plans::OnChat(Player* player, std::string const& message)
    {
        std::string lower = Lower(message);
        // the owner calls it off or asks for it
        auto own = _plans.find(player->GetGUID());
        if (own != _plans.end())
        {
            if (Has(lower, { "cancel plan", "stop plan", "abort plan", "plan off", "forget the plan", "cancel the plan", "scrap that", "abort" })
                || lower == "cancel")
            {
                Cancel(player->GetGUID(), true);
                return true;
            }
            if (lower == "plan" || lower == "plan?" || lower == "what's the plan" || lower == "whats the plan")
            {
                Announce(player, "Plan: " + Describe(player->GetGUID()) + ".");
                return true;
            }
        }
        if (!ReadyWord(message))
            return false;
        bool used = false;
        for (auto& [owner, plan] : _plans)
        {
            if (plan.Current >= plan.Steps.size() || plan.Steps[plan.Current].Type != Step::Kind::Ready)
                continue;
            Player* ownerPlayer = ObjectAccessor::FindConnectedPlayer(owner);
            if (ownerPlayer && (ownerPlayer == player || (ownerPlayer->GetGroup() && ownerPlayer->GetGroup()->IsMember(player->GetGUID()))))
            {
                plan.Ready.insert(player->GetGUID());
                used = true;
            }
        }
        return used;
    }

    void Plans::StartStep(Plan& plan, Player* owner)
    {
        Step& step = plan.Steps[plan.Current];
        plan.StepStarted = true;
        plan.StepMs = 0;
        plan.QuietMs = 0;
        plan.RefreshMs = 0;
        plan.Engaged = false;
        plan.Released = false;
        plan.Ready.clear();
        std::vector<Player*> bots = sCompanionMgr->GetBotsOf(owner->GetGUID());

        switch (step.Type)
        {
            case Step::Kind::Pull:
            {
                if (step.NextPack || step.PackMembers.empty())
                {
                    std::vector<Pack> packs = FindPacks(owner, 70.0f);
                    if (!packs.empty())
                        step.PackMembers = packs.front().Members;
                }
                std::unordered_set<ObjectGuid> controlled;
                for (auto const& [bot, enemy] : step.Control)
                    controlled.insert(enemy);
                ObjectGuid pullTarget = (!step.Target.IsEmpty() && !controlled.count(step.Target)) ? step.Target : ObjectGuid::Empty;
                for (ObjectGuid const& member : step.PackMembers)
                    if (pullTarget.IsEmpty() && !controlled.count(member))
                        if (Creature* mob = ObjectAccessor::GetCreature(*owner, member); mob && mob->IsAlive())
                            pullTarget = member;
                Creature* target = pullTarget.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*owner, pullTarget);
                if (!target)
                {
                    Announce(owner, "Nothing left to pull there.");
                    plan.Current = plan.Steps.size();
                    return;
                }
                if (Group* group = owner->GetGroup())
                {
                    group->SetTargetIcon(ICON_SKULL, target->GetGUID(), owner->GetGUID());
                    if (!step.Control.empty())
                        group->SetTargetIcon(ICON_MOON, step.Control.front().second, owner->GetGUID());
                }
                for (auto const& [botGuid, enemy] : step.Control)
                    if (CompanionAI* ai = sCompanionMgr->GetAI(botGuid))
                    {
                        ai->OrderCrowdControl(enemy);
                        if (Unit* unit = ObjectAccessor::GetUnit(*owner, enemy))
                            ai->Say("I've got " + unit->GetName() + ".");
                    }
                Player* tank = nullptr;
                for (Player* bot : bots)
                    if (CompanionAI* ai = sCompanionMgr->GetAI(bot); ai && ai->GetEffectiveRole() == Role::Tank)
                        tank = bot;
                if (tank)
                {
                    sCompanionMgr->GetAI(tank)->OrderAttack(target->GetGUID(), true);
                    Announce(owner, "Pulling " + target->GetName() + ".", tank);
                }
                else
                    for (Player* bot : bots)
                        if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                            ai->OrderAttack(target->GetGUID(), false);
                step.Target = target->GetGUID();
                break;
            }
            case Step::Kind::Rest:
            {
                for (Player* bot : bots)
                    if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                        ai->OrderRest();
                Announce(owner, step.Mana && !step.Health ? "Drinking up before the next one." : "Resting up before the next one.");
                break;
            }
            case Step::Kind::Ready:
                Announce(owner, "Say ready (or r) when you're set.");
                break;
            case Step::Kind::Wait:
                break;
        }
    }

    bool Plans::StepDone(Plan& plan, Player* owner, uint32 diff)
    {
        Step& step = plan.Steps[plan.Current];
        plan.StepMs += diff;
        std::vector<Player*> bots = sCompanionMgr->GetBotsOf(owner->GetGUID());
        bool anyFighting = owner->IsInCombat();
        for (Player* bot : bots)
            anyFighting = anyFighting || bot->IsInCombat();

        switch (step.Type)
        {
            case Step::Kind::Pull:
            {
                std::unordered_set<ObjectGuid> controlled;
                for (auto const& [bot, enemy] : step.Control)
                    controlled.insert(enemy);
                std::vector<Creature*> alive, free;
                bool engaged = false;
                for (ObjectGuid const& member : step.PackMembers)
                    if (Creature* mob = ObjectAccessor::GetCreature(*owner, member); mob && mob->IsAlive())
                    {
                        alive.push_back(mob);
                        if (!controlled.count(member))
                            free.push_back(mob);
                        engaged = engaged || mob->IsInCombat();
                    }
                plan.Engaged = plan.Engaged || engaged;
                if (alive.empty())
                    return true;
                // the controlled ones are last: once the rest is down, break the control and kill them
                if (plan.Engaged && free.empty() && !plan.Released)
                {
                    plan.Released = true;
                    if (Group* group = owner->GetGroup())
                        group->SetTargetIcon(ICON_MOON, ObjectGuid::Empty, owner->GetGUID());
                    for (Player* bot : bots)
                        if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                        {
                            ai->ReleaseCrowdControl();
                            ai->OrderAttack(alive.front()->GetGUID(), false);
                        }
                }
                // keep the control going while the others die
                plan.RefreshMs += diff;
                if (!plan.Released && plan.RefreshMs >= 15000)
                {
                    plan.RefreshMs = 0;
                    for (auto const& [botGuid, enemy] : step.Control)
                        if (CompanionAI* ai = sCompanionMgr->GetAI(botGuid))
                            ai->OrderCrowdControl(enemy);
                }
                if (plan.Engaged && !anyFighting && !engaged)
                {
                    plan.QuietMs += diff;
                    return plan.QuietMs >= 4000;    // they reset or ran: on with the plan
                }
                plan.QuietMs = 0;
                if (!plan.Engaged && plan.StepMs >= 45000)
                {
                    Announce(owner, "Can't get that pull going, plan off.");
                    plan.Current = plan.Steps.size();
                }
                else if (!plan.Engaged && plan.StepMs % 15000 < diff)
                    for (Player* bot : bots)
                        if (CompanionAI* ai = sCompanionMgr->GetAI(bot); ai && ai->GetEffectiveRole() == Role::Tank)
                            ai->OrderAttack(step.Target, true);
                return false;
            }
            case Step::Kind::Rest:
            {
                if (anyFighting)
                    return false;
                std::vector<Player*> members = bots;
                members.push_back(owner);
                if (Group* group = owner->GetGroup())
                    for (GroupReference const& ref : group->GetMembers())
                        if (Player* member = ref.GetSource(); member && std::find(members.begin(), members.end(), member) == members.end())
                            members.push_back(member);
                bool full = true;
                for (Player* member : members)
                {
                    if (!member->IsAlive() || member->GetMap() != owner->GetMap())
                        continue;
                    bool low = member->GetHealthPct() + 1.0f < step.Health
                        || (member->GetMaxPower(POWER_MANA) > 0 && member->GetPowerPct(POWER_MANA) + 1.0f < step.Mana);
                    if (low)
                    {
                        full = false;
                        if (plan.StepMs % 8000 < diff)
                            if (CompanionAI* ai = sCompanionMgr->GetAI(member))
                                ai->OrderRest();
                    }
                }
                return full;
            }
            case Step::Kind::Ready:
            {
                if (anyFighting)
                    return false;
                if (Group* group = owner->GetGroup())
                {
                    for (GroupReference const& ref : group->GetMembers())
                        if (Player* member = ref.GetSource())
                            if (!sCompanionMgr->IsBotGuid(member->GetGUID()) && member->IsAlive() && member->GetMap() == owner->GetMap()
                                && !plan.Ready.count(member->GetGUID()))
                                return false;
                    return true;
                }
                return plan.Ready.count(owner->GetGUID()) != 0;
            }
            case Step::Kind::Wait:
                return !anyFighting && plan.StepMs >= step.Ms;
        }
        return true;
    }

    void Plans::Update(uint32 diff)
    {
        _timer += diff;
        if (_timer < 250)
            return;
        uint32 const elapsed = _timer;
        _timer = 0;
        for (auto itr = _plans.begin(); itr != _plans.end();)
        {
            Plan& plan = itr->second;
            Player* owner = ObjectAccessor::FindPlayer(plan.Owner);
            if (!owner || !owner->IsAlive() || plan.Current >= plan.Steps.size())
            {
                if (owner && !owner->IsAlive())
                    Announce(owner, "We lost that one, plan off.");
                itr = _plans.erase(itr);
                continue;
            }
            if (!plan.StepStarted)
                StartStep(plan, owner);
            if (plan.Current < plan.Steps.size() && StepDone(plan, owner, elapsed))
            {
                ++plan.Current;
                plan.StepStarted = false;
                if (plan.Current >= plan.Steps.size())
                    Announce(owner, "That's the plan done.");
            }
            ++itr;
        }
    }
}
