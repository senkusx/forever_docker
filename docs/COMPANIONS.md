# Companions: play with bots

Companions are bots that play alongside you like real players, so a dungeon party works with just you. They are
real characters (not NPCs): they group with you, take loot rules, quest credit and XP, and enter dungeons with you.
A server-side AI plays each one in its role:

- **Tank**: leads in front of you. Warriors wait in Battle Stance, charge the pull and switch to Defensive Stance;
  taking threat back comes first (healers first): taunt, the biggest hits when it is on cooldown, Challenging Shout /
  Roar when several got loose, and it keeps switching to the enemy it holds by the thinnest margin before a damage
  dealer pulls it. Warriors: Shield Slam, Revenge, Sunder, Thunder Clap, Demoralizing Shout, Concussion Blow, Shield
  Block, Cleave. Paladins (no taunt): Righteous Fury, Seal of Fury, Judgement, Consecration, Holy Shield, Holy Strike,
  Holy Wrath and Exorcism on undead and demons. Bears: Swipe on packs, Lacerate, Demoralizing Roar, Faerie Fire.
- **Healer**: heals by priority (tank first), picks the cheapest rank that covers the damage, counts the heals
  of the other healers, uses group heals, shields and HoTs, dispels and resurrects.
- **Damage**: attacks the skull, then the cross, then your target, then the tank's target. On a pull the melee walk
  up with the tank and wait behind the enemy until the tank has it. They stand behind enemies (melee) or at range
  (casters, hunters), interrupt casts, use AoE only when no crowd-controlled enemy is near, and keep an add out of a
  pack of three or more by themselves (polymorph, banish, hibernate, scare beast), calling it out like players do.
  Hunters feign death and rogues feint when they pull aggro; cooldowns go on bosses or on `burst`.
- **Everyone**: steps out of fire and other hostile ground effects (Flamestrike, Rain of Fire, Blizzard, Consecration,
  traps, the area triggers of newer content) at once, even mid-cast, and keeps out of them while they last. Buffs the group (Fortitude, Arcane Intellect, Mark of the Wild, blessings, totems...), follows you
  loosely like players do, each in its part of the group (tank in front, melee beside you, healers and casters a
  little further back), shuffling now and then, calls out what matters (out of mana, low health, adds, a loose moon, someone down, a rare or a
  dangerous elite nearby), mounts when you mount, eats and drinks after fights, accepts resurrections, ready checks and shared
  quests, and joins you when you change zone or enter a dungeon.

The 9 classic classes are supported, with their spells resolved by name to the best rank the bot knows. Hunters
get a pet and warlocks summon demons. Bots you create level with you and learn their trainer spells. They also
get talents for their role and the best gear for their level and role from what drops or is sold on this server.

## Quick start

```
.bot party                 a balanced party (tank, healer, damage) for your role
.bot party heal            ... when you are the healer
.bot raid [size]           a raid (default 20): 2 tanks, a healer for every 5, the rest damage
.bot create mage           one bot of a class (optional role and name: .bot create druid tank Bramble)
.bot add Myalt             one of your own characters, played by the AI
.bot list                  your companions
.bot dismiss               all leave (".bot dismiss Jaina" for one)
```

Up to 19 companions per character fit in a 20-player raid (`Companions.RaidSize`); the server holds 39 at once in
total by default (`Companions.MaxTotal`). Your companions come back when you log in again. A bot you create levels to your level when it joins and follows
your level ups. Your own characters keep their level and gear, and gain XP and loot with you.

## Web panel

`https://<your website>/companions` (or `http://<server>:8088/companions`): log in with your game account. Only
accounts with GM level 3 get in (`.account set gmlevel <game account> 3 -1` from the worldserver console). For each
of your characters:

