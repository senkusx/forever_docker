#include "CompanionPersonality.h"
#include "CompanionMgr.h"
#include "Player.h"
#include "Random.h"
#include "SharedDefines.h"
#include <map>
#include <vector>

namespace Companions::Personality
{
    namespace
    {
        enum class Voice : uint8 { Plain, Gruff, Cheerful, Formal, Sarcastic, Nervous, Boastful, Scholar, Pirate, Calm, Grumpy };

        struct Archetype
        {
            Voice Tone;
            char const* Text;
        };

        // the voices: how it talks, what it cares about, a quirk; varied on purpose so a party never sounds alike
        Archetype const Archetypes[] =
        {
            { Voice::Gruff, "Gruff veteran who has seen too many wipes. Few words, dry humour, never uses exclamation marks, calls everyone 'kid'." },
            { Voice::Gruff, "Battle-scarred mercenary, blunt and practical, judges people by how they pull. Short sentences." },
            { Voice::Cheerful, "Endlessly upbeat optimist who treats every pull like a festival. Lots of energy, compliments the party, loves loot." },
            { Voice::Cheerful, "Bubbly newcomer, eager to learn, asks earnest questions, celebrates small victories, a little clumsy with words." },
            { Voice::Formal, "Stiff, honour-bound knight who speaks in formal old-fashioned phrases and addresses the leader as 'commander'." },
            { Voice::Formal, "Prim court-trained scholar of etiquette, very polite, slightly condescending, never swears." },
            { Voice::Sarcastic, "Deadpan sarcastic cynic who comments on bad ideas but follows orders anyway. Dry one-liners." },
            { Voice::Sarcastic, "Snarky trickster who teases the party, makes bad puns, and is secretly loyal." },
            { Voice::Nervous, "Jumpy worrier who expects the worst from every pack, double-checks everything, relieved after each fight." },
            { Voice::Nervous, "Shy, quiet type who answers briefly and apologetically, opens up a bit about books and cooking." },
            { Voice::Boastful, "Swaggering show-off who counts kills, brags about damage meters and wants to pull bigger packs." },
            { Voice::Boastful, "Glory hound who narrates own deeds like a bard's epic, refers to self in the third person now and then." },
            { Voice::Scholar, "Lore nerd who drops trivia about the zone and its history, curious about every creature, a bit long-winded." },
            { Voice::Scholar, "Analytical min-maxer who talks in numbers, cooldowns and efficiency, gently critiques the rotation." },
            { Voice::Pirate, "Old sailor who talks like a pirate, everything is 'the sea', 'matey' and 'arr', fond of rum." },
            { Voice::Calm, "Serene, zen-like mystic, speaks softly in calm metaphors about balance and patience." },
            { Voice::Calm, "Laid-back drifter, unbothered by anything, casual slang, 'no worries' attitude." },
            { Voice::Grumpy, "Grumpy old-timer who complains about everything (the weather, the pull, kids these days) yet always shows up." },
            { Voice::Grumpy, "Perpetually hungry cynic who complains about food, mana and the walk, but is reliable in a fight." },
            { Voice::Plain, "Friendly, down-to-earth team player, practical, encouraging, keeps the chat light." },
        };

        Voice VoiceOf(std::string const& personality)
        {
            std::string lower = personality;
            for (char& c : lower)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            static std::pair<char const*, Voice> const keys[] = {
                { "pirate", Voice::Pirate }, { "gruff", Voice::Gruff }, { "blunt", Voice::Gruff }, { "veteran", Voice::Gruff },
                { "cheer", Voice::Cheerful }, { "upbeat", Voice::Cheerful }, { "bubbly", Voice::Cheerful }, { "eager", Voice::Cheerful },
                { "formal", Voice::Formal }, { "polite", Voice::Formal }, { "knight", Voice::Formal }, { "sarcas", Voice::Sarcastic },
                { "snark", Voice::Sarcastic }, { "cynic", Voice::Sarcastic }, { "nervous", Voice::Nervous }, { "worr", Voice::Nervous },
                { "shy", Voice::Nervous }, { "boast", Voice::Boastful }, { "brag", Voice::Boastful }, { "glory", Voice::Boastful },
                { "lore", Voice::Scholar }, { "scholar", Voice::Scholar }, { "min-max", Voice::Scholar }, { "zen", Voice::Calm },
                { "calm", Voice::Calm }, { "serene", Voice::Calm }, { "laid-back", Voice::Calm }, { "grump", Voice::Grumpy },
                { "complain", Voice::Grumpy } };
            for (auto const& [key, voice] : keys)
                if (lower.find(key) != std::string::npos)
                    return voice;
            return Voice::Plain;
        }

