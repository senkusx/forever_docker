// Forever Launcher UI. Everything privileged happens in Rust (src-tauri); this file only calls its commands.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const $ = (id) => document.getElementById(id);

let state = null;      // snapshot from get_state
let draft = null;      // settings being edited in the drawer
let toastTimer = 0;

function toast(text) {
  const t = $('toast');
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.hidden = true), 4500);
}

function log(line) {
  const pre = $('log');
  const time = new Date().toLocaleTimeString(lang);
  pre.textContent += `[${time}] ${line}\n`;
  pre.scrollTop = pre.scrollHeight;
}

function setStep(text, kind = '') {
  const s = $('step');
  s.textContent = text;
  s.className = 'step' + (kind ? ' ' + kind : '');
}

// ------------------------------------------------------------------------------------------------ state
// '' follows the system language; Rust gets the same language for its messages
function setLanguage(setting) {
  const code = resolveLanguage(setting);
  if (code === lang && document.documentElement.lang === code) return;
  applyLanguage(code);
  invoke('set_language', { language: code });
}

async function refresh() {
  state = await invoke('get_state');
  setLanguage(state.settings.language);
  $('realm-field').textContent = t('realm', { name: state.settings.server.name || 'Forever' });
  $('version').textContent = `v${state.version} · ${{ windows: 'Windows', macos: 'macOS', linux: 'Linux' }[state.platform]}`;
  $('signup').hidden = !state.settings.server.website;
  const logged = !!state.email;
  $('view-login').hidden = logged;
  $('view-play').hidden = !logged;
  $('account').hidden = !logged;
  $('game-field').textContent = `WoW Forever ${state.gameVersion || '1.60.1.70532'}`;
  if (logged) {
    $('account-email').textContent = state.email;
    $('avatar').textContent = state.email[0].toUpperCase();
  }
  updatePlayButton();
}

function updatePlayButton() {
  const play = $('play');
  play.disabled = state.playing || !state.gameFound;
  play.classList.toggle('running', state.playing);
  play.textContent = state.playing ? t('inGame') : t('play');
  if (!state.gameFound && !state.playing) setStep(t('chooseGameDir'), 'err');
}

async function refreshStatus() {
  const pill = $('status');
  try {
    const s = await invoke('server_status');
    pill.dataset.state = s.online && s.login ? 'online' : s.online || s.login ? 'partial' : 'offline';
    $('status-text').textContent = t(s.online && s.login ? 'online' : s.online || s.login ? 'starting' : 'offline');
  } catch {
    pill.dataset.state = 'offline';
    $('status-text').textContent = t('unreachable');
  }
}

async function refreshChecks() {
  try {
    const c = await invoke('checks');
    showCheck('check-hosts', c.hosts, c.hostsDetail);
    showCheck('check-cert', c.certificate, c.certificateDetail);
    // on Linux the prefix certificate is installed automatically at launch: only the hosts entry needs the user
    const needsCert = state.platform !== 'linux' && !c.certificate;
    const blocking = !c.hosts || needsCert;
    $('setup-notice').hidden = !blocking;
    $('setup-detail').textContent = [!c.hosts && c.hostsDetail, needsCert && c.certificateDetail]
      .filter(Boolean).join(' · ');
    return c;
  } catch (e) {
    log(t('checks', { e }));
  }
}

function showCheck(id, ok, detail) {
  const el = $(id);
  el.dataset.ok = String(ok);
  el.querySelector('p').textContent = detail;
}

async function repair(btn) {
  btn.disabled = true;
  const label = btn.textContent;
  btn.textContent = t('working');
  try {
    const c = await invoke('repair');
    showCheck('check-hosts', c.hosts, c.hostsDetail);
    showCheck('check-cert', c.certificate, c.certificateDetail);
    toast(c.hosts ? t('pcReady') : t('partialRepair'));
  } catch (e) {
    toast(t('failed', { e }));
  } finally {
    btn.disabled = false;
    btn.textContent = label;
    refreshChecks();
  }
}

