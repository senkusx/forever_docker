/*
 * Companions: class rotations and role duties (Classic spells, resolved by name to the best known rank)
 */

#include "CompanionAI.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <span>

namespace Companions
{
    namespace
    {
        constexpr uint32 MASK_MAGIC   = 1u << DISPEL_MAGIC;
        constexpr uint32 MASK_CURSE   = 1u << DISPEL_CURSE;
        constexpr uint32 MASK_DISEASE = 1u << DISPEL_DISEASE;
        constexpr uint32 MASK_POISON  = 1u << DISPEL_POISON;

        bool UsesMana(Unit const* unit) { return unit->GetMaxPower(POWER_MANA) > 0; }

        bool IsCreatureType(Unit const* unit, uint32 type) { return unit && unit->GetCreatureType() == type; }

        bool Bleeds(Unit const* unit)
        {
            uint32 type = unit->GetCreatureType();
            return type != CREATURE_TYPE_UNDEAD && type != CREATURE_TYPE_ELEMENTAL && type != CREATURE_TYPE_MECHANICAL;
        }

        bool IsPhysicalClass(Player const* player)
        {
            switch (player->GetClass())
            {
                case CLASS_WARRIOR:
                case CLASS_ROGUE:
                case CLASS_HUNTER:
                    return true;
                default:
                    return !UsesMana(player);
            }
        }
    }

    // shared helpers for all classes
    class ClassBase : public ClassAI
    {
    public:
        using ClassAI::ClassAI;

    protected:
        Player* Me() const { return _ai.GetBot(); }
        Role MyRole() const { return _ai.GetEffectiveRole(); }
        bool Cast(std::string_view spell, Unit* target = nullptr) { return _ai.Cast(spell, target); }
        bool Knows(std::string_view spell) const { return _ai.Knows(spell); }
        bool Has(Unit const* unit, std::string_view spell, bool mine = false) const { return _ai.HasAura(unit, spell, mine); }
        // Tauren War Stomp (any class): everything within 8 yards
        bool RacialStun(Unit* target) { return Me()->GetDistance(target) < 7.0f && Cast("war stomp"); }

    public:
        bool Stun(Unit* target) override { return RacialStun(target); }

    protected:

        // a buff that is missing or about to fade
        bool Buff(std::string_view spell, Unit* target)
        {
            if (!Knows(spell))
                return false;
            int32 left = _ai.AuraRemaining(target, spell);
            if (left == -1 || left > 10000)
                return false;
            return Cast(spell, target);
        }

        // a damage over time of ours that is missing or about to end
        bool Dot(std::string_view spell, Unit* target, float minHealthPct = 25.0f)
        {
            if (!Knows(spell) || target->GetHealthPct() < minHealthPct)
                return false;
            int32 left = _ai.AuraRemaining(target, spell, true);
            if (left == -1 || left > 1500)
                return false;
            return Cast(spell, target);
        }

        // a single shot at the lowest rank (interrupts, taunts: the rank does not matter, the mana does)
        bool CastLowest(std::string_view spell, Unit* target)
        {
            std::vector<uint32> const* ranks = _ai.Spells().Ranks(spell);
            return ranks && !ranks->empty() && _ai.CastId(ranks->front(), target);
        }

        bool AutoRepeat(std::string_view spell, Unit* target)
        {
            Spell* current = Me()->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
            if (current && _autoTarget == target->GetGUID())
                return false;
            if (Cast(spell, target))
            {
                _autoTarget = target->GetGUID();
                return true;
            }
            return false;
        }

        bool Wand(Unit* target)
        {
            if (!_ai.UsesWand())
                return false;
            return AutoRepeat("shoot", target);
        }

        // Heroic Strike, Cleave, Maul: they wait for the next swing; casting again would only block the rotation
        bool NextSwingQueued() const { return Me()->GetCurrentSpell(CURRENT_MELEE_SPELL) != nullptr; }

        bool HasAnyAura(Unit const* unit, std::initializer_list<std::string_view> spells, bool mine = false) const
        {
            for (std::string_view spell : spells)
                if (Has(unit, spell, mine))
                    return true;
            return false;
        }

        ObjectGuid _autoTarget;
    };

    // ================================================================== Warrior
    class WarriorAI : public ClassBase
    {
    public:
        using ClassBase::ClassBase;

        Role DefaultRole() const override { return _ai.HasShield() ? Role::Tank : Role::MeleeDps; }
        bool CanTank() const override { return true; }
        float RangedDistance() const override { return 0.0f; }

        static ShapeshiftForm FormOf(std::string_view stance)
        {
            if (stance == "defensive stance")
                return FORM_DEFENSIVE_STANCE;
            if (stance == "berserker stance")
                return FORM_BERSERKER_STANCE;
            return FORM_BATTLE_STANCE;
        }

        // another tank leads: a damage warrior does not charge in first
        bool OtherTank() const
        {
            Player* tank = _ai.View().Tank;
            return tank && tank != Me() && _ai.IsTank(tank);
        }

        bool Stance(std::string_view stance)
        {
            return Knows(stance) && Me()->GetShapeshiftForm() != FormOf(stance) && Cast(stance);
        }

        bool SelfBuffs(bool /*inCombat*/) override
        {
            // like players: out of a fight in Battle Stance, ready to charge; in it Defensive (tank) or Berserker
            bool fighting = Me()->IsInCombat();
            if (!fighting && Knows("charge") && (MyRole() == Role::Tank || !OtherTank()))
            {
                if (Stance("battle stance"))
                    return true;
            }
            else if (MyRole() == Role::Tank)
            {
                if (Stance("defensive stance"))
                    return true;
            }
            else if (Knows("berserker stance") ? Stance("berserker stance") : Stance("battle stance"))
                return true;

            if (fighting && _ai.Rage() >= 10 && Buff("battle shout", Me()))
                return true;
            return false;
        }

        bool Pull(Unit* target) override
        {
            float dist = Me()->GetDistance(target);
            bool charging = dist > 8.0f && dist < 25.0f && !Me()->IsInCombat() && Knows("charge") && _ai.IsSpellReady("charge");
            if (charging && (Stance("battle stance") || Cast("charge", target)))
                return true;
            // no charge (cooldown, too far, too close): a throw or a shot brings it
            if (!charging && dist > 8.0f && dist < 30.0f && (Cast("throw", target) || Cast("shoot", target)))
                return true;
            return Combat(target);
        }

        bool Taunt(Unit* target) override
        {
            if (Me()->GetShapeshiftForm() == FORM_DEFENSIVE_STANCE && Cast("taunt", target))
                return true;
            // taunt on cooldown: the biggest hits on it
            return _ai.InMelee(target) && _ai.Rage() >= 15 && (Cast("shield slam", target) || Cast("revenge", target));
        }

        bool AoeTaunt() override { return Cast("challenging shout"); }

        bool Interrupt(Unit* caster) override
        {
            if (!_ai.InMelee(caster))
                return false;
            return (_ai.HasShield() && Cast("shield bash", caster)) || Cast("pummel", caster) || Cast("concussion blow", caster);
        }