        using Lines = std::map<Voice, std::vector<char const*>>;
        std::map<std::string, Lines> const Table =
        {
            { "follow", {
                { Voice::Plain, { "Following.", "Right behind you.", "With you." } },
                { Voice::Gruff, { "Moving.", "Behind you, kid." } },
                { Voice::Cheerful, { "Right behind you!", "Lead the way!", "Ooh, where are we going?" } },
                { Voice::Formal, { "At your side, commander.", "I follow your lead." } },
                { Voice::Sarcastic, { "Following. Into what, I wonder.", "Lead on, oh fearless one." } },
                { Voice::Nervous, { "S-staying close.", "Right behind you. Very close behind you." } },
                { Voice::Boastful, { "Following. Try to keep up with me, though." } },
                { Voice::Scholar, { "Following. Optimal spacing maintained." } },
                { Voice::Pirate, { "Aye, in yer wake.", "Followin' ye, matey." } },
                { Voice::Calm, { "I walk with you.", "Sure thing, no rush." } },
                { Voice::Grumpy, { "Fine, fine. Walking.", "More walking. Wonderful." } } } },
            { "stay", {
                { Voice::Plain, { "Holding here.", "Staying put." } },
                { Voice::Gruff, { "Holding.", "Not moving." } },
                { Voice::Cheerful, { "Staying right here!", "Holding the fort!" } },
                { Voice::Formal, { "I shall hold this position.", "As you command, I remain." } },
                { Voice::Sarcastic, { "Standing. I'm great at standing.", "Holding. Riveting." } },
                { Voice::Nervous, { "Staying... alone... okay.", "Holding here. Don't be long?" } },
                { Voice::Boastful, { "I'll hold this spot single-handed." } },
                { Voice::Scholar, { "Holding position. Noted." } },
                { Voice::Pirate, { "Droppin' anchor.", "Aye, holdin' fast." } },
                { Voice::Calm, { "I will be still.", "Chilling right here." } },
                { Voice::Grumpy, { "Standing around, as usual.", "Fine. My feet hurt anyway." } } } },
            { "come", {
                { Voice::Plain, { "On my way.", "Coming." } },
                { Voice::Gruff, { "Coming." } },
                { Voice::Cheerful, { "Coming, coming!", "On my way!" } },
                { Voice::Formal, { "I come at once." } },
                { Voice::Sarcastic, { "Running. Look at me run." } },
                { Voice::Nervous, { "Coming! Wait for me!" } },
                { Voice::Boastful, { "Here in a flash." } },
                { Voice::Scholar, { "Recalculating route. Coming." } },
                { Voice::Pirate, { "Hoistin' sails, comin' about!" } },
                { Voice::Calm, { "On my way, no stress." } },
                { Voice::Grumpy, { "Yes, yes, I'm coming." } } } },
            { "rest", {
                { Voice::Plain, { "Taking a break.", "Eating and drinking." } },
                { Voice::Gruff, { "Drinking." } },
                { Voice::Cheerful, { "Snack time!", "Break time, yay!" } },
                { Voice::Formal, { "I shall recover my strength." } },
                { Voice::Sarcastic, { "Finally, a break. Don't pull without me." } },
                { Voice::Nervous, { "Thank goodness, I need a minute." } },
                { Voice::Boastful, { "Resting. Not that I need it." } },
                { Voice::Scholar, { "Regenerating. Ninety seconds, give or take." } },
                { Voice::Pirate, { "Time fer a swig o' rum!" } },
                { Voice::Calm, { "A pause for breath." } },
                { Voice::Grumpy, { "About time. I'm starving." } } } },
            { "burst", {
                { Voice::Plain, { "Cooldowns going!", "Popping everything." } },
                { Voice::Gruff, { "Everything I've got." } },
                { Voice::Cheerful, { "Going all out!", "Here comes the big stuff!" } },
                { Voice::Formal, { "Unleashing my full might." } },
                { Voice::Sarcastic, { "Oh, NOW we try hard." } },
                { Voice::Nervous, { "Okay, okay, everything, now!" } },
                { Voice::Boastful, { "Watch the meters, people." } },
                { Voice::Scholar, { "All cooldowns aligned." } },
                { Voice::Pirate, { "Fire all cannons!" } },
                { Voice::Calm, { "The storm awakens." } },
                { Voice::Grumpy, { "Fine, burning it all." } } } },
            { "focus", {
                { Voice::Plain, { "Focusing {x}.", "On {x}." } },
                { Voice::Gruff, { "{x}. Got it." } },
                { Voice::Cheerful, { "Going for {x}!" } },
                { Voice::Formal, { "{x} shall fall first." } },
                { Voice::Sarcastic, { "{x} it is. Poor thing." } },
                { Voice::Nervous, { "{x}? Okay, {x}." } },
                { Voice::Boastful, { "{x} is mine." } },
                { Voice::Scholar, { "Priority target: {x}." } },
                { Voice::Pirate, { "Boardin' {x}!" } },
                { Voice::Calm, { "{x} it is." } },
                { Voice::Grumpy, { "{x}. Whatever." } } } },
            { "prefer", {
                { Voice::Plain, { "Got it, more {x}.", "More {x} it is." } },
                { Voice::Gruff, { "More {x}. Fair." } },
                { Voice::Cheerful, { "Ooh, good idea, more {x}!" } },
                { Voice::Formal, { "Very well, I shall favour {x}." } },
                { Voice::Sarcastic, { "More {x}. Why didn't I think of that." } },
                { Voice::Nervous, { "M-more {x}, right, I'll try." } },
                { Voice::Boastful, { "More {x}? Easy. Watch this." } },
                { Voice::Scholar, { "Re-weighting my rotation towards {x}." } },
                { Voice::Pirate, { "Aye, more {x} it be." } },
                { Voice::Calm, { "{x}, more often. Understood." } },
                { Voice::Grumpy, { "Fine, more {x}. Happy now?" } } } },
            { "avoid", {
                { Voice::Plain, { "Okay, no more {x}.", "Leaving {x} alone." } },
                { Voice::Gruff, { "No {x}. Noted." } },
                { Voice::Cheerful, { "Oops, no more {x} then!" } },
                { Voice::Formal, { "I shall refrain from {x}." } },
                { Voice::Sarcastic, { "Fine, {x} was my favourite, but fine." } },
                { Voice::Nervous, { "Sorry! No more {x}, promise." } },
                { Voice::Boastful, { "I don't need {x} anyway." } },
                { Voice::Scholar, { "{x} removed from the rotation." } },
                { Voice::Pirate, { "Stowin' {x} below deck." } },
                { Voice::Calm, { "I let {x} go." } },
                { Voice::Grumpy, { "No {x}. Always something." } } } },
            { "ok", {
                { Voice::Plain, { "On it.", "Got it.", "Sure." } },
                { Voice::Gruff, { "Done.", "Right." } },
                { Voice::Cheerful, { "You got it!", "Sure thing!" } },
                { Voice::Formal, { "As you wish.", "It shall be done." } },
                { Voice::Sarcastic, { "Sure, why not.", "Your call, boss." } },
                { Voice::Nervous, { "O-okay.", "I'll try." } },
                { Voice::Boastful, { "Easy.", "Consider it done." } },
                { Voice::Scholar, { "Acknowledged.", "Noted." } },
                { Voice::Pirate, { "Aye aye.", "Aye, cap'n." } },
                { Voice::Calm, { "As it should be.", "No worries." } },
                { Voice::Grumpy, { "Fine.", "If I must." } } } },
        };
    }

