#include "CompanionSpells.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"

namespace Companions
{
    namespace
    {
        // 8515, 10609, 10612 - Windfury Totem passive: an area aura on the shaman's party that procs on melee hits (spell_proc:
        // 20% on a main hand hit). The client gives the aura no trigger spell, the Windfury attack (8516, 10608, 10610: one
        // extra swing with bonus attack power) is the effect's base points, so the default proc did nothing.
        class spell_companions_windfury_totem_passive : public AuraScript
        {
            static uint32 AttackSpell(uint32 spellId)
            {
                switch (spellId)
                {
                    case 8515:  return 8516;
                    case 10609: return 10608;
                    case 10612: return 10610;
                    default:    return 0;
                }
            }

            bool Validate(SpellInfo const* spellInfo) override
            {
                return AttackSpell(spellInfo->Id) && ValidateSpellInfo({ AttackSpell(spellInfo->Id) });
            }

            // main hand only, and not again while the last Windfury attack's bonus is up (no chain from the extra swing)
            bool CheckProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
            {
                DamageInfo const* damage = eventInfo.GetDamageInfo();
                Unit const* actor = eventInfo.GetActor();
                return damage && actor && damage->GetAttackType() == BASE_ATTACK && !actor->HasAura(AttackSpell(GetId()));
            }

            void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
            {
                PreventDefaultAction();
                Unit* actor = eventInfo.GetActor();
                actor->CastSpell(actor, AttackSpell(GetId()), CastSpellExtraArgs(aurEff));
            }

            void Register() override
            {
                DoCheckEffectProc += AuraCheckEffectProcFn(spell_companions_windfury_totem_passive::CheckProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
                OnEffectProc += AuraEffectProcFn(spell_companions_windfury_totem_passive::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
            }
        };

        // 8230, 8250, 10521, 15036 - Flametongue Totem passive: an area aura on the shaman's party like Windfury Totem's, its
        // proc spell (a dummy: 8253, 8248, 10523, 16389) in the effect's base points; rank 2 has none in the client and takes
        // 8248, whose points sit between ranks 1 and 3. Every melee hit deals Fire damage through Flametongue Attack, like
        // Flametongue Weapon: the proc spell's points / 100 per second of weapon speed (rank 1: 548, 7 to 22 damage).
        class spell_companions_flametongue_totem_passive : public AuraScript
        {
            static constexpr uint32 SPELL_FLAMETONGUE_ATTACK = 10444;

            static uint32 ProcSpell(uint32 spellId)
            {
                switch (spellId)
                {
                    case 8230:  return 8253;
                    case 8250:  return 8248;
                    case 10521: return 10523;
                    case 15036: return 16389;
                    default:    return 0;
                }
            }

            bool Validate(SpellInfo const* spellInfo) override
            {
                return ProcSpell(spellInfo->Id) && ValidateSpellInfo({ ProcSpell(spellInfo->Id), SPELL_FLAMETONGUE_ATTACK });
            }

            bool CheckProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
            {
                DamageInfo const* damage = eventInfo.GetDamageInfo();
                return damage && eventInfo.GetActor() && eventInfo.GetActionTarget()
                    && (damage->GetAttackType() == BASE_ATTACK || damage->GetAttackType() == OFF_ATTACK);
            }

            void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
            {
                PreventDefaultAction();
                Unit* actor = eventInfo.GetActor();
                WeaponAttackType const attack = eventInfo.GetDamageInfo()->GetAttackType();
                // the weapon that hit; a pet or a shapeshifted druid by its attack speed
                float speed = actor->GetBaseAttackTime(attack) / 1000.0f;
                if (Player* player = actor->ToPlayer())
                    if (Item* weapon = player->GetWeaponForAttack(attack, true))
                        speed = weapon->GetTemplate()->GetDelay() / 1000.0f;
                SpellInfo const* proc = sSpellMgr->AssertSpellInfo(ProcSpell(GetId()), DIFFICULTY_NONE);
                int32 const damage = std::max(1, int32(proc->GetEffect(EFFECT_0).CalcValue(actor) * speed / 100.0f));
                actor->CastSpell(eventInfo.GetActionTarget(), SPELL_FLAMETONGUE_ATTACK,
                    CastSpellExtraArgs(aurEff).AddSpellMod(SPELLVALUE_BASE_POINT0, damage));
            }

            void Register() override
            {
                DoCheckEffectProc += AuraCheckEffectProcFn(spell_companions_flametongue_totem_passive::CheckProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
                OnEffectProc += AuraEffectProcFn(spell_companions_flametongue_totem_passive::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
            }
        };
    }

    void AddSpellScripts()
    {
        RegisterSpellScript(spell_companions_windfury_totem_passive);
        RegisterSpellScript(spell_companions_flametongue_totem_passive);
    }
}