        bool Stun(Unit* target) override { return (_ai.InMelee(target) && Cast("concussion blow", target)) || RacialStun(target); }
        bool BreakCast(Unit* caster) override { return Me()->GetDistance(caster) < 8.0f && Cast("intimidating shout", caster); }

        bool Emergency() override
        {
            if (!_ai.IsTargetedByEnemy())
                return false;
            if (_ai.HealthPct() < 25.0f && (Cast("last stand") || (_ai.HasShield() && Cast("shield wall"))))
                return true;
            if (MyRole() != Role::Tank && _ai.HealthPct() < 30.0f && _ai.EnemiesAttacking(Me()) >= 2 && Cast("intimidating shout"))
                return true;
            return false;
        }

        bool Combat(Unit* target) override
        {
            float dist = Me()->GetDistance(target);
            if (!_ai.InMelee(target))
            {
                if (dist > 8.0f && dist < 25.0f && !Me()->IsInCombat() && (MyRole() == Role::Tank || !OtherTank()) && Cast("charge", target))
                    return true;
                if (dist > 8.0f && dist < 25.0f && Me()->IsInCombat() && Cast("intercept", target))
                    return true;
                return false;
            }

            if (_ai.Rage() < 20 && _ai.HealthPct() > 50.0f && Cast("bloodrage"))
                return true;
            if (_ai.HealthPct() < 80.0f && Cast("victory rush", target))
                return true;

            uint32 nearby = _ai.EnemiesNear(Me()->GetPosition(), 8.0f);
            if (MyRole() == Role::Tank)
            {
                // threat: Shield Slam and Revenge first, Sunder as the filler, the rest of the rage in Heroic Strike / Cleave
                if (_ai.HasShield() && target->GetVictim() == Me() && _ai.Rage() >= 10 && !Has(Me(), "shield block")
                    && (_ai.IsBossFight() || _ai.HealthPct() < 70.0f) && Cast("shield block"))
                    return true;
                // area hits never on a crowd controlled enemy
                bool controlledNear = _ai.AnyCrowdControlledNear(Me()->GetPosition(), 10.0f);
                if (Cast("shield slam", target) || Cast("revenge", target))
                    return true;
                // a pack: Thunder Clap and Demoralizing Shout put threat on all of them
                if (nearby >= 2 && !controlledNear && !Has(target, "thunder clap") && Cast("thunder clap"))
                    return true;
                if ((nearby >= 2 || _ai.IsBossFight()) && !Has(target, "demoralizing shout") && _ai.Rage() >= 10 && Cast("demoralizing shout"))
                    return true;
                if (_ai.Rage() >= 25 && Cast("concussion blow", target))
                    return true;
                if (nearby >= 3 && !controlledNear && Cast("thunder clap"))
                    return true;
                // the rage dump does not take the global cooldown: Sunder goes on in between
                if (_ai.Rage() >= 40 && !NextSwingQueued())
                    Cast(nearby >= 2 && !controlledNear ? "cleave" : "heroic strike", target);
                return Cast("sunder armor", target);
            }

            if (target->GetHealthPct() < 20.0f && Cast("execute", target))
                return true;
            if (_ai.Bursting() && (Cast("death wish") || Cast("recklessness")))
                return true;
            if (Cast("bloodthirst", target) || Cast("mortal strike", target) || Cast("overpower", target))
                return true;
            if (nearby >= 1 && !_ai.AnyCrowdControlledNear(Me()->GetPosition(), 10.0f) && _ai.SafeToAoe(Me()->GetPosition(), 8.0f) && Cast("whirlwind"))
                return true;
            if (Bleeds(target) && !_ai.IsBossFight() && Dot("rend", target, 40.0f))
                return true;
            if (_ai.Rage() >= 50 && !NextSwingQueued())
                return nearby >= 2 && !_ai.AnyCrowdControlledNear(Me()->GetPosition(), 10.0f) && _ai.SafeToAoe(Me()->GetPosition(), 8.0f)
                    ? Cast("cleave", target) : Cast("heroic strike", target);
            return false;
        }
    };

    // ================================================================== Paladin
    class PaladinAI : public ClassBase
    {
    public:
        using ClassBase::ClassBase;

        Role DefaultRole() const override { return Role::Healer; }
        bool CanTank() const override { return true; }
        bool CanHeal() const override { return true; }
        float RangedDistance() const override { return 0.0f; }

        bool HasSeal() const
        {
            return HasAnyAura(Me(), { "seal of righteousness", "seal of command", "seal of the crusader", "seal of wisdom", "seal of light", "seal of justice", "seal of fury" }, true);
        }

        bool SelfBuffs(bool inCombat) override
        {
            std::string_view aura = "devotion aura";
            if (MyRole() == Role::Healer && Knows("concentration aura"))
                aura = "concentration aura";
            else if (MyRole() == Role::MeleeDps && Knows("retribution aura"))
                aura = "retribution aura";
            if (Knows(aura) && !Has(Me(), aura, true) && Cast(aura))
                return true;

            if (MyRole() == Role::Tank && Buff("righteous fury", Me()))
                return true;
            if (inCombat && MyRole() != Role::Healer && !HasSeal() && PutSeal())
                return true;
            return false;
        }

        // Forever's tank seal is Seal of Fury
        bool PutSeal()
        {
            if (MyRole() == Role::Tank && Cast("seal of fury"))
                return true;
            return Cast("seal of command") || Cast("seal of righteousness");
        }

        // a blessing asked for ("salvation on me"): Forever calls Blessing of Salvation just "Salvation"
        std::string_view ChosenBlessing(Player* member) const
        {
            std::string kind = _ai.BlessingFor(member->GetGUID());
            if (kind.empty())
                return { };
            // plain arrays: an initializer_list inside a pair would point at a temporary that is long gone
            struct Kind { char const* Name; char const* Spells[2]; };
            static Kind const kinds[] = {
                { "salvation", { "blessing of salvation", "salvation" } }, { "might", { "blessing of might", nullptr } },
                { "wisdom", { "blessing of wisdom", nullptr } }, { "kings", { "blessing of kings", nullptr } },
                { "light", { "blessing of light", nullptr } }, { "sanctuary", { "blessing of sanctuary", nullptr } } };
            for (Kind const& entry : kinds)
                if (kind == entry.Name)
                    for (char const* spell : entry.Spells)
                        if (spell && Knows(spell))
                            return spell;
            return { };
        }

        std::vector<std::string_view> OwnBuffs(Player* member) override
        {
            if (std::string_view chosen = ChosenBlessing(member); !chosen.empty())
                return { chosen };
            return { "blessing of kings", "blessing of might", "blessing of wisdom", "blessing of light", "salvation", "blessing of salvation" };
        }

