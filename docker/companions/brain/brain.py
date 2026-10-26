"""Companions brain: an AI model talks for the bots, answers callouts and turns plain language into their orders.

The worldserver writes what the bots hear into characters.companion_llm_events (a whisper, a party line, a callout
the bot wants to make), with a snapshot of the situation. The brain answers each one in companion_llm_actions:
which bots speak, what they say, and orders from the list players use ("heal Bob", "focus skull", "retreat").
Combat itself stays in the server-side AI, so a slow answer never costs a fight.

The providers (Claude, Ollama, any OpenAI-compatible server) are set in the web panel (companion_llm_providers).
Two ways to run it:
  local   next to the database (the "brain" compose service): answers with the providers of "this server"
  remote  on any machine (a PC with a GPU running Ollama...): BRAIN_URL=https://your.site BRAIN_TOKEN=<token from
          the web panel>; it fetches its providers and the lines to answer from the website and posts the answers
Several brains share the work: each claims only as many lines as its providers have free slots.
"""

import json
import logging
import os
import random
import re
import secrets
import time
from collections import defaultdict, deque
from concurrent.futures import ThreadPoolExecutor

import httpx

import backends

log = logging.getLogger("companions-brain")

VERSION = "2"
POLL_SECONDS = float(os.environ.get("COMPANIONS_LLM_POLL", "0.5"))
MAX_PER_MINUTE = int(os.environ.get("COMPANIONS_LLM_MAX_PER_MINUTE", "60"))
PROVIDER_REFRESH = 15
MAX_AGE = 60            # seconds: an older line is not worth answering any more
MEMORY_TURNS = 10

ORDER_WORDS = {
    "follow", "stay", "hold", "wait", "come", "here", "regroup", "attack", "kill", "focus", "pull", "stop", "halt",
    "passive", "assist", "defensive", "aggressive", "role", "rest", "drink", "eat", "cast", "status", "gear",
    "dismiss", "heal", "dispel", "decurse", "cleanse", "protect", "save", "spread", "stack", "normal", "retreat",
    "run", "flee", "fallback", "cc", "sheep", "control", "interrupt", "taunt", "mark", "burst", "cooldowns",
    "bless", "rebuff", "buff", "equip", "salvation", "might", "wisdom", "kings", "light", "sanctuary",
    "plan", "prefer", "avoid", "allow", "prefs", "give", "hand", "loot", "potions", "autocc",
}
# what a player who is not the leader may ask of the companions
HELP_WORDS = {"heal", "dispel", "decurse", "cleanse", "protect", "save", "assist", "status", "bless", "rebuff", "buff",
              "salvation", "might", "wisdom", "kings", "light", "sanctuary"}
# what a tactical review may assign
TACTIC_WORDS = {"kill", "attack", "focus", "cc", "interrupt", "taunt", "mark", "burst", "spread", "stack", "retreat", "heal",
                "protect", "dispel", "assist", "stop"}

