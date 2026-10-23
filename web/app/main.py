"""Forever - account creation website.

Accounts are created by the worldserver itself through SOAP (`bnetaccount create`), exactly like from its console.
SOAP is only reachable on the internal Docker network. The SOAP admin (a plain game account with GM level 3) is
created / updated by this service at startup, directly in the auth database, with the password from SOAP_PASSWORD.
"""
import asyncio
import hashlib
import html
import logging
import os
import re
import secrets
import socket
import time
from collections import defaultdict, deque
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
import pymysql
from fastapi import FastAPI, Form, Request
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates

from . import i18n

log = logging.getLogger('forever-web')
logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s')

DB = dict(
    host=os.environ.get('DB_HOST', 'mysql'),
    port=int(os.environ.get('DB_PORT', '3306')),
    user=os.environ.get('DB_USER', 'trinity'),
    password=os.environ['DB_PASSWORD'],
    autocommit=True,
    connect_timeout=5,
)
SOAP_URL = os.environ.get('SOAP_URL', 'http://worldserver:7878/')
SOAP_USER = os.environ.get('SOAP_USER', 'FOREVERWEB').upper()
SOAP_PASSWORD = os.environ['SOAP_PASSWORD']
WORLD_HOST = os.environ.get('WORLD_HOST', 'worldserver')
WORLD_PORT = int(os.environ.get('WORLD_PORT', '8085'))
SERVER_NAME = os.environ.get('SERVER_NAME', 'Forever')
GAME_VERSION = os.environ.get('GAME_VERSION', '1.60.1.70532')
PUBLIC_ADDRESS = os.environ.get('PUBLIC_ADDRESS', '')
CA_CERT = Path(os.environ.get('CA_CERT', '/certs/ca/forever_root_ca.crt'))
LAUNCHER_LINUX_URL = os.environ.get('LAUNCHER_LINUX_URL', '')
LAUNCHER_WINDOWS_URL = os.environ.get('LAUNCHER_WINDOWS_URL', '')
LAUNCHER_MAC_URL = os.environ.get('LAUNCHER_MAC_URL', '')
# launcher downloads: files dropped in DOWNLOADS_DIR, else the latest release of LAUNCHER_REPO (a private repo needs
# GITHUB_TOKEN, read-only on its contents); LAUNCHER_*_URL win over both
DOWNLOADS_DIR = Path(os.environ.get('DOWNLOADS_DIR', '/downloads'))
LAUNCHER_REPO = os.environ.get('LAUNCHER_REPO', '')
GITHUB_TOKEN = os.environ.get('GITHUB_TOKEN', '')
# signups per client IP per hour (open registration)
RATE_PER_HOUR = int(os.environ.get('SIGNUPS_PER_IP_PER_HOUR', '3'))

EMAIL_RE = re.compile(r'^[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9.-]{1,190}\.[A-Za-z]{2,24}$')
# TrinityCore splits console arguments on spaces; keep passwords to printable ASCII without spaces
PASSWORD_RE = re.compile(r'^[\x21-\x7E]{8,64}$')

# GruntSRP6 (AccountMgr game accounts): v = g ^ SHA1(salt || SHA1(UPPER(user) ":" UPPER(pass))) mod N, little-endian
SRP_N = int('894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7', 16)
SRP_G = 7


def grunt_registration(username: str, password: str) -> tuple[bytes, bytes]:
    salt = secrets.token_bytes(32)
    inner = hashlib.sha1(f'{username.upper()}:{password.upper()}'.encode()).digest()
    x = int.from_bytes(hashlib.sha1(salt + inner).digest(), 'little')
    return salt, pow(SRP_G, x, SRP_N).to_bytes(32, 'little')


def db():
    return pymysql.connect(**DB)


def ensure_soap_admin() -> None:
    """Creates or refreshes the SOAP admin game account (GM level 3 on all realms)."""
    salt, verifier = grunt_registration(SOAP_USER, SOAP_PASSWORD)
    with db() as conn, conn.cursor() as cur:
        cur.execute('SELECT id FROM auth.account WHERE username = %s', (SOAP_USER,))
        row = cur.fetchone()
        if row:
            account_id = row[0]
            cur.execute('UPDATE auth.account SET salt = %s, verifier = %s WHERE id = %s', (salt, verifier, account_id))
        else:
            cur.execute('INSERT INTO auth.account (username, salt, verifier, email, reg_mail, joindate) '
                        'VALUES (%s, %s, %s, "", "", NOW())', (SOAP_USER, salt, verifier))
            account_id = cur.lastrowid
        cur.execute('DELETE FROM auth.account_access WHERE AccountID = %s', (account_id,))
        cur.execute('INSERT INTO auth.account_access (AccountID, SecurityLevel, RealmID) VALUES (%s, 3, -1)',
                    (account_id,))
    log.info('SOAP admin account %s ready (id %s)', SOAP_USER, account_id)