        bool GroupBuffs(Player* member) override
        {
            static std::initializer_list<std::string_view> const blessings = { "blessing of kings", "blessing of might", "blessing of wisdom",
                "blessing of salvation", "salvation", "blessing of light", "blessing of sanctuary", "greater blessing of kings",
                "greater blessing of might", "greater blessing of wisdom", "greater blessing of light" };
            // the one asked for replaces this paladin's other blessing
            if (std::string_view chosen = ChosenBlessing(member); !chosen.empty())
                return (!Has(member, chosen, true) || _ai.AuraRemaining(member, chosen) < 10000) && Cast(chosen, member);
            if (HasAnyAura(member, blessings, true))
                return false;
            if (_ai.IsTank(member) && Knows("blessing of kings"))
                return Cast("blessing of kings", member);
            if (UsesMana(member) && !IsPhysicalClass(member) && member->GetClass() != CLASS_PALADIN && Knows("blessing of wisdom"))
                return Cast("blessing of wisdom", member);
            if (member->GetClass() == CLASS_PALADIN && member != Me() && Knows("blessing of wisdom"))
                return Cast("blessing of wisdom", member);
            if (Knows("blessing of kings") && !Knows("blessing of might"))
                return Cast("blessing of kings", member);
            return Cast("blessing of might", member) || Cast("blessing of kings", member);
        }

        bool Heal(Unit* target, HealUrgency urgency, uint32 deficit) override
        {
            if (urgency == HealUrgency::Emergency)
            {
                if (target->GetHealthPct() < 15.0f && _ai.IsTank(target) && Cast("lay on hands", target))
                    return true;
                if (Cast("holy shock", target))
                    return true;
                return _ai.CastHeal("flash of light", target, deficit) || _ai.CastHeal("holy light", target, deficit);
            }
            if (urgency == HealUrgency::Normal && deficit > target->GetMaxHealth() / 3)
                return _ai.CastHeal("holy light", target, deficit) || _ai.CastHeal("flash of light", target, deficit);
            return _ai.CastHeal("flash of light", target, deficit) || _ai.CastHeal("holy light", target, deficit);
        }

        bool Dispel(Unit* target) override
        {
            uint32 mask = _ai.DispellableMask(target);
            if ((mask & (MASK_MAGIC | MASK_POISON | MASK_DISEASE)) && Cast("cleanse", target))
                return true;
            return (mask & (MASK_POISON | MASK_DISEASE)) && Cast("purify", target);
        }

        bool Resurrect(Player* dead) override { return Cast("redemption", dead); }
        bool Interrupt(Unit* caster) override { return Cast("hammer of justice", caster); }
        bool Stun(Unit* target) override { return Cast("hammer of justice", target) || RacialStun(target); }
        // no taunt for paladins: Judgement and Holy Strike on the loose one
        bool Taunt(Unit* target) override
        {
            return MyRole() == Role::Tank && (Cast("hand of reckoning", target) || Cast("judgement", target) || (_ai.InMelee(target) && Cast("holy strike", target)));
        }
        // no area taunt in vanilla: Consecration puts holy threat on all of them
        bool AoeTaunt() override { return MyRole() == Role::Tank && Cast("consecration"); }

        bool Emergency() override
        {
            if (_ai.HealthPct() < 15.0f && _ai.IsTargetedByEnemy() && (Cast("divine shield") || Cast("divine protection")))
                return true;
            for (Player* member : _ai.View().Members)
                if (member != Me() && member->IsAlive() && member->GetHealthPct() < 20.0f && !_ai.IsTank(member)
                    && _ai.EnemiesAttacking(member) && !Has(member, "forbearance") && Cast("blessing of protection", member))
                    return true;
            return false;
        }

        bool Combat(Unit* target) override
        {
            if (target->GetHealthPct() < 20.0f && Cast("hammer of wrath", target))
                return true;
            bool unholy = IsCreatureType(target, CREATURE_TYPE_UNDEAD) || IsCreatureType(target, CREATURE_TYPE_DEMON);
            if (MyRole() == Role::Tank)
                return TankRotation(target, unholy);
            if (unholy && _ai.ManaPct() > 40.0f && Cast("exorcism", target))
                return true;
            if (!HasSeal() && MyRole() != Role::Healer)
                return PutSeal();
            if (_ai.InMelee(target) && _ai.ManaPct() > 40.0f && Cast("holy strike", target))
                return true;
            if (HasSeal() && _ai.InMelee(target) && _ai.ManaPct() > 30.0f && Cast("judgement", target))
                return true;
            return false;
        }

        // threat is holy damage: Righteous Fury, Seal of Fury, Judgement on cooldown, Consecration under the pack,
        // Holy Shield and Holy Strike; Holy Wrath and Exorcism on undead and demons
        bool TankRotation(Unit* target, bool unholy)
        {
            if (!Has(Me(), "righteous fury") && Cast("righteous fury"))
                return true;
            if (!HasSeal())
                return PutSeal();
            bool controlledNear = _ai.AnyCrowdControlledNear(Me()->GetPosition(), 10.0f);
            uint32 nearby = _ai.EnemiesNear(Me()->GetPosition(), 8.0f);
            if (_ai.IsTargetedByEnemy() && !Has(Me(), "holy shield") && Cast("holy shield"))
                return true;
            if (_ai.InMelee(target) && Cast("judgement", target))
                return true;
            if (_ai.InMelee(target) && !controlledNear && _ai.ManaPct() > 20.0f && Cast("consecration"))
                return true;
            if (_ai.InMelee(target) && Cast("holy strike", target))
                return true;
            if (nearby >= 2 && !controlledNear && unholy && _ai.ManaPct() > 30.0f && Cast("holy wrath"))
                return true;
            if (unholy && _ai.ManaPct() > 40.0f && Cast("exorcism", target))
                return true;
            if (_ai.HealthPct() < 70.0f && _ai.IsTargetedByEnemy() && Cast("templar's bulwark"))
                return true;
            return false;
        }
    };

    // ================================================================== Hunter
    class HunterAI : public ClassBase
    {
    public:
        using ClassBase::ClassBase;

        Role DefaultRole() const override { return Role::RangedDps; }
        float RangedDistance() const override { return 28.0f; }

        bool SelfBuffs(bool /*inCombat*/) override
        {
            if (Knows("aspect of the hawk"))
            {
                if (!Has(Me(), "aspect of the hawk") && Cast("aspect of the hawk"))
                    return true;
            }
            else if (!Has(Me(), "aspect of the monkey") && Cast("aspect of the monkey"))
                return true;
            return Buff("trueshot aura", Me());
        }

        bool OutOfCombat() override
        {
            if (Me()->GetLevel() < 10)
                return false;
            Creature* pet = _ai.GetPet();
            if (!pet)
                return Cast("call pet");
            if (!pet->IsAlive())
                return Cast("revive pet");
            if (pet->GetHealthPct() < 70.0f && !Has(pet, "mend pet"))
                return Cast("mend pet", pet);
            return false;
        }

        bool Pet(Unit* target) override
        {
            Creature* pet = _ai.GetPet();
            if (!pet || !pet->IsAlive())
                return false;
            _ai.PetAttack(target);
            if (pet->GetHealthPct() < 40.0f && !Has(pet, "mend pet") && Cast("mend pet", pet))
                return true;
            return false;
        }