// ------------------------------------------------------------------------------------------------ login
$('login-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  const btn = $('login-btn');
  const err = $('login-error');
  err.hidden = true;
  btn.disabled = true;
  btn.textContent = t('loggingIn');
  try {
    await invoke('login', { email: $('email').value, password: $('password').value });
    $('password').value = '';
    await refresh();
    refreshChecks();
  } catch (e) {
    err.textContent = String(e);
    err.hidden = false;
  } finally {
    btn.disabled = false;
    btn.textContent = t('loginButton');
  }
});

$('logout').addEventListener('click', async () => {
  await invoke('logout');
  await refresh();
});

$('signup-link').addEventListener('click', (ev) => {
  ev.preventDefault();
  window.__TAURI__.opener.openUrl(state.settings.server.website);
});

// ------------------------------------------------------------------------------------------------ play
$('play').addEventListener('click', async () => {
  $('log').textContent = '';
  state.playing = true;
  updatePlayButton();
  setStep(t('preparing'));
  try {
    await invoke('play');
  } catch (e) {
    state.playing = false;
    updatePlayButton();
    setStep(String(e), 'err');
    log(t('error', { e }));
    refresh();   // an expired session sends back to the login view
  }
});

$('setup-fix').addEventListener('click', (ev) => repair(ev.currentTarget));
$('toggle-log').addEventListener('click', (ev) => {
  ev.preventDefault();
  $('log-box').hidden = !$('log-box').hidden;
});
$('close-log').addEventListener('click', () => ($('log-box').hidden = true));
$('repair').addEventListener('click', (ev) => repair(ev.currentTarget));

listen('game', ({ payload }) => {
  const { kind, message } = payload;
  if (kind === 'log') return log(message);
  log(message);
  if (kind === 'step' || kind === 'running') setStep(message);
  if (kind === 'ready') setStep(message, 'ok');
  if (kind === 'error') setStep(message, 'err');
  if (kind === 'closed' || kind === 'error') {
    state.playing = false;
    updatePlayButton();
    if (kind === 'closed') setStep('');
  }
});

// ------------------------------------------------------------------------------------------------ settings
function option(select, value, label) {
  const o = document.createElement('option');
  o.value = value;
  o.textContent = label;
  select.append(o);
}

function fillSettings() {
  draft = structuredClone(state.settings);
  $('s-game-dir').value = draft.gameDir;
  const loc = $('s-locale');
  loc.replaceChildren();
  state.locales.forEach((l) => option(loc, l, l));
  loc.value = draft.locale;
  $('linux-settings').hidden = state.platform !== 'linux';
  if (state.platform === 'linux') {
    const r = $('s-runner');
    r.replaceChildren();
    state.runners.forEach(([name, path]) => option(r, path, name));
    if (![...r.options].some((o) => o.value === draft.linux.runner) && draft.linux.runner) option(r, draft.linux.runner, draft.linux.runner);
    r.value = draft.linux.runner;
    $('s-prefix').value = draft.linux.prefix;
    $('s-gamescope').checked = draft.linux.gamescope;
    $('s-gamescope-args').value = draft.linux.gamescopeArgs;
    $('s-keyboard').value = draft.linux.keyboard;
  }
  const language = $('s-language');
  language.replaceChildren();
  option(language, '', t('systemLanguage'));
  Object.entries(LANGUAGES).forEach(([code, name]) => option(language, code, name));
  language.value = draft.language || '';
  $('s-address').value = draft.server.address;
  $('s-website').value = draft.server.website;
  $('s-portal').value = draft.server.portal;
  gameDirState();
}

function gameDirState() {
  const el = $('game-dir-state');
  const dir = $('s-game-dir').value.trim();
  const same = dir === state.settings.gameDir;
  const game = state.platform === 'macos' ? t('gameApp') : 'WowB.exe';
  el.textContent = !dir ? t('noFolder') : same && state.gameFound ? t('gameFound', { game }) : same ? t('gameMissing', { game }) : '';
  el.className = 'small ' + (same && state.gameFound ? 'ok' : dir && same ? 'ko' : '');
}

function openSettings() {
  fillSettings();
  $('settings').hidden = false;
  $('scrim').hidden = false;
  refreshChecks();
}

function closeSettings() {
  $('settings').hidden = true;
  $('scrim').hidden = true;
}

