#include "CompanionSpells.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
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
    }

    void AddSpellScripts()
    {
        RegisterSpellScript(spell_companions_windfury_totem_passive);
    }
}
