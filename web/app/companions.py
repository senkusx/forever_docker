"""Companions panel: build and manage the player bots of your characters (accounts with GM level 3 only).

Login goes through bnetserver's REST login (the launcher's call), so the server checks the password and its lockout
rules; the account must have GM level 3 on this realm. Everything that touches a bot goes to the worldserver as a
".bot admin" console command over SOAP: the worldserver keeps the bots in memory, so the database is only read here.
Templates (a bot, or a whole party with its settings) are stored by this service in characters.companion_web_templates.
"""
import asyncio
import base64
import functools
import hashlib
import hmac
import json
import logging
import os
import re
import ssl
import time
from collections import defaultdict, deque
from pathlib import Path

import httpx
import pymysql
from fastapi import APIRouter, Request
from fastapi.responses import HTMLResponse, JSONResponse

from . import companions_ai

log = logging.getLogger('forever-web.companions')

BNET_REST_URL = os.environ.get('BNET_REST_URL', 'https://bnetserver:8081/bnetserver')
REALM_ID = int(os.environ.get('REALM_ID', '70'))
SESSION_HOURS = 12
SECRET = hashlib.sha256(('companions-panel:' + os.environ.get('WEB_SECRET', os.environ['SOAP_PASSWORD'])).encode()).digest()
COOKIE = 'companions'

CLASSES = {1: 'warrior', 2: 'paladin', 3: 'hunter', 4: 'rogue', 5: 'priest', 7: 'shaman', 8: 'mage', 9: 'warlock', 11: 'druid'}
RACES = {1: 'human', 2: 'orc', 3: 'dwarf', 4: 'nightelf', 5: 'undead', 6: 'tauren', 7: 'gnome', 8: 'troll'}
ROLES = {0: 'auto', 1: 'tank', 2: 'healer', 3: 'melee', 4: 'ranged'}
ROLE_WORDS = {'auto': 'auto', 'tank': 'tank', 'healer': 'heal', 'melee': 'melee', 'ranged': 'ranged'}
# talent tabs in the order of the game data (checked against the Forever 1.60 trees)
SPECS = {
    1: ['arms', 'fury', 'protection'], 2: ['holy', 'protection', 'retribution'], 3: ['beast_mastery', 'marksmanship', 'survival'],
    4: ['assassination', 'combat', 'subtlety'], 5: ['discipline', 'holy', 'shadow'], 7: ['elemental', 'enhancement', 'restoration'],
    8: ['arcane', 'fire', 'frost'], 9: ['affliction', 'demonology', 'destruction'], 11: ['balance', 'feral', 'restoration'],
}
QUALITIES = [2, 3, 4]

NAME_RE = re.compile(r'^[A-Za-z]{2,12}$')

SURNAME_RE = re.compile(r"^[A-Za-z][A-Za-z' -]{0,47}$")
SPEC_RE = re.compile(r'^[0-2](,[0-2]){0,2}$')
LOOKS_RE = re.compile(r'^\d{1,9}:\d{1,9}(,\d{1,9}:\d{1,9}){0,40}$')
SPELL_RE = re.compile(r"^[a-z][a-z' :()-]{1,40}$")
SIMPLE_ORDERS = {'follow', 'stay', 'come', 'attack', 'pull', 'stop', 'passive', 'assist', 'aggressive', 'rest', 'status', 'gear',
                 'retreat', 'spread', 'stack', 'normal'}
TARGET_ORDERS = {'heal', 'dispel', 'protect', 'assist'}   # + a party member's name or "me"
ICONS = {'star', 'circle', 'diamond', 'triangle', 'moon', 'square', 'cross', 'skull'}
GROUP_KEYS = ('llm', 'aggression', 'stance', 'quality', 'provider')

_attempts: dict[str, deque] = defaultdict(deque)
_looks_cache: dict[tuple, dict] = {}
_builds_cache: dict = {}