- **Party**: live health, mana, item level, talents and zone of each companion. Give orders to all or one, summon or
  dismiss, and set the role, talents, gear quality, aggression, stance, AI brain and personality. Changes wait on the
  card (marked in gold) until **Apply**, which confirms what was applied and when it acts (talents and gear are
  relearned within seconds for a companion with you). Talents: one meta build per tree, as played in classic raids
  (for example Fury 17/34/0, Protection 8/5/38, Holy 31/20/0), or the role's one. Rename (while resting), new gear, new talents, delete. Whole-party settings: brain on or off for
  all, aggression, stance and gear quality, plus summon or dismiss all. Also per companion: looting (what it can use,
  everything, nothing), potions when low, crowd control of extra adds on its own.
- **Details** (on each companion): its gear (take a piece off), its bags (equip, destroy; upgrades marked), its stats
  (health, mana, attributes, armor, attack and spell power, crit, dodge, parry, block, resistances), its talents by tree,
  and its abilities, each set to normal, use more or never use (with what players said about it). Stats, talents and
  abilities need it in the game; gear and bags show either way.
- **New companion**: class, race (the ones your faction can play, with Forever's Undead and Tauren paladins, Dwarf
  shamans and Troll druids), gender, custom or random first name and family name (the dice give both, of that race and
  gender; companions made in game with a random name get a family name too), role, talent
  tabs, gear quality, aggression, stance, brain and personality, and the appearance (each option picked or random).
- **Templates**: save a companion as a template, and build party templates from them with their group settings.
  Applying a party template brings back the companions made from it, creates the missing ones and can dismiss the
  others, so you can switch between, say, a dungeon party and a questing duo.

- **Server & AI** (whole server): the limits (companions per character, on the server, largest group), follow
  distances, behaviour switches, the AI switches, the AI providers and the remote brain workers (below). What is
  changed here is stored in `characters.companion_settings` and wins over `worldserver.conf`; "back to default" uses
  the file again.

The panel sends `.bot admin` commands to the worldserver over SOAP (internal network only) and checks your password
through bnetserver's login, like the launcher.

## Orders

Type them in party chat. Start with a bot's name to talk to one bot ("Jaina stay"), or whisper it.

| Order | What it does |
|---|---|
| `follow` / `stay` / `come` | follow you / hold position / run back to you |
| `attack` / `pull` | attack your target / the tank pulls your target, the others wait for the fight |
| `focus skull` (or cross, moon...) | attack the marked target |
| `assist <name>` | attack what that player attacks |
| `heal <name>` / `dispel <name>` / `protect <name>` | heal them now / cleanse them / the tank taunts off them, the others help them (`me` works) |
| `spread` / `stack` / `normal` | stand farther apart / close together / as usual |
| `retreat` | run back to you and stop fighting for a moment |
| `cc <marker>` / `interrupt <marker>` / `taunt <marker>` | keep it crowd controlled / interrupt its casts / the tank takes it |
| `burst` | offensive cooldowns now |
| `bless <name\|me> salvation` (or `salvation on me`) | a paladin gives that blessing (salvation, might, wisdom, kings, light, sanctuary) from now on |
| `rebuff <name\|me> [buff]` | cast the buff again now, even if it is still up (`rebuff me intellect`); passed to a companion that has it |
| `equip it` / `equip <item>` / `equip upgrades` | put on what you handed over (master loot), an item by name, or everything better in its bags |
| `stop` | stop attacking for a few seconds |
| `passive` / `assist` / `aggressive` | never attack / fight what the group fights (default) / also attack hostiles nearby |
| `role tank` / `role heal` / `role dps` / `role auto` | change role (talents and gear follow) |
| `rest` | eat and drink to full |
| `cast <spell> [on me / on target]` | cast a spell now ("cast polymorph on target") |
| `tactics` | the encounter in a sentence or two: your target's, else the boss they fight |
| `plan <steps>` | several steps carried out in order (see below); `plan` shows it, `cancel plan` stops it |
| `prefer <spell> [below 30]` / `avoid <spell>` / `allow <spell>` / `prefs [clear]` | lasting changes to how it uses an ability (see below) |
| `give me <item\|potions\|food\|drink\|bandages\|junk>` | it opens a trade with you and puts it in |
| `loot all\|useful\|off`, `potions on\|off`, `autocc on\|off` | what it picks up from its corpses, potions when low, crowd control of extra adds on its own |
| `status` | health, mana, role |
| `gear` | look again for the best gear |
| `dismiss` | leave |