SYSTEM_PROMPT = """You voice companion characters in World of Warcraft Classic on a private server. They are bots that \
adventure with human players; each has its own name, class, role and personality, and they should feel like real, \
distinct people in the party.

Personality comes first. Each companion's personality (in the situation) decides how it talks: its words, length, \
attitude, humour, quirks, what it notices and cares about. Two companions never sound alike, even saying the same \
thing. Stay in character in every line, short acknowledgements included. No generic filler ("On it!", "Let's do \
this!", "Sounds good!", "Got it!", "Will do!") unless that exact phrase is that character; never repeat a line from \
the recent chat; vary how it starts. A personality also shows in opinions: a cautious one may voice a doubt about a \
risky plan (and still do it), a show-off may brag, a scholar may add a fact it was given.

How they talk: party chat, short lines (one sentence, under 150 characters), in character, no emotes in \
asterisks, no markdown, no quoting the situation back. Answer in the language the human wrote in (callouts: the \
language of the recent chat, else English). Never invent game facts you were not given (quest steps, item stats, \
coordinates); say you are not sure. Real players do not all answer every line: usually one companion answers, the \
one who is concerned (the healer for heals, the tank for pulls, the one who was named); nobody answers small talk \
between humans that is not for them. An empty "replies" list is often right.

Their fighting, healing, buffing and following are automatic; never describe it. With a tank in the group the tank \
pulls and holds the enemies; nobody else is ordered to pull, and "attack" for the others waits until the tank is on \
them. When a human asks for something, \
add the matching orders to the companion that does it. Humans speak loosely and abstractly ("that caster is trouble", \
"be careful with the next pack", "our healer keeps dying", "go all out on the boss", "take it slow"): work out what \
they mean and turn it into the concrete orders below, for the companions concerned. Orders (exact words, one per entry; <name> is a party member, \
"me" means the leader):
- follow | stay | come | stop | retreat (run back to the leader and stop fighting)
- attack (the leader's target) | pull (tank: pull the leader's target) | focus skull|cross|moon|square|triangle|diamond|circle|star (the marked target)
- assist <name> (attack what that player attacks) | heal <name> | dispel <name> | protect <name> (tank: taunt off them, others: help them)
- spread | stack | normal (how close they stand)
- in fights, enemies are numbered E1, E2...: kill E2 | cc E3 (crowd control: polymorph, shackle undead, banish, hibernate,
  scare beast, gouge - only a companion that knows it, on a creature type it works on) | interrupt E1 (that caster's casts)
  | taunt E2 (tank) | mark skull E2 (raid markers: skull = kill first, cross = second, moon = crowd controlled) | burst (cooldowns now)
- passive | assist | aggressive (when they fight on their own)
- role tank | role heal | role dps
- bless <name> salvation|might|wisdom|kings|light|sanctuary (a paladin gives that blessing to them from now on)
- rebuff <name> [buff] (cast the buff again now even if it is still up: "rebuff me intellect", "rebuff bob fortitude"), given to
  the companion whose class has it: Arcane Intellect = mage, Fortitude and Divine Spirit = priest, Mark of the Wild and Thorns =
  druid, blessings = paladin
- equip it (put on the item the player just handed over) | equip <item name> | equip upgrades (everything better in its bags)
- rest | gear | status | dismiss
- cast <spell name> | cast <spell name> on me | cast <spell name> on target | cast <spell name> on <name>
- plan <steps, separated by ;> for instructions in several steps or crowd control on a pull. Steps: pull target|P#|E#|next \
(P1, P2... are the packs listed in the situation; "next" is the nearest pack when the step comes) followed by \
", <companion> cc E#" for each crowd control on that pull | rest [percent] (everyone to full health and mana, or that \
percent) | ready (wait until every player says ready) | wait <seconds>. Example: "attack this pack, then drink, then \
pull the other one once everyone is ready" -> plan pull target; rest; ready; pull next. "When we pull P2 I want \
Jaina to sheep the caster" -> plan pull p2, jaina cc E7. Give the plan to one companion only.
- prefer <spell> [below <percent>] [because <reason>] | avoid <spell> [because <reason>] | allow <spell>: lasting changes \
to how a companion fights, from feedback ("use more Frost Nova, the adds keep reaching the healer" -> that mage: \
prefer frost nova because adds keep reaching the healer; "Shield Wall when you get low" -> prefer shield wall below 30; \
"stop breaking my sheep with Arcane Explosion" -> avoid arcane explosion because it breaks sheep). Only spells that \
companion's class has. "prefs" makes them tell what they were told.
- give me <item name|potions|food|drink|bandages|junk> (it opens a trade with the leader) | loot all|useful|off | \
potions on|off (drink potions when low) | autocc on|off (crowd control extra adds on its own)
Only the leader decides following, roles, stance and dismiss; other players may ask for help (heal, dispel, protect, \
assist). Relay what the companions know when asked (health, mana, who is low, enemies, markers, rares nearby, what \
someone said earlier) from the situation, briefly.

Reply with JSON: {"replies": [{"bot": "<companion name>", "say": "<line or empty>", "orders": [...], "delay_ms": <0-4000>}]}. \
Use delay_ms so several companions do not answer in the same instant (the first one 300-1200, the next ones later)."""