        bool Emergency() override
        {
            // pulled aggro with a tank around: drop it like a hunter does
            if (_ai.IsTargetedByEnemy() && _ai.View().Tank && _ai.View().Tank != Me() && Cast("feign death"))
                return true;
            return false;
        }

        bool Interrupt(Unit* caster) override { return Cast("scatter shot", caster); }
        bool Stun(Unit* target) override { return Cast("intimidation", target) || Cast("scatter shot", target) || RacialStun(target); }
        bool BreakCast(Unit* caster) override { return Cast("scatter shot", caster); }
        bool CanCrowdControl(Unit const* target) const override { return IsCreatureType(target, CREATURE_TYPE_BEAST) && Knows("scare beast"); }
        bool CrowdControl(Unit* target) override { return CanCrowdControl(target) && !Has(target, "scare beast") && Cast("scare beast", target); }

        bool Combat(Unit* target) override
        {
            if (_ai.InMelee(target))
            {
                if (target->GetVictim() == Me() && !Has(target, "wing clip") && Cast("wing clip", target))
                    return true;
                // Raptor Strike waits for the next swing: queued once, not every tick
                if (Cast("mongoose bite", target) || (!NextSwingQueued() && Cast("raptor strike", target)))
                    return true;
                return false;
            }

            if (!Has(target, "hunter's mark") && target->GetHealthPct() > 30.0f && Cast("hunter's mark", target))
                return true;
            if (AutoRepeat("auto shot", target))
                return true;
            if (_ai.Bursting() && (Cast("rapid fire") || Cast("bestial wrath")))
                return true;
            if (target->GetVictim() == Me() && Cast("concussive shot", target))
                return true;
            if (Bleeds(target) && _ai.ManaPct() > 30.0f && Dot("serpent sting", target, 30.0f))
                return true;
            if (_ai.EnemiesNear(target->GetPosition(), 8.0f) >= 3 && !_ai.AnyCrowdControlledNear(target->GetPosition(), 10.0f)
                && _ai.SafeToAoe(target->GetPosition(), 8.0f) && Cast("multi-shot", target))
                return true;
            if (target->GetVictim() != Me() && !_ai.IsMoving() && Cast("aimed shot", target))
                return true;
            if (_ai.ManaPct() > 40.0f && Cast("arcane shot", target))
                return true;
            return false;
        }
    };

    // ================================================================== Rogue
    class RogueAI : public ClassBase
    {
    public:
        using ClassBase::ClassBase;

        Role DefaultRole() const override { return Role::MeleeDps; }
        float RangedDistance() const override { return 0.0f; }

        bool Interrupt(Unit* caster) override { return _ai.InMelee(caster) && (Cast("kick", caster) || Cast("gouge", caster)); }
        bool Stun(Unit* target) override { return _ai.InMelee(target) && (Cast("kidney shot", target) || Cast("gouge", target)); }
        bool BreakCast(Unit* caster) override { return _ai.InMelee(caster) && (Cast("gouge", caster) || Cast("blind", caster)); }
        bool CanCrowdControl(Unit const* target) const override { return Knows("gouge") && IsCreatureType(target, CREATURE_TYPE_HUMANOID); }
        bool CrowdControl(Unit* target) override { return _ai.InMelee(target) && CanCrowdControl(target) && Cast("gouge", target); }

        bool Emergency() override
        {
            if (!_ai.IsTargetedByEnemy())
                return false;
            // the tank lost it to us: shed threat first
            if (_ai.View().Tank && _ai.View().Tank != Me() && Cast("feint"))
                return true;
            if (_ai.HealthPct() < 35.0f && Cast("evasion"))
                return true;
            if (_ai.HealthPct() < 15.0f && Cast("vanish"))
                return true;
            return false;
        }

        bool Combat(Unit* target) override
        {
            if (!_ai.InMelee(target))
                return false;

            int32 cp = _ai.ComboPoints(target);
            if (_ai.Bursting() && Cast("adrenaline rush"))
                return true;
            if (_ai.EnemiesNear(Me()->GetPosition(), 8.0f) >= 2 && _ai.SafeToAoe(Me()->GetPosition(), 8.0f) && Cast("blade flurry"))
                return true;
            if (cp >= 1 && target->GetHealthPct() > 25.0f && !Has(Me(), "slice and dice") && Cast("slice and dice"))
                return true;
            if ((cp >= 5 || (cp >= 3 && target->GetHealthPct() < 25.0f)) && Cast("eviscerate", target))
                return true;
            if (Cast("riposte", target))
                return true;

            Item* weapon = _ai.MainHand();
            bool dagger = weapon && weapon->GetTemplate()->GetSubClass() == ITEM_SUBCLASS_WEAPON_DAGGER;
            if (dagger && _ai.IsBehind(target) && Cast("backstab", target))
                return true;
            return Cast("sinister strike", target);
        }
    };

    // ================================================================== Priest
    class PriestAI : public ClassBase
    {
    public:
        using ClassBase::ClassBase;

        bool BreakCast(Unit* caster) override { return Me()->GetDistance(caster) < 8.0f && Cast("psychic scream"); }
        Role DefaultRole() const override { return Role::Healer; }
        bool CanHeal() const override { return true; }
        float RangedDistance() const override { return 28.0f; }

        bool SelfBuffs(bool /*inCombat*/) override
        {
            if (Buff("inner fire", Me()))
                return true;
            if (MyRole() == Role::RangedDps && Knows("shadowform") && !Has(Me(), "shadowform") && Cast("shadowform"))
                return true;
            return false;
        }

        std::vector<std::string_view> OwnBuffs(Player* member) override
        {
            if (UsesMana(member))
                return { "power word: fortitude", "divine spirit", "shadow protection" };
            return { "power word: fortitude", "shadow protection" };
        }

        bool GroupBuffs(Player* member) override
        {
            if (!Has(member, "prayer of fortitude") && Buff("power word: fortitude", member))
                return true;
            if (UsesMana(member) && !Has(member, "prayer of spirit") && Buff("divine spirit", member))
                return true;
            return false;
        }

        bool Shield(Unit* target)
        {
            return !Has(target, "power word: shield") && !Has(target, "weakened soul") && Cast("power word: shield", target);
        }

