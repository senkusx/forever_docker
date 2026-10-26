/*
 * Companions: ".bot" commands, chat orders and the script hooks that drive the module
 */

#include "CompanionCommands.h"
#include "CompanionAdmin.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CompanionAI.h"
#include "CompanionConfig.h"
#include "CompanionFactory.h"
#include "CompanionLLM.h"
#include "CompanionMechanics.h"
#include "CompanionMgr.h"
#include "CompanionScaling.h"
#include "Group.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Language.h"
#include "MotionMaster.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include <sstream>

using namespace Trinity::ChatCommands;

namespace Companions
{
    namespace
    {
        // every player may use ".bot": the permission of ".save", which the player role always has
        constexpr rbac::RBACPermissions PERM_BOT = rbac::RBAC_PERM_COMMAND_SAVE;

        std::vector<std::string> Split(std::string const& text)
        {
            std::vector<std::string> words;
            std::istringstream ss(text);
            std::string word;
            while (ss >> word)
                words.push_back(word);
            return words;
        }

        std::string Join(std::vector<std::string> const& words, size_t from)
        {
            std::string out;
            for (size_t i = from; i < words.size(); ++i)
                out += (out.empty() ? "" : " ") + words[i];
            return out;
        }

        uint8 ParseClass(std::string const& text)
        {
            std::string t = ToLower(text);
            if (t == "warrior") return CLASS_WARRIOR;
            if (t == "paladin" || t == "pala") return CLASS_PALADIN;
            if (t == "hunter") return CLASS_HUNTER;
            if (t == "rogue") return CLASS_ROGUE;
            if (t == "priest") return CLASS_PRIEST;
            if (t == "shaman") return CLASS_SHAMAN;
            if (t == "mage") return CLASS_MAGE;
            if (t == "warlock" || t == "lock") return CLASS_WARLOCK;
            if (t == "druid") return CLASS_DRUID;
            return 0;
        }

        Role DefaultRoleFor(uint8 classId)
        {
            switch (classId)
            {
                case CLASS_WARRIOR: return Role::Tank;
                case CLASS_PRIEST:  return Role::Healer;
                default:            return DpsRoleForClass(classId);
            }
        }

        // the bots of this owner matching a name, or all of them
        std::vector<Player*> SelectBots(Player* owner, std::string const& name)
        {
            std::vector<Player*> bots = sCompanionMgr->GetBotsOf(owner->GetGUID());
            if (name.empty() || ToLower(name) == "all")
                return bots;
            std::vector<Player*> selected;
            std::string lower = ToLower(name);
            for (Player* bot : bots)
                if (ToLower(bot->GetName()) == lower)
                    selected.push_back(bot);
            return selected;
        }

        // a group member named in an order ("heal Jaina", "protect me")
        Player* NamedMember(Player* commander, std::string const& name)
        {
            if (name == "me" || name == "myself")
                return commander;
            if (Group* group = commander->GetGroup())
                for (GroupReference const& ref : group->GetMembers())
                    if (Player* member = ref.GetSource())
                        if (ToLower(member->GetName()) == name)
                            return member;
            return nullptr;
        }

        int32 IconIndex(std::string const& word)
        {
            static std::pair<char const*, int32> const icons[] = { { "star", 0 }, { "circle", 1 }, { "diamond", 2 }, { "triangle", 3 },
                { "moon", 4 }, { "square", 5 }, { "cross", 6 }, { "x", 6 }, { "skull", 7 } };
            for (auto const& [name, index] : icons)
                if (word == name)
                    return index;
            return -1;
        }

        // "skull" (a raid marker) or "e3" (an enemy as the AI brain numbers them)
        Unit* TargetRef(Player* commander, std::string const& word)
        {
            int32 icon = IconIndex(word);
            if (icon >= 0)
            {
                Group* group = commander->GetGroup();
                ObjectGuid marked = group ? group->GetTargetIcon(uint8(icon)) : ObjectGuid::Empty;
                return marked.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*commander, marked);
            }
            return CompanionLLM::ResolveEnemy(commander, word);
        }

        Unit* CommanderTarget(Player* commander)
        {
            Unit* target = commander->GetSelectedUnit();
            if (target && target != commander && commander->IsValidAttackTarget(target))
                return target;
            if (Unit* victim = commander->GetVictim())
                return victim;
            return nullptr;
        }