OUTPUT_SCHEMA = {
    "type": "object",
    "properties": {
        "replies": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "bot": {"type": "string", "description": "Name of the companion who acts."},
                    "say": {"type": "string", "description": "What it says (empty for nothing)."},
                    "orders": {"type": "array", "items": {"type": "string"}, "description": "Orders it carries out, in order."},
                    "delay_ms": {"type": "integer", "description": "Milliseconds before it acts."},
                },
                "required": ["bot", "say", "orders", "delay_ms"],
                "additionalProperties": False,
            },
        }
    },
    "required": ["replies"],
    "additionalProperties": False,
}


# ---------------------------------------------------------------------------- requests and answers

def roster_of(event):
    """[(guid, name, class, role)] of the companions this line is for."""
    return [b[:4] for b in roster_full(event)]


def roster_full(event):
    """[(guid, name, class, role, abilities)]: abilities are the crowd control, interrupts and taunts it knows."""
    bots = []
    for line in (event.get("bots") or "").splitlines():
        parts = line.split("|")
        if len(parts) >= 4 and parts[0].isdigit():
            bots.append((int(parts[0]), parts[1], parts[2], parts[3], parts[4] if len(parts) > 4 else ""))
    if not bots:
        bots.append((int(event["bot_guid"]), "companion", "", "", ""))
    return bots


def context_line(event, name):
    match = re.search(rf"^{name}: (.+)$", event.get("context") or "", re.M)
    return match.group(1).strip() if match else ""


def leader_of(event):
    return context_line(event, "leader")


def build_request(event, memory):
    bots = roster_of(event)
    names = ", ".join(f"{name} ({klass}, {role})" for _, name, klass, role in bots)
    kind, channel, speaker, message = event["kind"], event["channel"], event["speaker"], event["message"]
    if kind == "tactics":
        team = "\n".join(f"- {name} ({klass}, {role}){': ' + can if can else ''}" for _, name, klass, role, can in roster_full(event))
        task = ("Tactical review of the fight (" + message + "). You lead the companions like a raid leader. Look at the enemies "
                "(E1, E2...) and give orders only where they change something; companions without orders keep doing their job "
                "(the tank tanks, healers heal, damage dealers follow the skull, then the tank's target). With a tank in the group "
                "the tank opens every fight and holds the threat: no attack or burst orders for the damage dealers before the tank "
                "has the enemies, and pull orders only for the tank (the damage dealers already hold back near the tank's threat "
                "and keep area spells until it holds the pack).\n"
                "- several enemies: one kill target for the damage dealers (kill E#, or mark skull E# so the player sees it too)\n"
                "- a dangerous extra enemy that can be controlled: cc E# by a companion that can (and mark moon E#)\n"
                "- a healer or caster enemy casting: interrupt E# by a companion that has an interrupt\n"
                "- an enemy hitting a healer or caster: taunt E# for the tank\n"
                "- a boss or a hard fight: burst for the damage dealers\n"
                "- a \"boss tactics\" line: what the encounter needs (the companions already step out of casts, keep out of breaths, "
                "run with debuffs, kill the named adds first and swap tanks on their own); order what it adds, like the kill order "
                "of its adds, crowd control or interrupts\n"
                "At most one short line in total, in the words of the companion who calls it (naming the enemies, not their numbers), "
                "everyone else with an empty say. Never change roles in a fight. Companions and what they can do:\n" + team)
    elif kind == "test":
        task = f"Connection test from the web panel. Reply as {bots[0][1]} with one short friendly line."
    elif kind == "callout":
        task = (f"A callout from {bots[0][1]} about what {bots[0][1]} notices ({speaker}): {message}.\nWrite the short line {bots[0][1]} types "
                "in party chat to warn the others, the way a real player types it mid-fight: a few words about that, in the first person, "
                f"in {bots[0][1]}'s own voice from its personality (a gruff one barks two words, a nervous one panics a little, a pirate "
                "says it like a pirate). No orders.")
    elif kind == "event":
        task = (f"Something happened to {bots[0][1]} ({speaker}): {message}. A short remark from {bots[0][1]} that only this "
                "personality would make, or nothing.")
    elif channel == "whisper":
        task = f"{speaker} whispers to {bots[0][1]}: {message}\nOnly {bots[0][1]} can answer (the answer is whispered back)."
    elif kind == "group" and context_line(event, "spoken to"):
        named = context_line(event, "spoken to")
        task = (f"{speaker} says in party chat, to {named}: {message}\n{named} answers (and does what is asked); another companion "
                f"adds something only if it is clearly concerned. Companions you voice: {names}.")
    elif kind == "group":
        task = (f"{speaker} says in party chat: {message}\nDecide which of the companions answer, if any, and what they do. "
                f"Companions you voice: {names}.")
    else:
        task = f"{speaker} says in party chat to {bots[0][1]}: {message}"

    parts = [f"<situation>\n{(event.get('context') or '').strip()}\n</situation>"]
    if memory:
        parts.append("<earlier_whispers>\n" + "\n".join(memory) + "\n</earlier_whispers>")
    parts.append(f"<now>\n{task}\n</now>")
    return "\n".join(parts)