    std::string Random(uint8 race, uint8 classId)
    {
        // a few nudges: pirate-talking sailors are goblins and humans more often, zen mystics are druids and shamans...
        std::vector<Archetype const*> pool;
        for (Archetype const& archetype : Archetypes)
        {
            uint32 weight = 2;
            if (archetype.Tone == Voice::Pirate && race != RACE_HUMAN && race != RACE_GOBLIN && race != RACE_TROLL)
                weight = 1;
            if (archetype.Tone == Voice::Calm && (classId == CLASS_DRUID || classId == CLASS_SHAMAN || classId == CLASS_PRIEST))
                weight = 4;
            if (archetype.Tone == Voice::Formal && classId == CLASS_PALADIN)
                weight = 4;
            if (archetype.Tone == Voice::Grumpy && race == RACE_DWARF)
                weight = 4;
            if (archetype.Tone == Voice::Boastful && (classId == CLASS_WARRIOR || classId == CLASS_ROGUE || classId == CLASS_HUNTER))
                weight = 3;
            for (uint32 i = 0; i < weight; ++i)
                pool.push_back(&archetype);
        }
        return pool[urand(0, pool.size() - 1)]->Text;
    }

    std::string Line(Player* bot, std::string const& kind, std::string const& detail)
    {
        BotRecord const* record = bot ? sCompanionMgr->GetRecord(bot->GetGUID()) : nullptr;
        Voice voice = record ? VoiceOf(record->Personality) : Voice::Plain;
        auto kindItr = Table.find(kind);
        if (kindItr == Table.end())
            return detail;
        auto voiceItr = kindItr->second.find(voice);
        if (voiceItr == kindItr->second.end())
            voiceItr = kindItr->second.find(Voice::Plain);
        std::string line = voiceItr->second[urand(0, voiceItr->second.size() - 1)];
        for (size_t at = line.find("{x}"); at != std::string::npos; at = line.find("{x}"))
            line.replace(at, 3, detail);
        return line;
    }
}
