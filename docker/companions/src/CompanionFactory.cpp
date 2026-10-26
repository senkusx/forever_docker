/*
 * Companions: makes bots ready to play
 */

#include "CompanionFactory.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "CharacterPackets.h"
#include "CompanionAI.h"
#include "CompanionConfig.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Item.h"
#include "ItemBonusMgr.h"
#include "ItemEnchantmentMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Loot.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Containers.h"
#include "RealmList.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TraitMgr.h"
#include "TraitPacketsCommon.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <numeric>
#include <span>
#include <set>
#include <sstream>
#include <unordered_set>

namespace TraitMgr
{
    // defined with external linkage in TraitMgr.cpp (not declared in its header)
    void FillOwnedCurrenciesMap(WorldPackets::Traits::TraitConfig const& traitConfig, PlayerDataAccessor player, std::map<int32, int32>& currencies);
}

namespace Companions
{
    namespace
    {
        constexpr int32 CLASSIC_TALENT_CURRENCY = 3820;
        constexpr uint32 SKILL_RIDING_ID = 762;

        // classic race/class combinations (playercreateinfo also lists every race for the other classes)
        struct RaceClasses { uint8 Race; std::vector<uint8> Classes; };
        std::vector<RaceClasses> const ClassicCombos =
        {
            { RACE_HUMAN,     { CLASS_WARRIOR, CLASS_PALADIN, CLASS_ROGUE, CLASS_PRIEST, CLASS_MAGE, CLASS_WARLOCK } },
            { RACE_ORC,       { CLASS_WARRIOR, CLASS_HUNTER, CLASS_ROGUE, CLASS_SHAMAN, CLASS_WARLOCK } },
            { RACE_DWARF,     { CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST } },
            { RACE_NIGHTELF,  { CLASS_WARRIOR, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_DRUID } },
            { RACE_UNDEAD_PLAYER, { CLASS_WARRIOR, CLASS_ROGUE, CLASS_PRIEST, CLASS_MAGE, CLASS_WARLOCK } },
            { RACE_TAUREN,    { CLASS_WARRIOR, CLASS_HUNTER, CLASS_SHAMAN, CLASS_DRUID } },
            { RACE_GNOME,     { CLASS_WARRIOR, CLASS_ROGUE, CLASS_MAGE, CLASS_WARLOCK } },
            { RACE_TROLL,     { CLASS_WARRIOR, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_SHAMAN, CLASS_MAGE } },
        };

        bool IsAllianceRace(uint8 race)
        {
            return race == RACE_HUMAN || race == RACE_DWARF || race == RACE_NIGHTELF || race == RACE_GNOME;
        }

        // weapon and armor proficiency spells by class (classic rules)
        std::vector<uint32> ProficienciesFor(uint8 classId)
        {
            switch (classId)
            {
                case CLASS_WARRIOR: return { 196, 197, 198, 199, 200, 201, 202, 227, 264, 266, 1180, 2567, 5011, 15590, 9116, 9077, 9078, 8737 };
                case CLASS_PALADIN: return { 196, 197, 198, 199, 200, 201, 202, 9116, 9077, 9078, 8737 };
                case CLASS_HUNTER:  return { 196, 197, 200, 201, 202, 227, 264, 266, 1180, 2567, 5011, 15590, 9077, 9078 };
                case CLASS_ROGUE:   return { 198, 201, 1180, 2567, 264, 266, 5011, 15590, 9077, 9078 };
                case CLASS_PRIEST:  return { 198, 227, 1180, 5009, 9078 };
                case CLASS_SHAMAN:  return { 196, 197, 198, 199, 227, 1180, 15590, 9116, 9077, 9078 };
                case CLASS_MAGE:    return { 201, 227, 1180, 5009, 9078 };
                case CLASS_WARLOCK: return { 201, 227, 1180, 5009, 9078 };
                case CLASS_DRUID:   return { 198, 199, 227, 1180, 15590, 9077, 9078 };
                default:            return { };
            }
        }

        // class spells that come from quests in classic, not from trainers: (level, spell)
        std::vector<std::pair<uint8, uint32>> QuestSpellsFor(uint8 classId)
        {
            switch (classId)
            {
                case CLASS_WARRIOR: return { { 10, 71 }, { 10, 355 }, { 10, 7386 }, { 30, 2458 } };          // Defensive Stance, Taunt, Sunder Armor, Berserker Stance
                case CLASS_PALADIN: return { { 12, 7328 } };                                                    // Redemption
                case CLASS_HUNTER:  return { { 10, 1515 }, { 10, 883 }, { 10, 2641 }, { 10, 982 }, { 10, 6991 } }; // Tame/Call/Dismiss/Revive/Feed Pet
                case CLASS_SHAMAN:  return { { 4, 8071 }, { 10, 3599 }, { 20, 5394 } };                        // Stoneskin, Searing, Healing Stream Totem
                case CLASS_WARLOCK: return { { 1, 688 }, { 10, 697 }, { 20, 712 }, { 30, 691 } };               // Imp, Voidwalker, Succubus, Felhunter
                case CLASS_DRUID:   return { { 10, 5487 }, { 10, 6795 }, { 16, 1066 } };                       // Bear Form, Growl, Aquatic Form
                default:            return { };
            }
        }

        std::vector<uint32> const WeaponSkills = { 43, 44, 45, 46, 54, 55, 95, 136, 160, 162, 172, 173, 176, 226, 228, 229, 473 };

        float StatWeight(int32 stat, uint8 classId, Role role)
        {
            bool tank = role == Role::Tank;
            bool healer = role == Role::Healer;
            bool caster = role == Role::RangedDps && classId != CLASS_HUNTER;
            bool agile = classId == CLASS_ROGUE || classId == CLASS_HUNTER || (classId == CLASS_DRUID && !caster && !healer);
            switch (stat)
            {
                case ITEM_MOD_STAMINA:          return tank ? 1.5f : 0.4f;
                case ITEM_MOD_STRENGTH:         return (tank || role == Role::MeleeDps) ? (agile ? 0.5f : 1.0f) : 0.0f;
                case ITEM_MOD_AGILITY:          return agile ? 1.2f : (tank || role == Role::MeleeDps ? 0.6f : 0.0f);
                case ITEM_MOD_INTELLECT:        return healer ? 1.0f : (caster ? 0.8f : (classId == CLASS_HUNTER || classId == CLASS_PALADIN || classId == CLASS_SHAMAN ? 0.3f : 0.0f));
                case ITEM_MOD_SPIRIT:           return healer ? 0.7f : (classId == CLASS_PRIEST || classId == CLASS_MAGE ? 0.3f : 0.05f);
                case ITEM_MOD_DEFENSE_SKILL_RATING: return tank ? 1.3f : 0.0f;
                case ITEM_MOD_DODGE_RATING:     return tank ? 1.0f : 0.1f;
                case ITEM_MOD_PARRY_RATING:     return tank ? 0.8f : 0.0f;
                case ITEM_MOD_BLOCK_RATING:     return tank ? 0.5f : 0.0f;
                case ITEM_MOD_BLOCK_VALUE:      return tank ? 0.3f : 0.0f;
                case ITEM_MOD_HIT_MELEE_RATING:
                case ITEM_MOD_HIT_RANGED_RATING:
                case ITEM_MOD_HIT_RATING:       return (caster || healer) ? 0.2f : 1.2f;
                case ITEM_MOD_CRIT_MELEE_RATING:
                case ITEM_MOD_CRIT_RANGED_RATING:
                case ITEM_MOD_CRIT_RATING:      return (caster || healer) ? 0.3f : 1.0f;
                case ITEM_MOD_HIT_SPELL_RATING: return caster ? 1.0f : 0.0f;
                case ITEM_MOD_CRIT_SPELL_RATING: return caster ? 1.0f : (healer ? 0.5f : 0.0f);
                case ITEM_MOD_HASTE_RATING:     return 0.5f;
                case ITEM_MOD_ATTACK_POWER:     return (caster || healer) ? 0.0f : 0.5f;
                case ITEM_MOD_RANGED_ATTACK_POWER: return classId == CLASS_HUNTER ? 0.5f : 0.0f;
                case ITEM_MOD_SPELL_POWER:
                case ITEM_MOD_SPELL_DAMAGE_DONE: return caster ? 1.2f : (healer ? 0.8f : 0.0f);
                case ITEM_MOD_SPELL_HEALING_DONE: return healer ? 1.0f : 0.0f;
                case ITEM_MOD_MANA_REGENERATION: return healer ? 1.5f : (caster ? 0.6f : 0.0f);
                case ITEM_MOD_HEALTH_REGEN:     return 0.2f;
                default:                        return 0.0f;
            }
        }

        // best armor type a class wears at a level
        uint32 BestArmorSubClass(uint8 classId, uint8 level)
        {
            switch (classId)
            {
                case CLASS_WARRIOR:
                case CLASS_PALADIN: return level >= 40 ? ITEM_SUBCLASS_ARMOR_PLATE : ITEM_SUBCLASS_ARMOR_MAIL;
                case CLASS_HUNTER:
                case CLASS_SHAMAN:  return level >= 40 ? ITEM_SUBCLASS_ARMOR_MAIL : ITEM_SUBCLASS_ARMOR_LEATHER;
                case CLASS_ROGUE:
                case CLASS_DRUID:   return ITEM_SUBCLASS_ARMOR_LEATHER;
                default:            return ITEM_SUBCLASS_ARMOR_CLOTH;
            }
        }

        bool LooksUnobtainable(ItemTemplate const* proto)
        {
            if (proto->HasFlag(ITEM_FLAG_DEPRECATED))
                return true;
            char const* name = proto->GetName(LOCALE_enUS);
            if (!name || !*name)
                return true;
            std::string n(name);
            for (char const* marker : { "Test", "TEST", "QA ", "Monster -", "Deprecated", "[PH]", "OLD", "zz" })
                if (n.find(marker) != std::string::npos)
                    return true;
            return false;
        }
    }

    CompanionFactory* CompanionFactory::instance()
    {
        static CompanionFactory instance;
        return &instance;
    }

    void CompanionFactory::Initialize()
    {
        uint32 start = getMSTime();
        LoadTrainerSpells();
        LoadGearPool();
        LoadTalentTrees();

        _petEntries.clear();
        for (auto const& [entry, ct] : sObjectMgr->GetCreatureTemplates())
        {
            if (ct.type != CREATURE_TYPE_BEAST || ct.Classification != CreatureClassifications::Normal)
                continue;
            if (ct.family != CREATURE_FAMILY_WOLF && ct.family != CREATURE_FAMILY_CAT && ct.family != CREATURE_FAMILY_BOAR && ct.family != CREATURE_FAMILY_BEAR)
                continue;
            if (CreatureDifficulty const* difficulty = ct.GetDifficulty(DIFFICULTY_NONE))
                if (ct.IsTameable(false, difficulty) && ct.Name.find("Test") == std::string::npos)
                    _petEntries.push_back(entry);
        }
        std::sort(_petEntries.begin(), _petEntries.end());

        TC_LOG_INFO("server.worldserver", "Companions: {} trainer spells, {} gear items, {} foods, {} drinks, {} talent classes, {} pets in {} ms",
            _trainerSpells.size(), std::accumulate(_gearByInvType.begin(), _gearByInvType.end(), size_t(0), [](size_t n, auto const& p) { return n + p.second.size(); }),
            _foodSpells.size(), _drinkSpells.size(), _talentTrees.size(), _petEntries.size(), GetMSTimeDiffToNow(start));
        TC_LOG_INFO("server.worldserver", "Companions: food at level 20/40/60: spells {}/{}/{}, drink {}/{}/{}",
            GetFoodSpell(20), GetFoodSpell(40), GetFoodSpell(60), GetDrinkSpell(20), GetDrinkSpell(40), GetDrinkSpell(60));
    }

    void CompanionFactory::LoadTrainerSpells()
    {
        _trainerSpells.clear();
        // class trainers (Type 0): one row per spell, lowest level wins
        QueryResult result = WorldDatabase.Query("SELECT ts.SpellId, MIN(ts.ReqLevel), MAX(ts.ReqSkillLine), MAX(ts.ReqSkillRank), MAX(ts.ReqAbility1), MAX(ts.ReqAbility2), MAX(ts.ReqAbility3) "
            "FROM trainer_spell ts JOIN trainer t ON t.Id = ts.TrainerId WHERE t.Type = 0 GROUP BY ts.SpellId");
        if (!result)
            return;
        do
        {
            Field* fields = result->Fetch();
            TrainerSpell spell;
            spell.SpellId = fields[0].GetUInt32();
            spell.ReqLevel = fields[1].GetUInt8();
            spell.ReqSkillLine = fields[2].GetUInt32();
            spell.ReqSkillRank = fields[3].GetUInt32();
            for (uint8 i = 0; i < 3; ++i)
                spell.ReqAbility[i] = fields[4 + i].GetUInt32();
            if (sSpellMgr->GetSpellInfo(spell.SpellId, DIFFICULTY_NONE))
                _trainerSpells.push_back(spell);
        } while (result->NextRow());
        std::sort(_trainerSpells.begin(), _trainerSpells.end(), [](TrainerSpell const& a, TrainerSpell const& b) { return a.ReqLevel < b.ReqLevel; });
    }