TARGET = r"(e\d{1,2}|skull|cross|moon|square|triangle|diamond|circle|star)"
CC_SPELLS = r"(?:polymorph|sheep|shackle undead|shackle|banish|hibernate|scare beast|gouge|sap|cc|crowd control)"
KICKS = r"(?:kick|pummel|shield bash|counterspell|earth shock|hammer of justice|bash|interrupt)"
NORMALIZE = [
    # models write tactics loosely: "cast polymorph on E2", "shackle undead E3", "kill skull E1", "taunt E1, E2"
    (re.compile(rf"^(?:cast |use )?{CC_SPELLS}(?: on)? {TARGET}\b"), r"cc \1"),
    (re.compile(rf"^(?:cast |use )?{KICKS}(?: on)? {TARGET}\b"), r"interrupt \1"),
    (re.compile(rf"^(kill|attack|focus) {TARGET} {TARGET}\b"), r"kill \3"),
    (re.compile(rf"^(kill|attack|focus|taunt|interrupt|cc) {TARGET}\b.*"), r"\1 \2"),
    (re.compile(rf"^mark (skull|cross|moon|square|triangle|diamond|circle|star)(?: on)? (e\d{{1,2}})\b.*"), r"mark \1 \2"),
]


def clean_orders(orders, allowed):
    clean = []
    for order in orders or []:
        if not isinstance(order, str):
            continue
        order = " ".join(order.strip().lower().split())[:200]
        for pattern, replacement in NORMALIZE:
            if pattern.match(order):
                order = pattern.sub(replacement, order, count=1)
                break
        if order and order.split()[0] in allowed and order not in clean:
            clean.append(order)
    return clean[:4]