Plain words work too, without the brain: "go all out", "hold up", "fall back", "help me", "take a breather" are the
orders they mean.

**Plans.** Several steps in one go, said plainly or as an order:

- "attack this pack, then wait for full mana and then we attack the other pack when we have full health and mana and
  everyone confirmed they're ready" becomes: pull your target's pack, rest to full, wait until every player of the
  group says `ready` (or `r`, `go`), pull the next pack;
- "when we attack this pack, I want the mage (or a name) to polymorph this mob": the mage keeps your target controlled,
  the tank pulls another one of the pack, and the controlled one dies last;
- the order form, the one the brain writes: `plan pull p1, jaina cc e5; rest; ready; pull next` (P1, P2... are the packs
  around, E1, E2... the creatures, both numbered for the brain; `next` is the nearest pack when the step comes;
  `rest 80`, `wait 10`).

**What you tell them about their abilities** stays with them (and shows in the web panel): "Jaina, use more Frost Nova
because the adds keep reaching the healer", "stop using Curse of Recklessness, it pulls extra mobs", "Shield Wall when
you drop below 30%". A spell to use more gets the first chance whenever it helps (an area spell with enemies around,
a buff that is not up, below the health named); one to avoid is never cast on their own (`cast <spell>` still does).

**Items.** Open a trade with a companion and it accepts once you accept; ask for something and it opens the trade
(`give me your potions`). It thanks you for what you hand over and tells you whether it would wear it (`equip it`).
Companions loot the corpses that fall to them under your loot rules: coins and what they can use (food, drink,
potions, ammunition, gear better than theirs, which they put on). In fights they drink a healing potion or use a
healthstone below 30% health, a mana potion below 15% mana.

**Personality.** Each companion gets a personality of its own (a gruff veteran, a cheerful newcomer, a sarcastic
trickster, a pirate, a lore nerd...) unless you write one in the web panel; the brain speaks in it, and their short
answers without the brain ("Following.", "More Frost Nova, got it.") are said in that voice too.

Other players in your group can ask your companions for help (`heal me`, `Jaina dispel me`, `protect Bob`,
`assist Bob`); following, roles and stance stay yours.

Raid markers work like with players: **skull** = kill first, **cross** = kill second, **moon** = crowd control,
do not break it.

Other commands: `.bot role <name|all> <role>`, `.bot gear [name]`, `.bot talents <name> [respec]`,
`.bot personality <name> <text>`, `.bot llm on|off [name]`, `.bot delete <name>` (only companions you created;
your own characters are never deleted), `.bot help`.

## Boss fights

The companions know the scripted encounters of the classic dungeons and raids (Molten Core, Onyxia, Blackwing Lair,
Zul'Gurub, both Ahn'Qiraj, Naxxramas and the dungeon bosses), with the spell and creature ids of this server's scripts
(`docker/companions/src/CompanionMechanics.cpp`, 150 rules and 75 boss notes). In a fight they:

