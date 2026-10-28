/*
 * Companions: the bot brain, what players told it about its abilities ("use more Frost Nova, adds keep reaching the
 * healer", "stop using Arcane Explosion", "Shield Wall when you drop below 30%"). Kept with the companion
 * (companion_bots.prefs) and used in every fight: avoided spells are never cast on its own, preferred ones get the
 * first chance whenever they help.
 */

#include "CompanionAI.h"
#include "CompanionMechanics.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <sstream>

namespace Companions
{
    namespace
    {
        std::vector<std::string> SplitOn(std::string const& text, char separator)
        {
            std::vector<std::string> parts;
            std::string part;
            std::istringstream in(text);
            while (std::getline(in, part, separator))
                parts.push_back(part);
            return parts;
        }

        std::string Lower(std::string text)
        {
            for (char& c : text)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        bool Heals(SpellInfo const* info)
        {
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.IsEffect(SPELL_EFFECT_HEAL) || effect.IsAura(SPELL_AURA_PERIODIC_HEAL))
                    return true;
            return false;
        }
    }

    // "prefer|frost nova|0|adds reach the healer;avoid|arcane explosion|0|"
    void CompanionAI::LoadPrefs(std::string const& text)
    {
        _prefs.clear();
        for (std::string const& entry : SplitOn(text, ';'))
        {
            std::vector<std::string> fields = SplitOn(entry, '|');
            if (fields.size() < 2 || fields[1].empty())
                continue;
            SpellPref pref;
            pref.Avoid = fields[0] == "avoid";
            pref.Spell = Lower(fields[1]);
            pref.BelowPct = fields.size() > 2 ? uint8(std::min<unsigned long>(std::strtoul(fields[2].c_str(), nullptr, 10), 100)) : 0;
            pref.Reason = fields.size() > 3 ? fields[3] : "";
            _prefs.push_back(pref);
        }
    }

    std::string CompanionAI::SavePrefs() const
    {
        std::string out;
        for (SpellPref const& pref : _prefs)
        {
            std::string reason = pref.Reason;
            for (char& c : reason)
                if (c == '|' || c == ';')
                    c = ',';
            out += (out.empty() ? "" : ";") + std::string(pref.Avoid ? "avoid" : "prefer") + "|" + pref.Spell + "|"
                + std::to_string(pref.BelowPct) + "|" + reason;
        }
        return Utf8Prefix(out, 1000);
    }

    bool CompanionAI::SetPref(std::string const& spell, bool avoid, uint8 belowPct, std::string const& reason)
    {
        if (!_spells.Has(spell))
            return false;
        ClearPref(spell);
        _prefs.push_back({ spell, avoid, belowPct, Utf8Prefix(reason, 120) });
        while (_prefs.size() > 12)
            _prefs.erase(_prefs.begin());
        return true;
    }

    bool CompanionAI::ClearPref(std::string const& spell)
    {
        size_t before = _prefs.size();
        _prefs.erase(std::remove_if(_prefs.begin(), _prefs.end(), [&](SpellPref const& pref) { return pref.Spell == spell; }), _prefs.end());
        return _prefs.size() != before;
    }

    std::string CompanionAI::MatchSpell(std::string const& text) const
    {
        std::string lower = Lower(text);
        std::string best;
        for (std::string const& name : _spells.Names())
        {
            if (name.size() <= best.size())
                continue;
            size_t at = lower.find(name);
            // whole words: "rend" is not in "friend"
            if (at != std::string::npos && (at == 0 || !std::isalpha(static_cast<unsigned char>(lower[at - 1])))
                && (at + name.size() == lower.size() || !std::isalpha(static_cast<unsigned char>(lower[at + name.size()]))
                    || lower[at + name.size()] == 's'))
                best = name;
        }
        return best;
    }

    bool CompanionAI::IsAvoided(std::string_view name) const
    {
        if (_allowAvoided)
            return false;
        for (SpellPref const& pref : _prefs)
            if (pref.Avoid && pref.Spell == name)
                return true;
        return false;
    }

    std::string CompanionAI::PrefsLine() const
    {
        std::string out;
        for (SpellPref const& pref : _prefs)
        {
            out += out.empty() ? "" : "; ";
            out += pref.Avoid ? "never " + pref.Spell : "more " + pref.Spell;
            if (pref.BelowPct)
                out += " below " + std::to_string(pref.BelowPct) + "% health";
            if (!pref.Reason.empty())
                out += " (" + pref.Reason + ")";
        }
        return out;
    }

    // a preferred ability gets the first chance whenever it helps (an area spell with enemies in it, a buff that is not
    // up, below the health it was asked for), tried at most every 1.5 s each so the rotation still runs
    bool CompanionAI::UsePreferred(Unit* target)
    {
        TimePoint const now = GameTime::Now();
        for (SpellPref const& pref : _prefs)
        {
            if (pref.Avoid || (pref.BelowPct && HealthPct() > pref.BelowPct))
                continue;
            TimePoint& tried = _prefTried[pref.Spell];
            if (now < tried)
                continue;
            tried = now + Milliseconds(1500);
            uint32 spellId = SpellIdOf(pref.Spell);
            SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE) : nullptr;
            if (!info || Heals(info))
                continue;   // heals: HealPreferred
            if (info->IsPositive())
            {
                if (!HasAura(_bot, pref.Spell) && Cast(pref.Spell))
                    return true;
                continue;
            }
            if (info->GetExplicitTargetMask() & TARGET_FLAG_UNIT_MASK)
            {
                if (target && Cast(pref.Spell, target))
                    return true;
                continue;
            }
            // around itself (Frost Nova, Thunder Clap, Arcane Explosion, Consecration): only with enemies in reach
            float radius = std::max(8.0f, Mechanics::CasterAreaRadius(info));
            if (EnemiesNear(_bot->GetPosition(), radius) > 0 && Cast(pref.Spell))
                return true;
        }
        return false;
    }

    bool CompanionAI::HealPreferred(Unit* target, uint32 deficit)
    {
        for (SpellPref const& pref : _prefs)
        {
            if (pref.Avoid)
                continue;
            uint32 spellId = SpellIdOf(pref.Spell);
            SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE) : nullptr;
            if (info && Heals(info) && CastHeal(pref.Spell, target, deficit))
                return true;
        }
        return false;
    }
}