def actions_from(event, data, order_mode="all"):
    """The answer of the model as rows for companion_llm_actions.

    order_mode (provider option "orders"): "all", "help" (heal, dispel, protect, assist, status: small local models
    tend to add random orders to small talk) or "none"."""
    bots = roster_of(event)
    by_name = {name.lower(): guid for guid, name, _, _ in bots}
    leader = leader_of(event).lower()
    speaker = (event.get("speaker") or "").lower()
    # callouts and banter only talk; a player who is not the leader only gets help; tactics only assign tactics
    if event["kind"] in ("callout", "event", "test") or order_mode == "none":
        allowed = set()
    elif event["kind"] == "tactics":
        allowed = TACTIC_WORDS
    elif order_mode == "help" or (leader and speaker != leader):
        allowed = HELP_WORDS
    else:
        allowed = ORDER_WORDS
    addressed = by_name.get(context_line(event, "spoken to").lower())
    single = event["kind"] not in ("group", "tactics")
    # "E2" in what they say becomes the enemy's name, like players talk
    enemies = dict(re.findall(r"^\s*E(\d+) (.+?) \(level", event.get("context") or "", re.M))
    whisper = event["channel"] == "whisper"

    replies = data.get("replies")
    if replies is None and ("say" in data or "orders" in data):    # a small model answering the old shape
        replies = [{"bot": bots[0][1], **data}]
    actions = []
    used = set()
    for reply in replies if isinstance(replies, list) else []:
        if not isinstance(reply, dict):
            continue
        name = str(reply.get("bot", "")).strip().lower()
        guid = by_name.get(name) or (bots[0][0] if single or len(bots) == 1 else None)
        if addressed and not actions and guid != addressed and not any(
                by_name.get(str(r.get("bot", "")).strip().lower()) == addressed for r in replies if isinstance(r, dict)):
            guid = addressed   # the one spoken to answers (small models mix the names up)
        if guid is None or guid in used:
            continue
        say = " ".join(str(reply.get("say") or "").split())[:250]
        say = re.sub(r"\b[Ee](\d{1,2})\b", lambda m: enemies.get(m.group(1), m.group(0)), say)
        orders = clean_orders(reply.get("orders"), allowed)
        if not say and not orders:
            continue
        try:
            delay = int(reply.get("delay_ms") or 0)
        except (TypeError, ValueError):
            delay = 0
        used.add(guid)
        actions.append({
            "bot_guid": guid,
            "channel": "whisper" if whisper else "party",
            "target": event["speaker"] if whisper else "",
            "say": say,
            "commands": "\n".join(orders),
            "delay_ms": max(0, min(delay, 6000)),
        })
        if single or len(actions) >= (len(bots) if event["kind"] == "tactics" else 2):
            break
    if event["kind"] == "tactics":
        # orders now, one voice, and only when the fight starts (later reviews adjust quietly)
        spoken = "goes on" in (event.get("message") or "")
        for action in actions:
            action["delay_ms"] = 0
            if action["say"] and spoken:
                action["say"] = ""
            spoken = spoken or bool(action["say"])
        return [a for a in actions if a["say"] or a["commands"]]
    # never two companions in the same instant
    for i, action in enumerate(sorted(actions, key=lambda a: a["delay_ms"])):
        action["delay_ms"] = max(action["delay_ms"], 250 + i * 1400 + random.randint(0, 500))
    return actions


# ---------------------------------------------------------------------------- where the work comes from

CLAIM_COLUMNS = "id, bot_guid, owner_guid, kind, channel, speaker, message, context, bots, provider_id, TIMESTAMPDIFF(SECOND, created, NOW()) AS age"


KINDS = ("chat", "group", "callout", "event", "tactics", "test")


def claim_condition(provider_ids, kinds=None):
    """Lines this brain may take: for its providers, for any provider, or waiting too long for theirs;
    only the kinds its free providers answer (a small model only for callouts...)."""
    mine = f"provider_id IN ({','.join(str(int(i)) for i in provider_ids)})" if provider_ids else "0"
    condition = (f"(provider_id = 0 OR {mine} OR (kind <> 'test' AND (created < NOW() - INTERVAL 8 SECOND "
                 f"OR provider_id NOT IN (SELECT id FROM companion_llm_providers WHERE enabled = 1))))")
    if kinds is not None and set(kinds) != set(KINDS):
        condition += " AND kind IN (" + ",".join(f"'{k}'" for k in kinds if k in KINDS) + ")"
    return condition


