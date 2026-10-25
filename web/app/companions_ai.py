"""Companions panel, "Server" tab and the brain workers' API.

Server tab (GM level 3 accounts, like the rest of the panel): the server-wide companion settings (".bot admin config",
stored by the worldserver in characters.companion_settings), the AI providers (characters.companion_llm_providers) and
the remote brain workers (characters.companion_llm_workers). The worldserver creates these tables at its start.

Brain API (/api/brain/*): a brain running on another machine (BRAIN_URL + BRAIN_TOKEN) fetches its providers, claims
the lines its providers can answer and posts the answers. The token is shown once in the panel; only its SHA-256 is kept.
"""
import asyncio
import hashlib
import json
import logging
import re
import secrets

import pymysql
from fastapi import Request
from fastapi.responses import JSONResponse

log = logging.getLogger('forever-web.companions')

PROVIDER_KINDS = ('anthropic', 'ollama', 'openai')
ENDPOINT_RE = re.compile(r'^https?://[A-Za-z0-9._:\-\[\]]+(/[A-Za-z0-9._~/%-]*)?$')
MODEL_RE = re.compile(r'^[A-Za-z0-9._:/@+-]{1,128}$')
SETTING_KEY_RE = re.compile(r'^[A-Za-z.]{2,48}$')
MAX_AGE = 60
TEST_TIMEOUT = 75
SECRET_KEEP = '********'


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def claim_condition(provider_ids):
    """Same rule as brain.py: lines for these providers, for any provider, or waiting too long for theirs."""
    mine = f"provider_id IN ({','.join(str(int(i)) for i in provider_ids)})" if provider_ids else '0'
    return (f"(provider_id = 0 OR {mine} OR (kind <> 'test' AND (created < NOW() - INTERVAL 8 SECOND "
            f"OR provider_id NOT IN (SELECT id FROM characters.companion_llm_providers WHERE enabled = 1))))")