        bool Heal(Unit* target, HealUrgency urgency, uint32 deficit) override
        {
            // shadowform cannot heal: only for emergencies
            if (Has(Me(), "shadowform"))
            {
                if (urgency != HealUrgency::Emergency)
                    return false;
                Me()->RemoveAurasDueToSpell(_ai.SpellIdOf("shadowform"));
            }

            // the shield costs much: for the tank, a life in danger, or with mana to spare
            bool beingHit = _ai.EnemiesAttacking(target) > 0;
            if (urgency != HealUrgency::TopUp && beingHit && (urgency == HealUrgency::Emergency || _ai.IsTank(target) || _ai.ManaPct() > 60.0f)
                && Shield(target))
                return true;
            if (beingHit && _ai.IsTank(target) && !Has(target, "prayer of mending") && Cast("prayer of mending", target))
                return true;

            if (urgency == HealUrgency::Emergency)
                return _ai.CastHeal("flash heal", target, deficit) || _ai.CastHeal("heal", target, deficit) || _ai.CastHeal("lesser heal", target, deficit);

            if (beingHit && !Has(target, "renew", true) && Cast("renew", target))
                return true;

            if (deficit > target->GetMaxHealth() / 2)
                return _ai.CastHeal("greater heal", target, deficit) || _ai.CastHeal("heal", target, deficit) || _ai.CastHeal("lesser heal", target, deficit);
            if (urgency == HealUrgency::TopUp && !Has(target, "renew", true) && Cast("renew", target))
                return true;
            return _ai.CastHeal("heal", target, deficit) || _ai.CastHeal("lesser heal", target, deficit) || _ai.CastHeal("flash heal", target, deficit);
        }

        bool GroupHeal(uint32 injured) override
        {
            return injured >= 3 && Cast("prayer of healing");
        }

        bool Dispel(Unit* target) override
        {
            uint32 mask = _ai.DispellableMask(target);
            if ((mask & MASK_MAGIC) && Cast("dispel magic", target))
                return true;
            return (mask & MASK_DISEASE) && (Cast("abolish disease", target) || Cast("cure disease", target));
        }

        bool Resurrect(Player* dead) override { return Cast("resurrection", dead); }
        bool CanCrowdControl(Unit const* target) const override { return IsCreatureType(target, CREATURE_TYPE_UNDEAD) && Knows("shackle undead"); }
        bool CrowdControl(Unit* target) override { return CanCrowdControl(target) && Cast("shackle undead", target); }

        bool Emergency() override
        {
            if (!_ai.IsTargetedByEnemy())
                return false;
            if (_ai.HealthPct() < 30.0f && Cast("desperate prayer"))
                return true;
            if (_ai.HealthPct() < 50.0f && _ai.EnemiesAttacking(Me()) >= 2 && !_ai.AnyCrowdControlledNear(Me()->GetPosition(), 15.0f)
                && Cast("psychic scream"))
                return true;
            if (_ai.HealthPct() < 60.0f && Shield(Me()))
                return true;
            if (_ai.View().Tank && _ai.View().Tank != Me() && Cast("fade"))
                return true;
            return false;
        }

        bool Combat(Unit* target) override
        {
            if (MyRole() == Role::Healer)
            {
                // a healer spends mana on damage only when it is plentiful
                if (_ai.ManaPct() > 80.0f && Dot("shadow word: pain", target, 40.0f))
                    return true;
                return Wand(target);
            }

            if (_ai.ManaPct() < 15.0f)
                return Wand(target);
            if (Dot("vampiric touch", target) || Dot("shadow word: pain", target))
                return true;
            if (Knows("vampiric embrace") && !Has(target, "vampiric embrace", true) && Cast("vampiric embrace", target))
                return true;
            if (Cast("mind blast", target))
                return true;
            if (!_ai.IsMoving() && Cast("mind flay", target))
                return true;
            if (!Knows("mind flay") && Cast("smite", target))
                return true;
            return Wand(target);
        }
    };

    // ================================================================== Shaman
    class ShamanAI : public ClassBase
    {
    public:
        using ClassBase::ClassBase;

        Role DefaultRole() const override { return Role::MeleeDps; }
        bool CanHeal() const override { return true; }
        float RangedDistance() const override { return MyRole() == Role::MeleeDps ? 0.0f : 28.0f; }