class DbStore:
    """Local mode: straight to the characters database."""

    def __init__(self):
        import pymysql
        self.pymysql = pymysql
        self.db = None
        self.prefix = "local"

    def cursor(self):
        if self.db is None:
            self.db = self.pymysql.connect(
                host=os.environ.get("DB_HOST", "mysql"),
                port=int(os.environ.get("DB_PORT", "3306")),
                user=os.environ.get("DB_USER", "trinity"),
                password=os.environ["DB_PASSWORD"],
                database=os.environ.get("DB_NAME", "characters"),
                charset="utf8mb4",
                autocommit=True,
                connect_timeout=10,
            )
        return self.db.cursor(self.pymysql.cursors.DictCursor)

    def reset(self):
        self.db = None

    def providers(self):
        with self.cursor() as cur:
            cur.execute("SELECT COUNT(*) AS n FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'companion_llm_providers'")
            if not cur.fetchone()["n"]:
                return []   # the worldserver creates the tables at its first start
            cur.execute("SELECT COUNT(*) AS n FROM companion_llm_providers")
            if not cur.fetchone()["n"] and os.environ.get("ANTHROPIC_API_KEY"):
                # first start with a key in .env: Claude becomes the first provider (editable in the web panel)
                cur.execute(
                    "INSERT INTO companion_llm_providers (name, kind, model, weight, max_concurrent, timeout_s) VALUES (%s, 'anthropic', %s, 1, 4, 60)",
                    ("Claude", os.environ.get("COMPANIONS_LLM_MODEL", "claude-opus-5-5")),
                )
                log.info("added Claude as the first AI provider")
            cur.execute("SELECT * FROM companion_llm_providers WHERE enabled = 1 AND worker_id = 0")
            return cur.fetchall()

    def claim(self, count, provider_ids, kinds=None):
        token = f"{self.prefix}:{secrets.token_hex(6)}"
        with self.cursor() as cur:
            cur.execute(
                f"UPDATE companion_llm_events SET status = 1, claimed_by = %s, claimed_at = NOW() WHERE status = 0 AND claimed_by <> %s "
                f"AND created > NOW() - INTERVAL {MAX_AGE} SECOND AND {claim_condition(provider_ids, kinds)} ORDER BY id LIMIT %s",
                (token, f"x:{self.prefix}", count),
            )
            if not cur.rowcount:
                return []
            cur.execute(f"SELECT {CLAIM_COLUMNS} FROM companion_llm_events WHERE claimed_by = %s AND status = 1 ORDER BY id", (token,))
            return cur.fetchall()

    def release(self, event, failed=False):
        """Back to the queue; after a failure marked so that another brain takes it."""
        with self.cursor() as cur:
            cur.execute("UPDATE companion_llm_events SET status = 0, claimed_by = %s WHERE id = %s AND status = 1",
                        (f"x:{self.prefix}" if failed else "", event["id"]))

    def answer(self, event, actions, provider_id, ok, ms, error):
        with self.cursor() as cur:
            for a in actions:
                cur.execute(
                    "INSERT INTO companion_llm_actions (event_id, bot_guid, channel, target, say, commands, delay_ms) VALUES (%s, %s, %s, %s, %s, %s, %s)",
                    (event["id"], a["bot_guid"], a["channel"], a["target"], a["say"], a["commands"], a["delay_ms"]),
                )
            cur.execute("UPDATE companion_llm_events SET status = 2 WHERE id = %s", (event["id"],))
            if provider_id:
                record_stats(cur, provider_id, ok, ms, error)

    def failure(self, provider_id, error):
        with self.cursor() as cur:
            record_stats(cur, provider_id, False, 0, error)


def record_stats(cur, provider_id, ok, ms, error):
    if ok:
        cur.execute(
            "UPDATE companion_llm_providers SET answered = answered + 1, avg_ms = IF(avg_ms = 0, %s, ROUND(avg_ms * 0.8 + %s * 0.2)), "
            "last_ok = NOW(), last_error = '' WHERE id = %s",
            (ms, ms, provider_id),
        )
    else:
        cur.execute("UPDATE companion_llm_providers SET failed = failed + 1, last_error = %s WHERE id = %s", ((error or "")[:250], provider_id))