async def bootstrap() -> None:
    # the auth tables only exist once worldserver has finished its first start
    while True:
        try:
            await asyncio.to_thread(ensure_soap_admin)
            return
        except pymysql.MySQLError as ex:
            log.info('waiting for the auth database (%s)', ex.args[-1] if ex.args else ex)
            await asyncio.sleep(10)


@asynccontextmanager
async def lifespan(_app):
    task = asyncio.create_task(bootstrap())
    yield
    task.cancel()


app = FastAPI(lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)
BASE = Path(__file__).parent
app.mount('/static', StaticFiles(directory=BASE / 'static'), name='static')
templates = Jinja2Templates(directory=BASE / 'templates')
# appended to the stylesheet URL so browsers fetch the new one after an update
ASSET_VERSION = hashlib.sha1((BASE / 'static' / 'style.css').read_bytes()).hexdigest()[:10]

_signups: dict[str, deque] = defaultdict(deque)


def client_ip(request: Request) -> str:
    # behind Dokploy's Traefik: it appends the real client address at the end of X-Forwarded-For
    forwarded = request.headers.get('x-forwarded-for', '')
    if forwarded:
        return forwarded.split(',')[-1].strip()
    return request.client.host if request.client else '?'


def rate_limited(ip: str) -> bool:
    now, window = time.time(), _signups[ip]
    while window and now - window[0] > 3600:
        window.popleft()
    return len(window) >= RATE_PER_HOUR


class SoapError(Exception):
    pass


async def soap_command(command: str) -> str:
    body = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" '
        'xmlns:ns1="urn:TC"><SOAP-ENV:Body><ns1:executeCommand>'
        f'<command>{html.escape(command, quote=False)}</command>'
        '</ns1:executeCommand></SOAP-ENV:Body></SOAP-ENV:Envelope>'
    )
    try:
        async with httpx.AsyncClient(timeout=15) as client:
            r = await client.post(SOAP_URL, content=body.encode(), auth=(SOAP_USER, SOAP_PASSWORD),
                                  headers={'Content-Type': 'text/xml; charset=utf-8'})
    except httpx.HTTPError as ex:
        raise SoapError(f'serveur injoignable ({ex.__class__.__name__})') from ex
    text = html.unescape(r.text)
    result = re.search(r'<result>(.*?)</result>', text, re.S)
    if r.status_code == 200 and result:
        return result.group(1).strip()
    fault = re.search(r'<faultstring>(.*?)</faultstring>', text, re.S)
    detail = re.search(r'<detail>(.*?)</detail>', text, re.S)
    raise SoapError((detail or fault).group(1).strip() if (detail or fault) else f'HTTP {r.status_code}')


def world_online() -> bool:
    try:
        with socket.create_connection((WORLD_HOST, WORLD_PORT), timeout=1.5):
            return True
    except OSError:
        return False


def players_online() -> int | None:
    try:
        with db() as conn, conn.cursor() as cur:
            cur.execute('SELECT COUNT(*) FROM characters.characters WHERE online = 1')
            return int(cur.fetchone()[0])
    except pymysql.MySQLError:
        return None


async def status() -> dict:
    online, players = await asyncio.gather(asyncio.to_thread(world_online), asyncio.to_thread(players_online))
    return {'online': online, 'players': players}


# platform -> (name shown, preferred file suffixes)
PLATFORMS = {
    'windows': ('Windows', ('-setup.exe', '.exe')),
    'linux': ('Linux', ('.appimage',)),
    'macos': ('macOS', ('.dmg',)),
}
URL_OVERRIDES = {'windows': LAUNCHER_WINDOWS_URL, 'linux': LAUNCHER_LINUX_URL, 'macos': LAUNCHER_MAC_URL}
_release = {'at': 0.0, 'assets': []}


def pick(names, suffixes):
    for suffix in suffixes:
        found = sorted(n for n in names if n.lower().endswith(suffix))
        if found:
            return found[-1]
    return None


def local_file(platform: str) -> Path | None:
    if not DOWNLOADS_DIR.is_dir():
        return None
    files = {f.name: f for f in DOWNLOADS_DIR.iterdir() if f.is_file()}
    name = pick(files, PLATFORMS[platform][1])
    return files[name] if name else None


async def release_assets() -> list:
    """Assets of the latest GitHub release, cached 5 minutes."""
    if not LAUNCHER_REPO:
        return []
    if time.time() - _release['at'] < 300:
        return _release['assets']
    headers = {'Accept': 'application/vnd.github+json'}
    if GITHUB_TOKEN:
        headers['Authorization'] = f'Bearer {GITHUB_TOKEN}'
    try:
        async with httpx.AsyncClient(timeout=10) as client:
            res = await client.get(f'https://api.github.com/repos/{LAUNCHER_REPO}/releases/latest', headers=headers)
        assets = res.json().get('assets', []) if res.status_code == 200 else []
        if res.status_code != 200:
            log.warning('launcher release of %s: HTTP %s', LAUNCHER_REPO, res.status_code)
    except (httpx.HTTPError, ValueError) as ex:
        log.warning('launcher release of %s: %s', LAUNCHER_REPO, ex)
        assets = _release['assets']
    _release.update(at=time.time(), assets=assets)
    return assets