async function pick(input, title) {
  const dir = await window.__TAURI__.dialog.open({ directory: true, title, defaultPath: input.value || undefined });
  if (dir) {
    input.value = dir;
    if (input.id === 's-game-dir') {
      const loc = await invoke('detect_locale', { gameDir: dir });
      if (loc) $('s-locale').value = loc;
    }
  }
}

$('open-settings').addEventListener('click', openSettings);
$('play-settings').addEventListener('click', openSettings);
$('close-settings').addEventListener('click', closeSettings);
$('cancel-settings').addEventListener('click', closeSettings);
$('scrim').addEventListener('click', closeSettings);
$('pick-game').addEventListener('click', () => pick($('s-game-dir'), t('pickGameDir')));
$('pick-prefix').addEventListener('click', () => pick($('s-prefix'), t('pickPrefix')));
$('s-game-dir').addEventListener('input', gameDirState);
document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !$('settings').hidden) closeSettings(); });

$('save-settings').addEventListener('click', async () => {
  draft.gameDir = $('s-game-dir').value.trim();
  draft.locale = $('s-locale').value;
  draft.language = $('s-language').value;
  if (state.platform === 'linux') {
    draft.linux.runner = $('s-runner').value;
    draft.linux.prefix = $('s-prefix').value.trim();
    draft.linux.gamescope = $('s-gamescope').checked;
    draft.linux.gamescopeArgs = $('s-gamescope-args').value.trim();
    draft.linux.keyboard = $('s-keyboard').value.trim();
  }
  draft.server.address = $('s-address').value.trim();
  draft.server.website = $('s-website').value.trim();
  draft.server.portal = $('s-portal').value.trim();
  try {
    await invoke('save_settings', { settings: draft });
    closeSettings();
    await refresh();
    refreshStatus();
    refreshChecks();
    refreshNews();
    toast(t('saved'));
  } catch (e) {
    toast(t('saveFailed', { e }));
  }
});

// ------------------------------------------------------------------------------------------------ news
// shown until the website has a blog (<website>/api/news)
const defaultNews = () => [1, 2, 3].map((i) => ({ tag: t(`news${i}Tag`), title: t(`news${i}`), image: '', url: '' }));

function openLink(url) {
  if (url) window.__TAURI__.opener.openUrl(url);
}

async function refreshNews() {
  let items = [];
  try {
    items = await invoke('news');
  } catch (e) {
    console.debug('news:', e);
  }
  const fromSite = items.length > 0;
  if (!fromSite) items = defaultNews();
  const website = state.settings.server.website;
  $('news-more').hidden = !fromSite || !website;
  $('news-more').onclick = (ev) => { ev.preventDefault(); openLink(website); };
  $('news').replaceChildren(...items.slice(0, 3).map((n) => {
    const card = document.createElement('button');
    card.className = 'news-card';
    card.type = 'button';
    // a drawn placeholder when the post has no image (or it does not load)
    const thumb = () => Object.assign(document.createElement('div'), { className: 'thumb', textContent: 'F' });
    let img = thumb();
    if (n.image) {
      img = document.createElement('img');
      img.src = n.image;
      img.alt = '';
      img.onerror = () => img.replaceWith(thumb());
    }
    const body = document.createElement('div');
    body.className = 'body';
    const tag = document.createElement('span');
    tag.className = 'tag';
    tag.textContent = n.tag || t('newsTag');
    const title = document.createElement('strong');
    title.textContent = n.title;
    body.append(tag, title);
    if (n.date) {
      const date = document.createElement('span');
      date.className = 'date';
      date.textContent = n.date;
      body.append(date);
    }
    card.append(img, body);
    card.addEventListener('click', () => openLink(n.url || website));
    return card;
  }));
}

// ------------------------------------------------------------------------------------------------ background
function rotateBackground() {
  const slides = [...document.querySelectorAll('.backdrop .bg')];
  let current = Math.floor(Math.random() * slides.length);
  slides[current].classList.add('on');
  setInterval(() => {
    slides[current].classList.remove('on');
    current = (current + 1) % slides.length;
    slides[current].classList.add('on');
  }, 20000);
}

// ------------------------------------------------------------------------------------------------ start
rotateBackground();
(async () => {
  await refresh();
  refreshStatus();
  refreshNews();
  if (state.email) refreshChecks();
  setInterval(refreshStatus, 20000);
})();