        void Ack(std::vector<Player*> const& bots, bool quiet, std::string const& text)
        {
            if (quiet || bots.empty() || !GetConfig().AnnounceInParty)
                return;
            if (CompanionAI* ai = sCompanionMgr->GetAI(bots.front()))
                ai->Say(text);
        }
    }

    // ------------------------------------------------------------------ orders

    std::string Commands::OrdersHelp()
    {
        return "Orders (party chat, whisper, or \".bot order\"; start with a bot name for one bot): follow, stay, come, attack [skull|cross|...], "
               "pull, stop, retreat, assist [name], heal <name|me>, dispel <name|me>, protect <name|me>, cc <marker>, interrupt <marker>, "
               "taunt <marker>, burst, bless <name|me> salvation|might|wisdom|kings|light, rebuff <name|me> [buff], equip <item|it|upgrades>, "
               "spread, stack, normal, passive, aggressive, "
               "role tank|heal|dps, rest, cast <spell> [on me|on target|on <name>], tactics, status, gear, dismiss, help";
    }

    bool Commands::ExecuteOrder(Player* commander, std::vector<Player*> const& bots, std::string const& text, bool quiet)
    {
        std::vector<std::string> words = Split(ToLower(text));
        if (words.empty() || bots.empty())
            return false;

        std::string const& order = words[0];
        auto forEach = [&](auto&& fn)
        {
            for (Player* bot : bots)
                if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                    fn(bot, ai);
        };

        if (order == "follow" || order == "f")
        {
            forEach([](Player*, CompanionAI* ai) { ai->SetStance(Stance::Follow); });
            Ack(bots, quiet, "Following.");
        }
        else if (order == "stay" || order == "hold" || order == "wait")
        {
            forEach([](Player*, CompanionAI* ai) { ai->SetStance(Stance::Stay); });
            Ack(bots, quiet, "Holding here.");
        }
        else if (order == "come" || order == "here" || order == "regroup")
        {
            forEach([](Player*, CompanionAI* ai) { ai->OrderComeHere(); });
            Ack(bots, quiet, "On my way.");
        }
        else if ((order == "attack" || order == "kill" || order == "focus") && words.size() >= 2
            && (IconIndex(words[1]) >= 0 || CompanionLLM::IsEnemyRef(words[1])))
        {
            // "focus skull": the raid marker's target; "kill e2": an enemy as the brain numbers them
            Unit* target = TargetRef(commander, words[1]);
            if (!target || !target->IsAlive())
                return true;
            forEach([&](Player*, CompanionAI* ai) { ai->OrderAttack(target->GetGUID(), false); });
            Ack(bots, quiet, "Focusing " + target->GetName() + ".");
        }
        else if ((order == "cc" || order == "sheep" || order == "control") && words.size() >= 2)
        {
            // keep it out of the fight: whoever of these bots can (polymorph, shackle, banish, hibernate...)
            Unit* target = TargetRef(commander, words[1]);
            if (!target || !target->IsAlive())
                return true;
            bool any = false;
            forEach([&](Player*, CompanionAI* ai)
            {
                if (any || !ai->GetClassAI() || !ai->GetClassAI()->CanCrowdControl(target))
                    return;
                ai->OrderCrowdControl(target->GetGUID());
                any = true;
            });
            if (any)
                Ack(bots, quiet, "I'll keep " + target->GetName() + " out of it.");
        }
        else if (order == "interrupt" && words.size() >= 2)
        {
            Unit* target = TargetRef(commander, words[1]);
            if (!target || !target->IsAlive())
                return true;
            forEach([&](Player*, CompanionAI* ai) { ai->OrderInterrupt(target->GetGUID()); });
            Ack(bots, quiet, "I'll interrupt " + target->GetName() + ".");
        }
        else if (order == "taunt" && words.size() >= 2)
        {
            Unit* target = TargetRef(commander, words[1]);
            if (!target || !target->IsAlive())
                return true;
            forEach([&](Player*, CompanionAI* ai)
            {
                if (ai->GetEffectiveRole() == Role::Tank)
                    ai->OrderAttack(target->GetGUID(), false);
            });
        }
        else if (order == "mark" && words.size() >= 3 && IconIndex(words[1]) >= 0)
        {
            // "mark skull e2": the raid marker the players see too
            Unit* target = TargetRef(commander, words[2]);
            Group* group = commander->GetGroup();
            if (!target || !target->IsAlive() || !group)
                return true;
            group->SetTargetIcon(uint8(IconIndex(words[1])), target->GetGUID(), bots.front()->GetGUID());
        }
        else if (order == "bless" || ((order == "salvation" || order == "salv" || order == "might" || order == "wisdom" || order == "kings"
            || order == "light" || order == "sanctuary") && words.size() >= 3 && words[1] == "on"))
        {
            // "bless me salvation", "bless bob with kings", "salvation on me": that blessing instead of the usual one
            std::string kind;
            Player* member = commander;
            if (order == "bless")
            {
                for (size_t i = 1; i < words.size(); ++i)
                {
                    if (words[i] == "with" || words[i] == "of" || words[i] == "blessing")
                        continue;
                    if (Player* named = i == 1 ? NamedMember(commander, words[i]) : nullptr)
                        member = named;
                    else
                        kind = words[i];
                }
            }
            else
            {
                kind = order;
                member = NamedMember(commander, words[2]);
            }
            if (kind == "salv")
                kind = "salvation";
            static char const* const kinds[] = { "salvation", "might", "wisdom", "kings", "light", "sanctuary" };
            if (!member || std::find_if(std::begin(kinds), std::end(kinds), [&](char const* k) { return kind == k; }) == std::end(kinds))
                return false;
            bool any = false;
            forEach([&](Player* bot, CompanionAI* ai)
            {
                if (bot->GetClass() != CLASS_PALADIN || any)
                    return;
                ai->SetBlessingFor(member->GetGUID(), kind);
                any = true;
                Ack({ bot }, quiet, "Blessing of " + std::string(1, char(std::toupper(kind[0]))) + kind.substr(1) + " on " + member->GetName() + " from now on.");
            });
            if (!any)
                Ack(bots, quiet, "No paladin here to bless you.");
        }
        else if (order == "rebuff" || order == "buff")
        {
            // "rebuff me intellect", "rebuff bob", "buff me": cast again now, even if it is still up
            Player* member = commander;
            size_t first = 1;
            if (words.size() >= 2)
                if (Player* named = NamedMember(commander, words[1]))
                {
                    member = named;
                    first = 2;
                }
            std::string filter = Join(words, first);
            bool any = false;
            forEach([&](Player*, CompanionAI* ai) { any = ai->OrderRebuff(member, filter) || any; });
            // asked of a companion without that buff: one of the others who has it does it
            if (!any && !bots.empty())
                if (CompanionAI* lead = sCompanionMgr->GetAI(bots.front()))
                    for (Player* other : sCompanionMgr->GetBotsOf(lead->GetOwnerGUID()))
                        if (CompanionAI* ai = sCompanionMgr->GetAI(other); ai && std::find(bots.begin(), bots.end(), other) == bots.end()
                            && ai->OrderRebuff(member, filter))
                        {
                            any = true;
                            break;
                        }
            if (!any)
                Ack(bots, quiet, filter.empty() ? "I have no buff for you." : "I can't give " + filter + ".");
            else
                Ack(bots, quiet, "Buffing " + member->GetName() + ".");
        }
        else if (order == "equip" || order == "wear")
        {
            // master loot: the leader hands it over and says "equip it" (or names it, or "equip upgrades")
            std::string what = Join(words, 1);
            forEach([&](Player*, CompanionAI* ai) { ai->Say(ai->OrderEquip(what)); });
        }
        else if (order == "burst" || order == "cooldowns" || order == "bloodlust")
        {
            forEach([](Player*, CompanionAI* ai) { ai->OrderBurst(); });
            Ack(bots, quiet, "Everything I've got!");
        }
        else if (order == "assist" && words.size() >= 2)
        {
            Player* member = NamedMember(commander, words[1]);
            Unit* target = member ? member->GetVictim() : nullptr;
            if (!target)
                target = member ? member->GetSelectedUnit() : nullptr;
            if (!target || !target->IsAlive() || !commander->IsValidAttackTarget(target))
                return true;
            forEach([&](Player*, CompanionAI* ai) { ai->OrderAttack(target->GetGUID(), false); });
            Ack(bots, quiet, "Helping " + member->GetName() + ".");
        }
        else if ((order == "heal" || order == "dispel" || order == "decurse" || order == "cleanse" || order == "protect" || order == "save")
            && words.size() >= 2)
        {
            Player* member = NamedMember(commander, words[1]);
            if (!member)
                return false;
            bool any = false;
            forEach([&](Player*, CompanionAI* ai)
            {
                ClassAI* klass = ai->GetClassAI();
                if (order == "heal")
                {
                    if (klass && klass->CanHeal())
                    {
                        ai->OrderHeal(member->GetGUID());
                        any = true;
                    }
                }
                else if (order == "protect" || order == "save")
                {
                    ai->OrderProtect(member->GetGUID());
                    any = true;
                }
                else
                {
                    ai->OrderDispel(member->GetGUID());
                    any = true;
                }
            });
            if (any)
                Ack(bots, quiet, order == "heal" ? "Healing " + member->GetName() + "!" : (order == "protect" || order == "save") ? "Got " + member->GetName() + "." : "Cleansing " + member->GetName() + ".");
        }
        else if (order == "spread" || order == "stack" || order == "normal")
        {
            float spread = order == "spread" ? 1.8f : order == "stack" ? 0.35f : 1.0f;
            forEach([spread](Player*, CompanionAI* ai) { ai->SetSpread(spread); });
            Ack(bots, quiet, order == "spread" ? "Spreading out." : order == "stack" ? "Stacking up." : "Okay.");
        }
        else if (order == "retreat" || order == "run" || order == "flee" || order == "fallback")
        {
            forEach([](Player*, CompanionAI* ai) { ai->OrderRetreat(); });
            Ack(bots, quiet, "Falling back!");
        }
        else if (order == "attack" || order == "kill" || order == "focus" || order == "pull")
        {
            Unit* target = CommanderTarget(commander);
            if (!target)
            {
                ChatHandler(commander->GetSession()).SendSysMessage("Companions: target an enemy first.");
                return true;
            }
            bool pull = order == "pull";
            forEach([&](Player*, CompanionAI* ai)
            {
                // a pull is the tank's job: the others wait for the fight to start
                if (!pull || ai->GetEffectiveRole() == Role::Tank)
                    ai->OrderAttack(target->GetGUID(), pull);
            });
            Ack(bots, quiet, std::string(pull ? "Pulling " : "Attacking ") + target->GetName() + ".");
        }
        else if (order == "stop" || order == "halt" || order == "back")
        {
            forEach([](Player*, CompanionAI* ai) { ai->OrderStop(); });
            Ack(bots, quiet, "Stopping.");
        }
        else if (order == "passive")
        {
            forEach([](Player*, CompanionAI* ai) { ai->SetAggression(Aggression::Passive); });
            Ack(bots, quiet, "I won't attack unless you say so.");
        }
        else if (order == "assist" || order == "defensive" || order == "normal")
        {
            forEach([](Player*, CompanionAI* ai) { ai->SetAggression(Aggression::Assist); });
            Ack(bots, quiet, "I'll fight what you fight.");
        }
        else if (order == "aggressive")
        {
            forEach([](Player*, CompanionAI* ai) { ai->SetAggression(Aggression::Aggressive); });
            Ack(bots, quiet, "I'll attack anything hostile nearby.");
        }
        else if (order == "role" && words.size() >= 2)
        {
            bool any = false;
            forEach([&](Player* bot, CompanionAI* ai)
            {
                Role role;
                if (!ParseRole(words[1], role, bot->GetClass()))
                    return;
                ai->SetRole(role);
                if (BotRecord* record = sCompanionMgr->GetRecord(bot->GetGUID()))
                {
                    record->BotRole = role;
                    sCompanionMgr->SaveRecord(*record);
                }
                sCompanionMgr->QueuePrepare(bot->GetGUID(), 0, false);
                ai->Say(std::string("I'm your ") + RoleName(ai->GetEffectiveRole()) + " now.");
                any = true;
            });
            if (!any)
                return false;
        }
        else if (order == "rest" || order == "drink" || order == "eat")
        {
            forEach([](Player*, CompanionAI* ai) { ai->OrderRest(); });
            Ack(bots, quiet, "Taking a break.");
        }
        else if (order == "cast" && words.size() >= 2)
        {
            // "cast flash heal on me", "cast polymorph on target", "cast arcane intellect"
            std::string spell;
            ObjectGuid target;
            size_t i = 1;
            for (; i < words.size() && words[i] != "on"; ++i)
                spell += (spell.empty() ? "" : " ") + words[i];
            if (i + 1 < words.size())
            {
                if (words[i + 1] == "target")
                {
                    if (Unit* selected = commander->GetSelectedUnit())
                        target = selected->GetGUID();
                }
                else if (Player* member = NamedMember(commander, words[i + 1]))
                    target = member->GetGUID();
            }
            forEach([&](Player*, CompanionAI* ai) { ai->OrderCast(spell, target); });
        }
        else if (order == "status" || order == "report" || order == "stats")
        {
            forEach([](Player*, CompanionAI* ai) { ai->Say(ai->StatusLine()); });
        }
        else if (order == "tactics" || order == "strat" || order == "strategy")
        {
            // what the companions know of a boss: the commander's target, else one they fight
            Unit* selected = commander->GetSelectedUnit();
            Mechanics::Encounter const* encounter = selected && selected->ToCreature() ? Mechanics::EncounterFor(selected->GetEntry()) : nullptr;
            for (Player* bot : bots)
                if (CompanionAI* ai = sCompanionMgr->GetAI(bot); ai && !encounter)
                    for (Unit* enemy : ai->View().Enemies)
                        if (Creature* creature = enemy->ToCreature(); creature && !encounter)
                            encounter = Mechanics::EncounterFor(creature->GetEntry());
            if (CompanionAI* ai = sCompanionMgr->GetAI(bots.front()))
                ai->Say(encounter ? std::string(encounter->Name) + ": " + encounter->Note
                                  : "Target the boss and ask again, I know the dungeon and raid bosses.");
        }
        else if (order == "gear" || order == "regear")
        {
            forEach([](Player* bot, CompanionAI*) { sCompanionMgr->QueuePrepare(bot->GetGUID(), 0, true); });
            Ack(bots, quiet, "Checking my gear.");
        }
        else if (order == "dismiss" || order == "leave" || order == "bye")
        {
            forEach([](Player* bot, CompanionAI* ai) { ai->Say("See you."); sCompanionMgr->Dismiss(bot->GetGUID(), false); });
        }
        else if (order == "help")
        {
            ChatHandler(commander->GetSession()).SendSysMessage(OrdersHelp());
        }
        else
            return false;
        return true;
    }

    // ------------------------------------------------------------------ .bot command

    class companions_commandscript : public CommandScript
    {
    public:
        companions_commandscript() : CommandScript("companions_commandscript") { }

        std::span<ChatCommandBuilder const> GetCommands() const override
        {
            static ChatCommandTable testTable =
            {
                { "login",  HandleTestLogin,  rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "logout", HandleTestLogout, rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "create", HandleTestCreate, rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "party",  HandleTestParty,  rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "raid",   HandleTestRaid,   rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "say",    HandleTestSay,    rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "fight",  HandleTestFight,  rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "pull",   HandleTestPull,   rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "spawn",  HandleTestSpawn,  rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "status", HandleTestStatus, rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "go",     HandleTestGo,     rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "near",   HandleTestNear,   rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "order",  HandleTestOrder,  rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "spells", HandleTestSpells, rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "why",    HandleTestWhy,    rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "kill",   HandleTestKill,   rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "give",   HandleTestGive,   rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "equip",  HandleTestEquip,  rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
                { "fire",   HandleTestFire,   rbac::RBAC_PERM_COMMAND_SERVER_SHUTDOWN, Console::Yes },
            };
            static ChatCommandTable botTable =
            {
                { "create",      HandleCreate,      PERM_BOT, Console::No },
                { "party",       HandleParty,       PERM_BOT, Console::No },
                { "raid",        HandleRaid,        PERM_BOT, Console::No },
                { "add",         HandleAdd,         PERM_BOT, Console::No },
                { "dismiss",     HandleDismiss,     PERM_BOT, Console::No },
                { "remove",      HandleDismiss,     PERM_BOT, Console::No },
                { "list",        HandleList,        PERM_BOT, Console::No },
                { "role",        HandleRole,        PERM_BOT, Console::No },
                { "gear",        HandleGear,        PERM_BOT, Console::No },
                { "talents",     HandleTalents,     PERM_BOT, Console::No },
                { "delete",      HandleDelete,      PERM_BOT, Console::No },
                { "llm",         HandleLLM,         PERM_BOT, Console::No },
                { "personality", HandlePersonality, PERM_BOT, Console::No },
                { "order",       HandleOrder,       PERM_BOT, Console::No },
                { "help",        HandleHelp,        PERM_BOT, Console::No },
                { "test",        testTable },
                { "admin",       AdminCommandTable() },
            };
            static ChatCommandTable commandTable =
            {
                { "bot",       botTable },
                { "companion", botTable },
            };
            return commandTable;
        }

        static bool CheckEnabled(ChatHandler* handler)
        {
            if (GetConfig().Enable)
                return true;
            handler->SendSysMessage("Companions are disabled on this server.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // the character is saved asynchronously: it joins when the save is done (messages then go to the owner)
        static bool CreateAndSummon(ChatHandler* handler, Player* owner, uint8 classId, Role role, std::string name)
        {
            std::string error;
            ObjectGuid ownerGuid = owner->GetGUID();
            ObjectGuid guid = sCompanionFactory->CreateCharacter(owner, classId, 0, 2, name, error, [ownerGuid, role, classId](ObjectGuid botGuid, bool saved)
            {
                Player* currentOwner = ObjectAccessor::FindPlayer(ownerGuid);
                if (!saved || !currentOwner)
                    return;
                std::string botName;
                std::string summonError;
                sCharacterCache->GetCharacterNameByGuid(botGuid, botName);
                ChatHandler ownerChat(currentOwner->GetSession());
                if (sCompanionMgr->Summon(currentOwner, botGuid, summonError))
                    ownerChat.PSendSysMessage("Companions: %s the %s joins you.", botName.c_str(), RoleName(role == Role::Auto ? DefaultRoleFor(classId) : role));
                else
                    ownerChat.PSendSysMessage("Companions: %s was created, but %s", botName.c_str(), summonError.c_str());
            });
            if (guid.IsEmpty())
            {
                handler->PSendSysMessage("Companions: %s", error.c_str());
                return false;
            }
            BotRecord record;
            record.Guid = guid;
            record.Owner = owner->GetGUID();
            record.BotRole = role;
            record.Created = true;
            record.UseLLM = GetConfig().LLMDefaultOn;
            sCompanionMgr->AddRecord(record);
            return true;
        }

        // .bot create <class> [role] [name]
        static bool HandleCreate(ChatHandler* handler, std::string className, Optional<std::string> roleOrName, Optional<std::string> name)
        {
            if (!CheckEnabled(handler))
                return true;
            Player* owner = handler->GetPlayer();
            uint8 classId = ParseClass(className);
            if (!classId)
            {
                handler->SendSysMessage("Companions: class must be warrior, paladin, hunter, rogue, priest, shaman, mage, warlock or druid.");
                return true;
            }
            Role role = DefaultRoleFor(classId);
            std::string botName;
            if (roleOrName)
            {
                Role parsed;
                if (ParseRole(*roleOrName, parsed, classId))
                    role = parsed;
                else
                    botName = *roleOrName;
            }
            if (name)
                botName = *name;
            CreateAndSummon(handler, owner, classId, role, botName);
            return true;
        }

        // .bot party [tank|heal|dps]: fill the group up to 5 with what it lacks
        static bool HandleParty(ChatHandler* handler, Optional<std::string> myRole)
        {
            if (!CheckEnabled(handler))
                return true;
            FillParty(handler, handler->GetPlayer(), myRole);
            return true;
        }

        // .bot raid [size] [tank|heal|dps]: fill a raid up to Companions.RaidSize (tanks, healers, damage)
        static bool HandleRaid(ChatHandler* handler, Optional<uint32> size, Optional<std::string> myRole)
        {
            if (!CheckEnabled(handler))
                return true;
            FillParty(handler, handler->GetPlayer(), myRole, size.value_or(GetConfig().RaidSize));
            return true;
        }

        static void FillParty(ChatHandler* handler, Player* owner, Optional<std::string> myRole, uint32 targetSize = 0)
        {
            Role ownerRole = DefaultRoleFor(owner->GetClass());
            if (myRole)
                ParseRole(*myRole, ownerRole, owner->GetClass());
            else if (owner->GetClass() == CLASS_PALADIN || owner->GetClass() == CLASS_DRUID || owner->GetClass() == CLASS_SHAMAN)
                ownerRole = DpsRoleForClass(owner->GetClass());

            uint32 tanks = ownerRole == Role::Tank ? 1 : 0, healers = ownerRole == Role::Healer ? 1 : 0;
            std::vector<uint8> classes { owner->GetClass() };
            uint32 members = 1;
            for (Player* bot : sCompanionMgr->GetBotsOf(owner->GetGUID()))
            {
                ++members;
                classes.push_back(bot->GetClass());
                if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                {
                    tanks += ai->GetEffectiveRole() == Role::Tank ? 1 : 0;
                    healers += ai->GetEffectiveRole() == Role::Healer ? 1 : 0;
                }
            }
            // other real players in the group take a slot; the owner's own companions rejoin when summoned
            if (Group* group = owner->GetGroup())
                for (Group::MemberSlot const& slot : group->GetMemberSlots())
                {
                    if (slot.guid == owner->GetGUID() || sCompanionMgr->IsBotGuid(slot.guid))
                        continue;
                    BotRecord const* record = sCompanionMgr->GetRecord(slot.guid);
                    if (!record || record->Owner != owner->GetGUID())
                        ++members;
                }

            // a 5-man party has one tank and one healer; a raid two tanks and a healer for every five players
            uint32 const size = std::clamp<uint32>(targetSize ? targetSize : MAX_GROUP_SIZE, 2, std::max<uint32>(GetConfig().RaidSize, 2));
            uint32 const tanksWanted = size > 10 ? 2 : 1;
            uint32 const healersWanted = size <= MAX_GROUP_SIZE ? 1 : (size + 4) / 5;
            bool alliance = owner->GetTeam() == ALLIANCE;
            // Forever gives both factions paladins and shamans: the faction's classic one first, the other in raids
            uint8 const hybrid = alliance ? CLASS_PALADIN : CLASS_SHAMAN;
            uint8 const otherHybrid = alliance ? CLASS_SHAMAN : CLASS_PALADIN;
            std::vector<std::pair<uint8, Role>> wanted;
            uint8 const tankClasses[] = { CLASS_WARRIOR, CLASS_PALADIN, CLASS_DRUID };
            for (uint32 i = tanks; i < tanksWanted; ++i)
                wanted.emplace_back(tankClasses[i % 3], Role::Tank);
            uint8 const healClasses[] = { CLASS_PRIEST, CLASS_DRUID, hybrid, CLASS_PRIEST, otherHybrid };
            for (uint32 i = healers; i < healersWanted; ++i)
                wanted.emplace_back(healClasses[i % 5], Role::Healer);
            static uint8 const dpsClasses[] = { CLASS_MAGE, CLASS_ROGUE, CLASS_HUNTER, CLASS_WARLOCK, CLASS_DRUID, CLASS_WARRIOR };
            for (uint8 classId : dpsClasses)
                if (std::find(classes.begin(), classes.end(), classId) == classes.end())
                    wanted.emplace_back(classId, DpsRoleForClass(classId));
            // a shaman or paladin brings what only one faction has
            wanted.emplace_back(hybrid, Role::MeleeDps);
            // a raid: more of every damage dealer
            for (uint32 i = 0; wanted.size() < size; ++i)
            {
                uint8 classId = i % 8 == 6 ? otherHybrid : i % 8 == 7 ? hybrid : dpsClasses[i % 8];
                bool isHybrid = classId == CLASS_PALADIN || classId == CLASS_SHAMAN;
                wanted.emplace_back(classId, isHybrid ? Role::MeleeDps : DpsRoleForClass(classId));
            }

            uint32 free = members < size ? size - members : 0;
            free = std::min(free, GetConfig().MaxPerPlayer - std::min(GetConfig().MaxPerPlayer, sCompanionMgr->CountBotsOf(owner->GetGUID())));
            uint32 const serverFree = GetConfig().MaxTotal - std::min(GetConfig().MaxTotal, sCompanionMgr->CountAll());
            if (!serverFree)
            {
                handler->SendSysMessage("Companions: the server has reached its companion limit.");
                return;
            }
            free = std::min(free, serverFree);
            if (!free)
            {
                handler->SendSysMessage(members >= size ? "Companions: your group is already full." : "Companions: you have as many companions as you may.");
                return;
            }

            // reuse companions you already have before making new ones
            for (BotRecord const* record : sCompanionMgr->GetRecordsOf(owner->GetGUID()))
            {
                if (!free || !record->Created || sCompanionMgr->IsBotGuid(record->Guid) || sCompanionMgr->IsLoggingIn(record->Guid))
                    continue;
                CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(record->Guid);
                if (!cache)
                    continue;
                // same class and the same kind of job: a damage paladin does not fill a healer slot
                Role had = record->BotRole == Role::Auto ? DefaultRoleFor(cache->Class) : record->BotRole;
                auto job = [](Role role) { return role == Role::Tank || role == Role::Healer ? role : Role::MeleeDps; };
                auto want = std::find_if(wanted.begin(), wanted.end(), [&](auto const& w) { return w.first == cache->Class && job(w.second) == job(had); });
                if (want == wanted.end())
                    continue;
                std::string error;
                if (sCompanionMgr->Summon(owner, record->Guid, error))
                {
                    handler->PSendSysMessage("Companions: %s joins you.", cache->Name.c_str());
                    wanted.erase(want);
                    --free;
                }
            }

            for (auto const& [classId, role] : wanted)
            {
                if (!free)
                    break;
                if (sCompanionFactory->RacesFor(owner->GetTeam(), classId).empty())
                    continue;
                if (CreateAndSummon(handler, owner, classId, role, ""))
                    --free;
            }
        }

        // .bot add <name|all>
        static bool HandleAdd(ChatHandler* handler, std::string name)
        {
            if (!CheckEnabled(handler))
                return true;
            Player* owner = handler->GetPlayer();
            std::vector<ObjectGuid> guids;
            if (ToLower(name) == "all")
            {
                for (BotRecord const* record : sCompanionMgr->GetRecordsOf(owner->GetGUID()))
                    guids.push_back(record->Guid);
            }
            else
            {
                if (!normalizePlayerName(name))
                {
                    handler->SendSysMessage("Companions: bad name.");
                    return true;
                }
                ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
                if (guid.IsEmpty())
                {
                    handler->PSendSysMessage("Companions: no character named %s.", name.c_str());
                    return true;
                }
                guids.push_back(guid);
            }

            for (ObjectGuid guid : guids)
            {
                std::string error, botName;
                sCharacterCache->GetCharacterNameByGuid(guid, botName);
                if (sCompanionMgr->Summon(owner, guid, error))
                    handler->PSendSysMessage("Companions: %s is on the way.", botName.c_str());
                else if (guids.size() == 1)
                    handler->PSendSysMessage("Companions: %s", error.c_str());
            }
            return true;
        }

        // .bot dismiss <name|all>
        static bool HandleDismiss(ChatHandler* handler, Optional<std::string> name)
        {
            Player* owner = handler->GetPlayer();
            std::vector<Player*> bots = SelectBots(owner, name.value_or("all"));
            if (bots.empty())
            {
                handler->SendSysMessage("Companions: no such companion with you.");
                return true;
            }
            for (Player* bot : bots)
            {
                sCompanionMgr->Dismiss(bot->GetGUID(), false);
                handler->PSendSysMessage("Companions: %s leaves.", bot->GetName().c_str());
            }
            return true;
        }

        static bool HandleList(ChatHandler* handler)
        {
            Player* owner = handler->GetPlayer();
            std::vector<BotRecord const*> records = sCompanionMgr->GetRecordsOf(owner->GetGUID());
            if (records.empty())
            {
                handler->SendSysMessage("Companions: none yet. Try \".bot party\" or \".bot create mage\".");
                return true;
            }
            for (BotRecord const* record : records)
            {
                CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(record->Guid);
                if (!cache)
                    continue;
                CompanionAI* ai = sCompanionMgr->GetAI(record->Guid);
                std::string line = ai ? ai->StatusLine() : cache->Name + " (level " + std::to_string(cache->Level) + ", " + RoleName(record->BotRole) + ", resting)";
                if (!record->Created)
                    line += " [your character]";
                handler->SendSysMessage(line);
            }
            return true;
        }

        // .bot role <name|all> <tank|heal|dps|melee|ranged|auto>
        static bool HandleRole(ChatHandler* handler, std::string name, std::string role)
        {
            Player* owner = handler->GetPlayer();
            std::vector<Player*> bots = SelectBots(owner, name);
            if (bots.empty() || !Commands::ExecuteOrder(owner, bots, "role " + role, false))
                handler->SendSysMessage("Companions: usage .bot role <name|all> <tank|heal|dps|melee|ranged|auto>");
            return true;
        }

        static bool HandleGear(ChatHandler* handler, Optional<std::string> name)
        {
            Player* owner = handler->GetPlayer();
            for (Player* bot : SelectBots(owner, name.value_or("all")))
            {
                sCompanionMgr->QueuePrepare(bot->GetGUID(), 0, true);
                handler->PSendSysMessage("Companions: %s: %s", bot->GetName().c_str(), sCompanionFactory->DescribeGear(bot).c_str());
            }
            return true;
        }

        // .bot talents <name> [respec]
        static bool HandleTalents(ChatHandler* handler, std::string name, Optional<std::string> action)
        {
            Player* owner = handler->GetPlayer();
            for (Player* bot : SelectBots(owner, name))
            {
                CompanionAI* ai = sCompanionMgr->GetAI(bot);
                if (action && ToLower(*action) == "respec" && ai)
                    sCompanionFactory->Respec(bot, ai->GetEffectiveRole());
                handler->PSendSysMessage("Companions: %s: %s", bot->GetName().c_str(), sCompanionFactory->DescribeTalents(bot).c_str());
            }
            return true;
        }

        static bool HandleDelete(ChatHandler* handler, std::string name)
        {
            Player* owner = handler->GetPlayer();
            if (!normalizePlayerName(name))
                return false;
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
            std::string error;
            if (guid.IsEmpty() || !sCompanionMgr->DeleteCompanion(owner->GetGUID(), guid, error))
                handler->PSendSysMessage("Companions: %s", error.empty() ? "no such companion." : error.c_str());
            else
                handler->PSendSysMessage("Companions: %s is gone.", name.c_str());
            return true;
        }

        // .bot llm <on|off> [name]
        static bool HandleLLM(ChatHandler* handler, std::string state, Optional<std::string> name)
        {
            Player* owner = handler->GetPlayer();
            bool on = ToLower(state) == "on";
            uint32 changed = 0;
            for (BotRecord const* constRecord : sCompanionMgr->GetRecordsOf(owner->GetGUID()))
            {
                BotRecord* record = sCompanionMgr->GetRecord(constRecord->Guid);
                std::string botName;
                sCharacterCache->GetCharacterNameByGuid(record->Guid, botName);
                if (name && ToLower(*name) != ToLower(botName) && ToLower(*name) != "all")
                    continue;
                record->UseLLM = on;
                sCompanionMgr->SaveRecord(*record);
                ++changed;
            }
            handler->PSendSysMessage("Companions: brain %s for %u companion(s)%s.", on ? "on" : "off", changed,
                GetConfig().LLMEnable ? "" : " (the server has Companions.LLM.Enable = 0)");
            return true;
        }

        // .bot personality <name> <text>
        static bool HandlePersonality(ChatHandler* handler, std::string name, Tail text)
        {
            Player* owner = handler->GetPlayer();
            std::vector<Player*> bots = SelectBots(owner, name);
            for (BotRecord const* constRecord : sCompanionMgr->GetRecordsOf(owner->GetGUID()))
            {
                std::string botName;
                sCharacterCache->GetCharacterNameByGuid(constRecord->Guid, botName);
                if (ToLower(botName) != ToLower(name))
                    continue;
                BotRecord* record = sCompanionMgr->GetRecord(constRecord->Guid);
                record->Personality = std::string(text).substr(0, 250);
                sCompanionMgr->SaveRecord(*record);
                handler->PSendSysMessage("Companions: %s will be: %s", botName.c_str(), record->Personality.c_str());
                return true;
            }
            handler->SendSysMessage("Companions: no such companion.");
            return true;
        }

        // .bot order <text>: an order to all companions
        static bool HandleOrder(ChatHandler* handler, Tail text)
        {
            Player* owner = handler->GetPlayer();
            std::vector<std::string> words = Split(std::string(text));
            std::vector<Player*> bots = sCompanionMgr->GetBotsOf(owner->GetGUID());
            std::string order(text);
            if (!words.empty())
            {
                std::vector<Player*> named = SelectBots(owner, words[0]);
                if (!named.empty() && ToLower(words[0]) != "all")
                {
                    bots = named;
                    order = Join(words, 1);
                }
            }
            if (!Commands::ExecuteOrder(owner, bots, order, false))
                handler->SendSysMessage(Commands::OrdersHelp());
            return true;
        }

        // ---- test harness: a headless stand-in owner, driven from the console or SOAP ----

        static Player* Puppet(ChatHandler* handler, std::string const& name)
        {
            Player* puppet = sCompanionMgr->GetPuppet(name);
            if (!puppet)
                handler->PSendSysMessage("Companions test: %s is not a logged in puppet (.bot test login <name>)", name.c_str());
            return puppet;
        }

        static bool HandleTestLogin(ChatHandler* handler, std::string name)
        {
            if (!normalizePlayerName(name))
                return false;
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
            std::string error;
            if (guid.IsEmpty() || !sCompanionMgr->LoginPuppet(guid, error))
                handler->PSendSysMessage("Companions test: cannot log in %s: %s", name.c_str(), guid.IsEmpty() ? "no such character" : error.c_str());
            else
                handler->PSendSysMessage("Companions test: logging in %s", name.c_str());
            return true;
        }

        static bool HandleTestLogout(ChatHandler* handler, std::string name)
        {
            if (Player* puppet = Puppet(handler, name))
                sCompanionMgr->LogoutPuppet(puppet->GetGUID());
            return true;
        }

        static bool HandleTestCreate(ChatHandler* handler, std::string owner, std::string className, Optional<std::string> role)
        {
            Player* puppet = Puppet(handler, owner);
            uint8 classId = ParseClass(className);
            if (!puppet || !classId)
                return true;
            Role botRole = DefaultRoleFor(classId);
            if (role)
                ParseRole(*role, botRole, classId);
            CreateAndSummon(handler, puppet, classId, botRole, "");
            return true;
        }

        static bool HandleTestParty(ChatHandler* handler, std::string owner, Optional<std::string> myRole)
        {
            if (Player* puppet = Puppet(handler, owner))
                FillParty(handler, puppet, myRole);
            return true;
        }

        static bool HandleTestRaid(ChatHandler* handler, std::string owner, Optional<uint32> size)
        {
            if (Player* puppet = Puppet(handler, owner))
                FillParty(handler, puppet, {}, size.value_or(GetConfig().RaidSize));
            return true;
        }

        // .bot test say <owner> <text>: a party line from the stand-in owner (orders, callouts, talk to the brain)
        static bool HandleTestSay(ChatHandler* handler, std::string owner, Tail text)
        {
            if (Player* puppet = Puppet(handler, owner))
                Commands::OnPartyChat(puppet, std::string(text), puppet->GetGroup());
            return true;
        }

        // .bot test spawn <owner> <entry> [count]: enemies 25 yards in front of the owner (5 minutes), for pack fights
        static bool HandleTestSpawn(ChatHandler* handler, std::string owner, uint32 entry, Optional<uint32> count)
        {
            Player* puppet = Puppet(handler, owner);
            if (!puppet)
                return true;
            uint32 n = std::clamp<uint32>(count.value_or(1), 1, 8);
            for (uint32 i = 0; i < n; ++i)
            {
                Position pos = puppet->GetPosition();
                puppet->MovePosition(pos, 25.0f + frand(-2.0f, 2.0f), frand(-0.4f, 0.4f));
                if (TempSummon* creature = puppet->SummonCreature(entry, pos, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, 5min))
                {
                    creature->SetFacingToObject(puppet);
                    handler->PSendSysMessage("Companions test: %s (level %u) spawned", creature->GetName().c_str(), uint32(creature->GetLevel()));
                }
            }
            return true;
        }

        // .bot test pull <owner>: the owner targets the nearest enemy and says "pull" (the tank goes, the melee walk with it)
        static bool HandleTestPull(ChatHandler* handler, std::string owner)
        {
            Player* puppet = Puppet(handler, owner);
            if (!puppet)
                return true;
            Unit* target = nullptr;
            float best = 100.0f;
            std::list<Unit*> units;
            Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(puppet, puppet, best);
            Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(puppet, units, check);
            Cell::VisitAllObjects(puppet, searcher, best);
            for (Unit* creature : units)
                if (creature->IsAlive() && creature->ToCreature() && puppet->IsValidAttackTarget(creature) && !creature->IsCritter()
                    && !creature->IsInCombat() && puppet->GetDistance(creature) < best)
                {
                    best = puppet->GetDistance(creature);
                    target = creature;
                }
            if (!target)
            {
                handler->SendSysMessage("Companions test: nothing to pull within 100 yards");
                return true;
            }
            puppet->SetSelection(target->GetGUID());
            handler->PSendSysMessage("Companions test: %s pulls %s (level %u, %.0f yards)", owner.c_str(), target->GetName().c_str(), uint32(target->GetLevel()), best);
            Commands::ExecuteOrder(puppet, sCompanionMgr->GetBotsOf(puppet->GetGUID()), "pull", false);
            return true;
        }

        static bool HandleTestFight(ChatHandler* handler, std::string owner)
        {
            Player* puppet = Puppet(handler, owner);
            if (!puppet)
                return true;
            Unit* target = nullptr;
            float best = 80.0f;
            std::list<Unit*> units;
            Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(puppet, puppet, best);
            Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(puppet, units, check);
            Cell::VisitAllObjects(puppet, searcher, best);
            for (Unit* creature : units)
            {
                if (!creature->IsAlive() || !creature->ToCreature() || !puppet->IsValidAttackTarget(creature) || creature->IsCritter())
                    continue;
                float dist = puppet->GetDistance(creature);
                if (dist < best)
                {
                    best = dist;
                    target = creature;
                }
            }
            if (!target)
            {
                handler->PSendSysMessage("Companions test: nothing to fight within 80 yards (%u unfriendly units seen)", uint32(units.size()));
                for (Unit* unit : units)
                {
                    CanSeeOrDetectExtraArgs noDistance;
                    noDistance.DistanceCheck = false;
                    handler->PSendSysMessage("  %s: sees %u sees(no distance) %u samePhase %u sight %.0f %.0f yards", unit->GetName().c_str(),
                        uint32(puppet->CanSeeOrDetect(unit)), uint32(puppet->CanSeeOrDetect(unit, noDistance)), uint32(puppet->InSamePhase(unit)),
                        puppet->GetSightRange(unit), puppet->GetDistance(unit));
                }
                return true;
            }
            puppet->SetSelection(target->GetGUID());
            puppet->Attack(target, true);
            puppet->GetMotionMaster()->MoveChase(target);
            handler->PSendSysMessage("Companions test: %s attacks %s (level %u, %.0f yards)", puppet->GetName().c_str(), target->GetName().c_str(),
                uint32(target->GetLevel()), best);
            return true;
        }

        static bool HandleTestStatus(ChatHandler* handler, std::string owner)
        {
            Player* puppet = Puppet(handler, owner);
            if (!puppet)
                return true;
            handler->PSendSysMessage("%s: level %u, %u%% health, map %u (%.1f %.1f %.1f), %s", puppet->GetName().c_str(), uint32(puppet->GetLevel()),
                uint32(puppet->GetHealthPct()), puppet->GetMapId(), puppet->GetPositionX(), puppet->GetPositionY(), puppet->GetPositionZ(),
                puppet->IsInCombat() ? "in combat" : "idle");
            for (Player* bot : sCompanionMgr->GetBotsOf(puppet->GetGUID()))
            {
                CompanionAI* ai = sCompanionMgr->GetAI(bot);
                Unit* victim = bot->GetVictim();
                handler->PSendSysMessage("  %s | %.1f yards | %s | spells %u | %s", ai ? ai->StatusLine().c_str() : bot->GetName().c_str(),
                    bot->GetDistance(puppet), victim ? victim->GetName().c_str() : "-", ai ? uint32(ai->Spells().Size()) : 0,
                    sCompanionFactory->DescribeGear(bot).c_str());
            }
            return true;
        }

        // creatures around the stand-in: entry, health (raid scaling) and what the mechanics know of them
        static bool HandleTestNear(ChatHandler* handler, std::string owner, Optional<float> range)
        {
            Player* puppet = Puppet(handler, owner);
            if (!puppet)
                return true;
            std::list<Creature*> creatures;
            Trinity::AllWorldObjectsInRange check(puppet, range.value_or(40.0f));
            Trinity::CreatureListSearcher<Trinity::AllWorldObjectsInRange> searcher(puppet, creatures, check);
            Cell::VisitGridObjects(puppet, searcher, range.value_or(40.0f));
            uint32 shown = 0;
            for (Creature* creature : creatures)
            {
                if (creature->IsPet() || creature->IsTotem() || ++shown > 25)
                    continue;
                Spell const* casting = creature->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                if (!casting)
                    casting = creature->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                std::string ruled;
                for (auto const& [spellId, application] : creature->GetAppliedAuras())
                    if (!Mechanics::RulesForSpell(spellId).empty())
                        ruled += " " + std::to_string(spellId);
                handler->PSendSysMessage("  %u %s | %llu/%llu hp | %.1f yards | rules %u | casting %u%s | at %.1f %.1f%s", creature->GetEntry(),
                    creature->GetName().c_str(), (unsigned long long)creature->GetHealth(), (unsigned long long)creature->GetMaxHealth(),
                    creature->GetDistance(puppet), uint32(Mechanics::RulesFor(creature->GetEntry()).size()),
                    casting ? casting->GetSpellInfo()->Id : 0, ruled.empty() ? "" : (" | auras" + ruled).c_str(), creature->GetPositionX(),
                    creature->GetPositionY(), creature->IsAlive() ? "" : " | dead");
            }
            return true;
        }

        static bool HandleTestGo(ChatHandler* handler, std::string owner, uint32 mapId, float x, float y, float z)
        {
            if (Player* puppet = Puppet(handler, owner))
                puppet->TeleportTo(mapId, x, y, z, puppet->GetOrientation());
            return true;
        }

        static bool HandleTestSpells(ChatHandler* handler, std::string botName)
        {
            Player* bot = ObjectAccessor::FindPlayerByName(botName);
            CompanionAI* ai = sCompanionMgr->GetAI(bot);
            if (!ai)
            {
                handler->SendSysMessage("Companions test: no such bot online");
                return true;
            }
            std::string line;
            for (std::string const& name : ai->Spells().Names())
                line += name + ", ";
            handler->PSendSysMessage("%s knows %u spells: %s", bot->GetName().c_str(), uint32(ai->Spells().Size()), line.c_str());
            handler->PSendSysMessage("talents: %s", sCompanionFactory->DescribeTalents(bot).c_str());
            return true;
        }

        // .bot test give <bot> <item>: into its bags, with what the loot roll would decide (Need on an upgrade)
        static bool HandleTestGive(ChatHandler* handler, std::string botName, uint32 itemId)
        {
            Player* bot = ObjectAccessor::FindPlayerByName(botName);
            CompanionAI* ai = sCompanionMgr->GetAI(bot);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!ai || !proto)
                return true;
            ItemPosCountVec dest;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) == EQUIP_ERR_OK)
                bot->StoreNewItem(dest, itemId, true);
            handler->PSendSysMessage("Companions test: %s gets %s - upgrade (would roll Need): %s, score %.1f", bot->GetName().c_str(),
                proto->GetName(LOCALE_enUS), sCompanionFactory->IsUpgrade(bot, proto, ai->GetEffectiveRole()) ? "yes" : "no",
                sCompanionFactory->ScoreTemplate(proto, bot, ai->GetEffectiveRole()));
            return true;
        }

        // .bot test fire <bot> [spell]: a hostile caster drops a ground effect (Flamestrike) under the bot
        static bool HandleTestFire(ChatHandler* handler, std::string botName, Optional<uint32> spellId)
        {
            Player* bot = ObjectAccessor::FindPlayerByName(botName);
            if (!bot || !sCompanionMgr->GetAI(bot))
                return true;
            Position where = bot->GetPosition();
            Position spot = where;
            bot->MovePosition(spot, 20.0f, 0.0f);
            TempSummon* caster = bot->SummonCreature(10477, spot, TEMPSUMMON_TIMED_DESPAWN, 30s);
            if (!caster)
                return true;
            caster->SetReactState(REACT_PASSIVE);
            caster->SetImmuneToPC(true);
            caster->SetFaction(14);     // hostile to all
            caster->CastSpell(where, spellId.value_or(2120), CastSpellExtraArgs(TRIGGERED_FULL_MASK));
            handler->PSendSysMessage("Companions test: fire under %s at %.1f %.1f", bot->GetName().c_str(), where.GetPositionX(), where.GetPositionY());
            return true;
        }

        // .bot test equip <bot> <it|name|upgrades>: what the "equip" order answers
        static bool HandleTestEquip(ChatHandler* handler, std::string botName, Tail what)
        {
            Player* bot = ObjectAccessor::FindPlayerByName(botName);
            if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                handler->PSendSysMessage("Companions test: %s: %s", bot->GetName().c_str(), ai->OrderEquip(std::string(what)).c_str());
            return true;
        }

        static bool HandleTestKill(ChatHandler* handler, std::string botName)
        {
            Player* bot = ObjectAccessor::FindPlayerByName(botName);
            if (!bot || !sCompanionMgr->GetAI(bot) || !bot->IsAlive())
                return true;
            Unit::Kill(bot, bot);
            handler->PSendSysMessage("Companions test: %s is dead", bot->GetName().c_str());
            return true;
        }

        static bool HandleTestWhy(ChatHandler* handler, std::string botName)
        {
            Player* bot = ObjectAccessor::FindPlayerByName(botName);
            CompanionAI* ai = sCompanionMgr->GetAI(bot);
            if (!ai)
                return true;
            GroupView const& view = ai->View();
            handler->PSendSysMessage("%s: members %u enemies %u tank %s owner %s aggression %u stance %u inCombat %u sees-flag %u group %u",
                bot->GetName().c_str(), uint32(view.Members.size()), uint32(view.Enemies.size()), view.Tank ? view.Tank->GetName().c_str() : "-",
                view.Owner ? view.Owner->GetName().c_str() : "-", uint32(ai->GetAggression()), uint32(ai->GetStance()), uint32(bot->IsInCombat()),
                uint32(bot->HasPlayerLocalFlag(PLAYER_LOCAL_FLAG_OVERRIDE_TRANSPORT_SERVER_TIME)), uint32(bot->GetGroup() != nullptr));
            if (view.Tank)
                if (Unit* victim = view.Tank->GetVictim())
                    handler->PSendSysMessage("  tank victim %s: valid %u sees %u dist %.1f", victim->GetName().c_str(), uint32(bot->IsValidAttackTarget(victim)),
                        uint32(bot->CanSeeOrDetect(victim)), bot->GetDistance(victim));
            for (Unit* enemy : view.Enemies)
                handler->PSendSysMessage("  enemy %s victim %s", enemy->GetName().c_str(), enemy->GetVictim() ? enemy->GetVictim()->GetName().c_str() : "-");
            if (view.Owner)
            {
                // where it stands: 0 = in front of the owner, 180 = behind
                float angle = Position::NormalizeOrientation(view.Owner->GetAbsoluteAngle(bot) - view.Owner->GetOrientation());
                if (angle > float(M_PI))
                    angle -= 2.0f * float(M_PI);
                handler->PSendSysMessage("  role %s, at %.0f degrees, %.1f yards from the owner, position %.1f %.1f", RoleName(ai->GetEffectiveRole()),
                    angle * 180.0f / float(M_PI), bot->GetExactDist2d(view.Owner), bot->GetPositionX(), bot->GetPositionY());
            }
            Unit* victim = bot->GetVictim();
            handler->PSendSysMessage("  form %u rage %d, victim %s at %.1f (melee %u), casts: %s", uint32(bot->GetShapeshiftForm()), ai->Rage(),
                victim ? victim->GetName().c_str() : "-", victim ? bot->GetDistance(victim) : 0.0f, uint32(victim && ai->InMelee(victim)),
                ai->RecentCasts().c_str());
            handler->PSendSysMessage("  mechanics: %s", ai->MechanicsLine().c_str());
            return true;
        }

        static bool HandleTestOrder(ChatHandler* handler, std::string owner, Tail text)
        {
            Player* puppet = Puppet(handler, owner);
            if (!puppet)
                return true;
            if (!Commands::ExecuteOrder(puppet, sCompanionMgr->GetBotsOf(puppet->GetGUID()), std::string(text), false))
                handler->SendSysMessage(Commands::OrdersHelp());
            return true;
        }

        static bool HandleHelp(ChatHandler* handler)
        {
            handler->SendSysMessage("Companions: .bot party [tank|heal|dps] | raid [size] [tank|heal|dps] | create <class> [role] [name] | add <name|all> | dismiss [name|all] | list | "
                "role <name|all> <role> | gear [name] | talents <name> [respec] | llm <on|off> [name] | personality <name> <text> | order <text> | delete <name>");
            handler->SendSysMessage(Commands::OrdersHelp());
            return true;
        }
    };

    // ------------------------------------------------------------------ party chat

    // what another player may ask of someone else's companions: help, never their orders
    static bool IsHelpOrder(std::string const& text)
    {
        std::vector<std::string> words = Split(ToLower(text));
        static char const* const help[] = { "heal", "dispel", "decurse", "cleanse", "protect", "save", "assist", "bless", "rebuff", "buff",
            "salvation", "salv", "might", "wisdom", "kings", "light", "sanctuary" };
        return words.size() >= 2 && std::find(std::begin(help), std::end(help), words[0]) != std::end(help);
    }

    void Commands::OnPartyChat(Player* player, std::string const& msg, Group* group)
    {
        if (!group || msg.empty() || msg[0] == '.' || msg[0] == '!')
            return;
        CompanionLLM::RecordChat(group, player->GetName(), msg);

        // the speaker's own companions take any order; the other companions of the group help on request
        std::vector<Player*> own = sCompanionMgr->GetBotsOf(player->GetGUID());
        std::vector<Player*> others;
        for (GroupReference const& ref : group->GetMembers())
            if (Player* member = ref.GetSource())
                if (CompanionAI* ai = sCompanionMgr->GetAI(member))
                    if (ai->GetOwnerGUID() != player->GetGUID())
                        others.push_back(member);
        if (own.empty() && others.empty())
            return;
        std::vector<Player*> all = own;
        all.insert(all.end(), others.begin(), others.end());

        std::vector<std::string> words = Split(msg);
        std::string first = ToLower(words.empty() ? "" : words[0]);
        while (!first.empty() && (first.back() == ',' || first.back() == ':'))
            first.pop_back();

        // "Jaina, stay" / "Jaina stay": one bot
        Player* named = nullptr;
        for (Player* bot : all)
            if (ToLower(bot->GetName()) == first)
                named = bot;

        Player* addressed = named;
        if (named)
        {
            bool mine = std::find(own.begin(), own.end(), named) != own.end();
            std::string rest = Join(words, 1);
            if (!rest.empty() && (mine || IsHelpOrder(rest)) && Commands::ExecuteOrder(player, { named }, rest, false))
                return;
        }
        else
        {
            if (!own.empty() && Commands::ExecuteOrder(player, own, msg, false))
                return;
            if (own.empty() && IsHelpOrder(msg) && Commands::ExecuteOrder(player, others, msg, false))
                return;
            // a bot named anywhere in the line is the one spoken to
            std::string lower = ToLower(msg);
            for (Player* bot : all)
                if (lower.find(ToLower(bot->GetName())) != std::string::npos)
                {
                    addressed = bot;
                    break;
                }
        }

        // talk: the brain hears it for every companion of the group and decides who answers (often nobody)
        Player* owner = !own.empty() ? player : nullptr;
        if (!owner)
            if (CompanionAI* ai = sCompanionMgr->GetAI(addressed ? addressed : all.front()))
                owner = ObjectAccessor::FindPlayer(ai->GetOwnerGUID());
        if (owner && CompanionLLM::OnGroupChat(owner, player, all, msg, addressed))
            return;
        if (addressed)
            CompanionLLM::OnChatToBot(addressed, player, "party", msg);
    }

    // ------------------------------------------------------------------ hooks

    class companions_playerscript : public PlayerScript
    {
    public:
        companions_playerscript() : PlayerScript("companions_playerscript") { }

        void OnLogin(Player* player, bool /*firstLogin*/) override
        {
            if (!player->GetSession()->IsBot())
                sCompanionMgr->OnOwnerLogin(player);
        }

        void OnLogout(Player* player) override
        {
            if (!player->GetSession()->IsBot())
                sCompanionMgr->OnOwnerLogout(player);
        }

        void OnLevelChanged(Player* player, uint8 /*oldLevel*/) override
        {
            if (!player->GetSession()->IsBot())
                sCompanionMgr->OnOwnerLevelChanged(player);
            else if (CompanionAI* ai = sCompanionMgr->GetAI(player))
            {
                ai->RefreshSpells();
                sCompanionMgr->QueuePrepare(player->GetGUID(), 0, false);
                CompanionLLM::OnEvent(player, "levelup", "you reached level " + std::to_string(player->GetLevel()));
            }
        }

        // party chat, after it was shown: orders for the owner's bots, else a question for the brain
        void OnChatSent(Player* player, uint32 type, uint32 /*lang*/, std::string const& msg, Group* group) override
        {
            if (type == CHAT_MSG_PARTY || type == CHAT_MSG_PARTY_LEADER)
                if (!player->GetSession()->IsBot())
                    Commands::OnPartyChat(player, msg, group);
        }

        void OnChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* group) override
        {
            if (type == CHAT_MSG_RAID || type == CHAT_MSG_RAID_LEADER)
                if (!player->GetSession()->IsBot())
                    Commands::OnPartyChat(player, msg, group);
        }

        void OnChatSent(Player* player, uint32 /*type*/, uint32 /*lang*/, std::string const& msg, Player* receiver) override
        {
            if (!receiver || player->GetSession()->IsBot() || !receiver->GetSession()->IsBot())
                return;
            CompanionAI* ai = sCompanionMgr->GetAI(receiver);
            if (!ai)
                return;
            if (ai->GetOwnerGUID() != player->GetGUID())
            {
                if (!CompanionLLM::OnChatToBot(receiver, player, "whisper", msg))
                    receiver->Whisper("I'm adventuring with someone else right now.", LANG_UNIVERSAL, player);
                return;
            }
            if (Commands::ExecuteOrder(player, { receiver }, msg, true))
                return;
            if (!CompanionLLM::OnChatToBot(receiver, player, "whisper", msg))
                receiver->Whisper("I didn't get that. Whisper \"help\" for my orders.", LANG_UNIVERSAL, player);
        }

    };

    class companions_worldscript : public WorldScript
    {
    public:
        companions_worldscript() : WorldScript("companions_worldscript") { }

        void OnConfigLoad(bool /*reload*/) override
        {
            LoadConfig();
        }

        void OnStartup() override
        {
            sCompanionMgr->Initialize();
        }

        void OnUpdate(uint32 diff) override
        {
            sCompanionMgr->Update(diff);
        }

        void OnShutdown() override
        {
            sCompanionMgr->Shutdown();
        }
    };
}

void AddCompanionsScripts()
{
    new Companions::companions_commandscript();
    new Companions::companions_playerscript();
    new Companions::companions_worldscript();
    Companions::AddScalingScripts();
}