class PanelError(Exception):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def build_router(db, soap_command, client_ip, templates, page_context):
    router = APIRouter()

    # ------------------------------------------------------------------ session

    def sign(payload: dict) -> str:
        body = base64.urlsafe_b64encode(json.dumps(payload, separators=(',', ':')).encode()).decode()
        mac = hmac.new(SECRET, body.encode(), hashlib.sha256).hexdigest()
        return f'{body}.{mac}'

    def session_of(request: Request) -> dict:
        token = request.cookies.get(COOKIE, '')
        body, _, mac = token.partition('.')
        if not body or not hmac.compare_digest(mac, hmac.new(SECRET, body.encode(), hashlib.sha256).hexdigest()):
            raise PanelError('not_logged_in', 401)
        try:
            data = json.loads(base64.urlsafe_b64decode(body.encode()))
        except ValueError:
            raise PanelError('not_logged_in', 401)
        if data.get('exp', 0) < time.time():
            raise PanelError('not_logged_in', 401)
        return data

    def check_csrf(request: Request):
        # JSON calls from our page carry this header; a cross-site form or image cannot
        if request.headers.get('x-companions') != '1':
            raise PanelError('bad_request', 403)

    # ------------------------------------------------------------------ database

    def query(sql, args=()):
        with db() as conn, conn.cursor(pymysql.cursors.DictCursor) as cur:
            cur.execute(sql, args)
            return cur.fetchall()

    def execute(sql, args=()):
        with db() as conn, conn.cursor() as cur:
            cur.execute(sql, args)
            return cur.lastrowid

    def ensure_tables():
        execute(
            'CREATE TABLE IF NOT EXISTS characters.companion_web_templates ('
            '`id` INT UNSIGNED NOT NULL AUTO_INCREMENT, `bnet_account` INT UNSIGNED NOT NULL, '
            "`kind` VARCHAR(8) NOT NULL, `name` VARCHAR(64) NOT NULL, `data` TEXT NOT NULL, "
            '`updated` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, '
            "PRIMARY KEY (`id`), KEY `owner` (`bnet_account`, `kind`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COMMENT='Companions web panel: bot and party templates'")

    def account_access(email: str):
        rows = query('SELECT id FROM auth.battlenet_accounts WHERE email = UPPER(%s)', (email,))
        if not rows:
            return None
        bnet = rows[0]['id']
        accounts = [r['id'] for r in query('SELECT id FROM auth.account WHERE battlenet_account = %s', (bnet,))]
        if not accounts:
            return None
        marks = ','.join(['%s'] * len(accounts))
        level = query(f'SELECT MAX(SecurityLevel) AS level FROM auth.account_access WHERE AccountID IN ({marks}) AND RealmID IN (-1, %s)',
                      (*accounts, REALM_ID))[0]['level'] or 0
        return {'bnet': bnet, 'accounts': accounts, 'level': int(level)}

    def characters_of(session: dict):
        marks = ','.join(['%s'] * len(session['accounts']))
        return query(f'SELECT guid, name, race, class, gender, level, online FROM characters.characters '
                     f'WHERE account IN ({marks}) AND deleteDate IS NULL ORDER BY level DESC, name', tuple(session['accounts']))

    def owned_character(session: dict, owner: int) -> dict:
        for character in characters_of(session):
            if int(character['guid']) == owner:
                return character
        raise PanelError('not_your_character', 403)

    def bots_of(owner: int):
        return query('SELECT b.guid, c.name, c.race, c.class, c.gender, c.level, b.role, b.active, b.created, b.llm, b.personality, '
                     'b.spec, b.gear_quality, b.aggression, b.stance, b.template_id, b.llm_provider, b.loot_mode, b.potions, b.auto_cc, c.surname '
                     'FROM characters.companion_bots b '
                     'JOIN characters.characters c ON c.guid = b.guid WHERE b.owner_guid = %s AND c.deleteDate IS NULL ORDER BY c.name',
                     (owner,))

    def owned_bot(owner: int, bot: int) -> dict:
        for row in bots_of(owner):
            if int(row['guid']) == bot:
                return row
        raise PanelError('not_your_companion', 403)

    # ------------------------------------------------------------------ worldserver

    async def admin(command: str) -> list[list[str]]:
        """Runs ".bot admin <command>" and returns the COMP| lines split on '|'."""
        try:
            out = await soap_command(f'.bot admin {command}')
        except Exception as ex:   # SoapError or a worldserver that is down
            raise PanelError(f'server: {ex}', 503)
        lines = [line.split('|')[1:] for line in out.splitlines() if line.startswith('COMP|')]
        for parts in lines:
            if parts and parts[0] == 'ERROR':
                raise PanelError(parts[1] if len(parts) > 1 else 'error')
        return lines

    def bot_json(row: dict, live: dict | None) -> dict:
        return {
            'guid': int(row['guid']), 'name': row['name'], 'race': row['race'], 'class': row['class'], 'gender': row['gender'],
            'level': row['level'], 'role': ROLES.get(row['role'], 'auto'), 'llm': bool(row['llm']), 'personality': row['personality'],
            'spec': row['spec'], 'quality': row['gear_quality'], 'aggression': row['aggression'], 'stance': row['stance'],
            'template': row['template_id'], 'provider': row['llm_provider'], 'created': bool(row['created']), 'active': bool(row['active']),
            'loot': int(row.get('loot_mode') or 0), 'potions': bool(row.get('potions', 1)), 'autocc': bool(row.get('auto_cc', 1)),
            'surname': row.get('surname') or '',
            'live': live,
        }

    def parse_live(lines):
        owner, live, loading = None, {}, set()
        for parts in lines:
            if parts[0] == 'OWNER':
                owner = {'online': parts[2] == '1', 'level': int(parts[3]), 'zone': parts[4], 'bots': int(parts[5])}
            elif parts[0] == 'LIVE':
                live[int(parts[1])] = {'health': int(parts[2]), 'mana': int(parts[3]), 'state': parts[4], 'role': ROLES.get(int(parts[5]), 'auto'),
                                       'aggression': int(parts[6]), 'stance': int(parts[7]), 'item_level': int(parts[8]),
                                       'spells': int(parts[9]), 'zone': parts[10], 'level': int(parts[11]), 'talents': parts[12]}
            elif parts[0] == 'LOADING':
                loading.add(int(parts[1]))
        return owner, live, loading

    # ------------------------------------------------------------------ validation

    def clean_text(value, limit=250) -> str:
        text = re.sub(r'[\x00-\x1f|]', ' ', str(value or '')).strip()
        return text[:limit]

    def bot_settings(data: dict, partial=False) -> dict:
        """Validated bot fields from a request or a template."""
        out = {}

        def has(key):
            return key in data and data[key] is not None

        if has('class') or not partial:
            klass = int(data.get('class', 0))
            if klass not in CLASSES:
                raise PanelError('bad_class')
            out['class'] = klass
        if has('race') or not partial:
            race = int(data.get('race') or 0)
            if race and race not in RACES:
                raise PanelError('bad_race')
            out['race'] = race
        if has('gender') or not partial:
            gender = int(data.get('gender') if data.get('gender') is not None else 2)
            out['gender'] = gender if gender in (0, 1) else 2
        if has('role') or not partial:
            role = str(data.get('role') or 'auto')
            if role not in ROLE_WORDS:
                raise PanelError('bad_role')
            out['role'] = role
        if has('spec') or not partial:
            spec = str(data.get('spec') or '')
            if spec and not SPEC_RE.match(spec):
                raise PanelError('bad_spec')
            out['spec'] = spec
        if has('quality') or not partial:
            quality = int(data.get('quality') or 0)
            out['quality'] = quality if quality in QUALITIES else 0
        if has('llm') or not partial:
            out['llm'] = bool(data.get('llm', True))
        if has('aggression') or not partial:
            out['aggression'] = min(max(int(data.get('aggression') or 0), 0), 2)
        if has('stance') or not partial:
            out['stance'] = 1 if int(data.get('stance') or 0) == 1 else 0
        if has('provider') or not partial:
            out['provider'] = max(int(data.get('provider') or 0), 0)
        if has('personality') or not partial:
            out['personality'] = clean_text(data.get('personality'))
        if has('loot'):
            out['loot'] = min(max(int(data.get('loot') or 0), 0), 2)
        if has('potions'):
            out['potions'] = bool(data.get('potions'))
        if has('autocc'):
            out['autocc'] = bool(data.get('autocc'))
        if has('name') or not partial:
            name = str(data.get('name') or '').strip()
            if name and not NAME_RE.match(name):
                raise PanelError('bad_name')
            out['name'] = name.capitalize() if name else ''
        if has('surname') or not partial:
            surname = ' '.join(str(data.get('surname') or '').split())
            if surname and not SURNAME_RE.match(surname):
                raise PanelError('bad_surname')
            out['surname'] = surname
        if has('looks') or not partial:
            looks = data.get('looks') or {}
            if isinstance(looks, dict):
                looks = ','.join(f'{int(k)}:{int(v)}' for k, v in sorted(looks.items(), key=lambda kv: int(kv[0])) if v)
            looks = str(looks)
            if looks and not LOOKS_RE.match(looks):
                raise PanelError('bad_looks')
            out['looks'] = looks
        return out

    def order_text(value: str) -> str:
        words = clean_text(value, 80).lower().split()
        if not words:
            raise PanelError('bad_order')
        if words[0] in SIMPLE_ORDERS and len(words) == 1:
            return words[0]
        if words[0] in TARGET_ORDERS and len(words) == 2 and (words[1] == 'me' or re.match(r'^[a-z]{2,12}$', words[1])):
            return ' '.join(words)
        if words[0] == 'focus' and len(words) == 2 and words[1] in ICONS:
            return ' '.join(words)
        if words[0] == 'role' and len(words) == 2 and words[1] in ('tank', 'heal', 'dps', 'melee', 'ranged', 'auto'):
            return ' '.join(words)
        if words[0] == 'cast' and len(words) >= 2:
            text = ' '.join(words[1:])
            target = ''
            for suffix in (' on me', ' on target'):
                if text.endswith(suffix):
                    text, target = text[: -len(suffix)], suffix
            if SPELL_RE.match(text):
                return f'cast {text}{target}'
        raise PanelError('bad_order')

    async def create_bot(owner: int, settings: dict, template_id: int = 0) -> dict:
        role = ROLE_WORDS[settings['role']]
        lines = await admin(f"create {owner} {settings['class']} {settings['race']} {settings['gender']} {role} {settings['quality']} "
                            f"{int(settings['llm'])} {settings['aggression']} {settings['spec'] or '-'} {int(template_id)} "
                            f"{settings['looks'] or '-'} {settings['name'] or '-'} {settings.get('surname') or ''}".rstrip())
        created = next((p for p in lines if p[0] == 'CREATED'), None)
        if not created:
            raise PanelError('create_failed', 503)
        guid = int(created[1])
        # the save is asynchronous: the record exists right away in the worldserver
        if settings.get('personality'):
            await admin(f"set {owner} {guid} personality {settings['personality']}")
        if settings.get('stance'):
            await admin(f'set {owner} {guid} stance 1')
        if settings.get('provider'):
            await admin(f"set {owner} {guid} provider {settings['provider']}")
        return {'guid': guid, 'name': created[2]}

    # ------------------------------------------------------------------ routes

    def guarded(handler):
        @functools.wraps(handler)   # FastAPI reads the path and query parameters from the handler's signature
        async def wrapper(request: Request, *args, **kwargs):
            try:
                if request.method != 'GET':
                    check_csrf(request)
                return await handler(request, *args, **kwargs)
            except PanelError as ex:
                return JSONResponse({'error': str(ex)}, status_code=ex.status)
            except pymysql.MySQLError as ex:
                log.warning('database: %s', ex)
                return JSONResponse({'error': 'database'}, status_code=503)
        return wrapper

    @router.get('/companions', response_class=HTMLResponse)
    async def panel(request: Request):
        return templates.TemplateResponse(request, 'companions.html', await page_context(request))

    @router.post('/api/companions/login')
    @guarded
    async def login(request: Request):
        ip = client_ip(request)
        window = _attempts[ip]
        now = time.time()
        while window and now - window[0] > 900:
            window.popleft()
        if len(window) >= 10:
            raise PanelError('too_many_attempts', 429)
        window.append(now)

        data = await request.json()
        email, password = str(data.get('email', '')).strip(), str(data.get('password', ''))
        if not email or not password or len(email) > 320 or len(password) > 128:
            raise PanelError('bad_login', 401)

        # the bnetserver checks the password (and locks accounts after too many wrong ones)
        try:
            context = ssl.create_default_context(cafile=str(Path(os.environ.get('CA_CERT', '/certs/ca/forever_root_ca.crt'))))
        except (OSError, ssl.SSLError) as ex:
            log.warning('companions login: no CA certificate (%s)', ex)
            raise PanelError('login_unavailable', 503)
        context.check_hostname = False   # internal Docker name; the certificate is still checked against our CA
        body = {'version': '1.0', 'inputs': [{'input_id': 'account_name', 'value': email}, {'input_id': 'password', 'value': password}]}
        try:
            async with httpx.AsyncClient(timeout=15, verify=context) as client:
                res = await client.post(f'{BNET_REST_URL}/login/', json=body)
            answer = res.json()
        except (httpx.HTTPError, ValueError) as ex:
            log.warning('companions login: bnetserver unreachable (%s)', ex)
            raise PanelError('login_unavailable', 503)
        if not answer.get('login_ticket'):
            raise PanelError('bad_login', 401)

        access = await asyncio.to_thread(account_access, email)
        if not access or access['level'] < 3:
            log.info('companions login refused for %s (not GM level 3)', email)
            raise PanelError('not_allowed', 403)
        window.clear()
        token = sign({'bnet': access['bnet'], 'accounts': access['accounts'], 'email': email.lower(), 'exp': int(now + SESSION_HOURS * 3600)})
        response = JSONResponse({'ok': True})
        secure = request.headers.get('x-forwarded-proto', request.url.scheme) == 'https'
        response.set_cookie(COOKIE, token, max_age=SESSION_HOURS * 3600, httponly=True, samesite='strict', secure=secure, path='/')
        log.info('companions login: %s', email)
        return response

    @router.post('/api/companions/logout')
    async def logout():
        response = JSONResponse({'ok': True})
        response.delete_cookie(COOKIE, path='/')
        return response

    @router.get('/api/companions/me')
    @guarded
    async def me(request: Request):
        session = session_of(request)
        characters = await asyncio.to_thread(characters_of, session)
        info = {}
        try:
            parts = next((p for p in await admin('info') if p[0] == 'INFO'), None)
            if parts:
                info = {'enabled': parts[1] == '1', 'llm': parts[2] == '1', 'max_per_player': int(parts[3]),
                        'max_quality': int(parts[4]), 'max_level': int(parts[5])}
                if len(parts) >= 9:
                    info.update({'max_total': int(parts[6]), 'raid_size': int(parts[7]), 'online_total': int(parts[8])})
        except PanelError:
            info = {'offline': True}
        # the meta talent build of each tree, from the worldserver (one list, kept there)
        if not _builds_cache:
            try:
                for parts in await admin('builds'):
                    if parts[0] == 'BUILD' and len(parts) >= 5:
                        _builds_cache.setdefault(int(parts[1]), []).append({'tree': int(parts[2]), 'points': parts[3]})
            except PanelError:
                pass
        try:
            providers = [{'id': p['id'], 'name': p['name'], 'model': p['model'], 'enabled': p['enabled']}
                         for p in await asyncio.to_thread(ai['providers'])]
        except PanelError:
            providers = []
        return {'email': session['email'], 'characters': characters, 'server': info, 'providers': providers, 'builds': _builds_cache,
                'classes': CLASSES, 'races': RACES, 'specs': SPECS, 'qualities': QUALITIES, 'roles': list(ROLE_WORDS)}

    @router.get('/api/companions/owner/{owner}')
    @guarded
    async def owner_view(request: Request, owner: int):
        session = session_of(request)
        character = await asyncio.to_thread(owned_character, session, owner)
        rows = await asyncio.to_thread(bots_of, owner)
        try:
            owner_live, live, loading = parse_live(await admin(f'live {owner}'))
            races = {int(p[1]): [int(r) for r in p[2].split(',')] for p in await admin(f'races {owner}') if p[0] == 'RACES'}
        except PanelError:
            owner_live, live, loading, races = None, {}, set(), {}
        bots = [bot_json(row, live.get(int(row['guid']))) for row in rows]
        for bot in bots:
            bot['loading'] = bot['guid'] in loading
        return {'owner': character, 'live': owner_live, 'bots': bots, 'races': races}

    @router.get('/api/companions/looks')
    @guarded
    async def looks(request: Request, race: int, gender: int):
        session_of(request)
        if race not in RACES or gender not in (0, 1):
            raise PanelError('bad_race')
        key = (race, gender)
        if key not in _looks_cache:
            options, order = {}, []
            for parts in await admin(f'looks {race} {gender}'):
                if parts[0] == 'OPT':
                    options[parts[1]] = {'id': int(parts[1]), 'name': parts[2], 'choices': []}
                    order.append(parts[1])
                elif parts[0] == 'CHOICE' and parts[1] in options:
                    options[parts[1]]['choices'].append({'id': int(parts[2]), 'name': parts[3]})
            result = {'options': [options[k] for k in order if options[k]['choices']]}
            if not result['options']:
                return result   # nothing to remember: the worldserver may answer better after an update
            _looks_cache[key] = result
        return _looks_cache[key]

    @router.get('/api/companions/randomname')
    @guarded
    async def random_name(request: Request):
        session_of(request)
        race = int(request.query_params.get('race') or 0)
        gender = int(request.query_params.get('gender') or 2)
        race = race if race in RACES else 0
        gender = gender if gender in (0, 1) else 2
        parts = next((p for p in await admin(f'randomname {race} {gender}') if p[0] == 'NAME'), None)
        return {'name': parts[1] if parts else '', 'surname': parts[2] if parts and len(parts) > 2 else ''}

    @router.post('/api/companions/owner/{owner}/bots')
    @guarded
    async def new_bot(request: Request, owner: int):
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        settings = bot_settings(await request.json())
        return await create_bot(owner, settings)

    @router.patch('/api/companions/owner/{owner}/bots/{bot}')
    @guarded
    async def edit_bot(request: Request, owner: int, bot: int):
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        current = await asyncio.to_thread(owned_bot, owner, bot)
        data = await request.json()
        changes = bot_settings(data, partial=True)
        if 'name' in changes and changes['name'] and changes['name'] != current['name']:
            await admin(f"set {owner} {bot} name {changes['name']}")
        if 'role' in changes:
            await admin(f"set {owner} {bot} role {ROLE_WORDS[changes['role']]}")
        if 'spec' in changes:
            await admin(f"set {owner} {bot} spec {changes['spec'] or '-'}")
        for key in ('llm', 'quality', 'aggression', 'stance', 'provider', 'loot', 'potions', 'autocc'):
            if key in changes:
                await admin(f'set {owner} {bot} {key} {int(changes[key])}')
        if 'personality' in changes:
            await admin(f"set {owner} {bot} personality {changes['personality'] or ' '}")
        if 'surname' in changes:
            await admin(f"set {owner} {bot} surname {changes['surname'] or '-'}")
        return {'ok': True}

    def parse_inspect(lines):
        out = {'online': False, 'stats': {}, 'gear': [], 'bags': [], 'talents': [], 'spells': [], 'prefs': []}
        for p in lines:
            kind = p[0]
            if kind == 'INSPECT' and len(p) >= 14:
                out.update({'online': p[1] == '1', 'level': int(p[2]), 'class': int(p[3]), 'race': int(p[4]), 'role': ROLES.get(int(p[5]), 'auto'),
                            'health': int(p[6]), 'max_health': int(p[7]), 'power': int(p[8]), 'max_power': int(p[9]), 'money': int(p[10]),
                            'loot': int(p[11]), 'potions': p[12] == '1', 'autocc': p[13] == '1'})
            elif kind == 'STAT' and len(p) >= 3:
                out['stats'][p[1]] = float(p[2])
            elif kind == 'GEAR' and len(p) >= 8:
                out['gear'].append({'slot': int(p[1]), 'item': int(p[2]), 'entry': int(p[3]), 'name': p[4], 'quality': int(p[5]), 'ilvl': int(p[6])})
            elif kind == 'BAG' and len(p) >= 12:
                out['bags'].append({'bag': int(p[1]), 'slot': int(p[2]), 'item': int(p[3]), 'entry': int(p[4]), 'name': p[5], 'quality': int(p[6]),
                                    'ilvl': int(p[7]), 'count': int(p[8]), 'equippable': p[9] == '1', 'upgrade': p[10] == '1', 'bound': p[11] == '1'})
            elif kind == 'TALENT' and len(p) >= 5:
                out['talents'].append({'tab': int(p[1]), 'name': p[2], 'rank': int(p[3]), 'max': int(p[4])})
            elif kind == 'SPELL' and len(p) >= 2:
                out['spells'].append(p[1])
            elif kind == 'PREF' and len(p) >= 3:
                out['prefs'].append({'kind': p[1], 'spell': p[2], 'below': int(p[3] or 0) if len(p) > 3 else 0, 'reason': p[4] if len(p) > 4 else ''})
        return out

    SPELL_RE = re.compile(r"^[a-z' :-]{2,60}$")

    @router.get('/api/companions/owner/{owner}/bots/{bot}/inspect')
    @guarded
    async def inspect_bot(request: Request, owner: int, bot: int):
        """Gear, bags, stats, talents, spells and what it was told about them."""
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        await asyncio.to_thread(owned_bot, owner, bot)
        return parse_inspect(await admin(f'inspect {owner} {bot}'))

    @router.post('/api/companions/owner/{owner}/bots/{bot}/item')
    @guarded
    async def bot_item(request: Request, owner: int, bot: int):
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        await asyncio.to_thread(owned_bot, owner, bot)
        data = await request.json()
        action = str(data.get('action') or '')
        if action not in ('equip', 'unequip', 'destroy'):
            raise PanelError('bad_request')
        await admin(f"item {owner} {bot} {action} {int(data.get('item') or 0)}")
        return {'ok': True}

    @router.post('/api/companions/owner/{owner}/bots/{bot}/pref')
    @guarded
    async def bot_pref(request: Request, owner: int, bot: int):
        """prefer / avoid / allow a spell ("allow *" forgets them all)."""
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        await asyncio.to_thread(owned_bot, owner, bot)
        data = await request.json()
        kind = str(data.get('kind') or '')
        spell = str(data.get('spell') or '').strip().lower()
        if kind not in ('prefer', 'avoid', 'allow') or not (spell == '*' and kind == 'allow' or SPELL_RE.match(spell)):
            raise PanelError('bad_request')
        await admin(f'set {owner} {bot} {kind} {spell}')
        return {'ok': True}

    @router.post('/api/companions/owner/{owner}/bots/{bot}/{action}')
    @guarded
    async def bot_action(request: Request, owner: int, bot: int, action: str):
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        await asyncio.to_thread(owned_bot, owner, bot)
        if action not in ('summon', 'dismiss', 'regear', 'respec', 'delete'):
            raise PanelError('bad_request')
        await admin(f'{action} {owner} {bot}')
        return {'ok': True}

    @router.post('/api/companions/owner/{owner}/order')
    @guarded
    async def order(request: Request, owner: int):
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        data = await request.json()
        bot = int(data.get('bot') or 0)
        if bot:
            await asyncio.to_thread(owned_bot, owner, bot)
        await admin(f"order {owner} {bot} {order_text(data.get('order', ''))}")
        return {'ok': True}

    @router.post('/api/companions/owner/{owner}/group')
    @guarded
    async def group_settings(request: Request, owner: int):
        """The same setting for every companion of this character (brain, aggression, stance, gear quality)."""
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        changes = bot_settings(await request.json(), partial=True)
        rows = await asyncio.to_thread(bots_of, owner)
        for row in rows:
            for key in GROUP_KEYS:
                if key in changes:
                    await admin(f"set {owner} {row['guid']} {key} {int(changes[key])}")
        return {'ok': True, 'bots': len(rows)}

    @router.post('/api/companions/owner/{owner}/summon-all')
    @guarded
    async def summon_all(request: Request, owner: int):
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        data = await request.json()
        wanted = {int(g) for g in data.get('bots', [])}
        done, errors = 0, []
        for row in await asyncio.to_thread(bots_of, owner):
            if wanted and int(row['guid']) not in wanted:
                continue
            try:
                await admin(f"{'dismiss' if data.get('dismiss') else 'summon'} {owner} {row['guid']}")
                done += 1
            except PanelError as ex:
                errors.append(f"{row['name']}: {ex}")
        return {'ok': True, 'done': done, 'errors': errors}

    # ---- templates

    def templates_of(bnet: int, kind: str):
        rows = query('SELECT id, name, data, updated FROM characters.companion_web_templates WHERE bnet_account = %s AND kind = %s ORDER BY name',
                     (bnet, kind))
        return [{'id': r['id'], 'name': r['name'], 'data': json.loads(r['data']), 'updated': str(r['updated'])} for r in rows]

    @router.get('/api/companions/templates')
    @guarded
    async def list_templates(request: Request):
        session = session_of(request)
        bots = await asyncio.to_thread(templates_of, session['bnet'], 'bot')
        parties = await asyncio.to_thread(templates_of, session['bnet'], 'party')
        return {'bots': bots, 'parties': parties}

    @router.post('/api/companions/templates')
    @guarded
    async def save_template(request: Request):
        session = session_of(request)
        data = await request.json()
        kind = data.get('kind')
        name = clean_text(data.get('name'), 64)
        if kind not in ('bot', 'party') or not name:
            raise PanelError('bad_template')
        if kind == 'bot':
            payload = bot_settings(data.get('data') or {})
            payload['random_looks'] = bool((data.get('data') or {}).get('random_looks', not payload['looks']))
        else:
            raw = data.get('data') or {}
            members = [int(m) for m in raw.get('members', [])][:39]
            mine = {t['id'] for t in await asyncio.to_thread(templates_of, session['bnet'], 'bot')}
            if not members or any(m not in mine for m in members):
                raise PanelError('bad_template')
            group = bot_settings(raw.get('group') or {}, partial=True)
            payload = {'members': members, 'group': {k: group[k] for k in GROUP_KEYS if k in group},
                       'replace': bool(raw.get('replace', True))}
        template_id = int(data.get('id') or 0)
        if template_id:
            rows = await asyncio.to_thread(query, 'SELECT id FROM characters.companion_web_templates WHERE id = %s AND bnet_account = %s AND kind = %s',
                                           (template_id, session['bnet'], kind))
            if not rows:
                raise PanelError('bad_template', 404)
            await asyncio.to_thread(execute, 'UPDATE characters.companion_web_templates SET name = %s, data = %s WHERE id = %s',
                                    (name, json.dumps(payload), template_id))
        else:
            template_id = await asyncio.to_thread(execute, 'INSERT INTO characters.companion_web_templates (bnet_account, kind, name, data) '
                                                  'VALUES (%s, %s, %s, %s)', (session['bnet'], kind, name, json.dumps(payload)))
        return {'ok': True, 'id': template_id}

    @router.delete('/api/companions/templates/{template_id}')
    @guarded
    async def delete_template(request: Request, template_id: int):
        session = session_of(request)
        await asyncio.to_thread(execute, 'DELETE FROM characters.companion_web_templates WHERE id = %s AND bnet_account = %s',
                                (template_id, session['bnet']))
        return {'ok': True}

    @router.post('/api/companions/owner/{owner}/apply/{template_id}')
    @guarded
    async def apply_template(request: Request, owner: int, template_id: int):
        """A bot template makes one bot; a party template brings its bots (re-using the ones made from it before)."""
        session = session_of(request)
        await asyncio.to_thread(owned_character, session, owner)
        rows = await asyncio.to_thread(query, 'SELECT kind, data FROM characters.companion_web_templates WHERE id = %s AND bnet_account = %s',
                                       (template_id, session['bnet']))
        if not rows:
            raise PanelError('bad_template', 404)
        kind, data = rows[0]['kind'], json.loads(rows[0]['data'])
        bot_templates = {t['id']: t['data'] for t in await asyncio.to_thread(templates_of, session['bnet'], 'bot')}

        def settings_from(template: dict) -> dict:
            settings = bot_settings(template)
            if template.get('random_looks'):
                settings['looks'] = ''
            return settings

        if kind == 'bot':
            return {'ok': True, 'created': [await create_bot(owner, settings_from(data), template_id)]}

        existing = await asyncio.to_thread(bots_of, owner)
        _, live, _ = parse_live(await admin(f'live {owner}'))
        wanted, created, summoned, errors = set(), [], [], []
        used = set()
        for member in data['members']:
            template = bot_templates.get(member)
            if not template:
                continue
            # a bot made from this template earlier and not taken yet by another slot
            match = next((r for r in existing if r['template_id'] == member and int(r['guid']) not in used), None)
            try:
                if match:
                    used.add(int(match['guid']))
                    wanted.add(int(match['guid']))
                    if int(match['guid']) not in live:
                        await admin(f"summon {owner} {match['guid']}")
                        summoned.append(match['name'])
                else:
                    settings = settings_from(template)
                    if settings['name'] and any(r['name'].lower() == settings['name'].lower() for r in existing):
                        settings['name'] = ''
                    bot = await create_bot(owner, settings, member)
                    wanted.add(bot['guid'])
                    created.append(bot['name'])
            except PanelError as ex:
                errors.append(str(ex))
        if data.get('replace', True):
            for guid in live:
                if guid not in wanted:
                    await admin(f'dismiss {owner} {guid}')
        group = data.get('group') or {}
        for guid in wanted:
            for key in GROUP_KEYS:
                if key in group:
                    try:
                        await admin(f'set {owner} {guid} {key} {int(group[key])}')
                    except PanelError:
                        pass
        return {'ok': True, 'created': created, 'summoned': summoned, 'errors': errors}

    ai = companions_ai.add_routes(router, query=query, execute=execute, admin=admin, guarded=guarded, session_of=session_of,
                                  PanelError=PanelError, clean_text=clean_text)

    return router, ensure_tables