    void CompanionFactory::LoadGearPool()
    {
        _gearByInvType.clear();
        _ammo[0].clear();
        _ammo[1].clear();
        _bags.clear();
        _foodSpells.clear();
        _drinkSpells.clear();

        // items a player can actually get on this server: drops, vendors and quest rewards
        std::unordered_set<uint32> obtainable;
        auto collect = [&obtainable](char const* sql)
        {
            if (QueryResult result = WorldDatabase.Query(sql))
                do
                    obtainable.insert(result->Fetch()[0].GetUInt32());
                while (result->NextRow());
        };
        collect("SELECT DISTINCT Item FROM creature_loot_template WHERE ItemType = 0 AND Chance >= 0.2");
        collect("SELECT DISTINCT Item FROM reference_loot_template WHERE ItemType = 0");
        collect("SELECT DISTINCT Item FROM gameobject_loot_template WHERE ItemType = 0");
        collect("SELECT DISTINCT item FROM npc_vendor WHERE type = 1");
        collect("SELECT RewardItem1 FROM quest_template UNION SELECT RewardItem2 FROM quest_template UNION SELECT RewardItem3 FROM quest_template UNION SELECT RewardItem4 FROM quest_template");
        collect("SELECT RewardChoiceItemID1 FROM quest_template UNION SELECT RewardChoiceItemID2 FROM quest_template UNION SELECT RewardChoiceItemID3 FROM quest_template "
            "UNION SELECT RewardChoiceItemID4 FROM quest_template UNION SELECT RewardChoiceItemID5 FROM quest_template UNION SELECT RewardChoiceItemID6 FROM quest_template");
        obtainable.erase(0);

        std::vector<std::pair<uint32, uint32>> bags;     // slots, entry
        std::vector<std::pair<uint32, uint32>> ammo[2];  // level, entry

        for (uint32 entry : obtainable)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto || LooksUnobtainable(proto))
                continue;

            uint32 reqLevel = uint32(std::max(0, proto->GetBaseRequiredLevel()));
            uint32 itemLevel = proto->GetBaseItemLevel();

            // food and drink spells (eaten without the item)
            if (proto->GetClass() == ITEM_CLASS_CONSUMABLE)
            {
                for (ItemEffectEntry const* effect : proto->Effects)
                {
                    if (effect->TriggerType != ITEM_SPELLTRIGGER_ON_USE || effect->SpellID <= 0)
                        continue;
                    SpellInfo const* spell = sSpellMgr->GetSpellInfo(uint32(effect->SpellID), DIFFICULTY_NONE);
                    if (!spell)
                        continue;
                    SpellSpecificType specific = spell->GetSpellSpecific();
                    // how much it restores per tick: the strongest one its level allows is eaten, not the first one found
                    int32 health = 0, mana = 0;
                    for (SpellEffectInfo const& spellEffect : spell->GetEffects())
                    {
                        if (!spellEffect.IsEffect(SPELL_EFFECT_APPLY_AURA))
                            continue;
                        if (spellEffect.ApplyAuraName == SPELL_AURA_MOD_REGEN || spellEffect.ApplyAuraName == SPELL_AURA_OBS_MOD_HEALTH)
                            health = std::max(health, spellEffect.CalcValueAsInt());
                        else if ((spellEffect.ApplyAuraName == SPELL_AURA_MOD_POWER_REGEN || spellEffect.ApplyAuraName == SPELL_AURA_OBS_MOD_POWER)
                            && spellEffect.MiscValue == POWER_MANA)
                            mana = std::max(mana, spellEffect.CalcValueAsInt());
                    }
                    if ((specific == SPELL_SPECIFIC_FOOD || specific == SPELL_SPECIFIC_FOOD_AND_DRINK) && health > 0)
                        _foodSpells.push_back({ reqLevel, health, spell->Id });
                    if ((specific == SPELL_SPECIFIC_DRINK || specific == SPELL_SPECIFIC_FOOD_AND_DRINK) && mana > 0)
                        _drinkSpells.push_back({ reqLevel, mana, spell->Id });
                }
                continue;
            }

            if (proto->GetClass() == ITEM_CLASS_CONTAINER && proto->GetSubClass() == ITEM_SUBCLASS_CONTAINER && proto->GetContainerSlots() <= 16)
            {
                bags.emplace_back(proto->GetContainerSlots(), entry);
                continue;
            }

            if (proto->GetClass() == ITEM_CLASS_PROJECTILE && (proto->GetSubClass() == ITEM_SUBCLASS_ARROW || proto->GetSubClass() == ITEM_SUBCLASS_BULLET))
            {
                ammo[proto->GetSubClass() == ITEM_SUBCLASS_ARROW ? 0 : 1].emplace_back(reqLevel, entry);
                continue;
            }

            if (proto->GetClass() != ITEM_CLASS_WEAPON && proto->GetClass() != ITEM_CLASS_ARMOR)
                continue;
            if (proto->GetQuality() < ITEM_QUALITY_NORMAL || proto->GetQuality() > ITEM_QUALITY_EPIC)
                continue;
            if (reqLevel > 60 || itemLevel == 0 || itemLevel > 100)
                continue;
            uint32 invType = proto->GetInventoryType();
            if (invType == INVTYPE_NON_EQUIP || invType == INVTYPE_BODY || invType == INVTYPE_TABARD || invType == INVTYPE_TRINKET
                || invType == INVTYPE_BAG || invType == INVTYPE_AMMO || invType == INVTYPE_QUIVER || invType == INVTYPE_RELIC)
                continue;