- step out of what a boss casts around itself (Inferno, Whirlwind, Locust Swarm, Arcane Explosion...) and out of the band
  of a line attack (Onyxia's Deep Breath, Impale, Dark Glare);
- keep out of the front of breaths and cleaves, and out of the tail too where it sweeps (Onyxia, Vaelastrasz, the BWL
  drakes, Nefarian, Sapphiron): melee stand behind or at a flank, ranged at a side;
- run away from everyone with debuffs that hurt the others (Living Bomb, Burning Adrenaline, Mutating Injection,
  Detonate Mana, Conflagration), and the others keep away from whoever has one;
- spread where hits jump from player to player (C'Thun, Hakkar, Kel'Thuzad, Zeliek...), keep away from void zones,
  tornadoes, clouds and fissures;
- kill first the adds a fight is won by (Flamewaker Priests and Healers, Sons of Flame, Jin'do's totems, Larvae, Spawns
  of Fankriss, Web Wraps, Eye Tentacles...) and leave alone what must not be attacked (Core Ragers, Majordomo, the twin
  emperor they are immune to; casters stop on Magic Reflection, melee on Damage Shield);
- interrupt heals, fears and the casts a rule names first (Dark Mending, Great Heal, Holy Fire, Arcane Explosion, Frostbolt);
  dispel the debuffs a rule names first and leave those that must stay (Hakkar's Poisonous Blood, Wyvern Sting);
- swap tanks on stacks (Mortal Wound, the Horsemen's marks, Shadow of Ebonroc, Burning Adrenaline);
- answer fears (priests keep Fear Ward on the tank, shamans drop Tremor Totem, warriors Berserker Rage), Tranquilizing Shot
  frenzies, purge Deaden Magic and Renew, crowd control mind controlled friends (never kill them), stand still under
  Mandokir's Threatening Gaze, stop healing under Nefarian's priest call, take their Thaddius charge to its side and hide
  behind an ice block from Sapphiron's Frost Breath.

Casts nobody wrote a rule for are read from the spell itself: an area around the caster with a cast time is left by all
but the tank, a boss seen casting a cone is not faced again, heals and crowd control are interrupted first. The brain
gets each boss's note with its tactical review. Some fights still need a player: Razorgore's orb, Razuvious's
Understudies (priest mind control), Heigan's dance and Gothik's split are only described to the bots.

**40-player raids for 20**: Onyxia, Molten Core, Blackwing Lair, Temple of Ahn'Qiraj and Naxxramas are tuned for a raid
of 20: their creatures, adds the scripts summon included, have half their health (`Raid20.Health`, applied when they
spawn: reset the instance after a change) and deal 80% of their damage to players and pets (`Raid20.Damage`). Zul'Gurub and
Ruins of Ahn'Qiraj are 20-player raids already. `Raid20.Enable 0` brings the 40-player numbers back.

## A living world: ambient bots

Random characters appear around players in the open world and live there on their own, so a zone does not feel empty:

- each has a job for its time in the world: in the open world a hunter working one kind of mob in one spot (they
  respawn), or a quester going between a quest giver and the mobs around it (never a mob a player is near); in capital
  cities a trader at the auction house (and the bank), a crafter calling out its trade (enchanting, tailoring,
  blacksmithing, alchemy... "Enchanter LF work!"), a shopper with a few errands (a vendor, its class trainer, the bank)
  who then leaves by flight master, or someone resting at the inn;
- their level comes from the mobs around (any level, half of them at the cap, in a city); their gear is a fresh outfit
  each time: greens of their level (item level within a few of theirs), their armor type (plate warriors and paladins
  from 40, mail hunters and shamans from 40, leather, cloth), stats of their class (random enchantments such as "of the
  Bear" chosen to fit) and weapons of their class, picked at random from all that fit so no two look alike; at the level
  cap a blue now and then (`Ambient.GearQuality` caps it);
- they have Forever's first and family names: first names from the game's own name generator for their race and gender,
  family names in their people's style (Hawthorne, Barleybrew, Feathermoon, Cogspinner, Thunderhoof...);
- a zone of one faction gets bots of that faction (a Horde zone gets Horde bots, even around an Alliance player);
  contested zones (Stranglethorn, Arathi, Hillsbrad...) both;
- they leave when no player has been around for 15 seconds, after dying, or after 8 to 20 minutes out of sight, and
  others come;
- **invite one to your group and it joins you as a companion** (yours from then on: `.bot` commands, the web panel,
  level and gear with you), as long as you have room for another companion. Whispered, they answer that they are busy
  and to send an invite.

`Ambient.Density` sets how many are around each player in the open world (5), `Ambient.CityDensity` in a capital (15),
`Ambient.MaxTotal` on the whole server (60), `Ambient.Radius` how far around a player they count (150 yards); all of
them in the web panel, "A living world". The characters are kept in a pool (`Ambient.PoolSize`, 200; table
`characters.companion_ambient`) and reused, levelled up where needed; when the pool is full one far above the level
needed is deleted for a new one. They count as online players.

## Optional: the AI brain

With the brain on, an AI model speaks for the companions linked to it. They hear the party chat and react like
players: the one you name answers ("Jaina, how much mana do you have?"), the one concerned answers a callout
("need heals!" gets the healer's answer and a `heal <you>`), they relay what they see (health and mana of the group,
enemies and whom they attack, raid markers, rares and elites nearby, what was said earlier) and word their own
callouts. Plain language becomes orders ("hold here and let the tank pull" gives `stay` plus `pull`). Combat stays
with the server AI, so a slow answer never costs a fight.

**Tactics**: in a fight worth it (a pack of three or more, two with an elite, a boss), the brain reviews the
situation when it starts and every `LLM.TacticsInterval` seconds: enemies are numbered (E1, E2...) with their type,
target and what they cast, and it assigns kill order, crowd control, interrupts, taunts, raid markers (that you see
too) and cooldowns, with one short call in party chat. The server AI carries it out at once and checks everything
(a mage cannot polymorph an undead); the brain is too slow to press buttons, so it leads instead. Off with
`LLM.Tactics = 0`.

Providers (web panel, Server & AI): **Claude** (Anthropic API), **Ollama**, or any **OpenAI-compatible** server
(llama.cpp, vLLM, LM Studio, LocalAI, OpenRouter...). Each has a weight and a number of requests at once; the lines
are shared between the providers with free slots, and a provider that fails hands its line to another. A companion
can be tied to one provider (its card, the group settings or a template), else any provider answers. Small local
models only give help orders (heal, dispel, protect, assist) because they tend to add random orders to small talk:
`{"orders": "all"}` in a provider's options allows every order, `{"orders": "none"}` makes it only talk. `{"kinds":
{"callout": 8, "event": 8}}` limits a provider to some lines with their own weights: a small model on a second card
(for example `qwen2.5:3b` on a 3 GB GTX 1060, about 1 s per callout, with `"num_gpu": 99` so all of it stays on the card)
takes the callouts, while the bigger one keeps the conversations and the tactics. Line kinds: `chat`, `group`,
`callout`, `event`, `tactics`.

1. In `.env`: `COMPANIONS_LLM=1` (and `ANTHROPIC_API_KEY=sk-ant-...` to start with Claude as the first provider),
   then `docker compose up -d worldserver` and `docker compose --profile brain up -d brain`.
2. A local model on this server: `docker compose --profile brain --profile ollama up -d`, then
   `docker compose exec ollama ollama pull qwen2.5:7b` and add the provider "Ollama", address `http://ollama:11434`.
   A GPU makes it fast (see the `deploy` lines of the `ollama` service); on a CPU a 7B model takes several seconds.
3. Test each provider with its **Test** button: the answer, the time, and which brain answered it.

**Brains on other machines** (remote workers): spread the work over a PC with a GPU or another server. In the panel,
Server & AI > Remote brain workers > Add: the token is shown once, with the command to run there:

```sh
docker build -t forever-companions-brain docker/companions/brain
docker run -d --name companions-brain --restart unless-stopped --add-host=host.docker.internal:host-gateway \
  -e BRAIN_URL=https://your.site -e BRAIN_TOKEN=cbw_... forever-companions-brain
```

Then add providers "run by" that worker, for example Ollama at `http://host.docker.internal:11434` (Ollama on the
worker's machine). The worker only needs to reach the website (`/api/brain/*`, the token's hash is all the server
keeps); it fetches its providers, takes the lines its providers can answer and posts the answers. Deleting a worker
turns its providers off.

**A GPU server the website cannot be reached from** (the website only on the LAN): keep Ollama private on the GPU
server (`OLLAMA_HOST=127.0.0.1:11434`) and let the game server open an SSH tunnel to it, for example a small
`autossh -N -L 0.0.0.0:11434:127.0.0.1:11434 tunneluser@gpu-server` container on the compose network with the alias
`gpu-ollama.internal`, and a tunnel user restricted to that port
(`restrict,port-forwarding,permitopen="127.0.0.1:11434"` in its authorized_keys). The provider address is then
`http://gpu-ollama.internal:11434`: Ollama listening on localhost refuses other host names (it accepts `localhost`,
`*.internal`, `*.local` and IP addresses). Sizes that fit: an 8 GB card (GTX 1080, RTX 2070...) runs `qwen2.5:7b`
(about 5.6 GB with an 8k context, 1-3 s per answer on a GTX 1080, good at French, Spanish and Portuguese too), with
`{"orders": "all"}`; a 4 GB card `qwen2.5:3b`. A GTX 10xx (Pascal) needs the NVIDIA 580 driver branch (the last one
for these cards).

`COMPANIONS_LLM_MAX_PER_MINUTE` (default 60) caps the requests of each brain. `.bot personality Jaina grumpy dwarf
who loves ale` (or the panel) gives a bot a character. Banter (comments on level ups) is off by default.

## Settings

`overrides/worldserver.conf` (see `overrides/worldserver.conf.example`):

| Key | Default | |
|---|---|---|
| `Companions.Enable` | 1 | |
| `Companions.MaxPerPlayer` | 4 | 4 fills a 5-man party; at most `RaidSize` - 1 |
| `Companions.MaxTotal` | 39 | companions in the game at once, whole server |
| `Companions.RaidSize` | 20 | largest group companions fill (5-40) |
| `Companions.Follow.MinDistance` / `MaxDistance` | 2 / 9 | yards from you when they gather around |
| `Companions.Callouts` | 1 | bots without AI call out the essentials themselves |
| `Companions.AllowAlts` | 1 | `.bot add` works with your own characters |
| `Companions.SyncLevel` | 1 | created bots follow your level |
| `Companions.AutoGear` | 1 | new gear when their level changes |
| `Companions.Gear.MaxQuality` | 3 | 2 green, 3 blue, 4 epic |
| `Companions.FreeReagents` | 1 | reagents, ammo, totems and soul shards are free |
| `Companions.FreeFood` | 1 | eat and drink without items |
| `Companions.AutoRevive` | 1 | revive next to you when nobody can resurrect them |
| `Companions.AutoRevive.Delay` | 30 | seconds out of combat before that |
| `Companions.TeleportDistance` | 90 | out of combat, a bot farther than this (or stuck) joins you |
| `Companions.MountLevel` | 40 | |
| `Companions.AnnounceInParty` | 1 | bots acknowledge orders in party chat |
| `Companions.LLM.Enable` | 0 | set by `COMPANIONS_LLM=1` |
| `Companions.LLM.PartyChat` | 1 | the brain hears every party line that is not an order |
| `Companions.LLM.Callouts` | 1 | the brain words the callouts of the bots linked to it |
| `Companions.LLM.Tactics` | 1 | the brain leads fights (kill order, crowd control, interrupts) |
| `Companions.LLM.TacticsInterval` | 15 | seconds between two tactical reviews of a fight |
| `Companions.LLM.Banter` | 0 | comments on level ups |
| `Companions.LLM.MaxAge` | 45 | seconds after which an unanswered line is dropped |
| `Companions.Raid20.Enable` | 1 | the 40-player raids are tuned for 20 |
| `Companions.Raid20.Health` | 0.5 | share of their creatures' health (when they spawn) |
| `Companions.Raid20.Damage` | 0.8 | share of the damage they deal to players and pets |
| `Companions.Ambient.Enable` | 1 | ambient bots around players in the open world |
| `Companions.Ambient.Density` / `CityDensity` | 5 / 15 | around each player, in the open world / in a capital |
| `Companions.Ambient.MaxTotal` | 60 | ambient bots on the whole server |
| `Companions.Ambient.Radius` | 150 | yards around a player they appear and count in |
| `Companions.Ambient.GearQuality` | 2 | best item quality they wear |
| `Companions.Ambient.PoolSize` | 200 | characters kept for them |

All of these (but `Enable` and the account) can be changed live in the web panel.

## For admins: testing without a game client

From the worldserver console (`docker compose attach worldserver`) or SOAP, `.bot test` drives a headless stand-in
owner: `login <character>`, `party <owner> [role]`, `raid <owner> [size]`, `say <owner> <text>` (a party line), `create <owner> <class> [role]`, `go <owner> <map> <x> <y> <z>`,
`fight <owner>` (attack the nearest enemy), `pull <owner>` (the tank pulls the nearest one), `spawn <owner> <entry>
[count]` (enemies in front of the group for 5 minutes), `fire <bot> [spell]` (a hostile ground effect under it), `give <bot>
<item>` / `equip <bot> <it|name>` (loot decisions), `status <owner>`, `near <owner> [yards]` (creatures around, their health and mechanics rules), `ambient <owner>` (the zone as the ambient
bots see it and what each one does), `invite <owner> <name>` (a /invite), `order <owner> <order>`, `why <bot>` (what it sees),
`spells <bot>`, `kill <bot>`, `logout <owner>`. `why <bot>` also shows where it stands around you and its last casts. Administrators only (the permission of `.server shutdown`).

## Good to know

- **Pathfinding**: without mmaps (`MMAP_PATHFINDING=0`, the default here) bots, like creatures, move in straight
  lines; they catch up with you when they get stuck. With `EXTRACT_MMAPS=1` (re-run the extractor, it takes
  hours) and `MMAP_PATHFINDING=1` they find their way around walls.
- Loot: with group loot a companion rolls **Need** only on a clear upgrade it can use (at least 5% better than what it
  wears in that slot) and puts it on; it passes on everything else, the greed rolls are yours. With master loot it keeps
  what you give it in its bags until you say `equip it`. Gear it looted or was given is kept when it regears.
- Dungeon and raid loot: `docker/sql/world/2026_10_25_90_world_companions_vanilla_loot.sql` adds the vanilla loot
  Forever's tables lack, for 302 creatures of the classic dungeons (Ragefire Chasm to Dire Maul, Scholomance, Stratholme)
  and of Molten Core, Blackwing Lair, Zul'Gurub, both Ahn'Qiraj raids and Lord Kazzak (boss drops, recipes, the Qiraji idols, world
  drops of the level range...) and for the dungeon chests that lack items, from TrinityCore's 3.3.5 database where these
  places are unchanged. Only items the Forever client knows, never replacing Forever's own loot, each item on the creature
  that drops it. Onyxia and Naxxramas are left out (remade at level 80 in that database); the Forever dungeons (Ruins of
  Lordaeron, Hall of Thanes) already have their own boss loot. The worldserver's updater applies it.
- The companions you create live on the account `COMPANIONS` (nobody can log into it).
- How it works: a small core patch (`docker/patches/0005-companions-core-hooks.patch`) lets a session without a
  client exist (`WorldSession::IsBot`), gives each bot a per-update hook and exposes the login loader. The module
  is in `docker/companions/src` (scripts/Custom/Companions in the core).