        bool Imbue()
        {
            Item* weapon = _ai.MainHand();
            if (!weapon || weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                return false;
            // plain arrays: a list chosen by ?: would point at a temporary that is gone by the loop
            static constexpr std::string_view MeleeImbues[] = { "windfury weapon", "flametongue weapon", "rockbiter weapon" };
            static constexpr std::string_view CasterImbues[] = { "flametongue weapon", "rockbiter weapon" };
            std::span<std::string_view const> imbues = MyRole() == Role::MeleeDps ? std::span<std::string_view const>(MeleeImbues)
                                                                                  : std::span<std::string_view const>(CasterImbues);
            for (std::string_view imbue : imbues)
            {
                uint32 spellId = _ai.SpellIdOf(imbue);
                if (!spellId || !_ai.CanCastId(spellId, Me()))
                    continue;
                return Me()->CastSpell(weapon, spellId, CastSpellExtraArgs(TRIGGERED_NONE)) == SPELL_CAST_OK;
            }
            return false;
        }

        bool SelfBuffs(bool inCombat) override
        {
            if (Imbue())
                return true;
            if (MyRole() != Role::Healer && Buff("lightning shield", Me()))
                return true;
            // the totems in a fight, whatever its role (a healer's Combat runs only with mana to spare)
            if (inCombat && std::chrono::duration_cast<Milliseconds>(GameTime::Now() - _lastTotemCheck).count() > 1500)
            {
                _lastTotemCheck = GameTime::Now();
                if (Totems(Me()->GetVictim()))
                    return true;
            }
            return false;
        }

        // one totem per element, recast when missing or left behind; known by the spell that made it (the creatures' names
        // differ: "Cleansing Totem" for Disease Cleansing, "Elemental Resistance Totem" for Fire Resistance)
        bool HasTotemNear(std::string_view spell) const
        {
            for (uint8 slot = SUMMON_SLOT_TOTEM; slot <= SUMMON_SLOT_TOTEM_4; ++slot)
            {
                Creature* totem = ObjectAccessor::GetCreature(*Me(), Me()->m_SummonSlot[slot]);
                if (!totem || !totem->IsAlive() || Me()->GetDistance(totem) > 25.0f)
                    continue;
                if (_ai.IsSpellNamed(uint32(*totem->m_unitData->CreatedBySpell), spell))
                    return true;
            }
            return false;
        }

        bool DropTotem(std::initializer_list<std::string_view> choices)
        {
            for (std::string_view totem : choices)
            {
                if (!Knows(totem))
                    continue;
                if (HasTotemNear(totem))
                    return false;
                return Cast(totem);
            }
            return false;
        }

        // one per element for what the group needs now: Tremor against a fear, a cleansing totem against poison or disease,
        // else the buffs (Strength of Earth and Windfury for melee, Mana Spring for a healer), and fire on the enemies
        bool Totems(Unit* target)
        {
            if (_ai.View().Enemies.empty())
                return false;
            // a healer running dry: Mana Tide first, it costs little
            if (MyRole() == Role::Healer && _ai.ManaPct() < 35.0f && DropTotem({ "mana tide totem" }))
                return true;
            if (_ai.ManaPct() < 25.0f)
                return false;
            bool feared = false, poisoned = false, diseased = false;
            bool melee = MyRole() == Role::MeleeDps || _ai.View().Tank != nullptr;
            for (Player* member : _ai.View().Members)
            {
                if (!member->IsAlive() || Me()->GetDistance(member) > 25.0f)
                    continue;
                feared = feared || member->HasAuraType(SPELL_AURA_MOD_FEAR);
                uint32 mask = _ai.DispellableMask(member);
                poisoned = poisoned || (mask & MASK_POISON);
                diseased = diseased || (mask & MASK_DISEASE);
            }
            // earth
            if (feared && DropTotem({ "tremor totem" }))
                return true;
            if (!feared && DropTotem(melee ? std::initializer_list<std::string_view>{ "strength of earth totem", "stoneskin totem" }
                                           : std::initializer_list<std::string_view>{ "stoneskin totem", "strength of earth totem" }))
                return true;
            // water
            if (poisoned && DropTotem({ "poison cleansing totem" }))
                return true;
            if (!poisoned && diseased && DropTotem({ "disease cleansing totem" }))
                return true;
            if (!poisoned && !diseased
                && (MyRole() == Role::Healer ? DropTotem({ "mana spring totem", "healing stream totem" }) : DropTotem({ "healing stream totem", "mana spring totem" })))
                return true;
            // air: for the ones who hit in melee
            if (melee && DropTotem({ "windfury totem", "grace of air totem" }))
                return true;
            // fire: a pack around a melee shaman takes Magma, else Searing on the target; a Magma Totem stays while the pack
            // thins out (one totem per element, each would replace the other)
            if (MyRole() == Role::MeleeDps && _ai.EnemiesNear(Me()->GetPosition(), 8.0f) >= 3
                && !_ai.AnyCrowdControlledNear(Me()->GetPosition(), 12.0f) && _ai.SafeToAoe(Me()->GetPosition(), 8.0f) && DropTotem({ "magma totem" }))
                return true;
            if (target && Me()->GetDistance(target) < 20.0f && MyRole() != Role::Healer && !HasTotemNear("magma totem") && DropTotem({ "searing totem" }))
                return true;
            return false;
        }

        bool Heal(Unit* target, HealUrgency urgency, uint32 deficit) override
        {
            if (urgency == HealUrgency::Emergency)
                return _ai.CastHeal("lesser healing wave", target, deficit) || _ai.CastHeal("healing wave", target, deficit);
            return _ai.CastHeal("healing wave", target, deficit) || _ai.CastHeal("lesser healing wave", target, deficit);
        }

        bool GroupHeal(uint32 injured) override
        {
            if (injured < 2)
                return false;
            Unit* lowest = _ai.LowestAlly(75.0f, 40.0f);
            return lowest && Cast("chain heal", lowest);
        }

        bool Dispel(Unit* target) override
        {
            uint32 mask = _ai.DispellableMask(target);
            if ((mask & MASK_POISON) && Cast("cure poison", target))
                return true;
            return (mask & MASK_DISEASE) && Cast("cure disease", target);
        }

        bool Resurrect(Player* dead) override { return Cast("ancestral spirit", dead); }
        bool Interrupt(Unit* caster) override { return CastLowest("earth shock", caster); }

        bool Combat(Unit* target) override
        {
            if (MyRole() == Role::Healer)
                return _ai.ManaPct() > 80.0f && Cast("lightning bolt", target);

            if (MyRole() == Role::MeleeDps)
            {
                if (!_ai.InMelee(target))
                    return false;
                if (Cast("stormstrike", target))
                    return true;
                if (_ai.ManaPct() > 40.0f && Dot("flame shock", target, 30.0f))
                    return true;
                if (_ai.ManaPct() > 60.0f && Cast("earth shock", target))
                    return true;
                return false;
            }

            if (Dot("flame shock", target, 30.0f))
                return true;
            if (_ai.EnemiesNear(target->GetPosition(), 10.0f) >= 2 && !_ai.AnyCrowdControlledNear(target->GetPosition(), 12.0f)
                && _ai.SafeToAoe(target->GetPosition(), 12.0f) && Cast("chain lightning", target))
                return true;
            return Cast("lightning bolt", target);
        }

    private:
        TimePoint _lastTotemCheck = GameTime::Now();
    };

    // ================================================================== Mage
    class MageAI : public ClassBase
    {
    public:
        using ClassBase::ClassBase;

        Role DefaultRole() const override { return Role::RangedDps; }
        float RangedDistance() const override { return 28.0f; }

        bool SelfBuffs(bool /*inCombat*/) override
        {
            if (Knows("mage armor"))
                return Buff("mage armor", Me());
            if (Knows("ice armor"))
                return Buff("ice armor", Me());
            return Buff("frost armor", Me());
        }

        bool GroupBuffs(Player* member) override
        {
            return UsesMana(member) && !Has(member, "arcane brilliance") && Buff("arcane intellect", member);
        }

        std::vector<std::string_view> OwnBuffs(Player* /*member*/) override
        {
            return { "arcane intellect", "dampen magic", "amplify magic" };
        }

        bool Interrupt(Unit* caster) override { return Cast("counterspell", caster); }
        bool BreakCast(Unit* caster) override { return Cast("polymorph", caster); }

        bool CanCrowdControl(Unit const* target) const override
        {
            return Knows("polymorph") && (IsCreatureType(target, CREATURE_TYPE_HUMANOID) || IsCreatureType(target, CREATURE_TYPE_BEAST));
        }

        bool CrowdControl(Unit* target) override
        {
            return CanCrowdControl(target) && !target->IsPolymorphed() && Cast("polymorph", target);
        }

        bool Emergency() override
        {
            if (_ai.HealthPct() < 15.0f && _ai.IsTargetedByEnemy() && Cast("ice block"))
                return true;
            if (_ai.EnemiesNear(Me()->GetPosition(), 8.0f) >= 1 && _ai.EnemyAttackingMe() && _ai.InMelee(_ai.EnemyAttackingMe())
                && !_ai.AnyCrowdControlledNear(Me()->GetPosition(), 12.0f) && Cast("frost nova"))
                return true;
            if (_ai.ManaPct() < 10.0f && !_ai.IsTargetedByEnemy() && Cast("evocation"))
                return true;
            return false;
        }

        bool Combat(Unit* target) override
        {
            if (_ai.ManaPct() < 8.0f)
                return Wand(target);

            Position const& me = Me()->GetPosition();
            if (_ai.EnemiesNear(me, 10.0f) >= 3 && _ai.ManaPct() > 40.0f && !_ai.AnyCrowdControlledNear(me, 12.0f) && _ai.SafeToAoe(me, 10.0f) && Cast("arcane explosion"))
                return true;
            if (_ai.EnemiesNear(target->GetPosition(), 8.0f) >= 4 && _ai.ManaPct() > 40.0f && !_ai.AnyCrowdControlledNear(target->GetPosition(), 12.0f)
                && !_ai.IsTargetedByEnemy() && _ai.SafeToAoe(target->GetPosition(), 10.0f) && (Cast("blizzard", target) || Cast("flamestrike", target)))
                return true;
            if (_ai.Bursting() && (Cast("arcane power") || Cast("combustion") || Cast("presence of mind")))
                return true;
            if (_ai.ManaPct() > 30.0f && Cast("fire blast", target))
                return true;

            bool fire = Knows("pyroblast") || Knows("combustion") || !Knows("frostbolt");
            if (fire ? Cast("fireball", target) : Cast("frostbolt", target))
                return true;
            if (Cast("arcane missiles", target))
                return true;
            return Wand(target);
        }
    };