async def release_asset(platform: str) -> dict | None:
    assets = {a['name']: a for a in await release_assets()}
    name = pick(assets, PLATFORMS[platform][1])
    return assets[name] if name else None


async def downloads() -> list:
    """[{id, name, available}] for the download buttons."""
    result = []
    for platform, (name, _) in PLATFORMS.items():
        available = bool(URL_OVERRIDES[platform] or local_file(platform) or await release_asset(platform))
        result.append({'id': platform, 'name': name, 'available': available})
    return result


async def page(request: Request, status_code: int = 200, **ctx):
    query_lang = request.query_params.get('lang')
    lang = i18n.pick_language(query_lang, request.cookies.get('lang'), request.headers.get('accept-language'))
    response = templates.TemplateResponse(request, 'index.html', {
        'lang': lang,
        'languages': i18n.LANGUAGES,
        't': i18n.translator(lang),
        'js_texts': i18n.js_texts(lang),
        'server_name': SERVER_NAME,
        'game_version': GAME_VERSION,
        'public_address': PUBLIC_ADDRESS,
        'has_ca': CA_CERT.is_file(),
        'asset_version': ASSET_VERSION,
        'downloads': await downloads(),
        **ctx,
    }, status_code=status_code)
    if query_lang in i18n.TEXTS:
        response.set_cookie('lang', query_lang, max_age=365 * 24 * 3600, samesite='lax')
    return response


@app.get('/', response_class=HTMLResponse)
async def index(request: Request):
    return await page(request, status=await status())


@app.get('/api/status')
async def api_status():
    return JSONResponse(await status())


@app.get('/download/{platform}')
async def download(platform: str):
    if platform not in PLATFORMS:
        return JSONResponse({'error': 'unknown platform'}, status_code=404)
    if URL_OVERRIDES[platform]:
        return RedirectResponse(URL_OVERRIDES[platform])
    if f := local_file(platform):
        return FileResponse(f, media_type='application/octet-stream', filename=f.name)
    if asset := await release_asset(platform):
        # GitHub answers with a short-lived signed link to the file: send the browser there
        headers = {'Accept': 'application/octet-stream'}
        if GITHUB_TOKEN:
            headers['Authorization'] = f'Bearer {GITHUB_TOKEN}'
        try:
            async with httpx.AsyncClient(timeout=10) as client:
                res = await client.get(asset['url'], headers=headers)
            if res.status_code in (301, 302, 303, 307) and res.headers.get('location'):
                return RedirectResponse(res.headers['location'])
            log.warning('download %s: HTTP %s', asset['name'], res.status_code)
        except httpx.HTTPError as ex:
            log.warning('download %s: %s', asset['name'], ex)
    return JSONResponse({'error': 'launcher not available yet'}, status_code=404)


@app.get('/forever_root_ca.crt')
async def ca_cert():
    if not CA_CERT.is_file():
        return JSONResponse({'error': 'no CA certificate'}, status_code=404)
    return FileResponse(CA_CERT, media_type='application/x-x509-ca-cert', filename='forever_root_ca.crt')


@app.get('/healthz')
async def healthz():
    return {'ok': True}


@app.post('/register', response_class=HTMLResponse)
async def register(request: Request, email: str = Form(''), password: str = Form(''), confirm: str = Form(''),
                   website: str = Form('')):
    email = email.strip()
    form = {'email': email}

    async def fail(message, code=400):
        return await page(request, error=message, form=form, status_code=code, status=None)

    if website:                      # honeypot field, invisible to humans
        return await fail('err_refused')
    if not EMAIL_RE.match(email) or len(email) > 320:
        return await fail('err_email')
    if not PASSWORD_RE.match(password):
        return await fail('err_password')
    if password != confirm:
        return await fail('err_mismatch')
    ip = client_ip(request)
    if rate_limited(ip):
        return await fail('err_rate', 429)

    try:
        result = await soap_command(f'bnetaccount create {email} {password}')
    except SoapError as ex:
        message = str(ex)
        log.warning('signup %s from %s failed: %s', email, ip, message)
        if 'already exist' in message.lower():
            return await fail('err_exists', 409)
        return await fail('err_server', 503)
    if 'created' not in result.lower():
        log.warning('signup %s from %s: unexpected answer %r', email, ip, result)
        return await fail('err_server', 503)

    _signups[ip].append(time.time())
    log.info('account created: %s from %s', email, ip)
    return await page(request, created=email, status=None)
