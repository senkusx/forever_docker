#include "CompanionScaling.h"
#include "CompanionConfig.h"
#include "Creature.h"
#include "ScriptMgr.h"
#include "TemporarySummon.h"
#include "Unit.h"
#include <cmath>

namespace Companions
{
    bool IsScaledRaid(uint32 mapId)
    {
        switch (mapId)
        {
            case 249:   // Onyxia's Lair
            case 409:   // Molten Core
            case 469:   // Blackwing Lair
            case 531:   // Temple of Ahn'Qiraj
            case 533:   // Naxxramas
                return true;
            default:
                return false;
        }
    }

    namespace
    {
        // the raid's own creatures: not what players brought in (pets, guardians, totems) and not players
        bool IsRaidCreature(Unit const* unit)
        {
            if (!unit || unit->GetTypeId() != TYPEID_UNIT || unit->IsControlledByPlayer())
                return false;
            if (TempSummon const* summon = unit->ToTempSummon())
                if (summon->GetSummonerGUID().IsPlayer())
                    return false;
            return IsScaledRaid(unit->GetMapId());
        }

        template <typename T>
        void Scale(T& damage, float scale)
        {
            if (scale < 1.0f && damage > 0)
                damage = std::max<T>(1, T(std::lround(damage * scale)));
        }
    }

    float RaidHealthScale(Creature const* creature)
    {
        Config const& config = GetConfig();
        if (!config.Raid20Enable || !IsRaidCreature(creature))
            return 1.0f;
        return config.Raid20Health;
    }

    float RaidDamageScale(Unit const* attacker, Unit const* target)
    {
        Config const& config = GetConfig();
        if (!config.Raid20Enable || !target || !target->IsControlledByPlayer() || !IsRaidCreature(attacker))
            return 1.0f;
        return config.Raid20Damage;
    }

    namespace
    {
        class companions_raid20_unitscript : public UnitScript
        {
        public:
            companions_raid20_unitscript() : UnitScript("companions_raid20_unitscript") { }

            void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
            {
                Scale(damage, RaidDamageScale(attacker, target));
            }

            void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
            {
                Scale(damage, RaidDamageScale(attacker, target));
            }

            void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage) override
            {
                Scale(damage, RaidDamageScale(attacker, target));
            }
        };
    }

    void AddScalingScripts()
    {
        CreatureScaleHooks::HealthScale = &RaidHealthScale;
        new companions_raid20_unitscript();
    }
}