            GearItem item = MakeGear(proto);
            item.ReqLevel = reqLevel;
            _gearByInvType[item.InventoryType].push_back(std::move(item));
        }

        std::sort(bags.begin(), bags.end(), std::greater<>());
        for (auto const& [slots, entry] : bags)
            _bags.push_back(entry);
        for (uint8 i = 0; i < 2; ++i)
        {
            std::sort(ammo[i].begin(), ammo[i].end());
            for (auto const& [level, entry] : ammo[i])
                _ammo[i].push_back(entry);
        }
    }

    void CompanionFactory::LoadTalentTrees()
    {
        _talentTrees.clear();

        // choice nodes have several entries: keep the first (lowest index)
        std::unordered_map<uint32, TraitNodeXTraitNodeEntryEntry const*> firstLink;
        for (TraitNodeXTraitNodeEntryEntry const* link : sTraitNodeXTraitNodeEntryStore)
        {
            auto [itr, inserted] = firstLink.emplace(link->TraitNodeID, link);
            if (!inserted && link->Index < itr->second->Index)
                itr->second = link;
        }

        std::unordered_map<int32, std::vector<TalentNode>> nodesByTree;
        for (auto const& [nodeId, link] : firstLink)
        {
            TraitNodeEntry const* node = sTraitNodeStore.LookupEntry(link->TraitNodeID);
            TraitNodeEntryEntry const* nodeEntry = sTraitNodeEntryStore.LookupEntry(link->TraitNodeEntryID);
            if (!node || !nodeEntry)
                continue;
            TalentNode talent;
            talent.NodeId = int32(node->ID);
            talent.EntryId = int32(nodeEntry->ID);
            talent.MaxRanks = std::max(1, nodeEntry->MaxRanks);
            talent.PosX = node->PosX;
            talent.PosY = node->PosY;
            talent.TreeId = int32(node->TraitTreeID);
            talent.SubTreeId = node->TraitSubTreeID;
            if (TraitDefinitionEntry const* definition = sTraitDefinitionStore.LookupEntry(nodeEntry->TraitDefinitionID))
                if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(uint32(std::max(0, definition->SpellID)), DIFFICULTY_NONE))
                    if (spell->SpellName && spell->SpellName->Str[LOCALE_enUS])
                        talent.Name = ToLower(spell->SpellName->Str[LOCALE_enUS]);
            nodesByTree[int32(node->TraitTreeID)].push_back(std::move(talent));
        }

        // like TraitMgr: each class has a class skill line pointing at its talent tree(s)
        std::unordered_map<uint8, std::vector<int32>> treesByClass;
        for (SkillLineXTraitTreeEntry const* link : sSkillLineXTraitTreeStore)
        {
            SkillLineEntry const* skill = sSkillLineStore.LookupEntry(link->SkillLineID);
            if (!skill || skill->CategoryID != SKILL_CATEGORY_CLASS)
                continue;
            for (SkillRaceClassInfoEntry const* info : sDB2Manager.GetSkillRaceClassInfo(skill->ID))
                for (uint8 classId = CLASS_WARRIOR; classId <= CLASS_DRUID; ++classId)
                    if (info->ClassMask & (1 << (classId - 1)))
                    {
                        std::vector<int32>& trees = treesByClass[classId];
                        if (std::find(trees.begin(), trees.end(), link->TraitTreeID) == trees.end())
                            trees.push_back(link->TraitTreeID);
                    }
        }

        {
            uint32 links = 0, classLinks = 0;
            std::ostringstream ids;
            for (SkillLineXTraitTreeEntry const* link : sSkillLineXTraitTreeStore)
            {
                ++links;
                SkillLineEntry const* skill = sSkillLineStore.LookupEntry(link->SkillLineID);
                if (skill && skill->CategoryID == SKILL_CATEGORY_CLASS)
                {
                    ++classLinks;
                    ids << " " << link->SkillLineID << "->" << link->TraitTreeID << "(" << sDB2Manager.GetSkillRaceClassInfo(skill->ID).size() << ")";
                }
            }
            TC_LOG_DEBUG("server.worldserver", "Companions: talent data: {} skill/tree links ({} class:{}), {} trees with nodes, {} classes",
                links, classLinks, ids.str().substr(0, 400), nodesByTree.size(), treesByClass.size());
        }

        for (auto const& [classId, trees] : treesByClass)
        {
            std::vector<TalentNode> nodes;
            for (int32 treeId : trees)
                if (auto itr = nodesByTree.find(treeId); itr != nodesByTree.end())
                    nodes.insert(nodes.end(), itr->second.begin(), itr->second.end());
            if (nodes.empty())
                continue;

            // the three classic tabs: sub trees, separate trees, or columns far apart
            std::set<int32> subTrees;
            for (TalentNode const& node : nodes)
                if (node.SubTreeId)
                    subTrees.insert(node.SubTreeId);
            std::function<int64(TalentNode const&)> tabOf;
            if (subTrees.size() >= 2)
                tabOf = [](TalentNode const& node) { return int64(node.SubTreeId); };
            else if (trees.size() >= 2)
                tabOf = [](TalentNode const& node) { return int64(node.TreeId); };
            else
            {
                std::vector<int32> columns;
                for (TalentNode const& node : nodes)
                    columns.push_back(node.PosX);
                std::sort(columns.begin(), columns.end());
                columns.erase(std::unique(columns.begin(), columns.end()), columns.end());
                // two cuts between columns giving the most even tabs (an outlier column must not make a tab of its own)
                std::vector<int32> cuts;
                if (columns.size() >= 3)
                {
                    auto countBelow = [&nodes](int32 x) { return int32(std::count_if(nodes.begin(), nodes.end(), [x](TalentNode const& n) { return n.PosX < x; })); };
                    int32 total = int32(nodes.size());
                    int32 bestMin = -1, bestGap = -1;
                    for (size_t i = 1; i < columns.size(); ++i)
                        for (size_t j = i + 1; j < columns.size(); ++j)
                        {
                            int32 a = countBelow(columns[i]), b = countBelow(columns[j]) - a, c = total - a - b;
                            int32 smallest = std::min({ a, b, c });
                            int32 gap = (columns[i] - columns[i - 1]) + (columns[j] - columns[j - 1]);
                            if (smallest > bestMin || (smallest == bestMin && gap > bestGap))
                            {
                                bestMin = smallest;
                                bestGap = gap;
                                cuts = { columns[i], columns[j] };
                            }
                        }
                }
                tabOf = [cuts](TalentNode const& node)
                {
                    int64 tab = 0;
                    for (int32 cut : cuts)
                        if (node.PosX >= cut)
                            ++tab;
                    return tab;
                };
            }

            std::map<int64, TalentTree> tabs;
            for (TalentNode const& node : nodes)
            {
                TalentTree& tab = tabs[tabOf(node)];
                tab.TraitTreeId = node.TreeId;
                tab.Nodes.push_back(node);
            }

            std::ostringstream summary;
            for (auto& [key, tab] : tabs)
            {
                std::sort(tab.Nodes.begin(), tab.Nodes.end(), [](TalentNode const& a, TalentNode const& b)
                {
                    return a.PosY != b.PosY ? a.PosY < b.PosY : a.PosX < b.PosX;
                });
                summary << " [" << tab.Nodes.size() << ": " << (tab.Nodes.front().Name.empty() ? "?" : tab.Nodes.front().Name) << "]";
                _talentTrees[classId].push_back(std::move(tab));
            }
            TC_LOG_DEBUG("server.worldserver", "Companions: class {} talents, {} trees, {} nodes, tabs:{}", classId, trees.size(), nodes.size(), summary.str());
            for (TalentTree const& tab : _talentTrees[classId])
            {
                std::string names;
                for (TalentNode const& node : tab.Nodes)
                    names += node.Name + "(" + std::to_string(node.MaxRanks) + ") ";
                TC_LOG_DEBUG("server.worldserver", "Companions: class {} tab: {}", classId, names);
            }
        }
    }

    // ------------------------------------------------------------------ creation

    bool CompanionFactory::IsClassicCombo(uint8 race, uint8 classId) const
    {
        for (RaceClasses const& combo : ClassicCombos)
            if (combo.Race == race)
                return std::find(combo.Classes.begin(), combo.Classes.end(), classId) != combo.Classes.end();
        return false;
    }

    std::vector<uint8> CompanionFactory::RacesFor(Team team, uint8 classId) const
    {
        std::vector<uint8> races;
        // the classic pairs, plus Forever's additions for the hybrid classes (Undead and Tauren paladins, Dwarf shamans,
        // Troll druids) as its playercreateinfo lists them; the other classes list every race there (retail leftovers)
        bool forever = classId == CLASS_PALADIN || classId == CLASS_SHAMAN || classId == CLASS_DRUID;
        for (RaceClasses const& combo : ClassicCombos)
            if ((team == ALLIANCE) == IsAllianceRace(combo.Race) && (forever || IsClassicCombo(combo.Race, classId))
                && sObjectMgr->GetPlayerInfo(combo.Race, classId))
                races.push_back(combo.Race);
        return races;
    }

    std::string CompanionFactory::RandomName() const
    {
        static char const* const starts[] = { "Ael", "Bra", "Cor", "Dar", "Eld", "Fen", "Gal", "Hal", "Ith", "Jor", "Kael", "Lor", "Mar", "Nor", "Or",
            "Per", "Quel", "Ral", "Syl", "Tor", "Ul", "Val", "Wyn", "Xan", "Yor", "Zan", "Bel", "Cal", "Dra", "Gor", "Thal", "Mor", "Ash", "Bren" };
        static char const* const middles[] = { "", "", "a", "e", "i", "o", "an", "ar", "en", "il", "or", "un", "ae", "ia" };
        static char const* const ends[] = { "dor", "wyn", "ric", "las", "mir", "nar", "ros", "thas", "vin", "dan", "gar", "lin", "mus", "rah", "ten",
            "vos", "zul", "ith", "eon", "ara", "ina", "elle", "ys", "ok", "ash" };
        std::string name = starts[urand(0, std::size(starts) - 1)];
        name += middles[urand(0, std::size(middles) - 1)];
        name += ends[urand(0, std::size(ends) - 1)];
        return name;
    }

    void CompanionFactory::Update()
    {
        _saveCallbacks.ProcessReadyCallbacks();
    }

    ObjectGuid CompanionFactory::CreateCharacter(Player* owner, uint8 classId, uint8 race, uint8 gender, std::string name, std::string& error,
        std::function<void(ObjectGuid guid, bool saved)> afterSave)
    {
        return CreateCharacter(owner->GetTeam(), owner->GetSession()->GetClientBuild(), classId, race, gender, std::move(name), { }, error, std::move(afterSave));
    }

    bool CompanionFactory::IsChoiceAvailable(ChrCustomizationChoiceEntry const* choice, uint8 race)
    {
        if (!choice->ChrCustomizationReqID)
            return true;
        ChrCustomizationReqEntry const* req = sChrCustomizationReqStore.LookupEntry(choice->ChrCustomizationReqID);
        if (!req || !req->GetFlags().HasFlag(ChrCustomizationReqFlag::HasRequirements))
            return true;
        if (!req->RaceMask.IsEmpty() && req->RaceMask != RACEMASK_ALL_v<int32, 2> && !req->RaceMask.HasRace(race))
            return false;
        return !req->AchievementID && !req->ItemModifiedAppearanceID && !req->QuestID;
    }

    std::string CompanionFactory::RandomFreeName(uint8 race, uint8 gender) const
    {
        auto free = [](std::string candidate)
        {
            return normalizePlayerName(candidate) && ObjectMgr::CheckPlayerName(candidate, LOCALE_enUS, true) == CHAR_NAME_SUCCESS
                && !sObjectMgr->IsReservedName(candidate) && !sCharacterCache->GetCharacterCacheByName(candidate);
        };
        // the names the client's "random name" button gives that race and gender
        if (race)
            for (uint32 attempt = 0; attempt < 30; ++attempt)
            {
                std::string candidate = sDB2Manager.GetNameGenEntry(race, gender > GENDER_FEMALE ? uint8(urand(0, 1)) : gender);
                if (!candidate.empty() && free(candidate))
                {
                    normalizePlayerName(candidate);
                    return candidate;
                }
            }
        return RandomFreeName();
    }

    // family names in the style of each people (Forever shows "Name Surname")
    std::string CompanionFactory::RandomSurname(uint8 race)
    {
        static std::vector<char const*> const human = { "Ashford", "Blackwood", "Brightwater", "Caldwell", "Crowley", "Dawnbrook", "Fairbanks",
            "Greystone", "Hawthorne", "Kingsley", "Lightwood", "Marlow", "Northwind", "Oakheart", "Pembrook", "Ravenhill", "Redbrook",
            "Stonefield", "Stormridge", "Thornbury", "Underhill", "Westfall", "Whitlock", "Wolfe", "Goldcrest", "Ironmonger", "Shaw",
            "Ellwood", "Harrowmere", "Langston", "Moorcroft", "Silverlake", "Talbot", "Winterbourne", "Brennan", "Cobb" };
        static std::vector<char const*> const dwarf = { "Ironforge", "Stonebeard", "Bronzehammer", "Anvilmar", "Thunderbrew", "Coppervein",
            "Deepdelver", "Fireforge", "Granitehand", "Hammerfall", "Ironfoot", "Mountainheart", "Oakenshield", "Rockjaw", "Steelgrip",
            "Stoutmantle", "Tunneltoe", "Wildhammer", "Frostbeard", "Goldfist", "Blackanvil", "Ironbelly", "Barleybrew", "Shalehammer" };
        static std::vector<char const*> const nightElf = { "Moonwhisper", "Starbreeze", "Shadowleaf", "Nightwind", "Silverleaf", "Dawnstrider",
            "Moonshadow", "Starweaver", "Windwhisper", "Feathermoon", "Starfall", "Thistlewood", "Duskbrook", "Leafshade", "Mistwalker",
            "Ravencrest", "Sagebough", "Glimmerdew", "Nightsong", "Whisperwind", "Elunara", "Swiftarrow", "Bladesong", "Wildbloom" };
        static std::vector<char const*> const gnome = { "Cogspinner", "Fizzlebolt", "Gearloose", "Sprocketwhistle", "Tinkerton", "Wobblesprocket",
            "Boltbucket", "Copperpot", "Fastgear", "Gizmowrench", "Sparkfuse", "Nuttlebolt", "Pinwheel", "Spannerspark", "Thistlefuzz",
            "Twinklespark", "Widgetwhirl", "Clickclack", "Fizzwidget", "Springlock", "Brassbuckle", "Gadgetspring" };
        static std::vector<char const*> const orc = { "Bloodfist", "Skullsplitter", "Blackrock", "Stonehammer", "Frostwolf", "Warsong",
            "Ironjaw", "Thunderlord", "Burningblade", "Shatterskull", "Bonechewer", "Grimtotem", "Earthfury", "Steelfang", "Axebreaker",
            "Rageclaw", "Blackfang", "Gorehowl", "Wolfrider", "Stonebash", "Redmane", "Scarjaw" };
        static std::vector<char const*> const undead = { "Graves", "Blackheart", "Coldwater", "Duskwither", "Gravemoor", "Hollowell",
            "Marrowind", "Nightfall", "Rotbrook", "Shadebrook", "Thorne", "Vale", "Wormwood", "Ashgrave", "Bleakmoor", "Crowsbane",
            "Deadwind", "Grimsby", "Mournvale", "Ravenscar", "Sorrowmere", "Witherby" };
        static std::vector<char const*> const tauren = { "Thunderhoof", "Mistrunner", "Redhoof", "Earthsong", "Grimtotem", "Highmountain",
            "Ragetotem", "Runetotem", "Skyhorn", "Stonehorn", "Windfury", "Plainstrider", "Proudmane", "Cloudsong", "Dawnhoof",
            "Longhorn", "Sunwalker", "Stormhorn", "Thunderhorn", "Wildmane", "Swiftwind", "Greatmane" };
        static std::vector<char const*> const troll = { "Darkspear", "Zul'gath", "Shadowhunter", "Bloodscalp", "Vilebranch", "Witherbark",
            "Skullsplitter", "Sandfury", "Bloodtusk", "Jinzal", "Mojoka", "Vol'kar", "Zalazane", "Tikka", "Juju", "Raptorjaw",
            "Voodoomane", "Spiritwalker", "Razortusk", "Zanzil" };
        std::vector<char const*> const* list = &human;
        switch (race)
        {
            case RACE_DWARF: list = &dwarf; break;
            case RACE_NIGHTELF: list = &nightElf; break;
            case RACE_GNOME: list = &gnome; break;
            case RACE_ORC: list = &orc; break;
            case RACE_UNDEAD_PLAYER: list = &undead; break;
            case RACE_TAUREN: list = &tauren; break;
            case RACE_TROLL: list = &troll; break;
            default: list = Player::TeamForRace(race) == HORDE ? &orc : &human; break;
        }
        return (*list)[urand(0, list->size() - 1)];
    }

    bool CompanionFactory::ValidSurname(std::string& surname)
    {
        std::string out;
        for (char c : surname)
        {
            if (!std::isalpha(static_cast<unsigned char>(c)) && c != ' ' && c != '\'' && c != '-')
                return false;
            // a capital first letter; the rest as written ("van der Hollow" stays)
            out += out.empty() ? char(std::toupper(static_cast<unsigned char>(c))) : c;
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        if (out.size() > 48)
            return false;
        surname = out;
        return true;
    }

    void CompanionFactory::SetSurname(ObjectGuid guid, std::string const& surname)
    {
        std::string escaped = surname;
        CharacterDatabase.EscapeString(escaped);
        CharacterDatabase.PExecute("UPDATE characters SET surname = '{}' WHERE guid = {}", escaped, guid.GetCounter());
        sCharacterCache->UpdateCharacterSurname(guid, surname);
    }

    std::string CompanionFactory::RandomFreeName() const
    {
        for (uint32 attempt = 0; attempt < 50; ++attempt)
        {
            std::string candidate = RandomName();
            if (normalizePlayerName(candidate) && ObjectMgr::CheckPlayerName(candidate, LOCALE_enUS, true) == CHAR_NAME_SUCCESS
                && !sObjectMgr->IsReservedName(candidate) && !sCharacterCache->GetCharacterCacheByName(candidate))
                return candidate;
        }
        return "";
    }

    std::vector<std::pair<uint32, std::string>> CompanionFactory::DescribeTabs(uint8 classId) const
    {
        std::vector<std::pair<uint32, std::string>> tabs;
        auto itr = _talentTrees.find(classId);
        if (itr == _talentTrees.end())
            return tabs;
        for (TalentTree const& tab : itr->second)
        {
            std::string names;
            for (size_t i = 0; i < tab.Nodes.size() && i < 4; ++i)
                names += (names.empty() ? "" : ", ") + tab.Nodes[i].Name;
            tabs.emplace_back(uint32(tab.Nodes.size()), names);
        }
        return tabs;
    }

    ObjectGuid CompanionFactory::CreateCharacter(Team team, uint32 build, uint8 classId, uint8 race, uint8 gender, std::string name,
        std::vector<std::pair<uint32, uint32>> const& looks, std::string& error, std::function<void(ObjectGuid guid, bool saved)> afterSave)
    {
        std::vector<uint8> races = RacesFor(team, classId);
        if (races.empty())
        {
            error = "That class is not available to your faction.";
            return ObjectGuid::Empty;
        }
        if (race && std::find(races.begin(), races.end(), race) == races.end())
        {
            error = "That race and class combination is not possible for your faction.";
            return ObjectGuid::Empty;
        }
        if (!race)
            race = races[urand(0, races.size() - 1)];
        if (gender > GENDER_FEMALE)
            gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));

        if (name.empty())
        {
            name = RandomFreeName(race, gender);
            if (name.empty())
            {
                error = "Could not find a free name, give one.";
                return ObjectGuid::Empty;
            }
        }
        else
        {
            if (!normalizePlayerName(name) || ObjectMgr::CheckPlayerName(name, LOCALE_enUS, true) != CHAR_NAME_SUCCESS || sObjectMgr->IsReservedName(name))
            {
                error = "That name is not allowed.";
                return ObjectGuid::Empty;
            }
            if (sCharacterCache->GetCharacterCacheByName(name))
            {
                error = "That name is taken.";
                return ObjectGuid::Empty;
            }
        }

        uint32 accountId = sCompanionMgr->GetOrCreateCreationAccount();
        if (!accountId)
        {
            error = "The companion account could not be created (see the server log).";
            return ObjectGuid::Empty;
        }

        WorldSession* session = new WorldSession(accountId, std::string(GetConfig().AccountName), 0, std::string(), nullptr, SEC_PLAYER,
            uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, std::string("Wn64"), Minutes(0), build, { }, LOCALE_enUS, 0, false);
        session->SetBot(true);

        auto tryCreate = [&](bool withLook) -> Player*
        {
            WorldPackets::Character::CharacterCreateInfo info;
            info.Race = race;
            info.Class = classId;
            info.Sex = gender;
            info.Name = name;
            if (withLook && !looks.empty())
            {
                // the choices picked in the web panel, kept sorted by option like the client sends them
                std::vector<std::pair<uint32, uint32>> sorted = looks;
                std::sort(sorted.begin(), sorted.end());
                for (auto const& [optionId, choiceId] : sorted)
                {
                    UF::ChrCustomizationChoice choice;
                    choice.ChrCustomizationOptionID = optionId;
                    choice.ChrCustomizationChoiceID = choiceId;
                    info.Customizations.push_back(choice);
                }
            }
            else if (withLook)
            {
                if (std::vector<ChrCustomizationOptionEntry const*> const* options = sDB2Manager.GetCustomiztionOptions(race, gender))
                {
                    std::vector<ChrCustomizationOptionEntry const*> sorted(options->begin(), options->end());
                    std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->ID < b->ID; });
                    for (ChrCustomizationOptionEntry const* option : sorted)
                    {
                        std::vector<ChrCustomizationChoiceEntry const*> const* choices = sDB2Manager.GetCustomiztionChoices(option->ID);
                        if (!choices || choices->empty())
                            continue;
                        std::vector<ChrCustomizationChoiceEntry const*> free;
                        for (ChrCustomizationChoiceEntry const* choice : *choices)
                            if (IsChoiceAvailable(choice, race))
                                free.push_back(choice);
                        if (free.empty())
                            continue;
                        UF::ChrCustomizationChoice choice;
                        choice.ChrCustomizationOptionID = option->ID;
                        choice.ChrCustomizationChoiceID = free[urand(0, std::min<uint32>(free.size(), 6) - 1)]->ID;
                        info.Customizations.push_back(choice);
                    }
                }
            }

            Player* player = new Player(session);
            player->GetMotionMaster()->Initialize();
            if (!player->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &info))
            {
                player->CleanupsBeforeDelete();
                delete player;
                return nullptr;
            }
            return player;
        };

        Player* player = tryCreate(true);
        if (!player)
            player = tryCreate(false);
        if (!player)
        {
            delete session;
            error = "The character could not be created (race/class data).";
            return ObjectGuid::Empty;
        }

        player->setCinematic(1);
        player->SetAtLoginFlag(AT_LOGIN_FIRST);

        CharacterDatabaseTransaction characterTransaction = CharacterDatabase.BeginTransaction();
        LoginDatabaseTransaction loginTransaction = LoginDatabase.BeginTransaction();
        player->SaveToDB(loginTransaction, characterTransaction, true);

        uint32 count = 0;
        if (QueryResult result = CharacterDatabase.PQuery("SELECT COUNT(*) FROM characters WHERE account = {}", accountId))
            count = uint32(result->Fetch()[0].GetUInt64());
        LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_REP_REALM_CHARACTERS);
        stmt->setUInt32(0, count + 1);
        stmt->setUInt32(1, accountId);
        stmt->setUInt32(2, sRealmList->GetCurrentRealmId().Realm);
        loginTransaction->Append(stmt);

        // the core prepares these statements for the asynchronous connections only
        ObjectGuid guid = player->GetGUID();
        std::string characterName = player->GetName();
        uint8 nativeGender = player->GetNativeGender();
        _saveCallbacks.AddCallback(CharacterDatabase.AsyncCommitTransaction(characterTransaction)).AfterComplete(
            [guid, accountId, characterName, nativeGender, race, classId, loginTransaction, afterSave = std::move(afterSave)](bool success)
        {
            if (success)
            {
                LoginDatabase.CommitTransaction(loginTransaction);
                sCharacterCache->AddCharacterCacheEntry(guid, accountId, characterName, nativeGender, race, classId, 1, false);
                TC_LOG_INFO("server.worldserver", "Companions: created {} ({} race {} class {})", characterName, guid.ToString(), race, classId);
            }
            else
                TC_LOG_ERROR("server.worldserver", "Companions: saving the new character {} failed", characterName);
            if (afterSave)
                afterSave(guid, success);
        });

        player->CleanupsBeforeDelete();
        delete player;
        delete session;
        return guid;
    }

    // ------------------------------------------------------------------ preparation

    void CompanionFactory::Prepare(Player* bot, uint8 level, Role role, bool regear)
    {
        if (level > bot->GetLevel())
            LevelTo(bot, level);

        LearnProficiencies(bot);
        TrainSpells(bot);
        LearnQuestSpells(bot);
        MaxWeaponSkills(bot);
        if (bot->GetLevel() >= GetConfig().MountLevel && sSkillLineStore.LookupEntry(SKILL_RIDING_ID))
        {
            uint16 riding = bot->GetLevel() >= 60 ? 150 : 75;
            if (bot->GetSkillValue(SKILL_RIDING_ID) < riding)
                bot->SetSkill(SKILL_RIDING_ID, riding >= 150 ? 2 : 1, riding, riding);
        }

        SpendTalents(bot, role);
        if (regear)
        {
            CompanionAI const* ai = sCompanionMgr->GetAI(bot);
            if (ai && ai->IsAmbient())
                DressAmbient(bot, role);
            else
                EquipBestGear(bot, role);
        }
        EquipBags(bot);
        RestockConsumables(bot);
        EnsureHunterPet(bot);

        bot->SetFullHealth();
        if (bot->GetMaxPower(POWER_MANA) > 0)
            bot->SetFullPower(POWER_MANA);
        bot->DurabilityRepairAll(false, 0.0f, false);
        bot->SaveToDB();

        if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
            ai->RefreshSpells();
    }

    void CompanionFactory::LevelTo(Player* bot, uint8 level)
    {
        level = std::min<uint8>(level, uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));
        if (level <= bot->GetLevel())
            return;
        bot->GiveLevel(level);
        bot->InitTalentForLevel();
        bot->SetXP(0);
    }

    void CompanionFactory::LearnProficiencies(Player* bot)
    {
        for (uint32 spellId : ProficienciesFor(bot->GetClass()))
            if (!bot->HasSpell(spellId) && sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE))
                bot->LearnSpell(spellId, false, 0, true);

        if (bot->GetLevel() >= 40)
        {
            uint32 armor = 0;
            if (bot->GetClass() == CLASS_WARRIOR || bot->GetClass() == CLASS_PALADIN)
                armor = 750;    // Plate Mail
            else if (bot->GetClass() == CLASS_HUNTER || bot->GetClass() == CLASS_SHAMAN)
                armor = 8737;   // Mail
            if (armor && !bot->HasSpell(armor) && sSpellMgr->GetSpellInfo(armor, DIFFICULTY_NONE))
                bot->LearnSpell(armor, false, 0, true);
        }
    }

    void CompanionFactory::LearnQuestSpells(Player* bot)
    {
        for (auto const& [level, spellId] : QuestSpellsFor(bot->GetClass()))
            if (bot->GetLevel() >= level && !bot->HasSpell(spellId) && sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE))
                bot->LearnSpell(spellId, false, 0, true);
    }

    void CompanionFactory::TrainSpells(Player* bot)
    {
        uint8 level = bot->GetLevel();
        // requirements chain (rank 2 needs rank 1): a few passes
        for (uint32 pass = 0; pass < 6; ++pass)
        {
            bool learned = false;
            for (TrainerSpell const& spell : _trainerSpells)
            {
                if (spell.ReqLevel > level)
                    break;
                if (bot->HasSpell(spell.SpellId) || !bot->IsSpellFitByClassAndRace(spell.SpellId))
                    continue;
                bool ok = true;
                for (uint32 ability : spell.ReqAbility)
                    if (ability && !bot->HasSpell(ability))
                        ok = false;
                if (spell.ReqSkillLine && bot->GetBaseSkillValue(spell.ReqSkillLine) < spell.ReqSkillRank)
                    ok = false;
                if (!ok)
                    continue;

                SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId, DIFFICULTY_NONE);
                // class trainers also list a few recipes: only spells of the class's own family
                ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(bot->GetClass());
                auto ofClass = [&](SpellInfo const* candidate)
                {
                    return candidate && classEntry && candidate->SpellFamilyName == classEntry->SpellClassSet;
                };
                bool classSpell = ofClass(info);
                for (SpellEffectInfo const& effect : info->GetEffects())
                    if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell)
                        classSpell |= ofClass(sSpellMgr->GetSpellInfo(effect.TriggerSpell, DIFFICULTY_NONE));
                if (!classSpell)
                    continue;
                bool teaches = false;
                for (SpellEffectInfo const& effect : info->GetEffects())
                {
                    if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) && effect.TriggerSpell)
                    {
                        teaches = true;
                        if (!bot->HasSpell(effect.TriggerSpell))
                        {
                            bot->LearnSpell(effect.TriggerSpell, false, 0, true);
                            learned = true;
                        }
                    }
                }
                if (!teaches)
                {
                    bot->LearnSpell(spell.SpellId, false, 0, true);
                    learned = true;
                }
            }
            if (!learned)
                break;
        }
    }

    void CompanionFactory::MaxWeaponSkills(Player* bot)
    {
        bot->UpdateSkillsForLevel();
        uint16 max = bot->GetMaxSkillValueForLevel();
        for (uint32 skill : WeaponSkills)
            if (bot->HasSkill(skill) && bot->GetSkillValue(skill) < max)
                bot->SetSkill(skill, bot->GetSkillStep(skill), max, max);
    }

    // ------------------------------------------------------------------ talents

    // key talents per role, from the Forever 1.60 talent tabs (the tab with most of them is the main tab)
    std::vector<std::string_view> CompanionFactory::PrioritiesFor(uint8 classId, Role role)
    {
        switch (classId)
        {
            case CLASS_WARRIOR:
                if (role == Role::Tank)
                    return { "shield specialization", "anticipation", "improved bloodrage", "improved revenge", "last stand", "defiance",
                        "improved sunder armor", "concussion blow", "focused rage", "bastion", "shield slam", "vanguard", "improved shield wall",
                        "master of defense", "improved thunder clap", "iron will", "improved heroic strike", "deflection", "improved tactical mastery" };
                // the defining talents first: points go to them as soon as their tier opens
                return { "cruelty", "unbridled wrath", "enrage", "flurry", "bloodthirst", "death wish", "mortal strike", "sweeping strikes",
                    "impale", "deep wounds", "improved heroic strike", "improved tactical mastery", "anger management",
                    "two-handed weapon specialization", "dual wield specialization", "improved execute", "booming voice", "blood craze",
                    "lingering rage", "furious precision", "raging blows", "gore drinker", "improved berserker rage", "improved overpower",
                    "deflection", "improved rend" };
            case CLASS_PALADIN:
                if (role == Role::Healer)
                    return { "divine intellect", "spiritual focus", "healing light", "illumination", "divine favor", "holy power", "holy shock",
                        "unyielding faith", "infusion of light", "reverence", "light's vigil", "purifying power", "divine strength", "improved seals",
                        "toughness", "guardian's favor", "redoubt", "precision" };
                if (role == Role::Tank)
                    return { "redoubt", "toughness", "anticipation", "improved righteous fury", "shield specialization", "reckoning",
                        "sacred duty", "one-handed weapon specialization", "holy shield", "iron creed", "templar's bulwark", "precision",
                        "guardian's favor", "improved hammer of justice", "divine strength", "divine intellect" };
                return { "benediction", "improved judgement", "deflection", "conviction", "vindication", "sanctified judgement",
                    "seal of command", "two-handed weapon specialization", "vengeance", "repentance", "champion of the light",
                    "instrument of law", "twist of light", "pursuit of justice", "divine strength", "divine intellect", "improved seals" };
            case CLASS_HUNTER:
                return { "lethal attacks", "efficiency", "hawk eye", "careful aim", "mortal shots", "improved arcane shot", "trueshot aura",
                    "barrage", "ranged weapon specialization", "improved serpent sting", "sniper shot", "rapid killing", "improved stings",
                    "deadly aspects", "endurance training", "unleashed fury", "ferocity", "focused fire", "frenzy", "bestial wrath" };
            case CLASS_ROGUE:
                return { "improved sinister strike", "lightning reflexes", "precision", "improved eviscerate", "dual wield specialization",
                    "blade flurry", "hack and slash", "weapon expertise", "aggression", "adrenaline rush", "flawless execution", "puncturing wounds",
                    "malice", "ruthlessness", "murder", "improved slice and dice", "relentless strikes", "lethality" };
            case CLASS_PRIEST:
                if (role == Role::Healer)
                    return { "holy specialization", "improved renew", "divine fury", "inspiration", "improved healing", "holy reach",
                        "spiritual guidance", "spiritual healing", "spirit of redemption", "blessed recovery", "litany of light", "twilight focus",
                        "mental agility", "meditation", "inner focus", "improved power word: shield", "mental strength", "twin disciplines",
                        "silent resolve", "wand specialization" };
                return { "shadow focus", "spirit tap", "improved shadow word: pain", "improved mind blast", "mind flay", "shadow reach",
                    "shadow weaving", "vampiric embrace", "darkness", "shadowform", "dark inspiration", "early demise", "improved mind flay",
                    "blackout", "wand specialization", "twin disciplines", "mental agility" };
            case CLASS_SHAMAN:
                if (role == Role::Healer)
                    return { "improved healing wave", "tidal focus", "totemic focus", "ancestral healing", "healing focus", "tidal mastery",
                        "healing way", "nature's swiftness", "purification", "restorative totems", "mana tide totem", "natural grace",
                        "mindfulness", "ancestral knowledge", "toughness", "guardian totems" };
                if (role == Role::RangedDps)
                    return { "convection", "concussion", "call of flame", "elemental focus", "reverberation", "elemental devastation",
                        "eye of the storm", "elemental fury", "lightning overload", "elemental mastery", "elemental reach", "trial by fire",
                        "improved healing wave", "tidal focus" };
                return { "ancestral knowledge", "thundering strikes", "improved lightning shield", "elemental weapons", "flurry", "toughness",
                    "anticipation", "stormstrike", "spirit weapons", "improved stormstrike", "maelstrom weapon", "mental quickness",
                    "shamanistic focus", "improved healing wave", "tidal focus" };
            case CLASS_MAGE:
                return { "improved frostbolt", "elemental precision", "ice shards", "frostbite", "piercing ice", "permafrost", "shatter",
                    "improved frost nova", "frost channeling", "ice block", "cold snap", "winter's chill", "arctic reach", "ice barrier",
                    "fingers of frost", "arcane focus", "arcane subtlety", "arcane concentration", "arcane meditation", "arcane mind" };
            case CLASS_WARLOCK:
                return { "improved corruption", "suppression", "improved life tap", "improved bane of agony", "amplify curse",
                    "fel concentration", "nightfall", "grim reach", "malevolence", "siphon life", "shadow mastery", "pandemic", "soul siphon",
                    "wrack", "demonic embrace", "improved imp", "improved voidwalker", "fel vitality", "fel domination", "demonic aegis" };
            case CLASS_DRUID:
                if (role == Role::Healer)
                    return { "furor", "nature's focus", "naturalist", "improved rejuvenation", "improved regrowth", "reflection", "tranquil spirit",
                        "gift of nature", "nature's swiftness", "swiftmend", "subtlety", "living spirit", "improved tranquility",
                        "improved wrath", "moonglow", "nature's grace" };
                if (role == Role::RangedDps)
                    return { "improved wrath", "improved moonfire", "moonglow", "vengeance", "improved starfire", "nature's grace", "genesis",
                        "moonfury", "eclipse", "moonkin form", "insect swarm", "nature's majesty", "furor", "naturalist" };
                return { "ferocity", "feral instinct", "thick hide", "sharpened claws", "feral charge", "predatory strikes", "savage fury",
                    "blood frenzy", "heart of the wild", "leader of the pack", "brutal impact", "shredding attacks", "natural reaction",
                    "rend and tear", "feral swiftness", "furor", "naturalist", "nature's focus" };
            default:
                return { };
        }
    }

    // tabs in the game's order (checked against the Forever trees): warrior arms/fury/protection, paladin holy/protection/
    // retribution, hunter beast mastery/marksmanship/survival, rogue assassination/combat/subtlety, priest discipline/holy/
    // shadow, shaman elemental/enhancement/restoration, mage arcane/fire/frost, warlock affliction/demonology/destruction,
    // druid balance/feral/restoration
    CompanionFactory::MetaBuild const* CompanionFactory::MetaBuildFor(uint8 classId, uint32 tree)
    {
        static std::map<uint8, std::array<MetaBuild, 3>> const builds =
        {
            { CLASS_WARRIOR, {{ { { 31, 20, 0 }, Role::MeleeDps }, { { 17, 34, 0 }, Role::MeleeDps }, { { 8, 5, 38 }, Role::Tank } }} },
            { CLASS_PALADIN, {{ { { 31, 20, 0 }, Role::Healer }, { { 14, 37, 0 }, Role::Tank }, { { 11, 5, 35 }, Role::MeleeDps } }} },
            { CLASS_HUNTER,  {{ { { 31, 20, 0 }, Role::RangedDps }, { { 0, 31, 20 }, Role::RangedDps }, { { 0, 20, 31 }, Role::RangedDps } }} },
            { CLASS_ROGUE,   {{ { { 31, 20, 0 }, Role::MeleeDps }, { { 21, 30, 0 }, Role::MeleeDps }, { { 21, 0, 30 }, Role::MeleeDps } }} },
            { CLASS_PRIEST,  {{ { { 31, 20, 0 }, Role::Healer }, { { 21, 30, 0 }, Role::Healer }, { { 5, 0, 46 }, Role::RangedDps } }} },
            { CLASS_SHAMAN,  {{ { { 30, 0, 21 }, Role::RangedDps }, { { 0, 30, 21 }, Role::MeleeDps }, { { 0, 5, 46 }, Role::Healer } }} },
            { CLASS_MAGE,    {{ { { 31, 0, 20 }, Role::RangedDps }, { { 21, 30, 0 }, Role::RangedDps }, { { 21, 0, 30 }, Role::RangedDps } }} },
            { CLASS_WARLOCK, {{ { { 30, 0, 21 }, Role::RangedDps }, { { 0, 31, 20 }, Role::RangedDps }, { { 0, 21, 30 }, Role::RangedDps } }} },
            { CLASS_DRUID,   {{ { { 30, 0, 21 }, Role::RangedDps }, { { 0, 30, 21 }, Role::Tank }, { { 11, 0, 40 }, Role::Healer } }} },
        };
        auto itr = builds.find(classId);
        return itr != builds.end() && tree < 3 ? &itr->second[tree] : nullptr;
    }

    uint32 CompanionFactory::MetaTreeForRole(uint8 classId, Role role)
    {
        switch (classId)
        {
            case CLASS_WARRIOR: return role == Role::Tank ? 2 : 1;
            case CLASS_PALADIN: return role == Role::Tank ? 1 : role == Role::Healer ? 0 : 2;
            case CLASS_HUNTER:  return 1;
            case CLASS_ROGUE:   return 1;
            case CLASS_PRIEST:  return role == Role::Healer ? 1 : 2;
            case CLASS_SHAMAN:  return role == Role::Healer ? 2 : role == Role::RangedDps ? 0 : 1;
            case CLASS_MAGE:    return 2;
            case CLASS_WARLOCK: return 0;
            case CLASS_DRUID:   return role == Role::Healer ? 2 : role == Role::RangedDps ? 0 : 1;
            default:            return 0;
        }
    }

    void CompanionFactory::SpendTalents(Player* bot, Role role)
    {
        if (role == Role::Auto)
            if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                role = ai->GetEffectiveRole();

        auto treesItr = _talentTrees.find(bot->GetClass());
        if (treesItr == _talentTrees.end())
            return;

        int32 configId = *bot->m_activePlayerData->ActiveCombatTraitConfigID;
        UF::TraitConfig const* ufConfig = bot->GetTraitConfig(configId);
        if (!ufConfig)
        {
            TC_LOG_DEBUG("server.worldserver", "Companions: {} has no active talent config", bot->GetName());
            return;
        }

        WorldPackets::Traits::TraitConfig config(*ufConfig);
        auto freePoints = [&]()
        {
            std::map<int32, int32> owned;
            TraitMgr::FillOwnedCurrenciesMap(config, bot, owned);
            std::map<int32, TraitMgr::SpentCurrency> spent;
            TraitMgr::FillSpentCurrenciesMap(config.Entries, spent);
            return owned[CLASSIC_TALENT_CURRENCY] - spent[CLASSIC_TALENT_CURRENCY].Total;
        };

        std::vector<std::string_view> priorities;
        auto priorityOf = [&](TalentNode const& node) -> uint32
        {
            for (uint32 i = 0; i < priorities.size(); ++i)
                if (node.Name == priorities[i])
                    return i;
            return 1000;
        };

        // the role's main tab is the one holding most of its key talents (or the tabs chosen in the web panel)
        std::vector<uint32> chosenTabs;
        if (BotRecord const* record = sCompanionMgr->GetRecord(bot->GetGUID()))
        {
            std::istringstream spec(record->Spec);
            std::string index;
            while (std::getline(spec, index, ','))
                if (!index.empty() && std::isdigit(static_cast<unsigned char>(index[0])))
                    chosenTabs.push_back(uint32(std::stoul(index)));
        }
        // one tree chosen (or none: the role's tree): its meta build, points per tab. Two or more tabs: the older free order.
        MetaBuild const* meta = nullptr;
        if (chosenTabs.size() <= 1)
        {
            meta = MetaBuildFor(bot->GetClass(), chosenTabs.empty() ? MetaTreeForRole(bot->GetClass(), role) : chosenTabs.front());
            if (meta)
            {
                role = meta->ForRole;   // a Fury build takes Fury's key talents, whatever the bot's role
                chosenTabs = { 0, 1, 2 };
                std::stable_sort(chosenTabs.begin(), chosenTabs.end(), [meta](uint32 a, uint32 b) { return meta->Points[a] > meta->Points[b]; });
            }
        }
        priorities = PrioritiesFor(bot->GetClass(), role);
        std::vector<std::pair<uint32, TalentTree const*>> ranked;
        for (TalentTree const& tab : treesItr->second)
        {
            uint32 score = 0;
            for (TalentNode const& node : tab.Nodes)
                if (uint32 p = priorityOf(node); p < 1000)
                    score += 1000 - p;
            ranked.emplace_back(score, &tab);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        if (!chosenTabs.empty())
        {
            std::vector<std::pair<uint32, TalentTree const*>> ordered;
            for (uint32 index : chosenTabs)
                if (index < treesItr->second.size())
                    ordered.emplace_back(0, &treesItr->second[index]);
            for (auto const& entry : ranked)
                if (std::none_of(ordered.begin(), ordered.end(), [&](auto const& o) { return o.second == entry.second; }))
                    ordered.push_back(entry);
            ranked = std::move(ordered);
        }

        auto treeIndex = [&](TalentTree const* tree) { return uint32(tree - treesItr->second.data()); };
        auto spentIn = [&](TalentTree const* tree)
        {
            uint32 points = 0;
            for (WorldPackets::Traits::TraitEntry const& entry : config.Entries)
                for (TalentNode const& node : tree->Nodes)
                    if (entry.TraitNodeID == node.NodeId && entry.TraitNodeEntryID == node.EntryId)
                        points += entry.Rank;
            return points;
        };

        bool changed = false;
        // with a meta build, each tab first gets its share; what a gated tab cannot take goes to the others in a second pass
        for (uint32 pass = 0; pass < (meta ? 2u : 1u); ++pass)
        for (auto const& [score, tree] : ranked)
        {
            uint32 cap = meta && pass == 0 && treeIndex(tree) < 3 ? meta->Points[treeIndex(tree)] : 1000;
            if (cap == 0)
                continue;
            std::vector<TalentNode const*> order;
            for (TalentNode const& node : tree->Nodes)
                order.push_back(&node);
            std::stable_sort(order.begin(), order.end(), [&](TalentNode const* a, TalentNode const* b)
            {
                uint32 pa = priorityOf(*a), pb = priorityOf(*b);
                if (pa != pb)
                    return pa < pb;
                return a->PosY != b->PosY ? a->PosY < b->PosY : a->PosX < b->PosX;
            });

            uint32 guard = 0;
            while (freePoints() > 0 && spentIn(tree) < cap && ++guard < 200)
            {
                bool spentOne = false;
                for (TalentNode const* node : order)
                {
                    WorldPackets::Traits::TraitConfig trial = config;
                    auto entry = std::find_if(trial.Entries.begin(), trial.Entries.end(), [&](WorldPackets::Traits::TraitEntry const& e)
                    {
                        return e.TraitNodeID == node->NodeId && e.TraitNodeEntryID == node->EntryId;
                    });
                    if (entry == trial.Entries.end())
                    {
                        entry = trial.Entries.emplace(trial.Entries.end());
                        entry->TraitNodeID = node->NodeId;
                        entry->TraitNodeEntryID = node->EntryId;
                        entry->Rank = 0;
                        entry->GrantedRanks = 0;
                    }
                    if (entry->Rank + entry->GrantedRanks >= node->MaxRanks)
                        continue;
                    ++entry->Rank;
                    if (TraitMgr::ValidateConfigForCommit(trial, bot) != TraitMgr::LearnResult::Ok)
                        continue;
                    config = std::move(trial);
                    changed = true;
                    spentOne = true;
                    break;
                }
                if (!spentOne)
                    break;  // this tree is full or gated: next tree
            }
        }

        if (changed)
            bot->UpdateTraitConfig(std::move(config), 0, false);
    }

    void CompanionFactory::Respec(Player* bot, Role role)
    {
        bot->ResetTalents(true);
        SpendTalents(bot, role);
        EnsureHunterPet(bot);
    }

    std::vector<CompanionFactory::TalentLine> CompanionFactory::ListTalents(Player* bot) const
    {
        std::vector<TalentLine> lines;
        UF::TraitConfig const* ufConfig = bot->GetTraitConfig(*bot->m_activePlayerData->ActiveCombatTraitConfigID);
        auto treesItr = _talentTrees.find(bot->GetClass());
        if (!ufConfig || treesItr == _talentTrees.end())
            return lines;
        WorldPackets::Traits::TraitConfig config(*ufConfig);
        std::unordered_map<int32, int32> ranks;
        for (WorldPackets::Traits::TraitEntry const& entry : config.Entries)
            ranks[entry.TraitNodeID] += entry.Rank;
        for (uint32 tab = 0; tab < treesItr->second.size(); ++tab)
            for (TalentNode const& node : treesItr->second[tab].Nodes)
                if (auto itr = ranks.find(node.NodeId); itr != ranks.end() && itr->second > 0)
                    lines.push_back({ tab, node.Name, itr->second, node.MaxRanks });
        return lines;
    }

    std::string CompanionFactory::DescribeTalents(Player* bot) const
    {
        UF::TraitConfig const* ufConfig = bot->GetTraitConfig(*bot->m_activePlayerData->ActiveCombatTraitConfigID);
        if (!ufConfig)
            return "no active talent configuration";

        std::unordered_map<int32, std::string> names;
        auto treesItr = _talentTrees.find(bot->GetClass());
        if (treesItr != _talentTrees.end())
            for (TalentTree const& tree : treesItr->second)
                for (TalentNode const& node : tree.Nodes)
                    names[node.NodeId] = node.Name;

        WorldPackets::Traits::TraitConfig config(*ufConfig);
        std::ostringstream ss;
        uint32 total = 0;
        for (WorldPackets::Traits::TraitEntry const& entry : config.Entries)
        {
            if (entry.Rank <= 0)
                continue;
            total += entry.Rank;
            auto name = names.find(entry.TraitNodeID);
            ss << (name != names.end() && !name->second.empty() ? name->second : std::to_string(entry.TraitNodeID)) << " " << entry.Rank << ", ";
        }
        std::string list = ss.str();
        if (list.size() > 2)
            list.resize(list.size() - 2);
        return std::to_string(total) + " points: " + list;
    }

    // ------------------------------------------------------------------ gear

    CompanionFactory::GearItem CompanionFactory::MakeGear(ItemTemplate const* proto)
    {
        BonusData bonus;
        bonus.Initialize(proto);
        return MakeGear(proto, bonus, proto->GetBaseItemLevel());
    }

    // the stats come from the bonus data: the template's, plus what the item rolled (random "of the Bear" stats, bonuses)
    CompanionFactory::GearItem CompanionFactory::MakeGear(ItemTemplate const* proto, BonusData const& bonus, uint32 itemLevel)
    {
        GearItem item;
        uint32 invType = proto->GetInventoryType();
        item.Entry = proto->GetId();
        item.Proto = proto;
        item.InventoryType = invType == INVTYPE_ROBE ? INVTYPE_CHEST : invType;
        item.Class = proto->GetClass();
        item.SubClass = proto->GetSubClass();
        item.Quality = bonus.Quality;
        item.ItemLevel = itemLevel;
        item.ReqLevel = proto->GetBaseRequiredLevel() > 0 ? uint32(proto->GetBaseRequiredLevel()) : 0;

        float points = GetRandomPropertyPoints(itemLevel, bonus.Quality, proto->GetInventoryType(), proto->GetSubClass());
        for (uint32 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        {
            int32 stat = bonus.ItemStatType[i];
            if (stat < 0 || !bonus.StatPercentEditor[i])
                continue;
            float value = float(bonus.StatPercentEditor[i]) * points * 0.0001f;
            if (value > 0.0f)
                item.Stats.emplace_back(stat, value);
        }
        if (item.Class == ITEM_CLASS_ARMOR)
            item.Armor = float(proto->GetArmor(itemLevel));
        if (item.Class == ITEM_CLASS_WEAPON && itemLevel <= 300 && proto->GetDelay())
            item.Dps = proto->GetDPS(itemLevel);
        return item;
    }

    float CompanionFactory::ScoreTemplate(ItemTemplate const* proto, Player* bot, Role role) const
    {
        return proto ? ScoreItem(MakeGear(proto), bot, role) : -1.0f;
    }

    float CompanionFactory::ScoreItem(Item const* item, Player* bot, Role role) const
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        return proto ? ScoreItem(MakeGear(proto, *item->GetBonus(), item->GetItemLevel(bot)), bot, role) : -1.0f;
    }

    float CompanionFactory::ScoreLoot(LootItem const& item, Player* bot, Role role) const
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
        if (!proto)
            return -1.0f;
        // what the item will be once taken: the loot's bonus lists and the random stats it rolled when it dropped
        BonusData bonus;
        bonus.Initialize(proto);
        for (int32 bonusListId : item.BonusListIDs)
            bonus.AddBonusList(bonusListId);
        if (item.randomBonusListId)
            bonus.AddBonusList(item.randomBonusListId);
        return ScoreItem(MakeGear(proto, bonus, uint32(std::max<int32>(1, int32(proto->GetBaseItemLevel()) + bonus.ItemLevelBonus))), bot, role);
    }

    // the equipment slots an item would go to, and what it competes with there
    std::vector<uint8> CompanionFactory::SlotsFor(ItemTemplate const* proto)
    {
        switch (proto->GetInventoryType())
        {
            case INVTYPE_HEAD:          return { EQUIPMENT_SLOT_HEAD };
            case INVTYPE_NECK:          return { EQUIPMENT_SLOT_NECK };
            case INVTYPE_SHOULDERS:     return { EQUIPMENT_SLOT_SHOULDERS };
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:          return { EQUIPMENT_SLOT_CHEST };
            case INVTYPE_WAIST:         return { EQUIPMENT_SLOT_WAIST };
            case INVTYPE_LEGS:          return { EQUIPMENT_SLOT_LEGS };
            case INVTYPE_FEET:          return { EQUIPMENT_SLOT_FEET };
            case INVTYPE_WRISTS:        return { EQUIPMENT_SLOT_WRISTS };
            case INVTYPE_HANDS:         return { EQUIPMENT_SLOT_HANDS };
            case INVTYPE_CLOAK:         return { EQUIPMENT_SLOT_BACK };
            case INVTYPE_FINGER:        return { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 };
            case INVTYPE_2HWEAPON:
            case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND: return { EQUIPMENT_SLOT_MAINHAND };
            case INVTYPE_SHIELD:
            case INVTYPE_HOLDABLE:
            case INVTYPE_WEAPONOFFHAND: return { EQUIPMENT_SLOT_OFFHAND };
            case INVTYPE_RANGED:
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_THROWN:        return { EQUIPMENT_SLOT_RANGED };
            default:                    return { };
        }
    }

    bool CompanionFactory::IsUpgrade(Player* bot, ItemTemplate const* proto, Role role) const
    {
        if (role == Role::Auto)
            if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                role = ai->GetEffectiveRole();
        return IsUpgrade(bot, proto, ScoreTemplate(proto, bot, role), nullptr, role);
    }

    bool CompanionFactory::IsUpgrade(Player* bot, Item const* item, Role role) const
    {
        if (!item || item->IsEquipped())
            return false;
        if (role == Role::Auto)
            if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                role = ai->GetEffectiveRole();
        return IsUpgrade(bot, item->GetTemplate(), ScoreItem(item, bot, role), item, role);
    }

    bool CompanionFactory::IsUpgrade(Player* bot, LootItem const& item, Role role) const
    {
        if (role == Role::Auto)
            if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                role = ai->GetEffectiveRole();
        return IsUpgrade(bot, sObjectMgr->GetItemTemplate(item.itemid), ScoreLoot(item, bot, role), nullptr, role);
    }

    // better than what it wears in that slot (its weaker one for rings) and than any piece for that slot in its bags
    // (self: the item being judged when it is one of them)
    bool CompanionFactory::IsUpgrade(Player* bot, ItemTemplate const* proto, float score, Item const* self, Role role) const
    {
        if (!proto || score <= 0.0f || !CanWear(bot, proto))
            return false;
        std::vector<uint8> slots = SlotsFor(proto);
        if (slots.empty())
            return false;
        // a weapon only against the same kind of weapon (a two-hander does not replace sword and board by itself)
        auto group = [](uint32 invType)
        {
            switch (invType)
            {
                case INVTYPE_WEAPON: case INVTYPE_WEAPONMAINHAND: return 1;
                case INVTYPE_2HWEAPON: return 2;
                case INVTYPE_SHIELD: return 3;
                case INVTYPE_HOLDABLE: return 4;
                case INVTYPE_WEAPONOFFHAND: return 5;
                default: return 0;
            }
        };
        uint32 const kind = group(proto->GetInventoryType());
        float worst = std::numeric_limits<float>::max();
        for (uint8 slot : slots)
        {
            Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!current)
            {
                worst = 0.0f;
                break;
            }
            if (kind != group(current->GetTemplate()->GetInventoryType()))
                return false;
            worst = std::min(worst, ScoreItem(current, bot, role));
        }
        if (score <= worst * 1.05f + 0.5f)
            return false;

        // what it carries: a piece for the same slot in its bags that is at least as good is the one to put on
        bool carriesBetter = false;
        bot->ForEachItem(ItemSearchLocation::Inventory, [&](Item* carried)
        {
            ItemTemplate const* other = carried->GetTemplate();
            if (carried == self || !other || SlotsFor(other) != slots || group(other->GetInventoryType()) != kind
                || !CanWear(bot, other))
                return ItemSearchCallbackResult::Continue;
            float const carriedScore = ScoreItem(carried, bot, role);
            // a loot item as good as one it has is not needed; of two equal pieces in the bags one is the upgrade
            if (carriedScore > score || (carriedScore == score && (!self || carried->GetGUID() < self->GetGUID())))
            {
                carriesBetter = true;
                return ItemSearchCallbackResult::Stop;
            }
            return ItemSearchCallbackResult::Continue;
        });
        return !carriesBetter;
    }

    bool CompanionFactory::CanWear(Player* bot, ItemTemplate const* proto)
    {
        if (!proto || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            return false;
        uint32 const skill = proto->GetSkill();
        return !skill || bot->HasSkill(skill);
    }

    bool CompanionFactory::EquipFromBags(Player* bot, Item* item)
    {
        if (!item || item->IsEquipped())
            return false;
        uint16 dest = 0;
        if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
            return false;
        // a second ring takes the weaker finger
        if (item->GetTemplate()->GetInventoryType() == INVTYPE_FINGER)
        {
            Item* ring1 = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_FINGER1);
            Item* ring2 = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_FINGER2);
            if (ring1 && ring2)
            {
                Role role = Role::Auto;
                if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                    role = ai->GetEffectiveRole();
                uint8 weaker = sCompanionFactory->ScoreItem(ring1, bot, role) <= sCompanionFactory->ScoreItem(ring2, bot, role)
                    ? EQUIPMENT_SLOT_FINGER1 : EQUIPMENT_SLOT_FINGER2;
                dest = (INVENTORY_SLOT_BAG_0 << 8) | weaker;
            }
        }
        bot->SwapItem(item->GetPos(), dest);
        return bot->GetItemByPos(dest) == item;
    }

    float CompanionFactory::ScoreItem(GearItem const& item, Player* bot, Role role) const
    {
        uint8 classId = bot->GetClass();
        float score = 0.0f;
        for (auto const& [stat, value] : item.Stats)
            score += StatWeight(stat, classId, role) * value;

        bool tank = role == Role::Tank;
        score += item.Armor * (tank ? 0.06f : 0.01f);

        bool caster = role == Role::Healer || (role == Role::RangedDps && classId != CLASS_HUNTER);
        if (item.Class == ITEM_CLASS_WEAPON && !caster)
        {
            bool rangedSlot = item.InventoryType == INVTYPE_RANGED || item.InventoryType == INVTYPE_RANGEDRIGHT || item.InventoryType == INVTYPE_THROWN;
            float dpsWeight = classId == CLASS_HUNTER ? (rangedSlot ? 4.0f : 1.0f) : (rangedSlot ? 0.3f : 3.0f);
            score += item.Dps * dpsWeight;
        }

        if (item.Class == ITEM_CLASS_ARMOR && item.SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH && item.SubClass <= ITEM_SUBCLASS_ARMOR_PLATE
            && item.InventoryType != INVTYPE_CLOAK && item.SubClass != BestArmorSubClass(classId, bot->GetLevel()))
            score *= 0.6f;     // a real player wears his best armor type

        // a little item level so a stat-less item still beats nothing
        return score + float(item.ItemLevel) * 0.05f + float(item.Quality) * 0.5f;
    }

    CompanionFactory::GearItem const* CompanionFactory::BestFor(Player* bot, Role role, std::vector<uint32> const& invTypes,
        std::vector<uint32> const& subClasses, uint32 exclude) const
    {
        GearItem const* best = nullptr;
        float bestScore = -1.0f;
        uint32 maxQuality = GetConfig().GearMaxQuality;
        if (BotRecord const* record = sCompanionMgr->GetRecord(bot->GetGUID()); record && record->GearQuality)
            maxQuality = record->GearQuality;
        else if (CompanionAI const* ai = sCompanionMgr->GetAI(bot); ai && ai->IsAmbient())
            maxQuality = GetConfig().AmbientGearQuality;     // the world's own adventurers: mostly greens
        for (uint32 invType : invTypes)
        {
            auto itr = _gearByInvType.find(invType);
            if (itr == _gearByInvType.end())
                continue;
            for (GearItem const& item : itr->second)
            {
                if (item.Entry == exclude || item.ReqLevel > bot->GetLevel() || item.Quality > maxQuality)
                    continue;
                if (!subClasses.empty() && std::find(subClasses.begin(), subClasses.end(), item.SubClass) == subClasses.end())
                    continue;
                if (!CanWear(bot, item.Proto))
                    continue;
                float score = ScoreItem(item, bot, role);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = &item;
                }
            }
        }
        return best;
    }

    bool CompanionFactory::EquipEntry(Player* bot, uint8 slot, uint32 entry, Role role)
    {
        Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (current && current->GetEntry() == entry)
            return true;
        // something looted or given that is at least as good stays
        if (current && role != Role::Auto && ScoreItem(current, bot, role) >= ScoreTemplate(sObjectMgr->GetItemTemplate(entry), bot, role))
            return true;
        if (current)
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        uint16 dest = 0;
        if (bot->CanEquipNewItem(slot, dest, entry, false) != EQUIP_ERR_OK)
            return false;
        return bot->EquipNewItem(dest, entry, ItemContext::NONE, true) != nullptr;
    }

    void CompanionFactory::EquipBestGear(Player* bot, Role role)
    {
        if (role == Role::Auto)
            if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                role = ai->GetEffectiveRole();

        uint8 classId = bot->GetClass();
        struct ArmorSlot { uint8 Slot; uint32 InvType; };
        static ArmorSlot const armorSlots[] =
        {
            { EQUIPMENT_SLOT_HEAD, INVTYPE_HEAD }, { EQUIPMENT_SLOT_NECK, INVTYPE_NECK }, { EQUIPMENT_SLOT_SHOULDERS, INVTYPE_SHOULDERS },
            { EQUIPMENT_SLOT_CHEST, INVTYPE_CHEST }, { EQUIPMENT_SLOT_WAIST, INVTYPE_WAIST }, { EQUIPMENT_SLOT_LEGS, INVTYPE_LEGS },
            { EQUIPMENT_SLOT_FEET, INVTYPE_FEET }, { EQUIPMENT_SLOT_WRISTS, INVTYPE_WRISTS }, { EQUIPMENT_SLOT_HANDS, INVTYPE_HANDS },
            { EQUIPMENT_SLOT_BACK, INVTYPE_CLOAK },
        };

        for (ArmorSlot const& slot : armorSlots)
            if (GearItem const* item = BestFor(bot, role, { slot.InvType }, { }))
                EquipEntry(bot, slot.Slot, item->Entry, role);

        if (GearItem const* ring = BestFor(bot, role, { INVTYPE_FINGER }, { }))
        {
            EquipEntry(bot, EQUIPMENT_SLOT_FINGER1, ring->Entry, role);
            if (GearItem const* second = BestFor(bot, role, { INVTYPE_FINGER }, { }, ring->Entry))
                EquipEntry(bot, EQUIPMENT_SLOT_FINGER2, second->Entry, role);
        }

        // weapons: compare the layouts this class and role may use
        std::vector<uint32> const oneHand = { INVTYPE_WEAPON, INVTYPE_WEAPONMAINHAND };
        std::vector<uint32> twoHandTypes;
        std::vector<uint32> oneHandTypes;
        bool shield = false, holdable = false, dualWield = false;
        bool caster = role == Role::Healer || (role == Role::RangedDps && classId != CLASS_HUNTER);
        switch (classId)
        {
            case CLASS_WARRIOR:
                if (role == Role::Tank) { oneHandTypes = { ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_FIST_WEAPON, ITEM_SUBCLASS_WEAPON_DAGGER }; shield = true; }
                else twoHandTypes = { ITEM_SUBCLASS_WEAPON_SWORD2, ITEM_SUBCLASS_WEAPON_AXE2, ITEM_SUBCLASS_WEAPON_MACE2, ITEM_SUBCLASS_WEAPON_POLEARM };
                break;
            case CLASS_PALADIN:
                if (role == Role::MeleeDps) twoHandTypes = { ITEM_SUBCLASS_WEAPON_SWORD2, ITEM_SUBCLASS_WEAPON_AXE2, ITEM_SUBCLASS_WEAPON_MACE2, ITEM_SUBCLASS_WEAPON_POLEARM };
                else { oneHandTypes = { ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_AXE }; shield = true; holdable = role == Role::Healer; }
                break;
            case CLASS_HUNTER:
                twoHandTypes = { ITEM_SUBCLASS_WEAPON_POLEARM, ITEM_SUBCLASS_WEAPON_STAFF, ITEM_SUBCLASS_WEAPON_SWORD2, ITEM_SUBCLASS_WEAPON_AXE2 };
                break;
            case CLASS_ROGUE:
                oneHandTypes = { ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_FIST_WEAPON };
                dualWield = true;
                break;
            case CLASS_SHAMAN:
                if (role == Role::MeleeDps) twoHandTypes = { ITEM_SUBCLASS_WEAPON_AXE2, ITEM_SUBCLASS_WEAPON_MACE2, ITEM_SUBCLASS_WEAPON_STAFF };
                else { oneHandTypes = { ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_FIST_WEAPON }; shield = true; twoHandTypes = { ITEM_SUBCLASS_WEAPON_STAFF }; }
                break;
            case CLASS_DRUID:
                twoHandTypes = { ITEM_SUBCLASS_WEAPON_STAFF, ITEM_SUBCLASS_WEAPON_MACE2 };
                if (caster) { oneHandTypes = { ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_DAGGER }; holdable = true; }
                break;
            default:    // priest, mage, warlock
                twoHandTypes = { ITEM_SUBCLASS_WEAPON_STAFF };
                oneHandTypes = { ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_SWORD };
                holdable = true;
                break;
        }

        float bestScore = -1.0f;
        uint32 bestMain = 0, bestOff = 0;
        if (!twoHandTypes.empty())
            if (GearItem const* twoHand = BestFor(bot, role, { INVTYPE_2HWEAPON }, twoHandTypes))
            {
                bestScore = ScoreItem(*twoHand, bot, role);
                bestMain = twoHand->Entry;
            }
        if (!oneHandTypes.empty())
        {
            if (GearItem const* main = BestFor(bot, role, oneHand, oneHandTypes))
            {
                float score = ScoreItem(*main, bot, role);
                GearItem const* off = nullptr;
                if (dualWield && bot->CanDualWield())
                    off = BestFor(bot, role, { INVTYPE_WEAPON, INVTYPE_WEAPONOFFHAND }, oneHandTypes, main->Entry);
                else
                {
                    GearItem const* shieldItem = shield ? BestFor(bot, role, { INVTYPE_SHIELD }, { }) : nullptr;
                    GearItem const* heldItem = holdable ? BestFor(bot, role, { INVTYPE_HOLDABLE }, { }) : nullptr;
                    off = shieldItem;
                    if (heldItem && (!off || ScoreItem(*heldItem, bot, role) > ScoreItem(*off, bot, role)))
                        off = heldItem;
                }
                if (off)
                    score += ScoreItem(*off, bot, role) * (dualWield ? 0.6f : 1.0f);
                // tanks always want their shield
                if (score > bestScore || (role == Role::Tank && shield && off))
                {
                    bestScore = score;
                    bestMain = main->Entry;
                    bestOff = off ? off->Entry : 0;
                }
            }
        }

        // the weapons worn now (looted, given) stay when they are at least as good as the pool's
        if (Item* mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND); mainHand && bestMain)
        {
            float worn = ScoreItem(mainHand, bot, role);
            if (Item* offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                worn += ScoreItem(offHand, bot, role) * (dualWield ? 0.6f : 1.0f);
            if (worn >= bestScore && mainHand->GetEntry() != bestMain)
                bestMain = 0;
        }

        if (bestMain)
        {
            if (!bestOff)
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
            else if (Item* current = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
                if (current->GetTemplate()->GetInventoryType() == INVTYPE_2HWEAPON)
                    bot->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND, true);
            EquipEntry(bot, EQUIPMENT_SLOT_MAINHAND, bestMain);
            if (bestOff)
                EquipEntry(bot, EQUIPMENT_SLOT_OFFHAND, bestOff);
        }

        // ranged slot
        std::vector<uint32> rangedTypes;
        std::vector<uint32> rangedSub;
        switch (classId)
        {
            case CLASS_HUNTER:
                rangedTypes = { INVTYPE_RANGED, INVTYPE_RANGEDRIGHT };
                rangedSub = { ITEM_SUBCLASS_WEAPON_BOW, ITEM_SUBCLASS_WEAPON_GUN, ITEM_SUBCLASS_WEAPON_CROSSBOW };
                break;
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
                rangedTypes = { INVTYPE_RANGED, INVTYPE_RANGEDRIGHT, INVTYPE_THROWN };
                rangedSub = { ITEM_SUBCLASS_WEAPON_BOW, ITEM_SUBCLASS_WEAPON_GUN, ITEM_SUBCLASS_WEAPON_CROSSBOW, ITEM_SUBCLASS_WEAPON_THROWN };
                break;
            case CLASS_PRIEST:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                rangedTypes = { INVTYPE_RANGEDRIGHT };
                rangedSub = { ITEM_SUBCLASS_WEAPON_WAND };
                break;
            default:
                break;
        }
        if (!rangedTypes.empty())
            if (GearItem const* ranged = BestFor(bot, role, rangedTypes, rangedSub))
                EquipEntry(bot, EQUIPMENT_SLOT_RANGED, ranged->Entry, role);

        GiveAmmo(bot);
        bot->UpdateAllStats();
    }

    // ------------------------------------------------------------------ the world's adventurers (ambient bots)

    namespace
    {
        // do these stats suit the class and role? (a warrior does not wear spirit, a mage no strength)
        bool StatsFit(std::vector<std::pair<int32, float>> const& stats, uint8 classId, Role role, float* value = nullptr)
        {
            float good = 0.0f, bad = 0.0f;
            for (auto const& [stat, amount] : stats)
            {
                float weight = StatWeight(stat, classId, role);
                if (weight >= 0.3f)
                    good += amount * weight;
                else if (stat == ITEM_MOD_STRENGTH || stat == ITEM_MOD_AGILITY || stat == ITEM_MOD_INTELLECT || stat == ITEM_MOD_SPIRIT
                    || stat == ITEM_MOD_SPELL_POWER || stat == ITEM_MOD_SPELL_DAMAGE_DONE || stat == ITEM_MOD_SPELL_HEALING_DONE
                    || stat == ITEM_MOD_ATTACK_POWER || stat == ITEM_MOD_DEFENSE_SKILL_RATING)
                    bad += amount;
            }
            if (value)
                *value = good;
            return bad <= 0.0f || good >= bad * 2.0f;
        }

        // the stats a random enchantment ("of the Bear") gives
        std::vector<std::pair<int32, float>> BonusStats(ItemRandomBonusListId bonusList)
        {
            std::vector<std::pair<int32, float>> stats;
            for (ItemBonusEntry const* bonus : ItemBonusMgr::GetItemBonuses(bonusList))
                if (bonus->Type == ITEM_BONUS_STAT)
                    stats.emplace_back(bonus->Value[0], float(std::max(1, bonus->Value[1])));
            return stats;
        }
    }

    std::vector<CompanionFactory::GearItem const*> CompanionFactory::AmbientCandidates(Player* bot, Role role, std::vector<uint32> const& invTypes,
        std::vector<uint32> const& subClasses, uint32 quality, bool armorType, uint32 exclude) const
    {
        std::vector<GearItem const*> out;
        uint32 const level = bot->GetLevel();
        uint8 const classId = bot->GetClass();
        // of its level: what a player of that level would wear (required level up to 8 below, or the item level near it)
        uint32 const lowReq = level > 8 ? level - 8 : 0;
        for (uint32 invType : invTypes)
        {
            auto itr = _gearByInvType.find(invType);
            if (itr == _gearByInvType.end())
                continue;
            for (GearItem const& item : itr->second)
            {
                if (item.Entry == exclude || item.Quality != quality || item.ReqLevel > level)
                    continue;
                if (item.ReqLevel && item.ReqLevel < lowReq)
                    continue;
                // the item's own level too: "required level 1" items of item level 24 are not for a level 7, holiday hats of
                // item level 1 not for anyone past the first levels
                if (item.ItemLevel > level + 6 || item.ItemLevel + 6 < level || (level >= 6 && item.ItemLevel < 5))
                    continue;
                if (!subClasses.empty() && std::find(subClasses.begin(), subClasses.end(), item.SubClass) == subClasses.end())
                    continue;
                if (armorType && item.Class == ITEM_CLASS_ARMOR && item.SubClass != BestArmorSubClass(classId, uint8(level)))
                    continue;
                if (!CanWear(bot, item.Proto))
                    continue;
                // stats of its class; items with a random enchantment get one that fits when they are made
                if (!item.Stats.empty() && !StatsFit(item.Stats, classId, role))
                    continue;
                if (item.Stats.empty() && !item.Proto->RandomBonusListTemplateId && item.Class == ITEM_CLASS_ARMOR && item.Armor <= 0.0f)
                    continue;
                out.push_back(&item);
            }
        }
        return out;
    }

    // made, given an enchantment that suits it, put on
    bool CompanionFactory::EquipRolled(Player* bot, uint8 slot, GearItem const& gear, Role role)
    {
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        uint16 dest = 0;
        if (bot->CanEquipNewItem(slot, dest, gear.Entry, false) != EQUIP_ERR_OK)
            return false;
        ItemRandomBonusListId chosen = 0;
        if (gear.Proto->RandomBonusListTemplateId)
        {
            float best = -1.0f;
            for (uint32 roll = 0; roll < 16; ++roll)
            {
                ItemRandomBonusListId id = GenerateItemRandomBonusListId(gear.Entry);
                if (!id)
                    break;
                float value = 0.0f;
                if (StatsFit(BonusStats(id), bot->GetClass(), role, &value) && value > best)
                {
                    best = value;
                    chosen = id;
                }
            }
            if (!chosen)
                return false;   // no enchantment of this one suits it: the caller picks another item
        }
        Item* item = Item::CreateItem(gear.Entry, 1, ItemContext::NONE, bot);
        if (!item)
            return false;
        if (chosen)
            item->SetItemRandomBonusList(chosen);
        if (!bot->EquipItem(dest, item, true))
        {
            delete item;
            return false;
        }
        return true;
    }

    void CompanionFactory::DressAmbient(Player* bot, Role role)
    {
        if (role == Role::Auto)
            if (CompanionAI* ai = sCompanionMgr->GetAI(bot))
                role = ai->GetEffectiveRole();
        uint8 const classId = bot->GetClass();
        bool const capped = bot->GetLevel() >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

        // a fresh start: what it wore and carried in its last life goes (the shirt stays)
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD && bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (item->GetTemplate()->GetClass() == ITEM_CLASS_WEAPON || item->GetTemplate()->GetClass() == ITEM_CLASS_ARMOR)
                    bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

        // one item for a slot: greens of its level and kind (a blue now and then at the level cap; white when no green exists,
        // at the lowest levels), at random with a lean towards its own level
        auto pick = [&](std::vector<uint32> const& invTypes, std::vector<uint32> const& subClasses, bool armorType, uint32 exclude) -> GearItem const*
        {
            std::vector<uint32> qualities = { ITEM_QUALITY_UNCOMMON };
            if (capped && urand(0, 99) < 35)
                qualities.insert(qualities.begin(), ITEM_QUALITY_RARE);
            if (bot->GetLevel() < 15)
                qualities.push_back(ITEM_QUALITY_NORMAL);
            for (uint32 quality : qualities)
            {
                std::vector<GearItem const*> candidates = AmbientCandidates(bot, role, invTypes, subClasses, quality, armorType, exclude);
                if (candidates.empty())
                    continue;
                uint32 lowest = std::numeric_limits<uint32>::max();
                for (GearItem const* c : candidates)
                    lowest = std::min(lowest, c->ItemLevel);
                std::vector<double> weights;
                for (GearItem const* c : candidates)
                    weights.push_back(1.0 + double(c->ItemLevel - lowest));
                return *Trinity::Containers::SelectRandomWeightedContainerElement(candidates, std::span<double>(weights));
            }
            return nullptr;
        };
        auto put = [&](uint8 slot, std::vector<uint32> const& invTypes, std::vector<uint32> const& subClasses, bool armorType, uint32 exclude = 0) -> uint32
        {
            for (uint32 attempt = 0; attempt < 6; ++attempt)
                if (GearItem const* item = pick(invTypes, subClasses, armorType, exclude))
                    if (EquipRolled(bot, slot, *item, role))
                        return item->Entry;
            return 0;
        };

        put(EQUIPMENT_SLOT_HEAD, { INVTYPE_HEAD }, { }, true);
        put(EQUIPMENT_SLOT_NECK, { INVTYPE_NECK }, { }, false);
        put(EQUIPMENT_SLOT_SHOULDERS, { INVTYPE_SHOULDERS }, { }, true);
        put(EQUIPMENT_SLOT_CHEST, { INVTYPE_CHEST, INVTYPE_ROBE }, { }, true);
        put(EQUIPMENT_SLOT_WAIST, { INVTYPE_WAIST }, { }, true);
        put(EQUIPMENT_SLOT_LEGS, { INVTYPE_LEGS }, { }, true);
        put(EQUIPMENT_SLOT_FEET, { INVTYPE_FEET }, { }, true);
        put(EQUIPMENT_SLOT_WRISTS, { INVTYPE_WRISTS }, { }, true);
        put(EQUIPMENT_SLOT_HANDS, { INVTYPE_HANDS }, { }, true);
        put(EQUIPMENT_SLOT_BACK, { INVTYPE_CLOAK }, { }, false);
        uint32 ring = put(EQUIPMENT_SLOT_FINGER1, { INVTYPE_FINGER }, { }, false);
        put(EQUIPMENT_SLOT_FINGER2, { INVTYPE_FINGER }, { }, false, ring);

        // weapons: one of the layouts its class and role use, chosen at random
        using W = std::vector<uint32>;
        W const twoHandMelee = { ITEM_SUBCLASS_WEAPON_SWORD2, ITEM_SUBCLASS_WEAPON_AXE2, ITEM_SUBCLASS_WEAPON_MACE2, ITEM_SUBCLASS_WEAPON_POLEARM };
        W const oneHandMelee = { ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_FIST_WEAPON };
        W const casterOneHand = { ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_SWORD };
        W const oneHand = { INVTYPE_WEAPON, INVTYPE_WEAPONMAINHAND };
        enum class Layout { TwoHand, Staff, OneAndShield, OneAndHeld, Dual };
        std::vector<Layout> layouts;
        W mainTypes;
        switch (classId)
        {
            case CLASS_WARRIOR:
                if (role == Role::Tank)
                    layouts = { Layout::OneAndShield };
                else
                    layouts = bot->CanDualWield() ? std::vector<Layout>{ Layout::TwoHand, Layout::TwoHand, Layout::Dual } : std::vector<Layout>{ Layout::TwoHand };
                mainTypes = oneHandMelee;
                break;
            case CLASS_PALADIN:
                layouts = role == Role::MeleeDps ? std::vector<Layout>{ Layout::TwoHand } : std::vector<Layout>{ Layout::OneAndShield };
                mainTypes = { ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_AXE };
                break;
            case CLASS_HUNTER:
                layouts = { Layout::TwoHand, Layout::TwoHand, Layout::Staff };
                break;
            case CLASS_ROGUE:
                layouts = { Layout::Dual };
                mainTypes = { ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_SWORD, ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_FIST_WEAPON };
                break;
            case CLASS_SHAMAN:
                layouts = role == Role::MeleeDps ? std::vector<Layout>{ Layout::TwoHand, Layout::Staff } : std::vector<Layout>{ Layout::OneAndShield, Layout::Staff };
                mainTypes = { ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_AXE, ITEM_SUBCLASS_WEAPON_DAGGER, ITEM_SUBCLASS_WEAPON_FIST_WEAPON };
                break;
            case CLASS_DRUID:
                layouts = { Layout::Staff, Layout::Staff, Layout::OneAndHeld };
                mainTypes = { ITEM_SUBCLASS_WEAPON_MACE, ITEM_SUBCLASS_WEAPON_DAGGER };
                break;
            default:    // priest, mage, warlock
                layouts = { Layout::Staff, Layout::OneAndHeld };
                mainTypes = casterOneHand;
                break;
        }
        Layout layout = layouts[urand(0, layouts.size() - 1)];
        bool weapon = false;
        switch (layout)
        {
            case Layout::TwoHand:
            {
                W types = classId == CLASS_SHAMAN ? W{ ITEM_SUBCLASS_WEAPON_AXE2, ITEM_SUBCLASS_WEAPON_MACE2 }
                    : classId == CLASS_HUNTER ? W{ ITEM_SUBCLASS_WEAPON_POLEARM, ITEM_SUBCLASS_WEAPON_SWORD2, ITEM_SUBCLASS_WEAPON_AXE2 } : twoHandMelee;
                weapon = put(EQUIPMENT_SLOT_MAINHAND, { INVTYPE_2HWEAPON }, types, false) != 0;
                break;
            }
            case Layout::Staff:
                weapon = put(EQUIPMENT_SLOT_MAINHAND, { INVTYPE_2HWEAPON }, { ITEM_SUBCLASS_WEAPON_STAFF }, false) != 0;
                break;
            case Layout::OneAndShield:
                weapon = put(EQUIPMENT_SLOT_MAINHAND, oneHand, mainTypes, false) != 0;
                put(EQUIPMENT_SLOT_OFFHAND, { INVTYPE_SHIELD }, { }, false);
                break;
            case Layout::OneAndHeld:
                weapon = put(EQUIPMENT_SLOT_MAINHAND, oneHand, mainTypes, false) != 0;
                put(EQUIPMENT_SLOT_OFFHAND, { INVTYPE_HOLDABLE }, { }, false);
                break;
            case Layout::Dual:
            {
                uint32 main = put(EQUIPMENT_SLOT_MAINHAND, oneHand, mainTypes, false);
                weapon = main != 0;
                put(EQUIPMENT_SLOT_OFFHAND, { INVTYPE_WEAPON, INVTYPE_WEAPONOFFHAND }, mainTypes, false, main);
                break;
            }
        }
        // nothing of that layout at its level: whatever one-hander or staff fits
        if (!weapon && !put(EQUIPMENT_SLOT_MAINHAND, oneHand, { }, false))
            put(EQUIPMENT_SLOT_MAINHAND, { INVTYPE_2HWEAPON }, { }, false);

        switch (classId)
        {
            case CLASS_HUNTER:
                put(EQUIPMENT_SLOT_RANGED, { INVTYPE_RANGED, INVTYPE_RANGEDRIGHT },
                    { ITEM_SUBCLASS_WEAPON_BOW, ITEM_SUBCLASS_WEAPON_GUN, ITEM_SUBCLASS_WEAPON_CROSSBOW }, false);
                break;
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
                put(EQUIPMENT_SLOT_RANGED, { INVTYPE_RANGED, INVTYPE_RANGEDRIGHT, INVTYPE_THROWN },
                    { ITEM_SUBCLASS_WEAPON_BOW, ITEM_SUBCLASS_WEAPON_GUN, ITEM_SUBCLASS_WEAPON_CROSSBOW, ITEM_SUBCLASS_WEAPON_THROWN }, false);
                break;
            case CLASS_PRIEST:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                put(EQUIPMENT_SLOT_RANGED, { INVTYPE_RANGEDRIGHT }, { ITEM_SUBCLASS_WEAPON_WAND }, false);
                break;
            default:
                break;
        }
        GiveAmmo(bot);
        bot->UpdateAllStats();
    }

    std::string CompanionFactory::DescribeGear(Player* bot) const
    {
        uint32 total = 0, count = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                total += item->GetItemLevel(bot);
                ++count;
            }
        return std::to_string(count) + " items, average item level " + std::to_string(count ? total / count : 0);
    }

    void CompanionFactory::EquipBags(Player* bot)
    {
        if (_bags.empty())
            return;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;
            for (uint32 entry : _bags)
            {
                uint16 dest = 0;
                if (bot->CanEquipNewItem(slot, dest, entry, false) == EQUIP_ERR_OK && bot->EquipNewItem(dest, entry, ItemContext::NONE, true))
                    break;
            }
        }
    }

    void CompanionFactory::GiveAmmo(Player* bot)
    {
        Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!ranged || ranged->GetTemplate()->GetClass() != ITEM_CLASS_WEAPON)
            return;
        uint32 sub = ranged->GetTemplate()->GetSubClass();
        int32 kind = (sub == ITEM_SUBCLASS_WEAPON_BOW || sub == ITEM_SUBCLASS_WEAPON_CROSSBOW) ? 0 : (sub == ITEM_SUBCLASS_WEAPON_GUN ? 1 : -1);
        if (kind < 0)
            return;

        uint32 best = 0;
        for (uint32 entry : _ammo[kind])
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (proto && proto->GetBaseRequiredLevel() <= int32(bot->GetLevel()) && proto->GetQuality() <= ITEM_QUALITY_UNCOMMON
                && bot->CanUseItem(proto) == EQUIP_ERR_OK)
                best = entry;
        }
        if (!best)
            return;
        if (bot->GetItemCount(best) < 400)
            bot->AddItem(best, 600 - bot->GetItemCount(best));
        if (bot->GetAmmoId() != best)
            bot->SetAmmo(best);
    }

    void CompanionFactory::RestockConsumables(Player* bot)
    {
        if (!GetConfig().FreeReagents)
            return;

        GiveAmmo(bot);

        switch (bot->GetClass())
        {
            case CLASS_WARLOCK:
                if (bot->GetItemCount(6265) < 5)    // Soul Shard
                    bot->AddItem(6265, 5 - bot->GetItemCount(6265));
                break;
            case CLASS_SHAMAN:
                for (uint32 totem : { 5175u, 5176u, 5177u, 5178u })   // Earth, Fire, Water, Air Totem
                    if (!bot->HasItemCount(totem, 1))
                        bot->AddItem(totem, 1);
                break;
            case CLASS_HUNTER:
                if (Pet* pet = bot->GetPet())
                    if (pet->IsAlive() && pet->getPetType() == HUNTER_PET)
                        pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(POWER_HAPPINESS));
                break;
            default:
                break;
        }
    }

    void CompanionFactory::EnsureHunterPet(Player* bot)
    {
        if (bot->GetClass() != CLASS_HUNTER || bot->GetLevel() < 10 || !bot->IsAlive() || !bot->IsInWorld() || _petEntries.empty())
            return;
        if (!bot->GetPetGUID().IsEmpty() || bot->GetTemporaryUnsummonedPetNumber())
            return;
        // a stabled/current pet exists: let Call Pet bring it
        if (PetStable const* stable = bot->GetPetStable())
            if (stable->GetCurrentPet() || std::any_of(stable->ActivePets.begin(), stable->ActivePets.end(), [](auto const& p) { return p.has_value(); }))
                return;

        uint32 entry = _petEntries[urand(0, std::min<uint32>(_petEntries.size(), 40) - 1)];
        Pet* pet = bot->CreateTamedPetFrom(entry, 1515);
        if (!pet)
            return;
        pet->GetMap()->AddToMap(pet->ToCreature());
        bot->SetMinion(pet, true);
        pet->SavePetToDB(PET_SAVE_AS_CURRENT);
        bot->PetSpellInitialize();
        TC_LOG_INFO("server.worldserver", "Companions: {} tamed a {}", bot->GetName(), pet->GetName());
    }

    uint32 CompanionFactory::PickRefreshment(std::vector<Refreshment> const& table, uint8 level)
    {
        Refreshment const* best = nullptr;
        Refreshment const* lowest = nullptr;
        for (Refreshment const& entry : table)
        {
            if (!lowest || entry.ReqLevel < lowest->ReqLevel)
                lowest = &entry;
            if (entry.ReqLevel > level)
                continue;
            if (!best || entry.Restores > best->Restores || (entry.Restores == best->Restores && entry.ReqLevel > best->ReqLevel))
                best = &entry;
        }
        if (!best)
            best = lowest;
        return best ? best->Spell : 0;
    }
}
