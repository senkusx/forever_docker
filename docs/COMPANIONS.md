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
- **Everyone**: buffs the group (Fortitude, Arcane Intellect, Mark of the Wild, blessings, totems...), follows you
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
  all, aggression, stance and gear quality, plus summon or dismiss all.
- **New companion**: class, race (the ones your faction can play, with Forever's Undead and Tauren paladins, Dwarf
  shamans and Troll druids), gender, custom or random name, role, talent
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
| `stop` | stop attacking for a few seconds |
| `passive` / `assist` / `aggressive` | never attack / fight what the group fights (default) / also attack hostiles nearby |
| `role tank` / `role heal` / `role dps` / `role auto` | change role (talents and gear follow) |
| `rest` | eat and drink to full |
| `cast <spell> [on me / on target]` | cast a spell now ("cast polymorph on target") |
| `status` | health, mana, role |
| `gear` | look again for the best gear |
| `dismiss` | leave |

Other players in your group can ask your companions for help (`heal me`, `Jaina dispel me`, `protect Bob`,
`assist Bob`); following, roles and stance stay yours.

Raid markers work like with players: **skull** = kill first, **cross** = kill second, **moon** = crowd control,
do not break it.

Other commands: `.bot role <name|all> <role>`, `.bot gear [name]`, `.bot talents <name> [respec]`,
`.bot personality <name> <text>`, `.bot llm on|off [name]`, `.bot delete <name>` (only companions you created;
your own characters are never deleted), `.bot help`.

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

All of these (but `Enable` and the account) can be changed live in the web panel.

## For admins: testing without a game client

From the worldserver console (`docker compose attach worldserver`) or SOAP, `.bot test` drives a headless stand-in
owner: `login <character>`, `party <owner> [role]`, `raid <owner> [size]`, `say <owner> <text>` (a party line), `create <owner> <class> [role]`, `go <owner> <map> <x> <y> <z>`,
`fight <owner>` (attack the nearest enemy), `pull <owner>` (the tank pulls the nearest one), `spawn <owner> <entry>
[count]` (enemies in front of the group for 5 minutes), `status <owner>`, `order <owner> <order>`, `why <bot>` (what it sees),
`spells <bot>`, `kill <bot>`, `logout <owner>`. `why <bot>` also shows where it stands around you and its last casts. Administrators only (the permission of `.server shutdown`).

## Good to know

- **Pathfinding**: without mmaps (`MMAP_PATHFINDING=0`, the default here) bots, like creatures, move in straight
  lines; they catch up with you when they get stuck. With `EXTRACT_MMAPS=1` (re-run the extractor, it takes
  hours) and `MMAP_PATHFINDING=1` they find their way around walls.
- Bots pass on loot rolls, so the loot is yours.
- The companions you create live on the account `COMPANIONS` (nobody can log into it).
- How it works: a small core patch (`docker/patches/0005-companions-core-hooks.patch`) lets a session without a
  client exist (`WorldSession::IsBot`), gives each bot a per-update hook and exposes the login loader. The module
  is in `docker/companions/src` (scripts/Custom/Companions in the core).