class HttpStore:
    """Remote mode: through the website's brain API, with the worker token made in the web panel."""

    def __init__(self, url, token):
        verify = os.environ.get("BRAIN_CA_FILE") or (os.environ.get("BRAIN_TLS_VERIFY", "1") != "0")
        self.client = httpx.Client(base_url=url.rstrip("/"), headers={"Authorization": f"Bearer {token}"}, timeout=20, verify=verify)

    def post(self, path, body):
        response = self.client.post(path, json=body)
        if response.status_code == 401:
            raise RuntimeError("the website refused the worker token (deleted in the web panel?)")
        response.raise_for_status()
        return response.json()

    def reset(self):
        pass

    def providers(self):
        info = os.environ.get("BRAIN_NAME") or os.uname().nodename
        return self.post("/api/brain/heartbeat", {"version": VERSION, "info": info})["providers"]

    def claim(self, count, provider_ids, kinds=None):
        return self.post("/api/brain/claim", {"max": count, "kinds": list(kinds) if kinds is not None else None})["events"]

    def release(self, event, failed=False):
        self.post("/api/brain/release", {"event_id": event["id"], "failed": failed})

    def answer(self, event, actions, provider_id, ok, ms, error):
        self.post("/api/brain/answer", {"event_id": event["id"], "provider_id": provider_id, "ok": ok, "ms": ms,
                                        "error": (error or "")[:250], "actions": actions})

    def failure(self, provider_id, error):
        self.post("/api/brain/failure", {"provider_id": provider_id, "error": (error or "")[:250]})


# ---------------------------------------------------------------------------- the brain

class Slot:
    """One provider and how many requests it is answering now."""

    FIELDS = ("kind", "endpoint", "model", "api_key", "timeout_s", "options")

    def __init__(self, row):
        self.row = row
        self.backend = backends.make(row)
        self.busy = 0
        options = backends.options_of(row)
        mode = options.get("orders")
        self.order_mode = mode if mode in ("all", "help", "none") else ("all" if row["kind"] == "anthropic" else "help")
        # "kinds": which lines it answers and with which weight, e.g. {"callout": 4, "event": 4} for a small model
        kinds = options.get("kinds")
        if isinstance(kinds, list):
            kinds = {k: 1 for k in kinds}
        self.kinds = {k: float(v) for k, v in kinds.items() if isinstance(v, (int, float)) and v > 0} if isinstance(kinds, dict) else None

    @property
    def id(self):
        return int(self.row["id"])

    @property
    def free(self):
        return max(0, int(self.row.get("max_concurrent") or 1) - self.busy)

    def serves(self, kind):
        return self.kinds is None or kind in self.kinds or kind == "test"

    def weight_for(self, kind):
        base = max(1, int(self.row.get("weight") or 1))
        return base if self.kinds is None else self.kinds.get(kind, base)

    def same_setup(self, row):
        return all(str(self.row.get(f)) == str(row.get(f)) for f in self.FIELDS)


