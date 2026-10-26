/*
 * Companions: makes bots ready to play: character creation, level, trainer spells, talents, gear, consumables
 */

#ifndef COMPANION_FACTORY_H
#define COMPANION_FACTORY_H

#include "AsyncCallbackProcessor.h"
#include "CompanionDefines.h"
#include "DatabaseEnvFwd.h"
#include "Transaction.h"
#include <functional>
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
struct ChrCustomizationChoiceEntry;
struct ItemTemplate;

namespace Companions
{
    class CompanionFactory
    {
    public:
        static CompanionFactory* instance();

        void Initialize();  // world startup: trainer spells, gear pool, food, talent trees

        // a new character on the companion account, same faction as the owner; race 0 / gender > 1 = random.
        // The save is asynchronous: afterSave runs on the world thread once it is in the database.
        ObjectGuid CreateCharacter(Player* owner, uint8 classId, uint8 race, uint8 gender, std::string name, std::string& error,
            std::function<void(ObjectGuid guid, bool saved)> afterSave);
        // same for an owner who may be offline; looks = (customization option, choice) pairs, empty = random
        ObjectGuid CreateCharacter(Team team, uint32 build, uint8 classId, uint8 race, uint8 gender, std::string name,
            std::vector<std::pair<uint32, uint32>> const& looks, std::string& error, std::function<void(ObjectGuid guid, bool saved)> afterSave);
        std::string RandomFreeName() const;
        // a customization choice a new character of this race may pick (the unlock checks of character creation)
        static bool IsChoiceAvailable(ChrCustomizationChoiceEntry const* choice, uint8 race);
        // talent tabs of a class: number of talents and the first talent names, for the web panel
        std::vector<std::pair<uint32, std::string>> DescribeTabs(uint8 classId) const;
        // the talents it has, by tab: (tab, name, rank, max rank)
        struct TalentLine { uint32 Tab; std::string Name; int32 Rank; int32 MaxRank; };
        std::vector<TalentLine> ListTalents(Player* bot) const;
        // one build per talent tree, the way it was played in classic raids: points per tab, e.g. Fury warrior 17/34/0
        struct MetaBuild { uint32 Points[3]; Role ForRole; };
        static MetaBuild const* MetaBuildFor(uint8 classId, uint32 tree);
        // loot: worth a Need roll (clearly better than what it wears there), and putting it on from the bags
        float ScoreTemplate(ItemTemplate const* proto, Player* bot, Role role) const;
        bool IsUpgrade(Player* bot, ItemTemplate const* proto, Role role) const;
        static bool EquipFromBags(Player* bot, Item* item);
        static uint32 MetaTreeForRole(uint8 classId, Role role);
        void Update();      // world thread
        bool IsClassicCombo(uint8 race, uint8 classId) const;
        std::vector<uint8> RacesFor(Team team, uint8 classId) const;

        // everything a freshly logged in bot needs; level 0 = keep the current level
        void Prepare(Player* bot, uint8 level, Role role, bool regear);
        void LevelTo(Player* bot, uint8 level);
        void TrainSpells(Player* bot);
        void SpendTalents(Player* bot, Role role);
        void Respec(Player* bot, Role role);
        void EquipBestGear(Player* bot, Role role);
        void RestockConsumables(Player* bot);
        void EnsureHunterPet(Player* bot);
        std::string DescribeTalents(Player* bot) const;
        std::string DescribeGear(Player* bot) const;

        uint32 GetFoodSpell(uint8 level) const { return PickByLevel(_foodSpells, level); }
        uint32 GetDrinkSpell(uint8 level) const { return PickByLevel(_drinkSpells, level); }

    private:
        CompanionFactory() = default;

        struct TrainerSpell
        {
            uint32 SpellId = 0;
            uint8 ReqLevel = 0;
            uint32 ReqSkillLine = 0;
            uint32 ReqSkillRank = 0;
            uint32 ReqAbility[3] = { };
        };

        struct GearItem
        {
            uint32 Entry = 0;
            ItemTemplate const* Proto = nullptr;
            uint32 InventoryType = 0;
            uint32 Class = 0;
            uint32 SubClass = 0;
            uint32 Quality = 0;
            uint32 ItemLevel = 0;
            uint32 ReqLevel = 0;
            std::vector<std::pair<int32, float>> Stats;     // ItemModType, value
            float Armor = 0.0f;
            float Dps = 0.0f;
        };

        struct TalentNode
        {
            int32 NodeId = 0;
            int32 EntryId = 0;
            int32 MaxRanks = 1;
            int32 PosX = 0;
            int32 PosY = 0;
            int32 TreeId = 0;
            int32 SubTreeId = 0;
            std::string Name;   // lower case
        };

        struct TalentTree   // one talent tab
        {
            int32 TraitTreeId = 0;
            std::vector<TalentNode> Nodes;
        };

        void LoadTrainerSpells();
        void LoadGearPool();
        void LoadTalentTrees();
        float ScoreItem(GearItem const& item, Player* bot, Role role) const;
        static GearItem MakeGear(ItemTemplate const* proto);
        static std::vector<uint8> SlotsFor(ItemTemplate const* proto);
        GearItem const* BestFor(Player* bot, Role role, std::vector<uint32> const& invTypes, std::vector<uint32> const& subClasses,
            uint32 exclude = 0) const;
        bool EquipEntry(Player* bot, uint8 slot, uint32 entry, Role role = Role::Auto);   // a role: keeps a better worn item
        void LearnProficiencies(Player* bot);
        void LearnQuestSpells(Player* bot);
        void MaxWeaponSkills(Player* bot);
        void EquipBags(Player* bot);
        void GiveAmmo(Player* bot);
        static uint32 PickByLevel(std::map<uint32, uint32> const& table, uint8 level);
        std::string RandomName() const;
        static std::vector<std::string_view> PrioritiesFor(uint8 classId, Role role);

        std::vector<TrainerSpell> _trainerSpells;
        std::unordered_map<uint32, std::vector<GearItem>> _gearByInvType;
        std::vector<uint32> _ammo[2];                   // arrows, bullets (sorted by level)
        std::vector<uint32> _bags;                      // best first
        std::map<uint32, uint32> _foodSpells;           // required level -> spell
        std::map<uint32, uint32> _drinkSpells;
        std::unordered_map<uint8, std::vector<TalentTree>> _talentTrees;    // class -> trees
        std::vector<uint32> _petEntries;                // tameable beasts for new hunters
        AsyncCallbackProcessor<TransactionCallback> _saveCallbacks;
    };
}

#define sCompanionFactory Companions::CompanionFactory::instance()

#endif