    // ================================================================== Warlock
    class WarlockAI : public ClassBase
    {
    public:
        using ClassBase::ClassBase;

        bool Stun(Unit* target) override { return Cast("death coil", target) || RacialStun(target); }
        bool BreakCast(Unit* caster) override { return Cast("fear", caster) || Cast("death coil", caster); }
        Role DefaultRole() const override { return Role::RangedDps; }
        float RangedDistance() const override { return 28.0f; }

        bool SelfBuffs(bool /*inCombat*/) override
        {
            if (Knows("demon armor"))
                return Buff("demon armor", Me());
            return Buff("demon skin", Me());
        }

        bool OutOfCombat() override
        {
            Creature* pet = _ai.GetPet();
            if (!pet || !pet->IsAlive())
            {
                // a tank in the group: the imp's stamina buff; alone: the voidwalker tanks
                bool groupTank = _ai.View().Tank && _ai.View().Tank != _ai.View().Owner;
                if (!groupTank && Cast("summon voidwalker"))
                    return true;
                return Cast("summon imp");
            }
            if (_ai.ManaPct() < 50.0f && _ai.HealthPct() > 75.0f && Cast("life tap"))
                return true;
            return false;
        }

        bool Pet(Unit* target) override
        {
            _ai.PetAttack(target);
            return false;
        }

        bool CanCrowdControl(Unit const* target) const override
        {
            return Knows("banish") && (IsCreatureType(target, CREATURE_TYPE_DEMON) || IsCreatureType(target, CREATURE_TYPE_ELEMENTAL));
        }
        bool CrowdControl(Unit* target) override { return CanCrowdControl(target) && !Has(target, "banish") && Cast("banish", target); }

        bool Emergency() override
        {
            Unit* attacker = _ai.EnemyAttackingMe();
            if (attacker && _ai.HealthPct() < 25.0f && Cast("death coil", attacker))
                return true;
            return false;
        }

        bool Combat(Unit* target) override
        {
            if (_ai.ManaPct() < 15.0f && _ai.HealthPct() > 50.0f && Cast("life tap"))
                return true;
            if (_ai.ManaPct() < 8.0f)
                return Wand(target);
            if (_ai.HealthPct() < 40.0f && _ai.IsTargetedByEnemy() && !_ai.IsMoving() && Cast("drain life", target))
                return true;
            // Forever renames Curse of Agony to Bane of Agony
            if (!HasAnyAura(target, { "curse of agony", "bane of agony", "curse of the elements", "curse of shadow", "curse of recklessness" }, true)
                && (Dot("bane of agony", target) || Dot("curse of agony", target)))
                return true;
            if (Dot("corruption", target) || Dot("siphon life", target) || Dot("immolate", target, 30.0f))
                return true;
            if (_ai.EnemiesNear(target->GetPosition(), 8.0f) >= 4 && !_ai.AnyCrowdControlledNear(target->GetPosition(), 12.0f) && !_ai.IsTargetedByEnemy()
                && _ai.SafeToAoe(target->GetPosition(), 10.0f) && Cast("rain of fire", target))
                return true;
            if (Cast("shadow bolt", target))
                return true;
            return Wand(target);
        }
    };

    // ================================================================== Druid
    class DruidAI : public ClassBase
    {
    public:
        using ClassBase::ClassBase;

        Role DefaultRole() const override { return Role::Healer; }
        bool CanTank() const override { return true; }
        bool CanHeal() const override { return MyRole() == Role::Healer || MyRole() == Role::RangedDps; }
        float RangedDistance() const override { return (MyRole() == Role::Tank || MyRole() == Role::MeleeDps) ? 0.0f : 28.0f; }

        bool InForm(ShapeshiftForm form) const { return Me()->GetShapeshiftForm() == form; }
        bool Feral() const { return InForm(FORM_BEAR_FORM) || InForm(FORM_DIRE_BEAR_FORM) || InForm(FORM_CAT_FORM); }
        void CasterForm() { if (Me()->GetShapeshiftForm() != FORM_NONE) Me()->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT); }

        bool WantedForm()
        {
            switch (MyRole())
            {
                case Role::Tank:
                    if (Knows("dire bear form"))
                        return !InForm(FORM_DIRE_BEAR_FORM) && Cast("dire bear form");
                    return !InForm(FORM_BEAR_FORM) && Cast("bear form");
                case Role::MeleeDps:
                    if (Knows("cat form"))
                        return !InForm(FORM_CAT_FORM) && Cast("cat form");
                    return !InForm(FORM_BEAR_FORM) && Cast("bear form");
                case Role::RangedDps:
                    return Knows("moonkin form") && !InForm(FORM_MOONKIN_FORM) && Cast("moonkin form");
                default:
                    return false;
            }
        }

        bool SelfBuffs(bool inCombat) override
        {
            if (inCombat)
                return WantedForm();
            if (Feral())
                return false;
            return Buff("omen of clarity", Me());
        }

        std::vector<std::string_view> OwnBuffs(Player* /*member*/) override
        {
            CasterForm();
            return { "mark of the wild", "thorns" };
        }

        bool GroupBuffs(Player* member) override
        {
            bool needsMark = Knows("mark of the wild") && !Has(member, "gift of the wild") && _ai.AuraRemaining(member, "mark of the wild") != -1
                && (!Has(member, "mark of the wild") || _ai.AuraRemaining(member, "mark of the wild") < 10000);
            bool needsThorns = _ai.IsTank(member) && Knows("thorns") && !Has(member, "thorns");
            if (!needsMark && !needsThorns)
                return false;
            CasterForm();
            if (needsMark && Cast("mark of the wild", member))
                return true;
            return needsThorns && Cast("thorns", member);
        }

        bool Heal(Unit* target, HealUrgency urgency, uint32 deficit) override
        {
            if (Feral() && MyRole() != Role::Healer && urgency != HealUrgency::Emergency)
                return false;
            CasterForm();
            if (urgency == HealUrgency::Emergency)
            {
                if ((Has(target, "rejuvenation") || Has(target, "regrowth")) && Cast("swiftmend", target))
                    return true;
                if (Cast("nature's swiftness"))
                    return true;
                if (Has(Me(), "nature's swiftness"))
                    return _ai.CastHeal("healing touch", target, deficit);
                return _ai.CastHeal("regrowth", target, deficit) || _ai.CastHeal("healing touch", target, deficit);
            }
            if (!Has(target, "rejuvenation", true) && Cast("rejuvenation", target))
                return true;
            if (urgency == HealUrgency::Normal && deficit > target->GetMaxHealth() / 3)
                return _ai.CastHeal("healing touch", target, deficit);
            if (!Has(target, "regrowth", true) && urgency == HealUrgency::Normal && _ai.CastHeal("regrowth", target, deficit))
                return true;
            return urgency != HealUrgency::TopUp && _ai.CastHeal("healing touch", target, deficit);
        }