class Brain:
    def __init__(self, store):
        self.store = store
        self.slots: dict[int, Slot] = {}
        self.refreshed = 0.0
        self.pool = ThreadPoolExecutor(max_workers=32)
        self.running = []          # (future, event, slot, started)
        self.whispers = defaultdict(lambda: deque(maxlen=MEMORY_TURNS))
        self.calls = deque()

    def refresh(self):
        if time.monotonic() - self.refreshed < PROVIDER_REFRESH and self.slots:
            return
        self.refreshed = time.monotonic()
        rows = self.store.providers()
        current = {}
        for row in rows:
            slot = self.slots.get(int(row["id"]))
            try:
                if slot and slot.same_setup(row):
                    slot.row = row
                else:
                    busy = slot.busy if slot else 0
                    slot = Slot(row)
                    slot.busy = busy
                    log.info("provider %s: %s %s %s", row["name"], row["kind"], row["model"], row.get("endpoint") or "")
                current[slot.id] = slot
            except backends.ProviderError as e:
                log.error("provider %s: %s", row["name"], e)
        for gone in set(self.slots) - set(current):
            log.info("provider %s removed or disabled", self.slots[gone].row["name"])
        self.slots = current

    def pick(self, event, exclude=()):
        kind = event.get("kind") or "chat"
        preferred = self.slots.get(int(event.get("provider_id") or 0))
        if preferred and preferred.free and preferred.id not in exclude and preferred.serves(kind):
            return preferred
        choices = [s for s in self.slots.values() if s.free and s.id not in exclude and s.serves(kind)]
        if not choices:
            return None
        return random.choices(choices, weights=[s.weight_for(kind) * s.free for s in choices])[0]

    def rate_limited(self):
        now = time.monotonic()
        while self.calls and now - self.calls[0] > 60:
            self.calls.popleft()
        return len(self.calls) >= MAX_PER_MINUTE

    def memory_key(self, event):
        return (int(event["bot_guid"]), (event.get("speaker") or "").lower())

    def start(self, event, slot):
        memory = list(self.whispers[self.memory_key(event)]) if event["channel"] == "whisper" else []
        request = build_request(event, memory)
        slot.busy += 1
        self.calls.append(time.monotonic())
        future = self.pool.submit(slot.backend.ask, SYSTEM_PROMPT, request, OUTPUT_SCHEMA)
        self.running.append((future, event, slot, time.monotonic()))

    def collect(self):
        still = []
        for future, event, slot, started in self.running:
            if not future.done():
                still.append((future, event, slot, started))
                continue
            slot.busy = max(0, slot.busy - 1)
            ms = int((time.monotonic() - started) * 1000)
            try:
                data = future.result()
                actions = actions_from(event, data, slot.order_mode)
                self.store.answer(event, actions, slot.id, True, ms, "")
                if event["channel"] == "whisper":
                    memory = self.whispers[self.memory_key(event)]
                    memory.append(f"{event['speaker']}: {event['message']}")
                    memory.extend(f"{a['say']}" for a in actions if a["say"])
                log.info("%s #%s (%s, %s ms): %s", slot.row["name"], event["id"], event["kind"], ms,
                         " / ".join(f"{a['say']!r} {a['commands'].splitlines()}" for a in actions) or "(no answer)")
            except Exception as e:  # one bad answer must not stop the bots' brain
                error = str(e) if isinstance(e, backends.ProviderError) else f"{type(e).__name__}: {e}"
                log.warning("%s failed on #%s: %s", slot.row["name"], event["id"], error)
                tried = event.setdefault("_tried", [])
                tried.append(slot.id)
                other = self.pick(event, exclude=tried) if event["kind"] != "test" and len(tried) < 3 else None
                try:
                    if other:
                        self.store.failure(slot.id, error)
                        self.start(event, other)       # failover: another provider answers it
                    elif event["kind"] != "test" and int(event.get("age") or 0) + ms // 1000 < 20:
                        self.store.failure(slot.id, error)
                        self.store.release(event, failed=True)   # another brain may still answer it in time
                    else:
                        self.store.answer(event, [], slot.id, False, ms, error)
                except Exception:
                    log.exception("cannot store the failure of #%s", event["id"])
        self.running = still

    def step(self):
        self.refresh()
        self.collect()
        free = sum(s.free for s in self.slots.values())
        if not free or self.rate_limited():
            return False
        # only the kinds a free provider answers
        kinds = None
        if any(s.kinds is not None for s in self.slots.values() if s.free):
            servable = set()
            for s in self.slots.values():
                if s.free:
                    servable |= set(KINDS) if s.kinds is None else set(s.kinds) | {"test"}
            kinds = sorted(servable)
        events = self.store.claim(min(free, 10), list(self.slots), kinds)
        for event in events:
            if int(event.get("age") or 0) > MAX_AGE:
                self.store.answer(event, [], 0, True, 0, "")
                continue
            slot = self.pick(event)
            if slot is None:
                self.store.release(event)
                continue
            self.start(event, slot)
        return bool(events)


def run():
    logging.basicConfig(level=os.environ.get("LOG_LEVEL", "INFO"), format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    url, token = os.environ.get("BRAIN_URL"), os.environ.get("BRAIN_TOKEN")
    if url and token:
        store = HttpStore(url, token)
        log.info("companions brain %s started as a remote worker of %s", VERSION, url)
    else:
        store = DbStore()
        log.info("companions brain %s started next to the database", VERSION)
    brain = Brain(store)
    warned = False

    while True:
        try:
            busy = brain.step()
            if not brain.slots and not warned:
                log.warning("no AI provider for this brain yet: add one in the web panel (Companions > Server)")
                warned = True
            elif brain.slots:
                warned = False
            time.sleep(0.1 if busy or brain.running else POLL_SECONDS)
        except Exception as e:
            log.error("%s (retrying)", e)
            store.reset()
            time.sleep(5)


if __name__ == "__main__":
    run()