def add_routes(router, *, query, execute, admin, guarded, session_of, PanelError, clean_text):

    def tables_ready(fn):
        """The worldserver makes the tables at its first start with the companions module."""
        def wrapper(*args, **kwargs):
            try:
                return fn(*args, **kwargs)
            except pymysql.err.ProgrammingError as ex:
                if ex.args and ex.args[0] == 1146:
                    raise PanelError('ai_tables_missing', 503)
                raise
        return wrapper

    # ------------------------------------------------------------------ data

    @tables_ready
    def providers(full=False):
        rows = query('SELECT p.*, w.name AS worker_name FROM characters.companion_llm_providers p '
                     'LEFT JOIN characters.companion_llm_workers w ON w.id = p.worker_id ORDER BY p.id')
        out = []
        for r in rows:
            item = {k: r[k] for k in ('id', 'name', 'kind', 'endpoint', 'model', 'weight', 'max_concurrent', 'timeout_s',
                                      'worker_id', 'answered', 'failed', 'avg_ms', 'last_error')}
            item.update({'enabled': bool(r['enabled']), 'has_key': bool(r['api_key']), 'options': r['options'] or '',
                         'worker_name': r['worker_name'] or '', 'last_ok': str(r['last_ok']) if r['last_ok'] else ''})
            if full:
                item['api_key'] = r['api_key']
            out.append(item)
        return out

    @tables_ready
    def workers():
        rows = query('SELECT id, name, info, last_seen, TIMESTAMPDIFF(SECOND, last_seen, NOW()) AS ago '
                     'FROM characters.companion_llm_workers ORDER BY id')
        return [{'id': r['id'], 'name': r['name'], 'info': r['info'], 'last_seen': str(r['last_seen']) if r['last_seen'] else '',
                 'online': r['ago'] is not None and r['ago'] < 60} for r in rows]

    @tables_ready
    def queue_stats():
        rows = query('SELECT status, kind, COUNT(*) AS n FROM characters.companion_llm_events '
                     'WHERE created > NOW() - INTERVAL 1 HOUR GROUP BY status, kind')
        stats = {'waiting': 0, 'working': 0, 'answered': 0, 'expired': 0, 'callouts': 0, 'chat': 0}
        for r in rows:
            stats[{0: 'waiting', 1: 'working', 2: 'answered'}.get(r['status'], 'expired')] += r['n']
            if r['kind'] == 'callout':
                stats['callouts'] += r['n']
            elif r['kind'] in ('chat', 'group'):
                stats['chat'] += r['n']
        return stats

    def provider_fields(data: dict, current: dict | None) -> dict:
        name = clean_text(data.get('name'), 64)
        kind = str(data.get('kind') or '')
        endpoint = str(data.get('endpoint') or '').strip()
        model = str(data.get('model') or '').strip()
        if not name or kind not in PROVIDER_KINDS:
            raise PanelError('bad_provider')
        if endpoint and (len(endpoint) > 255 or not ENDPOINT_RE.match(endpoint)):
            raise PanelError('bad_endpoint')
        if kind != 'anthropic' and not endpoint:
            raise PanelError('bad_endpoint')
        if not MODEL_RE.match(model):
            raise PanelError('bad_model')
        options = str(data.get('options') or '').strip()
        if options:
            try:
                if not isinstance(json.loads(options), dict):
                    raise ValueError
            except ValueError:
                raise PanelError('bad_options')
        key = str(data.get('api_key') or '')
        if key == SECRET_KEEP and current is not None:
            key = current['api_key']
        if len(key) > 255 or any(c in key for c in '\r\n'):
            raise PanelError('bad_provider')
        worker = int(data.get('worker_id') or 0)
        if worker and not query('SELECT id FROM characters.companion_llm_workers WHERE id = %s', (worker,)):
            raise PanelError('bad_provider')
        return {
            'name': name, 'kind': kind, 'endpoint': endpoint, 'model': model, 'api_key': key, 'options': options[:2000],
            'weight': min(max(int(data.get('weight') or 1), 1), 100),
            'max_concurrent': min(max(int(data.get('max_concurrent') or 2), 1), 64),
            'timeout_s': min(max(int(data.get('timeout_s') or 30), 5), 300),
            'enabled': 1 if data.get('enabled', True) else 0,
            'worker_id': worker,
        }

    # ------------------------------------------------------------------ server tab

    @router.get('/api/companions/server')
    @guarded
    async def server_view(request: Request):
        session_of(request)
        settings, info = [], {}
        try:
            lines = await admin('config list')
            settings = [{'key': p[1], 'value': p[2], 'type': p[3], 'min': float(p[4]), 'max': float(p[5]), 'stored': p[6] == '1'}
                        for p in lines if p[0] == 'SET' and len(p) >= 7]
            parts = next((p for p in await admin('info') if p[0] == 'INFO'), None)
            if parts and len(parts) >= 9:
                info = {'online': int(parts[8]), 'max_total': int(parts[6]), 'raid_size': int(parts[7]), 'llm': parts[2] == '1'}
        except PanelError:
            info = {'offline': True}
        try:
            ai = {'providers': await asyncio.to_thread(providers), 'workers': await asyncio.to_thread(workers),
                  'queue': await asyncio.to_thread(queue_stats)}
        except PanelError:
            ai = {'providers': [], 'workers': [], 'queue': {}, 'tables_missing': True}
        return {'settings': settings, 'info': info, **ai}

    @router.post('/api/companions/server/settings')
    @guarded
    async def change_settings(request: Request):
        session_of(request)
        data = await request.json()
        changed = 0
        for key, value in (data.get('set') or {}).items():
            if not SETTING_KEY_RE.match(str(key)):
                raise PanelError('bad_setting')
            value = '1' if value is True else '0' if value is False else str(value)
            if not re.match(r'^-?\d{1,6}(\.\d{1,3})?$', value):
                raise PanelError('bad_setting')
            await admin(f'config set {key} {value}')
            changed += 1
        for key in data.get('reset') or []:
            if not SETTING_KEY_RE.match(str(key)):
                raise PanelError('bad_setting')
            await admin(f'config reset {key}')
            changed += 1
        return {'ok': True, 'changed': changed}

    @router.post('/api/companions/server/providers')
    @guarded
    async def save_provider(request: Request):
        session_of(request)
        data = await request.json()
        provider_id = int(data.get('id') or 0)
        current = None
        if provider_id:
            rows = await asyncio.to_thread(tables_ready(query), 'SELECT * FROM characters.companion_llm_providers WHERE id = %s', (provider_id,))
            if not rows:
                raise PanelError('bad_provider', 404)
            current = rows[0]
        fields = await asyncio.to_thread(provider_fields, data, current)
        columns = list(fields)
        if provider_id:
            await asyncio.to_thread(execute, 'UPDATE characters.companion_llm_providers SET ' + ', '.join(f'{c} = %s' for c in columns)
                                    + ' WHERE id = %s', (*fields.values(), provider_id))
        else:
            provider_id = await asyncio.to_thread(tables_ready(execute), 'INSERT INTO characters.companion_llm_providers (' + ', '.join(columns)
                                                  + ') VALUES (' + ', '.join(['%s'] * len(columns)) + ')', tuple(fields.values()))
        log.info('companions: AI provider %s saved (%s %s)', fields['name'], fields['kind'], fields['model'])
        return {'ok': True, 'id': provider_id}

    @router.delete('/api/companions/server/providers/{provider_id}')
    @guarded
    async def delete_provider(request: Request, provider_id: int):
        session_of(request)
        await asyncio.to_thread(tables_ready(execute), 'DELETE FROM characters.companion_llm_providers WHERE id = %s', (provider_id,))
        # bots that used it go back to "any provider" (through the worldserver, which keeps the records in memory)
        for row in await asyncio.to_thread(query, 'SELECT guid FROM characters.companion_bots WHERE llm_provider = %s', (provider_id,)):
            try:
                await admin(f"set 0 {row['guid']} provider 0")
            except PanelError:
                await asyncio.to_thread(execute, 'UPDATE characters.companion_bots SET llm_provider = 0 WHERE guid = %s', (row['guid'],))
        return {'ok': True}

    @router.post('/api/companions/server/providers/{provider_id}/test')
    @guarded
    async def test_provider(request: Request, provider_id: int):
        """A test line for this provider only: answered by whichever brain runs it."""
        session_of(request)
        rows = await asyncio.to_thread(tables_ready(query), 'SELECT enabled, failed FROM characters.companion_llm_providers WHERE id = %s', (provider_id,))
        if not rows:
            raise PanelError('bad_provider', 404)
        if not rows[0]['enabled']:
            raise PanelError('provider_disabled')
        failed_before = rows[0]['failed']
        event_id = await asyncio.to_thread(
            execute,
            "INSERT INTO characters.companion_llm_events (bot_guid, owner_guid, kind, channel, speaker, message, context, bots, provider_id) "
            "VALUES (0, 0, 'test', 'party', 'Web panel', 'hello', %s, %s, %s)",
            ('leader: Tester\nplace: the web panel (a connection test)\n', '0|Tester|Mage|dps\n', provider_id))
        loop = asyncio.get_running_loop()
        started = loop.time()
        while loop.time() - started < TEST_TIMEOUT:
            await asyncio.sleep(0.7)
            event = (await asyncio.to_thread(query, 'SELECT status, claimed_by FROM characters.companion_llm_events WHERE id = %s', (event_id,)))[0]
            if event['status'] == 0 and loop.time() - started > 12:
                await asyncio.to_thread(execute, 'UPDATE characters.companion_llm_events SET status = 3 WHERE id = %s AND status = 0', (event_id,))
                raise PanelError('no_brain', 504)
            if event['status'] >= 2:
                action = await asyncio.to_thread(query, 'SELECT say FROM characters.companion_llm_actions WHERE event_id = %s', (event_id,))
                provider = (await asyncio.to_thread(query, 'SELECT failed, avg_ms, last_error FROM characters.companion_llm_providers WHERE id = %s',
                                                    (provider_id,)))[0]
                ms = int((loop.time() - started) * 1000)
                if action:
                    return {'ok': True, 'say': action[0]['say'], 'ms': ms, 'brain': event['claimed_by'].split(':')[0]}
                if provider['failed'] > failed_before:
                    return JSONResponse({'error': 'provider_failed', 'detail': provider['last_error']}, status_code=502)
                return {'ok': True, 'say': '', 'ms': ms, 'brain': event['claimed_by'].split(':')[0]}
        raise PanelError('test_timeout', 504)

    @router.post('/api/companions/server/workers')
    @guarded
    async def new_worker(request: Request):
        session_of(request)
        data = await request.json()
        name = clean_text(data.get('name'), 64)
        if not name:
            raise PanelError('bad_worker')
        token = 'cbw_' + secrets.token_urlsafe(32)
        worker_id = await asyncio.to_thread(tables_ready(execute), 'INSERT INTO characters.companion_llm_workers (name, token_hash) VALUES (%s, %s)',
                                            (name, token_hash(token)))
        log.info('companions: brain worker %s created', name)
        return {'ok': True, 'id': worker_id, 'token': token}

    @router.delete('/api/companions/server/workers/{worker_id}')
    @guarded
    async def delete_worker(request: Request, worker_id: int):
        session_of(request)
        await asyncio.to_thread(tables_ready(execute), 'DELETE FROM characters.companion_llm_workers WHERE id = %s', (worker_id,))
        # its providers ran on that machine (often "localhost" there): off until moved
        await asyncio.to_thread(execute, 'UPDATE characters.companion_llm_providers SET enabled = 0 WHERE worker_id = %s', (worker_id,))
        return {'ok': True}

    # ------------------------------------------------------------------ brain workers

    def worker_of(request: Request) -> dict:
        auth = request.headers.get('authorization', '')
        if not auth.lower().startswith('bearer ') or len(auth) > 200:
            raise PanelError('bad_token', 401)
        rows = tables_ready(query)('SELECT id, name FROM characters.companion_llm_workers WHERE token_hash = %s', (token_hash(auth[7:].strip()),))
        if not rows:
            raise PanelError('bad_token', 401)
        return rows[0]

    def worker_route(handler):
        async def wrapper(request: Request):
            try:
                worker = await asyncio.to_thread(worker_of, request)
                return await handler(request, worker, await request.json())
            except PanelError as ex:
                return JSONResponse({'error': str(ex)}, status_code=ex.status)
            except (ValueError, TypeError, KeyError):
                return JSONResponse({'error': 'bad_request'}, status_code=400)
            except pymysql.MySQLError as ex:
                log.warning('brain api database: %s', ex)
                return JSONResponse({'error': 'database'}, status_code=503)
        wrapper.__name__ = handler.__name__
        return wrapper

    def worker_providers(worker_id: int):
        return query('SELECT id, name, kind, endpoint, model, api_key, weight, max_concurrent, timeout_s, options '
                     'FROM characters.companion_llm_providers WHERE enabled = 1 AND worker_id = %s', (worker_id,))

    @router.post('/api/brain/heartbeat')
    @worker_route
    async def brain_heartbeat(request: Request, worker: dict, data: dict):
        info = clean_text(f"{data.get('info', '')} (brain {data.get('version', '?')})", 250)
        await asyncio.to_thread(execute, 'UPDATE characters.companion_llm_workers SET last_seen = NOW(), info = %s WHERE id = %s', (info, worker['id']))
        return {'providers': await asyncio.to_thread(worker_providers, worker['id'])}

    @router.post('/api/brain/claim')
    @worker_route
    async def brain_claim(request: Request, worker: dict, data: dict):
        count = min(max(int(data.get('max') or 1), 1), 20)

        def claim():
            ids = [p['id'] for p in worker_providers(worker['id'])]
            if not ids:
                return []
            token = f"w{worker['id']}:{secrets.token_hex(6)}"
            execute(f'UPDATE characters.companion_llm_events SET status = 1, claimed_by = %s, claimed_at = NOW() WHERE status = 0 '
                    f'AND claimed_by <> %s AND created > NOW() - INTERVAL {MAX_AGE} SECOND AND {claim_condition(ids)} ORDER BY id LIMIT %s',
                    (token, f"x:w{worker['id']}", count))
            execute('UPDATE characters.companion_llm_workers SET last_seen = NOW() WHERE id = %s', (worker['id'],))
            return query('SELECT id, bot_guid, owner_guid, kind, channel, speaker, message, context, bots, provider_id, '
                         'TIMESTAMPDIFF(SECOND, created, NOW()) AS age FROM characters.companion_llm_events '
                         'WHERE claimed_by = %s AND status = 1 ORDER BY id', (token,))
        return {'events': await asyncio.to_thread(claim)}

    def claimed_event(worker: dict, event_id: int) -> dict:
        rows = query('SELECT id, bot_guid, bots, status FROM characters.companion_llm_events WHERE id = %s AND claimed_by LIKE %s',
                     (event_id, f"w{worker['id']}:%"))
        if not rows:
            raise PanelError('not_your_event', 403)
        return rows[0]

    def own_provider(worker: dict, provider_id: int) -> bool:
        return bool(provider_id) and bool(query('SELECT id FROM characters.companion_llm_providers WHERE id = %s AND worker_id = %s',
                                                (provider_id, worker['id'])))

    def record_stats(provider_id, ok, ms, error):
        if ok:
            execute('UPDATE characters.companion_llm_providers SET answered = answered + 1, '
                    'avg_ms = IF(avg_ms = 0, %s, ROUND(avg_ms * 0.8 + %s * 0.2)), last_ok = NOW(), last_error = \'\' WHERE id = %s', (ms, ms, provider_id))
        else:
            execute('UPDATE characters.companion_llm_providers SET failed = failed + 1, last_error = %s WHERE id = %s',
                    (clean_text(error, 250), provider_id))

    @router.post('/api/brain/answer')
    @worker_route
    async def brain_answer(request: Request, worker: dict, data: dict):
        def answer():
            event = claimed_event(worker, int(data['event_id']))
            if event['status'] != 1:
                return {'ok': True, 'late': True}
            allowed = {int(event['bot_guid'])} | {int(line.split('|')[0]) for line in (event['bots'] or '').splitlines()
                                                  if line.split('|')[0].isdigit()}
            for action in (data.get('actions') or [])[:12]:   # a tactical review assigns every companion
                guid = int(action.get('bot_guid') or 0)
                if guid not in allowed:
                    continue
                channel = 'whisper' if action.get('channel') == 'whisper' else 'party'
                commands = '\n'.join(clean_text(line, 80) for line in str(action.get('commands') or '').splitlines()[:4] if line.strip())
                execute('INSERT INTO characters.companion_llm_actions (event_id, bot_guid, channel, target, say, commands, delay_ms) '
                        'VALUES (%s, %s, %s, %s, %s, %s, %s)',
                        (event['id'], guid, channel, clean_text(action.get('target'), 64), clean_text(action.get('say'), 250), commands,
                         min(max(int(action.get('delay_ms') or 0), 0), 6000)))
            execute('UPDATE characters.companion_llm_events SET status = 2 WHERE id = %s', (event['id'],))
            provider_id = int(data.get('provider_id') or 0)
            if own_provider(worker, provider_id):
                record_stats(provider_id, bool(data.get('ok')), min(max(int(data.get('ms') or 0), 0), 600000), str(data.get('error') or ''))
            return {'ok': True}
        return await asyncio.to_thread(answer)

    @router.post('/api/brain/failure')
    @worker_route
    async def brain_failure(request: Request, worker: dict, data: dict):
        provider_id = int(data.get('provider_id') or 0)
        if await asyncio.to_thread(own_provider, worker, provider_id):
            await asyncio.to_thread(record_stats, provider_id, False, 0, str(data.get('error') or ''))
        return {'ok': True}

    @router.post('/api/brain/release')
    @worker_route
    async def brain_release(request: Request, worker: dict, data: dict):
        def release():
            event = claimed_event(worker, int(data['event_id']))
            # after a failure: marked so that another brain takes it
            execute('UPDATE characters.companion_llm_events SET status = 0, claimed_by = %s WHERE id = %s AND status = 1',
                    (f"x:w{worker['id']}" if data.get('failed') else '', event['id']))
            return {'ok': True}
        return await asyncio.to_thread(release)

    return {'providers': providers}