        bool GroupHeal(uint32 injured) override
        {
            if (Feral())
                return false;
            if (Unit* lowest = _ai.LowestAlly(80.0f, 40.0f))
                if (Cast("wild growth", lowest))
                    return true;
            return injured >= 4 && !_ai.IsTargetedByEnemy() && Cast("tranquility");
        }

        bool Dispel(Unit* target) override
        {
            if (Feral())
                return false;
            uint32 mask = _ai.DispellableMask(target);
            if ((mask & MASK_CURSE) && Cast("remove curse", target))
                return true;
            return (mask & MASK_POISON) && (Cast("abolish poison", target) || Cast("cure poison", target));
        }

        bool Resurrect(Player* dead) override
        {
            // Forever gives druids Revive out of combat; Rebirth is the battle resurrection
            if (!Me()->IsInCombat() && Knows("revive"))
            {
                CasterForm();
                return Cast("revive", dead);
            }
            if (!Knows("rebirth") || !_ai.IsSpellReady("rebirth"))
                return false;
            CasterForm();
            return Cast("rebirth", dead);
        }

        bool Taunt(Unit* target) override { return (InForm(FORM_BEAR_FORM) || InForm(FORM_DIRE_BEAR_FORM)) && Cast("growl", target); }
        bool AoeTaunt() override { return (InForm(FORM_BEAR_FORM) || InForm(FORM_DIRE_BEAR_FORM)) && Cast("challenging roar"); }
        bool CanCrowdControl(Unit const* target) const override
        {
            return MyRole() != Role::Tank && Knows("hibernate") && (IsCreatureType(target, CREATURE_TYPE_BEAST) || IsCreatureType(target, CREATURE_TYPE_DRAGONKIN));
        }
        bool CrowdControl(Unit* target) override
        {
            if (!CanCrowdControl(target) || Has(target, "hibernate"))
                return false;
            CasterForm();
            return Cast("hibernate", target);
        }
        bool Interrupt(Unit* caster) override { return _ai.InMelee(caster) && Cast("bash", caster); }
        bool Stun(Unit* target) override { return (_ai.InMelee(target) && Cast("bash", target)) || RacialStun(target); }

        bool Emergency() override
        {
            if ((InForm(FORM_BEAR_FORM) || InForm(FORM_DIRE_BEAR_FORM)) && _ai.HealthPct() < 30.0f && Cast("frenzied regeneration"))
                return true;
            if (!Feral() && _ai.HealthPct() < 40.0f && _ai.IsTargetedByEnemy() && Cast("barkskin"))
                return true;
            // a cat that took it off the tank: Cower
            if (InForm(FORM_CAT_FORM) && _ai.View().Tank && _ai.View().Tank != Me())
                if (Unit* victim = Me()->GetVictim(); victim && victim->GetVictim() == Me() && Cast("cower", victim))
                    return true;
            if (MyRole() == Role::Healer && _ai.ManaPct() < 20.0f && !Has(Me(), "innervate") && Cast("innervate", Me()))
                return true;
            return false;
        }

        bool Combat(Unit* target) override
        {
            if (InForm(FORM_BEAR_FORM) || InForm(FORM_DIRE_BEAR_FORM))
            {
                if (!_ai.InMelee(target))
                    return Cast("feral charge", target);
                if (_ai.Rage() < 10 && _ai.HealthPct() > 60.0f && Cast("enrage"))
                    return true;
                uint32 nearby = _ai.EnemiesNear(Me()->GetPosition(), 8.0f);
                bool controlledNear = _ai.AnyCrowdControlledNear(Me()->GetPosition(), 10.0f);
                if ((nearby >= 2 || _ai.IsBossFight()) && !Has(target, "demoralizing roar") && Cast("demoralizing roar"))
                    return true;
                if (!HasAnyAura(target, { "faerie fire (feral)", "faerie fire" }) && (Cast("faerie fire (feral)", target) || Cast("faerie fire", target)))
                    return true;
                // a pack: Swipe on all; one: Lacerate stacks, then Swipe as the filler (Forever bears have no Maul)
                if (nearby >= 2 && !controlledNear && Cast("swipe", target))
                    return true;
                // in a pack the rage goes to Swipe: Lacerate only with plenty to spare
                if ((nearby < 2 || controlledNear || _ai.Rage() >= 35)
                    && (_ai.AuraStacks(target, "lacerate", true) < 5 || _ai.AuraRemaining(target, "lacerate", true) < 4000) && Cast("lacerate", target))
                    return true;
                if (!NextSwingQueued() && Knows("maul") && Cast("maul", target))
                    return true;
                if (Cast("primal bite", target))
                    return true;
                return !controlledNear && _ai.Rage() >= 20 && Cast("swipe", target);
            }

            if (InForm(FORM_CAT_FORM))
            {
                if (!_ai.InMelee(target))
                    return false;
                int32 cp = _ai.ComboPoints(target);
                if (cp >= 5 || (cp >= 3 && target->GetHealthPct() < 25.0f))
                    return (_ai.IsBossFight() && Bleeds(target) && Dot("rip", target)) || Cast("ferocious bite", target);
                if (Bleeds(target) && Dot("rake", target))
                    return true;
                if (_ai.IsBehind(target) && Cast("shred", target))
                    return true;
                return Cast("claw", target);
            }

            if (MyRole() == Role::Healer && _ai.ManaPct() < 80.0f)
                return false;
            if (Dot("moonfire", target) || Dot("insect swarm", target))
                return true;
            if (_ai.EnemiesNear(target->GetPosition(), 8.0f) >= 4 && !_ai.AnyCrowdControlledNear(target->GetPosition(), 12.0f) && !_ai.IsTargetedByEnemy()
                && _ai.SafeToAoe(target->GetPosition(), 10.0f) && Cast("hurricane", target))
                return true;
            if (target->GetVictim() != Me() && Cast("starfire", target))
                return true;
            return Cast("wrath", target);
        }
    };

    std::unique_ptr<ClassAI> CreateClassAI(CompanionAI& ai, uint8 classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR: return std::make_unique<WarriorAI>(ai);
            case CLASS_PALADIN: return std::make_unique<PaladinAI>(ai);
            case CLASS_HUNTER:  return std::make_unique<HunterAI>(ai);
            case CLASS_ROGUE:   return std::make_unique<RogueAI>(ai);
            case CLASS_PRIEST:  return std::make_unique<PriestAI>(ai);
            case CLASS_SHAMAN:  return std::make_unique<ShamanAI>(ai);
            case CLASS_MAGE:    return std::make_unique<MageAI>(ai);
            case CLASS_WARLOCK: return std::make_unique<WarlockAI>(ai);
            case CLASS_DRUID:   return std::make_unique<DruidAI>(ai);
            default:            return std::make_unique<WarriorAI>(ai);
        }
    }
}
