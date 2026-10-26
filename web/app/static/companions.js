// Companions panel: party, new companion and templates for the characters of a GM level 3 account.
// Everything is built with textContent (names and personalities come from players).
(() => {
  'use strict';
  const TX = window.CP_TEXTS || {};
  const T = (key, values) => {
    let text = TX[key] !== undefined ? TX[key] : key;
    for (const [k, v] of Object.entries(values || {})) text = text.split(`{${k}}`).join(String(v));
    return text;
  };

  // the party view refreshes every 5 s: never while someone types, picks in a list or just changed something
  let lastInput = 0;
  document.addEventListener('input', () => { lastInput = Date.now(); }, true);
  document.addEventListener('change', () => { lastInput = Date.now(); }, true);
  function editing() {
    const el = document.activeElement;
    if (el && /^(INPUT|TEXTAREA|SELECT)$/.test(el.tagName) && el.closest('.cp-content')) return true;
    return Date.now() - lastInput < 15000;
  }

  const state = { me: null, owner: null, view: null, templates: { bots: [], parties: [] }, tab: 'party', form: null, looks: {}, timer: null,
    page: 'owner', server: null, providerForm: null, newToken: null,
    drafts: {}, applied: {}, groupDraft: {}, groupApplied: null, teams: [], teamEdit: null };
  const $ = (id) => document.getElementById(id);

  // ------------------------------------------------------------------ helpers

  function h(tag, attrs, ...children) {
    const el = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs || {})) {
      if (value === undefined || value === null || value === false) continue;
      if (key === 'class') el.className = value;
      else if (key.startsWith('on')) el.addEventListener(key.slice(2), value);
      else if (key === 'value') el.value = value;
      else if (key === 'checked') el.checked = !!value;
      else el.setAttribute(key, value === true ? '' : value);
    }
    for (const child of children.flat()) {
      if (child === null || child === undefined || child === false) continue;
      el.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
    return el;
  }

  async function api(method, url, body) {
    const res = await fetch(url, {
      method,
      headers: { 'Content-Type': 'application/json', 'X-Companions': '1' },
      body: body === undefined ? undefined : JSON.stringify(body),
      credentials: 'same-origin',
    });
    let data = {};
    try { data = await res.json(); } catch (e) { /* empty answer */ }
    if (!res.ok) {
      if (res.status === 401) showLogin();
      const error = new Error(data.error || `HTTP ${res.status}`);
      error.detail = data.detail;
      throw error;
    }
    return data;
  }

  function toast(text, bad) {
    const el = $('cp-toast');
    el.textContent = text;
    el.className = 'cp-toast show' + (bad ? ' bad' : '');
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => { el.className = 'cp-toast'; }, 4000);
  }

  const errorText = (err) => {
    const key = 'err_' + err.message;
    const text = TX[key] !== undefined ? TX[key] : err.message;
    return err.detail ? `${text}: ${err.detail}` : text;
  };

  async function run(action, success) {
    try {
      const result = await action();
      if (success) toast(typeof success === 'function' ? success(result) : success);
      return result;
    } catch (err) {
      toast(errorText(err), true);
      return null;
    }
  }

  const className = (id) => T('class_' + id);
  const raceName = (id) => T('race_' + id);
  const roleName = (role) => T('role_' + role);
  const specName = (klass, index) => T(`spec_${klass}_${index}`);
  const qualityName = (q) => q ? T('quality_' + q) : T('quality_default');
  const aggressionName = (a) => T('aggression_' + a);

  function select(options, value, onchange, attrs) {
    return h('select', { ...attrs, onchange: (e) => onchange(e.target.value) },
      options.map(([v, label]) => h('option', { value: v, selected: String(v) === String(value) || undefined }, label)));
  }

  function toggle(label, checked, onchange) {
    return h('label', { class: 'cp-toggle' }, h('input', { type: 'checkbox', checked, onchange: (e) => onchange(e.target.checked) }), h('span', null, label));
  }

  // one meta build per talent tree (points per tree from the worldserver), or the role's one
  function specOptions(klass, current) {
    const specs = (state.me && state.me.specs[klass]) || [];
    const builds = (state.me && state.me.builds && state.me.builds[klass]) || [];
    const options = [['', T('spec_auto')]];
    specs.forEach((_, i) => {
      const build = builds.find((b) => b.tree === i);
      options.push([String(i), build ? T('spec_build', { tree: specName(klass, i), points: build.points }) : specName(klass, i)]);
    });
    // a combination chosen before the builds existed stays visible until changed
    if (current && !options.some(([v]) => v === current))
      options.push([current, T('spec_custom', { tabs: current.split(',').map((i) => specName(klass, Number(i))).join(' + ') })]);
    return options;
  }

  const qualityOptions = () => [['0', T('quality_default')], ...state.me.qualities.map((q) => [String(q), qualityName(q)])];
  const aggressionOptions = () => [0, 1, 2].map((a) => [String(a), aggressionName(a)]);
  const roleOptions = () => state.me.roles.map((r) => [r, roleName(r)]);
  const providerOptions = () => [['0', T('provider_any')], ...(state.me.providers || []).map((p) => [String(p.id), p.enabled ? p.name : `${p.name} (${T('off')})`])];
  const hasProviders = () => !!(state.me && state.me.providers && state.me.providers.length);

  // ------------------------------------------------------------------ login

  function showLogin() {
    clearInterval(state.timer);
    $('cp-app').hidden = true;
    $('cp-user').hidden = true;
    $('cp-login').hidden = false;
  }

  $('cp-login-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const form = new FormData(e.target);
    $('cp-login-error').textContent = '';
    try {
      await api('POST', '/api/companions/login', { email: form.get('email'), password: form.get('password') });
      e.target.reset();
      await start();
    } catch (err) {
      $('cp-login-error').textContent = errorText(err);
    }
  });

  $('cp-logout').addEventListener('click', async () => {
    await api('POST', '/api/companions/logout').catch(() => {});
    showLogin();
  });

  // ------------------------------------------------------------------ characters

  async function start() {
    try {
      state.me = await api('GET', '/api/companions/me');
    } catch (err) {
      showLogin();
      return;
    }
    $('cp-login').hidden = true;
    $('cp-app').hidden = false;
    $('cp-user').hidden = false;
    $('cp-email').textContent = state.me.email;
    const server = state.me.server;
    $('cp-server').textContent = server.offline ? T('server_offline')
      : T('server_info', { max: server.max_per_player, brain: server.llm ? T('on') : T('off') })
        + (server.max_total ? ' · ' + T('server_total', { n: server.online_total, max: server.max_total }) : '');
    renderCharacters();
    await loadTemplates();
    const remembered = Number(localStorage.getItem('cp-owner') || 0);
    const first = state.me.characters.find((c) => Number(c.guid) === remembered) || state.me.characters.find((c) => c.online);
    if (first) selectOwner(Number(first.guid));
  }

  function renderCharacters() {
    const list = $('cp-chars');
    list.replaceChildren(...state.me.characters.map((c) => h('li', null,
      h('button', { type: 'button', class: 'cp-char' + (Number(c.guid) === state.owner ? ' on' : ''), onclick: () => selectOwner(Number(c.guid)) },
        h('span', { class: 'dot ' + (c.online ? 'ok' : '') }),
        h('strong', null, c.name),
        h('small', { class: 'muted' }, `${c.level} ${raceName(c.race)} ${className(c.class)}`)))));
    if (!state.me.characters.length) list.append(h('li', { class: 'muted' }, T('no_characters')));
  }

  async function selectOwner(guid) {
    state.page = 'owner';
    $('cp-serverview').hidden = true;
    $('cp-server-open').classList.remove('on');
    state.owner = guid;
    try { localStorage.setItem('cp-owner', String(guid)); } catch (e) { /* private window */ }
    renderCharacters();
    $('cp-empty').hidden = true;
    $('cp-owner').hidden = false;
    state.form = null;
    state.drafts = {};
    state.groupDraft = {};
    state.teams = [];
    state.teamEdit = null;
    await loadTeams();
    await refresh();
    clearInterval(state.timer);
    state.timer = setInterval(() => { if (state.page === 'owner' && state.tab === 'party' && !document.hidden) refresh(true); }, 5000);
  }

  async function refresh(quiet) {
    if (!state.owner) return;
    try {
      state.view = await api('GET', `/api/companions/owner/${state.owner}`);
    } catch (err) {
      if (!quiet) toast(errorText(err), true);
      return;
    }
    const o = state.view.owner, live = state.view.live;
    $('cp-owner-name').textContent = o.name;
    $('cp-owner-info').textContent = `${o.level} ${raceName(o.race)} ${className(o.class)} · `
      + (live && live.online ? T('in_game', { zone: live.zone || '?' }) : T('not_in_game'))
      + ` · ${T('bots_count', { n: state.view.bots.length })}`;
    if (state.tab === 'party') { if (!quiet || !editing()) renderParty(); }
    else if (state.tab === 'create' && !state.form) renderCreate();
    else if (state.tab === 'templates') renderTemplates();
  }

  document.querySelectorAll('.cp-tab').forEach((tab) => tab.addEventListener('click', () => {
    state.tab = tab.dataset.tab;
    document.querySelectorAll('.cp-tab').forEach((t) => t.classList.toggle('on', t === tab));
    for (const name of ['party', 'create', 'templates']) $('tab-' + name).hidden = name !== state.tab;
    if (state.tab === 'party') renderParty();
    if (state.tab === 'create') renderCreate();
    if (state.tab === 'templates') renderTemplates();
  }));

  // ------------------------------------------------------------------ party

  const ownerOnline = () => !!(state.view && state.view.live && state.view.live.online);

  async function order(text, bot) {
    await run(() => api('POST', `/api/companions/owner/${state.owner}/order`, { order: text, bot: bot || 0 }), T('order_sent', { order: text }));
  }

  async function botAction(bot, action) {
    if (action === 'delete' && !confirm(T('confirm_delete', { name: bot.name }))) return;
    const done = await run(() => api('POST', `/api/companions/owner/${state.owner}/bots/${bot.guid}/${action}`), T('done_' + action, { name: bot.name }));
    if (done) setTimeout(() => refresh(true), action === 'summon' ? 2500 : 600);
  }

  async function editBot(bot, changes, message) {
    const done = await run(() => api('PATCH', `/api/companions/owner/${state.owner}/bots/${bot.guid}`, changes), message || T('saved'));
    if (done) refresh();
  }

  // ---- changes wait on the card until Apply: one request, then a clear confirmation of what changed and when it acts

  const FIELD_LABELS = { role: 'role', spec: 'spec', quality: 'gear_quality', aggression: 'aggression', stance: 'stance', llm: 'brain',
    provider: 'provider', personality: 'personality', loot: 'loot_mode', potions: 'use_potions', autocc: 'auto_cc' };
  const botValue = (bot, key) => (key === 'personality' ? (bot.personality || '') : key === 'spec' ? (bot.spec || '')
    : key === 'provider' ? (bot.provider || 0) : key === 'quality' ? (bot.quality || 0) : bot[key]);

  function pendingOf(bot) {
    const draft = state.drafts[bot.guid] || {};
    const changes = {};
    for (const [key, value] of Object.entries(draft))
      if (String(value) !== String(botValue(bot, key))) changes[key] = value;
    return changes;
  }

  async function applyBot(bot, button) {
    const changes = pendingOf(bot);
    if (!Object.keys(changes).length) return;
    button.disabled = true;
    button.textContent = T('applying');
    try {
      await api('PATCH', `/api/companions/owner/${state.owner}/bots/${bot.guid}`, changes);
      delete state.drafts[bot.guid];
      const what = Object.keys(changes).map((key) => T(FIELD_LABELS[key])).join(', ');
      const relearn = bot.live && ('spec' in changes || 'role' in changes || 'quality' in changes);
      state.applied[bot.guid] = { text: T(relearn ? 'applied_relearn' : bot.live ? 'applied_now' : 'applied_next', { what, name: bot.name }), at: Date.now() };
      toast(T('applied_bot', { name: bot.name }));
      await refresh();
    } catch (err) {
      toast(errorText(err), true);
      button.disabled = false;
      button.textContent = T('apply');
    }
  }

  async function applyGroup(button) {
    const changes = { ...state.groupDraft };
    if (!Object.keys(changes).length) return;
    button.disabled = true;
    button.textContent = T('applying');
    try {
      const result = await api('POST', `/api/companions/owner/${state.owner}/group`, changes);
      state.groupDraft = {};
      state.groupApplied = { text: T('applied_group', { n: result.bots, what: Object.keys(changes).map((key) => T(FIELD_LABELS[key])).join(', ') }), at: Date.now() };
      toast(T('applied_group_toast', { n: result.bots }));
      await refresh();
    } catch (err) {
      toast(errorText(err), true);
      button.disabled = false;
      button.textContent = T('apply_all');
    }
  }

  // a confirmation stays on the card for 20 s, through the 5 s refreshes
  function appliedLine(entry) {
    if (!entry || Date.now() - entry.at > 20000) return null;
    return h('div', { class: 'cp-applied', role: 'status' }, '✓ ', entry.text);
  }

  function bar(pct, kind) {
    return h('div', { class: 'cp-bar ' + kind }, h('span', { style: `width:${Math.max(0, Math.min(100, pct))}%` }));
  }

  function renderParty() {
    const panel = $('tab-party');
    const view = state.view;
    if (!view) return;
    const online = ownerOnline();
    const orderBot = h('select', { class: 'cp-order-bot' }, h('option', { value: '0' }, T('all_companions')),
      view.bots.filter((b) => b.live).map((b) => h('option', { value: b.guid }, b.name)));
    const custom = h('input', { type: 'text', placeholder: T('order_placeholder'), maxlength: 80 });

    const orders = h('div', { class: 'card cp-orders' },
      h('div', { class: 'label' }, T('orders')),
      !online ? h('p', { class: 'muted' }, T('orders_offline')) : null,
      h('div', { class: 'cp-row' }, orderBot,
        ...['follow', 'stay', 'come', 'attack', 'pull', 'stop', 'retreat', 'rest', 'status'].map((o) =>
          h('button', { type: 'button', class: 'cp-btn', disabled: !online, title: T('order_help_' + o), onclick: () => order(o, Number(orderBot.value)) }, T('order_' + o)))),
      h('form', { class: 'cp-row', onsubmit: (e) => { e.preventDefault(); if (custom.value.trim()) order(custom.value.trim(), Number(orderBot.value)); } },
        custom, h('button', { type: 'submit', class: 'cp-btn', disabled: !online }, T('send'))),
      h('p', { class: 'muted small' }, T('orders_hint')));

    const anyBrain = view.bots.some((b) => b.llm);
    const gd = state.groupDraft;
    const groupApply = h('button', { type: 'button', class: 'cp-btn primary cp-apply', disabled: !Object.keys(gd).length, onclick: (e) => applyGroup(e.target) }, T('apply_all'));
    const groupNote = h('span', { class: 'muted small' }, Object.keys(gd).length ? T('unsaved') : T('group_hint'));
    const setGroup = (key, value) => {
      if (value === '' || value === null) delete state.groupDraft[key]; else state.groupDraft[key] = value;
      const dirty = Object.keys(state.groupDraft).length > 0;
      groupApply.disabled = !dirty;
      groupNote.textContent = dirty ? T('unsaved') : T('group_hint');
      group.classList.toggle('dirty', dirty);
    };
    const group = h('div', { class: 'card cp-group' + (Object.keys(gd).length ? ' dirty' : '') },
      h('div', { class: 'label' }, T('group_settings')),
      h('div', { class: 'cp-row' },
        toggle(T('brain_all'), 'llm' in gd ? gd.llm : anyBrain, (on) => setGroup('llm', on)),
        h('label', null, T('aggression'), select([['', '—'], ...aggressionOptions()], 'aggression' in gd ? String(gd.aggression) : '', (v) => setGroup('aggression', v === '' ? '' : Number(v)), { 'aria-label': T('aggression') })),
        h('label', null, T('stance'), select([['', '—'], ['0', T('stance_0')], ['1', T('stance_1')]], 'stance' in gd ? String(gd.stance) : '', (v) => setGroup('stance', v === '' ? '' : Number(v)))),
        h('label', null, T('gear_quality'), select([['', '—'], ...qualityOptions()], 'quality' in gd ? String(gd.quality) : '', (v) => setGroup('quality', v === '' ? '' : Number(v)))),
        hasProviders() ? h('label', null, T('provider'), select([['', '—'], ...providerOptions()], 'provider' in gd ? String(gd.provider) : '', (v) => setGroup('provider', v === '' ? '' : Number(v)))) : null),
      state.me.server.llm ? null : h('p', { class: 'muted small' }, T('brain_server_off')),
      h('div', { class: 'cp-row' }, groupApply,
        h('button', { type: 'button', class: 'cp-btn', hidden: !Object.keys(gd).length, onclick: () => { state.groupDraft = {}; renderParty(); } }, T('discard')),
        groupNote),
      appliedLine(state.groupApplied),
      h('div', { class: 'cp-row' },
        h('button', { type: 'button', class: 'cp-btn', disabled: !online || !view.bots.length,
          onclick: async () => { await run(() => api('POST', `/api/companions/owner/${state.owner}/summon-all`, {}), (r) => T('summoned_n', { n: r.done })); setTimeout(() => refresh(true), 2500); } }, T('summon_all')),
        h('button', { type: 'button', class: 'cp-btn', disabled: !view.bots.some((b) => b.live),
          onclick: async () => { await run(() => api('POST', `/api/companions/owner/${state.owner}/summon-all`, { dismiss: true, bots: view.bots.filter((b) => b.live).map((b) => b.guid) }), T('dismissed_all')); setTimeout(() => refresh(true), 800); } }, T('dismiss_all')),
        h('button', { type: 'button', class: 'cp-btn', disabled: !view.bots.length, onclick: () => savePartyFromCurrent() }, T('save_party_template'))));

    const cards = view.bots.length ? view.bots.map((bot) => botCard(bot, online))
      : [h('div', { class: 'card muted' }, T('no_bots'))];
    panel.replaceChildren(orders, teamsCard(), group, h('div', { class: 'cp-bots' }, cards));
  }

  // ------------------------------------------------------------------ teams: companions summoned together, again and again

  async function loadTeams() {
    if (!state.owner) return;
    try {
      state.teams = (await api('GET', `/api/companions/owner/${state.owner}/teams`)).teams;
    } catch (err) {
      state.teams = [];
    }
  }

  async function summonTeam(team) {
    const r = await run(() => api('POST', `/api/companions/owner/${state.owner}/teams/${team.id}/summon`, {}),
      (res) => T('team_summoned', { name: team.name, n: res.summoned, m: res.dismissed }));
    if (r && r.errors && r.errors.length) toast(r.errors.join(' · '), true);
    setTimeout(() => refresh(true), 2500);
  }

  async function saveTeam() {
    const e = state.teamEdit;
    const name = e.name.trim();
    const bots = [...e.bots];
    if (!name || !bots.length) { toast(T('team_incomplete'), true); return; }
    const done = e.id
      ? await run(() => api('PATCH', `/api/companions/owner/${state.owner}/teams/${e.id}`, { name, bots }), T('team_saved', { name }))
      : await run(() => api('POST', `/api/companions/owner/${state.owner}/teams`, { name, bots }), T('team_saved', { name }));
    if (!done) return;
    state.teamEdit = null;
    await loadTeams();
    renderParty();
  }

  async function saveCurrentAsTeam() {
    const live = state.view.bots.filter((b) => b.live).map((b) => b.guid);
    if (!live.length) { toast(T('team_none_out'), true); return; }
    const name = prompt(T('team_name_prompt'), '');
    if (!name || !name.trim()) return;
    const done = await run(() => api('POST', `/api/companions/owner/${state.owner}/teams`, { name: name.trim(), bots: live }), T('team_saved', { name: name.trim() }));
    if (done) { await loadTeams(); renderParty(); }
  }

  async function deleteTeam(team) {
    if (!window.confirm(T('team_delete_confirm', { name: team.name }))) return;
    const done = await run(() => api('DELETE', `/api/companions/owner/${state.owner}/teams/${team.id}`), T('team_deleted', { name: team.name }));
    if (done) { await loadTeams(); renderParty(); }
  }

  function teamEditor() {
    const e = state.teamEdit;
    const nameInput = h('input', { type: 'text', maxlength: 32, value: e.name, placeholder: T('team_name'), oninput: (ev) => { e.name = ev.target.value; } });
    return h('div', { class: 'cp-team editing' },
      h('label', null, T('team_name'), nameInput),
      h('div', { class: 'label' }, T('team_pick')),
      h('div', { class: 'cp-team-pick' }, state.view.bots.map((b) => toggle(`${b.name} · ${className(b.class)} · ${roleName(b.role)}`, e.bots.has(b.guid),
        (on) => { if (on) e.bots.add(b.guid); else e.bots.delete(b.guid); }))),
      h('div', { class: 'cp-row' },
        h('button', { type: 'button', class: 'cp-btn primary', onclick: () => saveTeam() }, T('save')),
        h('button', { type: 'button', class: 'cp-btn', onclick: () => { state.teamEdit = null; renderParty(); } }, T('cancel'))));
  }

  function teamsCard() {
    const view = state.view;
    const online = ownerOnline();
    const byGuid = Object.fromEntries(view.bots.map((b) => [b.guid, b]));
    const rows = state.teams.map((team) => (state.teamEdit && state.teamEdit.id === team.id) ? teamEditor() : h('div', { class: 'cp-team' },
      h('div', { class: 'cp-team-head' }, h('strong', null, team.name), h('span', { class: 'muted small' }, T('team_members', { n: team.bots.length }))),
      h('div', { class: 'cp-team-members' }, team.bots.map((g) => byGuid[g]).filter(Boolean).map((b) =>
        h('span', { class: 'cp-chip' + (b.live ? ' live' : '') }, `${b.name} · ${className(b.class)}`))),
      h('div', { class: 'cp-row' },
        h('button', { type: 'button', class: 'cp-btn primary', disabled: !online, title: online ? T('team_summon_hint') : T('owner_must_play'),
          onclick: () => summonTeam(team) }, T('team_summon')),
        h('button', { type: 'button', class: 'cp-btn', onclick: () => { state.teamEdit = { id: team.id, name: team.name, bots: new Set(team.bots) }; renderParty(); } }, T('edit')),
        h('button', { type: 'button', class: 'cp-btn danger', onclick: () => deleteTeam(team) }, T('delete')))));
    const creating = state.teamEdit && !state.teamEdit.id;
    return h('div', { class: 'card cp-teams' },
      h('div', { class: 'label' }, T('teams')),
      state.teams.length ? null : h('p', { class: 'muted small' }, T('teams_hint')),
      ...rows,
      creating ? teamEditor() : null,
      creating ? null : h('div', { class: 'cp-row' },
        h('button', { type: 'button', class: 'cp-btn', disabled: !view.bots.length,
          onclick: () => { state.teamEdit = { id: 0, name: '', bots: new Set(view.bots.filter((b) => b.live).map((b) => b.guid)) }; renderParty(); } }, T('team_new')),
        h('button', { type: 'button', class: 'cp-btn', disabled: !view.bots.some((b) => b.live), onclick: () => saveCurrentAsTeam() }, T('team_save_current'))));
  }

  function botCard(bot, ownerIn) {
    const live = bot.live;
    const status = bot.loading ? T('state_loading') : live ? T('state_' + live.state) : T('state_offline');
    const value = (key) => { const draft = state.drafts[bot.guid]; return draft && key in draft ? draft[key] : botValue(bot, key); };
    const rename = h('input', { type: 'text', value: bot.name, maxlength: 12, disabled: !!live || !bot.created, title: bot.created ? T('rename_hint') : T('rename_own') });
    const resurname = h('input', { type: 'text', value: bot.surname || '', maxlength: 48, placeholder: T('surname_placeholder'), disabled: !bot.created });

    const applyButton = h('button', { type: 'button', class: 'cp-btn primary cp-apply', onclick: (e) => applyBot(bot, e.target) }, T('apply'));
    const discardButton = h('button', { type: 'button', class: 'cp-btn', onclick: () => { delete state.drafts[bot.guid]; renderParty(); } }, T('discard'));
    const note = h('span', { class: 'muted small' });
    const card = h('article', { class: 'card cp-bot' + (live ? ' live' : '') });
    const refreshState = () => {
      const dirty = Object.keys(pendingOf(bot)).length > 0;
      applyButton.disabled = !dirty;
      discardButton.hidden = !dirty;
      note.textContent = dirty ? T('unsaved') : '';
      card.classList.toggle('dirty', dirty);
    };
    const set = (key, v) => { (state.drafts[bot.guid] = state.drafts[bot.guid] || {})[key] = v; refreshState(); };

    card.append(...[
      h('header', null,
        h('div', null,
          h('h3', null, bot.surname ? `${bot.name} ${bot.surname}` : bot.name, bot.created ? null : h('small', { class: 'cp-tag' }, T('your_character'))),
          h('div', { class: 'muted' }, `${live ? live.level : bot.level} ${raceName(bot.race)} ${className(bot.class)} · ${roleName(bot.role)}`)),
        h('span', { class: 'cp-state ' + (live ? live.state : 'offline') }, status)),
      live ? h('div', { class: 'cp-vitals' },
        bar(live.health, 'hp'), live.mana >= 0 ? bar(live.mana, 'mana') : null,
        h('div', { class: 'muted small' }, T('live_line', { ilvl: live.item_level, spells: live.spells, zone: live.zone || '?' })),
        live.talents ? h('div', { class: 'muted small cp-talents' }, live.talents) : null) : null,
      h('div', { class: 'cp-grid' },
        h('label', null, T('role'), select(roleOptions(), value('role'), (v) => set('role', v))),
        h('label', null, T('spec'), select(specOptions(bot.class, bot.spec || ''), value('spec'), (v) => set('spec', v))),
        h('label', null, T('gear_quality'), select(qualityOptions(), String(value('quality')), (v) => set('quality', Number(v)))),
        h('label', null, T('aggression'), select(aggressionOptions(), String(value('aggression')), (v) => set('aggression', Number(v)))),
        h('label', null, T('stance'), select([['0', T('stance_0')], ['1', T('stance_1')]], String(value('stance')), (v) => set('stance', Number(v)))),
        h('div', { class: 'cp-check' }, toggle(T('brain'), value('llm'), (on) => set('llm', on))),
        hasProviders() ? h('label', null, T('provider'), select(providerOptions(), String(value('provider')), (v) => set('provider', Number(v)))) : null,
        h('label', null, T('loot_mode'), select([0, 1, 2].map((m) => [String(m), T('loot_' + m)]), String(value('loot')), (v) => set('loot', Number(v)))),
        h('div', { class: 'cp-check' }, toggle(T('use_potions'), value('potions'), (on) => set('potions', on))),
        h('div', { class: 'cp-check' }, toggle(T('auto_cc'), value('autocc'), (on) => set('autocc', on)))),
      h('label', { class: 'cp-wide' }, T('personality'),
        h('textarea', { rows: 2, maxlength: 250, placeholder: T('personality_placeholder'), oninput: (e) => set('personality', e.target.value) }, value('personality'))),
      h('div', { class: 'cp-row cp-apply-row' }, applyButton, discardButton, note),
      appliedLine(state.applied[bot.guid]),
      bot.created ? h('div', { class: 'cp-row' }, rename,
        h('button', { type: 'button', class: 'cp-btn', disabled: !!live, onclick: () => editBot(bot, { name: rename.value.trim() }, T('renamed')) }, T('rename')),
        resurname,
        h('button', { type: 'button', class: 'cp-btn', onclick: () => editBot(bot, { surname: resurname.value.trim() }, T('renamed')) }, T('set_surname'))) : null,
      h('footer', { class: 'cp-row' },
        live ? h('button', { type: 'button', class: 'cp-btn', onclick: () => botAction(bot, 'dismiss') }, T('dismiss'))
          : h('button', { type: 'button', class: 'cp-btn primary', disabled: !ownerIn || bot.loading, title: ownerIn ? null : T('owner_must_play'), onclick: () => botAction(bot, 'summon') }, T('summon')),
        h('button', { type: 'button', class: 'cp-btn', disabled: !live, onclick: () => botAction(bot, 'regear') }, T('regear')),
        h('button', { type: 'button', class: 'cp-btn', disabled: !live, onclick: () => botAction(bot, 'respec') }, T('respec')),
        h('button', { type: 'button', class: 'cp-btn', onclick: () => openDetails(bot) }, T('details')),
        h('button', { type: 'button', class: 'cp-btn', onclick: () => saveBotTemplate(templateFromBot(bot)) }, T('save_template')),
        h('button', { type: 'button', class: 'cp-btn danger', disabled: !!live, title: bot.created ? null : T('forget_hint'), onclick: () => botAction(bot, 'delete') },
          bot.created ? T('delete') : T('forget')))].filter(Boolean));
    refreshState();
    return card;
  }

  // ------------------------------------------------------------------ details of one companion

  const STAT_ORDER = ['strength', 'agility', 'stamina', 'intellect', 'spirit', 'armor', 'attack_power', 'ranged_attack_power', 'spell_power',
    'healing_power', 'melee_crit', 'ranged_crit', 'dodge', 'parry', 'block', 'fire_resistance', 'nature_resistance', 'frost_resistance',
    'shadow_resistance', 'arcane_resistance'];
  const PERCENT_STATS = new Set(['melee_crit', 'ranged_crit', 'dodge', 'parry', 'block']);
  const title = (text) => String(text).replace(/(^|[\s-])([a-z])/g, (m, a, b) => a + b.toUpperCase());
  const money = (copper) => `${Math.floor(copper / 10000)}g ${Math.floor(copper / 100) % 100}s ${copper % 100}c`;
  const escClose = (e) => { if (e.key === 'Escape') closeDetails(); };

  function closeDetails() {
    if (!state.details) return;
    state.details.overlay.remove();
    state.details = null;
    document.removeEventListener('keydown', escClose);
  }

  async function openDetails(bot) {
    closeDetails();
    const box = h('div', { class: 'card cp-modal-box', role: 'dialog', 'aria-modal': 'true', 'aria-label': bot.name });
    const overlay = h('div', { class: 'cp-modal', onclick: (e) => { if (e.target === overlay) closeDetails(); } }, box);
    document.body.append(overlay);
    document.addEventListener('keydown', escClose);
    state.details = { bot, overlay, box, tab: state.detailsTab || 'gear', data: null };
    renderDetails();
    await loadDetails();
  }

  async function loadDetails() {
    const d = state.details;
    if (!d) return;
    try {
      d.data = await api('GET', `/api/companions/owner/${state.owner}/bots/${d.bot.guid}/inspect`);
    } catch (err) {
      toast(errorText(err), true);
    }
    if (state.details === d) renderDetails();
  }

  async function itemAction(action, item) {
    if (action === 'destroy' && !window.confirm(T('destroy_confirm', { name: item.name }))) return;
    const d = state.details;
    const done = await run(() => api('POST', `/api/companions/owner/${state.owner}/bots/${d.bot.guid}/item`,
      { action, item: action === 'unequip' ? item.slot : item.item }), T('item_done'));
    if (done) await loadDetails();
  }

  async function setPref(spell, kind) {
    const d = state.details;
    const done = await run(() => api('POST', `/api/companions/owner/${state.owner}/bots/${d.bot.guid}/pref`, { kind, spell }), T('pref_saved'));
    if (done) await loadDetails();
  }

  function itemName(item) {
    return h('span', { class: 'cp-item q' + item.quality }, item.name, item.count > 1 ? ` ×${item.count}` : null);
  }

  function detailsBody(tab, data) {
    if (!data) return h('p', { class: 'muted' }, T('loading'));
    const online = data.online;
    if (tab === 'gear') {
      if (!data.gear.length) return h('p', { class: 'muted' }, T('no_items'));
      return h('table', { class: 'cp-table' }, h('tbody', null, data.gear.sort((a, b) => a.slot - b.slot).map((item) =>
        h('tr', null, h('td', { class: 'muted' }, T('slot_' + item.slot)), h('td', null, itemName(item)), h('td', { class: 'muted' }, `ilvl ${item.ilvl}`),
          h('td', null, online ? h('button', { type: 'button', class: 'cp-btn small', onclick: () => itemAction('unequip', item) }, T('unequip')) : null)))));
    }
    if (tab === 'bags') {
      if (!data.bags.length) return h('p', { class: 'muted' }, T('no_items'));
      return h('table', { class: 'cp-table' }, h('tbody', null, data.bags.map((item) =>
        h('tr', null, h('td', null, itemName(item), item.upgrade ? h('small', { class: 'cp-tag up' }, T('upgrade_tag')) : null,
          item.bound ? h('small', { class: 'cp-tag' }, T('bound_tag')) : null),
          h('td', { class: 'muted' }, item.ilvl ? `ilvl ${item.ilvl}` : ''),
          h('td', { class: 'cp-row' },
            online && item.equippable ? h('button', { type: 'button', class: 'cp-btn small primary', onclick: () => itemAction('equip', item) }, T('equip')) : null,
            online ? h('button', { type: 'button', class: 'cp-btn small danger', onclick: () => itemAction('destroy', item) }, T('destroy')) : null)))));
    }
    if (tab === 'stats') {
      if (!online) return h('p', { class: 'muted' }, T('details_offline'));
      const rows = [[T('health'), `${data.health} / ${data.max_health}`], [T('power'), `${data.power} / ${data.max_power}`], [T('money'), money(data.money)],
        ...STAT_ORDER.filter((k) => k in data.stats).map((k) => [T('stat_' + k), PERCENT_STATS.has(k) ? `${data.stats[k].toFixed(2)}%` : String(Math.round(data.stats[k]))])];
      return h('dl', { class: 'cp-stats' }, rows.map(([k, v]) => [h('dt', null, k), h('dd', null, v)]));
    }
    if (tab === 'talents') {
      if (!online) return h('p', { class: 'muted' }, T('details_offline'));
      if (!data.talents.length) return h('p', { class: 'muted' }, T('no_talents'));
      const tabs = [...new Set(data.talents.map((t) => t.tab))].sort();
      return h('div', { class: 'cp-talent-tabs' }, tabs.map((t) => {
        const list = data.talents.filter((x) => x.tab === t);
        const points = list.reduce((n, x) => n + x.rank, 0);
        return h('section', null, h('h4', null, `${specName(data.class, t)} · ${points}`),
          h('ul', null, list.map((x) => h('li', null, title(x.name), h('span', { class: 'muted' }, ` ${x.rank}/${x.max}`)))));
      }));
    }
    // abilities: what it was told about each one
    if (!online) return h('p', { class: 'muted' }, T('details_offline'));
    const prefs = Object.fromEntries((data.prefs || []).map((p) => [p.spell, p]));
    return h('div', null,
      h('p', { class: 'muted small' }, T('abilities_hint')),
      h('table', { class: 'cp-table' }, h('tbody', null, data.spells.map((spell) => {
        const pref = prefs[spell];
        const current = pref ? (pref.kind === 'avoid' ? 'avoid' : 'prefer') : 'allow';
        return h('tr', null, h('td', null, title(spell), pref && pref.reason ? h('div', { class: 'muted small' }, pref.reason) : null,
          pref && pref.below ? h('div', { class: 'muted small' }, T('pref_below', { pct: pref.below })) : null),
          h('td', null, select([['allow', T('pref_normal')], ['prefer', T('pref_more')], ['avoid', T('pref_never')]], current, (v) => setPref(spell, v))));
      }))),
      (data.prefs || []).length ? h('button', { type: 'button', class: 'cp-btn', onclick: () => setPref('*', 'allow') }, T('prefs_reset')) : null);
  }

  function renderDetails() {
    const d = state.details;
    if (!d) return;
    const data = d.data;
    const tabs = ['gear', 'bags', 'stats', 'talents', 'abilities'];
    d.box.replaceChildren(
      h('header', { class: 'cp-modal-head' },
        h('div', null, h('h3', null, d.bot.name),
          h('div', { class: 'muted' }, `${data && data.level ? data.level : d.bot.level} ${raceName(d.bot.race)} ${className(d.bot.class)}`)),
        h('button', { type: 'button', class: 'cp-btn', onclick: closeDetails }, T('close'))),
      data && !data.online ? h('p', { class: 'muted small' }, T('details_offline')) : null,
      h('div', { class: 'cp-tabs', role: 'tablist' }, tabs.map((t) => h('button', { type: 'button', role: 'tab', class: 'cp-tab' + (d.tab === t ? ' on' : ''),
        onclick: () => { d.tab = state.detailsTab = t; renderDetails(); } }, T('details_' + t)))),
      h('div', { class: 'cp-modal-body' }, detailsBody(d.tab, data)));
  }

  const templateFromBot = (bot) => ({
    class: bot.class, race: bot.race, gender: bot.gender, role: bot.role, spec: bot.spec, quality: bot.quality, llm: bot.llm,
    provider: bot.provider || 0, aggression: bot.aggression, stance: bot.stance, personality: bot.personality, name: '', looks: '', random_looks: true,
  });

  // ------------------------------------------------------------------ new companion

  function defaultForm() {
    return { class: 1, race: 0, gender: 2, role: 'auto', spec: '', quality: 0, llm: true, provider: 0, aggression: 0, stance: 0,
      personality: '', name: '', surname: '', looks: {}, random_looks: true, template: 0 };
  }

  function allowedRaces(klass) {
    const races = state.view && state.view.races && state.view.races[klass];
    return races || Object.keys(state.me.races).map(Number);
  }

  async function loadLooks(race, gender) {
    const key = `${race}-${gender}`;
    if (!state.looks[key]) state.looks[key] = await api('GET', `/api/companions/looks?race=${race}&gender=${gender}`);
    return state.looks[key];
  }

  async function renderCreate() {
    if (!state.form) state.form = defaultForm();
    const f = state.form;
    const panel = $('tab-create');
    const races = allowedRaces(f.class);
    if (f.race && !races.includes(Number(f.race))) f.race = 0;

    const nameInput = h('input', { type: 'text', value: f.name, maxlength: 12, placeholder: T('name_random'), oninput: (e) => { f.name = e.target.value.trim(); } });
    const surnameInput = h('input', { type: 'text', value: f.surname || '', maxlength: 48, placeholder: T('surname_placeholder'),
      oninput: (e) => { f.surname = e.target.value; } });
    const looksBox = h('div', { class: 'cp-looks' });

    const classButtons = h('div', { class: 'cp-classes' }, Object.keys(state.me.classes).map(Number).map((klass) =>
      h('button', { type: 'button', class: 'cp-class c' + klass + (klass === Number(f.class) ? ' on' : ''),
        disabled: !(state.view && state.view.races && state.view.races[klass]) && !!(state.view && Object.keys(state.view.races || {}).length),
        onclick: () => { f.class = klass; f.spec = ''; renderCreate(); } }, className(klass))));

    panel.replaceChildren(h('div', { class: 'card cp-create' },
      h('div', { class: 'label' }, f.template ? T('editing_template', { name: f.templateName || '' }) : T('new_companion')),
      h('h4', null, T('class')), classButtons,
      h('div', { class: 'cp-grid' },
        h('label', null, T('race'), select([['0', T('random')], ...races.map((r) => [String(r), raceName(r)])], String(f.race), (v) => { f.race = Number(v); renderLooks(); })),
        h('label', null, T('gender'), select([['2', T('random')], ['0', T('male')], ['1', T('female')]], String(f.gender), (v) => { f.gender = Number(v); renderLooks(); })),
        h('label', null, T('name'), h('div', { class: 'cp-inline' }, nameInput,
          h('button', { type: 'button', class: 'cp-btn', title: T('random_name'), onclick: async () => {
            const r = await run(() => api('GET', `/api/companions/randomname?race=${f.race || 0}&gender=${f.gender}`));
            if (r && r.name) { f.name = r.name; nameInput.value = r.name; f.surname = r.surname || ''; surnameInput.value = f.surname; }
          } }, '🎲'))),
        h('label', null, T('surname'), surnameInput),
        h('label', null, T('role'), select(roleOptions(), f.role, (v) => { f.role = v; })),
        h('label', null, T('spec'), select(specOptions(f.class), f.spec, (v) => { f.spec = v; })),
        h('label', null, T('gear_quality'), select(qualityOptions(), String(f.quality), (v) => { f.quality = Number(v); })),
        h('label', null, T('aggression'), select(aggressionOptions(), String(f.aggression), (v) => { f.aggression = Number(v); })),
        h('label', null, T('stance'), select([['0', T('stance_0')], ['1', T('stance_1')]], String(f.stance), (v) => { f.stance = Number(v); })),
        h('div', { class: 'cp-check' }, toggle(T('brain'), f.llm, (on) => { f.llm = on; })),
        hasProviders() ? h('label', null, T('provider'), select(providerOptions(), String(f.provider || 0), (v) => { f.provider = Number(v); })) : null),
      h('label', { class: 'cp-wide' }, T('personality'), h('textarea', { rows: 2, maxlength: 250, placeholder: T('personality_placeholder'), oninput: (e) => { f.personality = e.target.value; } }, f.personality)),
      h('h4', null, T('appearance')),
      h('div', { class: 'cp-row' }, toggle(T('random_looks'), f.random_looks, (on) => { f.random_looks = on; renderLooks(); })),
      looksBox,
      h('p', { class: 'muted small' }, T('create_hint')),
      h('div', { class: 'cp-row' },
        h('button', { type: 'button', class: 'cp-btn primary', onclick: createBot }, T('create')),
        h('button', { type: 'button', class: 'cp-btn', onclick: () => saveBotTemplate(formTemplate(), f.template, f.templateName) }, f.template ? T('update_template') : T('save_template')),
        h('button', { type: 'button', class: 'cp-btn', onclick: () => { state.form = defaultForm(); renderCreate(); } }, T('reset')))));

    async function renderLooks() {
      looksBox.replaceChildren();
      if (f.random_looks) return;
      if (!f.race || f.gender > 1) {
        looksBox.append(h('p', { class: 'muted small' }, T('looks_need_race')));
        return;
      }
      const data = await run(() => loadLooks(f.race, f.gender));
      if (!data) return;
      looksBox.replaceChildren(
        h('div', { class: 'cp-row' }, h('button', { type: 'button', class: 'cp-btn', onclick: () => {
          for (const option of data.options) f.looks[option.id] = option.choices[Math.floor(Math.random() * option.choices.length)].id;
          renderLooks();
        } }, T('randomize'))),
        h('div', { class: 'cp-grid' }, data.options.map((option) => h('label', null, option.name || `#${option.id}`,
          select([['0', T('random')], ...option.choices.map((c, i) => [String(c.id), c.name && !/^\d+$/.test(c.name) ? c.name : T('choice_n', { n: i + 1 })])],
            String(f.looks[option.id] || 0), (v) => { f.looks[option.id] = Number(v); })))));
    }
    renderLooks();
  }

  function formTemplate() {
    const f = state.form;
    let looks = '';
    if (!f.random_looks) {
      // options left on "Random" get a random choice here: the server needs a complete set to keep the others
      looks = { ...f.looks };
      const data = state.looks[`${f.race}-${f.gender}`];
      if (data) for (const option of data.options)
        if (!looks[option.id]) looks[option.id] = option.choices[Math.floor(Math.random() * option.choices.length)].id;
    }
    return { class: f.class, race: f.race, gender: f.gender, role: f.role, spec: f.spec, quality: f.quality, llm: f.llm, provider: f.provider || 0,
      aggression: f.aggression, stance: f.stance, personality: f.personality, name: f.name, surname: (f.surname || '').trim(), looks, random_looks: f.random_looks };
  }

  async function createBot() {
    const result = await run(() => api('POST', `/api/companions/owner/${state.owner}/bots`, formTemplate()),
      (r) => T(ownerOnline() ? 'created_joining' : 'created_offline', { name: r.name }));
    if (result) {
      state.form = defaultForm();
      setTimeout(() => refresh(true), 3000);
      renderCreate();
    }
  }

  // ------------------------------------------------------------------ templates

  async function loadTemplates() {
    const data = await run(() => api('GET', '/api/companions/templates'));
    if (data) state.templates = data;
  }

  async function saveBotTemplate(data, id, currentName) {
    const name = prompt(T('template_name'), currentName || T('class_' + data.class));
    if (!name) return;
    const saved = await run(() => api('POST', '/api/companions/templates', { kind: 'bot', name, data, id: id || 0 }), T('template_saved', { name }));
    if (saved) {
      await loadTemplates();
      if (state.form && id) state.form.templateName = name;
      if (state.tab === 'templates') renderTemplates();
    }
  }

  async function savePartyFromCurrent() {
    // a template per current companion (re-using matching ones), then a party template of them
    const live = state.view.bots.filter((b) => b.live);
    const bots = live.length ? live : state.view.bots;
    const name = prompt(T('party_template_name'), T('party_of', { name: state.view.owner.name }));
    if (!name) return;
    const members = [];
    for (const bot of bots) {
      let id = bot.template;
      if (!id || !state.templates.bots.some((t) => t.id === id)) {
        const saved = await run(() => api('POST', '/api/companions/templates', { kind: 'bot', name: `${name}: ${bot.name}`, data: templateFromBot(bot) }));
        if (!saved) return;
        id = saved.id;
      }
      members.push(id);
    }
    const saved = await run(() => api('POST', '/api/companions/templates', { kind: 'party', name, data: { members, group: {}, replace: true } }), T('template_saved', { name }));
    if (saved) await loadTemplates();
  }

  function templateSummary(data) {
    const parts = [className(data.class), roleName(data.role)];
    if (data.spec) parts.push(data.spec.split(',').map((i) => specName(data.class, Number(i))).join(' + '));
    if (data.race) parts.push(raceName(data.race));
    if (data.name) parts.push(`“${data.name}”`);
    if (data.quality) parts.push(qualityName(data.quality));
    parts.push(data.llm ? T('brain') : T('no_brain'));
    return parts.join(' · ');
  }

  async function applyTemplate(template) {
    const result = await run(() => api('POST', `/api/companions/owner/${state.owner}/apply/${template.id}`),
      (r) => T('applied', { created: (r.created || []).length, summoned: (r.summoned || []).length }) + ((r.errors || []).length ? ` (${r.errors.join('; ')})` : ''));
    if (result) setTimeout(() => refresh(true), 3000);
  }

  function renderTemplates() {
    const panel = $('tab-templates');
    const { bots, parties } = state.templates;
    const botsById = Object.fromEntries(bots.map((t) => [t.id, t]));

    const botList = h('div', { class: 'card' }, h('div', { class: 'label' }, T('bot_templates')),
      bots.length ? h('ul', { class: 'cp-list' }, bots.map((t) => h('li', null,
        h('div', null, h('strong', null, t.name), h('div', { class: 'muted small' }, templateSummary(t.data))),
        h('div', { class: 'cp-row' },
          h('button', { type: 'button', class: 'cp-btn primary', onclick: () => applyTemplate(t) }, T('use')),
          h('button', { type: 'button', class: 'cp-btn', onclick: () => {
            state.form = { ...defaultForm(), ...t.data, looks: typeof t.data.looks === 'object' ? t.data.looks : parseLooks(t.data.looks),
              template: t.id, templateName: t.name };
            document.querySelector('.cp-tab[data-tab="create"]').click();
          } }, T('edit')),
          h('button', { type: 'button', class: 'cp-btn danger', onclick: () => deleteTemplate(t) }, T('delete')))))) :
        h('p', { class: 'muted' }, T('no_bot_templates')));

    // party builder
    const builder = { members: [], group: { llm: true, aggression: 0, stance: 0, quality: 0 }, replace: true, name: '', id: 0 };
    const memberList = h('ul', { class: 'cp-list small' });
    const pick = h('select', null, bots.map((t) => h('option', { value: t.id }, t.name)));
    const nameInput = h('input', { type: 'text', maxlength: 64, placeholder: T('party_template_name') });
    function drawMembers() {
      memberList.replaceChildren(...builder.members.map((id, i) => h('li', null, h('span', null, botsById[id] ? botsById[id].name : `#${id}`),
        h('button', { type: 'button', class: 'cp-link', onclick: () => { builder.members.splice(i, 1); drawMembers(); } }, T('remove')))));
    }
    const groupControls = h('div', { class: 'cp-grid' },
      h('div', { class: 'cp-check' }, toggle(T('brain_all'), builder.group.llm, (on) => { builder.group.llm = on; })),
      h('label', null, T('aggression'), select(aggressionOptions(), '0', (v) => { builder.group.aggression = Number(v); })),
      h('label', null, T('stance'), select([['0', T('stance_0')], ['1', T('stance_1')]], '0', (v) => { builder.group.stance = Number(v); })),
      h('label', null, T('gear_quality'), select(qualityOptions(), '0', (v) => { builder.group.quality = Number(v); })),
      hasProviders() ? h('label', null, T('provider'), select([['', '—'], ...providerOptions()], '', (v) => {
        if (v === '') delete builder.group.provider; else builder.group.provider = Number(v); })) : null,
      h('div', { class: 'cp-check' }, toggle(T('replace_party'), true, (on) => { builder.replace = on; })));

    const partyBuilder = h('div', { class: 'card' }, h('div', { class: 'label' }, T('new_party_template')),
      bots.length ? [
        h('div', { class: 'cp-row' }, pick, h('button', { type: 'button', class: 'cp-btn', onclick: () => {
          if (builder.members.length >= state.me.server.max_per_player) { toast(T('party_full', { n: state.me.server.max_per_player }), true); return; }
          builder.members.push(Number(pick.value)); drawMembers();
        } }, T('add'))),
        memberList, groupControls,
        h('div', { class: 'cp-row' }, nameInput, h('button', { type: 'button', class: 'cp-btn primary', onclick: async () => {
          if (!nameInput.value.trim() || !builder.members.length) { toast(T('party_needs'), true); return; }
          const saved = await run(() => api('POST', '/api/companions/templates', { kind: 'party', name: nameInput.value.trim(), id: builder.id,
            data: { members: builder.members, group: builder.group, replace: builder.replace } }), T('template_saved', { name: nameInput.value.trim() }));
          if (saved) { await loadTemplates(); renderTemplates(); }
        } }, T('save')))] : h('p', { class: 'muted' }, T('party_needs_bots')));

    const partyList = h('div', { class: 'card' }, h('div', { class: 'label' }, T('party_templates')),
      parties.length ? h('ul', { class: 'cp-list' }, parties.map((t) => h('li', null,
        h('div', null, h('strong', null, t.name),
          h('div', { class: 'muted small' }, t.data.members.map((id) => botsById[id] ? botsById[id].name : '?').join(', ')),
          h('div', { class: 'muted small' }, [t.data.replace ? T('replaces_party') : T('adds_to_party'),
            t.data.group.llm === undefined ? null : (t.data.group.llm ? T('brain') : T('no_brain')),
            t.data.group.aggression === undefined ? null : aggressionName(t.data.group.aggression)].filter(Boolean).join(' · '))),
        h('div', { class: 'cp-row' },
          h('button', { type: 'button', class: 'cp-btn primary', onclick: () => applyTemplate(t) }, T('apply')),
          h('button', { type: 'button', class: 'cp-btn danger', onclick: () => deleteTemplate(t) }, T('delete')))))) :
        h('p', { class: 'muted' }, T('no_party_templates')),
      h('p', { class: 'muted small' }, T('apply_hint')));

    panel.replaceChildren(partyList, partyBuilder, botList);
  }

  function parseLooks(text) {
    const looks = {};
    for (const pair of String(text || '').split(',')) {
      const [k, v] = pair.split(':').map(Number);
      if (k && v) looks[k] = v;
    }
    return looks;
  }

  async function deleteTemplate(t) {
    if (!confirm(T('confirm_delete_template', { name: t.name }))) return;
    const done = await run(() => api('DELETE', `/api/companions/templates/${t.id}`), T('template_deleted'));
    if (done) { await loadTemplates(); renderTemplates(); }
  }

  // ------------------------------------------------------------------ server & AI (whole server)

  const SETTING_GROUPS = [
    ['set_group_limits', ['MaxPerPlayer', 'MaxTotal', 'RaidSize']],
    ['set_group_movement', ['FollowMinDistance', 'FollowMaxDistance', 'TeleportDistance', 'MountLevel']],
    ['set_group_behaviour', ['Callouts', 'AnnounceInParty', 'AutoRevive', 'AutoReviveDelay', 'FreeReagents', 'FreeFood',
      'RestoreOnLogin', 'AllowAlts', 'SyncLevel', 'AutoGear', 'GearMaxQuality']],
    ['set_group_ai', ['LLM.Enable', 'LLM.DefaultOn', 'LLM.PartyChat', 'LLM.Callouts', 'LLM.Tactics', 'LLM.TacticsInterval', 'LLM.Banter', 'LLM.MaxAge']],
    ['set_group_raid', ['Raid20.Enable', 'Raid20.Health', 'Raid20.Damage']],
    ['set_group_ambient', ['Ambient.Enable', 'Ambient.Density', 'Ambient.CityDensity', 'Ambient.MaxTotal', 'Ambient.Radius', 'Ambient.GearQuality', 'Ambient.PoolSize']],
  ];
  const settingLabel = (key) => T('set_' + key.replace('.', '_'));
  const ENDPOINT_HINTS = { anthropic: 'https://api.anthropic.com', ollama: 'http://ollama:11434', openai: 'http://192.168.1.20:8000/v1' };
  const MODEL_HINTS = { anthropic: 'claude-opus-5-5', ollama: 'qwen2.5:7b', openai: 'llama-3.1-8b-instruct' };

  $('cp-server-open').addEventListener('click', () => openServer());

  async function openServer(quiet) {
    state.page = 'server';
    $('cp-owner').hidden = true;
    $('cp-empty').hidden = true;
    $('cp-serverview').hidden = false;
    $('cp-server-open').classList.add('on');
    state.owner = null;
    renderCharacters();
    try {
      state.server = await api('GET', '/api/companions/server');
    } catch (err) {
      if (!quiet) toast(errorText(err), true);
      return;
    }
    state.me.providers = state.server.providers.map((p) => ({ id: p.id, name: p.name, model: p.model, enabled: p.enabled }));
    renderServer();
  }

  async function changeSetting(key, value, reset) {
    const done = await run(() => api('POST', '/api/companions/server/settings', reset ? { reset: [key] } : { set: { [key]: value } }), T('saved'));
    if (done) openServer(true);
  }

  function settingControl(s) {
    const stored = s.stored ? h('button', { type: 'button', class: 'cp-link small', title: T('setting_reset_hint'), onclick: () => changeSetting(s.key, null, true) }, T('setting_reset')) : null;
    if (s.type === 'b')
      return h('div', { class: 'cp-setting' }, toggle(settingLabel(s.key), s.value === '1', (on) => changeSetting(s.key, on)), stored);
    const input = h('input', { type: 'number', value: s.value, min: s.min, max: s.max, step: s.type === 'f' ? '0.5' : '1',
      onchange: (e) => changeSetting(s.key, e.target.value) });
    return h('label', { class: 'cp-setting' }, h('span', null, settingLabel(s.key), ' ', h('small', { class: 'muted' }, `(${s.min}–${s.max})`)), input, stored);
  }

  function renderServer() {
    const panel = $('cp-serverview');
    const sv = state.server;
    const byKey = Object.fromEntries(sv.settings.map((s) => [s.key, s]));
    const info = sv.info || {};
    const q = sv.queue || {};

    const status = h('div', { class: 'card cp-server-status' },
      h('div', { class: 'label' }, T('server_status')),
      info.offline ? h('p', { class: 'muted' }, T('server_offline')) : h('div', { class: 'cp-stats' },
        h('div', null, h('strong', null, `${info.online} / ${info.max_total}`), h('small', { class: 'muted' }, T('stat_online'))),
        h('div', null, h('strong', null, String(info.raid_size)), h('small', { class: 'muted' }, T('stat_raid'))),
        h('div', null, h('strong', null, info.llm ? T('on') : T('off')), h('small', { class: 'muted' }, T('stat_brain'))),
        h('div', null, h('strong', null, String(q.answered || 0)), h('small', { class: 'muted' }, T('stat_answered'))),
        h('div', null, h('strong', null, String(q.waiting || 0)), h('small', { class: 'muted' }, T('stat_waiting'))),
        h('div', null, h('strong', null, String(q.expired || 0)), h('small', { class: 'muted' }, T('stat_expired')))),
      sv.tables_missing ? h('p', { class: 'muted small' }, T('err_ai_tables_missing')) : null);

    const settings = SETTING_GROUPS.map(([title, keys]) => h('div', { class: 'card' },
      h('div', { class: 'label' }, T(title)),
      title === 'set_group_ai' ? h('p', { class: 'muted small' }, T('set_ai_hint')) : null,
      title === 'set_group_limits' ? h('p', { class: 'muted small' }, T('set_limits_hint')) : null,
      h('div', { class: 'cp-grid' }, keys.filter((k) => byKey[k]).map((k) => settingControl(byKey[k])))));

    panel.replaceChildren(status, ...settings, providersCard(), workersCard());
  }

  function providerRow(p) {
    const worker = p.worker_id ? (p.worker_name || `#${p.worker_id}`) : T('this_server');
    return h('li', { class: 'cp-provider' + (p.enabled ? '' : ' off') },
      h('div', null,
        h('strong', null, p.name), ' ', h('small', { class: 'cp-tag' }, T('kind_' + p.kind)),
        h('div', { class: 'muted small' }, [p.model, p.endpoint, T('runs_on', { where: worker }),
          T('provider_slots', { n: p.max_concurrent, w: p.weight })].filter(Boolean).join(' · ')),
        h('div', { class: 'muted small' }, T('provider_stats', { ok: p.answered, failed: p.failed, ms: p.avg_ms, last: p.last_ok || '—' })),
        p.last_error ? h('div', { class: 'small cp-error-text' }, p.last_error) : null),
      h('div', { class: 'cp-row' },
        toggle(T('enabled'), p.enabled, (on) => saveProvider({ ...p, api_key: p.has_key ? '********' : '', enabled: on })),
        h('button', { type: 'button', class: 'cp-btn', disabled: !p.enabled, onclick: (e) => testProvider(p, e.target) }, T('test')),
        h('button', { type: 'button', class: 'cp-btn', onclick: () => { state.providerForm = { ...p, api_key: p.has_key ? '********' : '' }; renderServer(); } }, T('edit')),
        h('button', { type: 'button', class: 'cp-btn danger', onclick: () => deleteProvider(p) }, T('delete'))));
  }

  function providersCard() {
    const sv = state.server;
    return h('div', { class: 'card' },
      h('div', { class: 'label' }, T('providers')),
      h('p', { class: 'muted small' }, T('providers_hint')),
      sv.providers.length ? h('ul', { class: 'cp-list' }, sv.providers.map(providerRow)) : h('p', { class: 'muted' }, T('no_providers')),
      state.providerForm ? providerFormView() : h('div', { class: 'cp-row' },
        h('button', { type: 'button', class: 'cp-btn primary', onclick: () => {
          state.providerForm = { kind: 'ollama', name: '', endpoint: '', model: '', api_key: '', weight: 1, max_concurrent: 2, timeout_s: 30,
            worker_id: 0, options: '', enabled: true };
          renderServer();
        } }, T('add_provider'))));
  }

  function providerFormView() {
    const f = state.providerForm;
    const field = (key, attrs) => h('input', { ...attrs, value: f[key] === undefined ? '' : f[key], oninput: (e) => { f[key] = e.target.value; } });
    const workers = [['0', T('this_server')], ...state.server.workers.map((w) => [String(w.id), w.name])];
    return h('form', { class: 'cp-provider-form', onsubmit: async (e) => { e.preventDefault(); if (await saveProvider(f)) { state.providerForm = null; openServer(true); } } },
      h('div', { class: 'label' }, f.id ? T('edit_provider', { name: f.name }) : T('add_provider')),
      h('div', { class: 'cp-grid' },
        h('label', null, T('provider_name'), field('name', { maxlength: 64, required: true, placeholder: 'GPU box' })),
        h('label', null, T('provider_kind'), select(['ollama', 'openai', 'anthropic'].map((k) => [k, T('kind_' + k)]), f.kind, (v) => { f.kind = v; renderServer(); })),
        h('label', null, T('provider_endpoint'), field('endpoint', { maxlength: 255, placeholder: ENDPOINT_HINTS[f.kind] })),
        h('label', null, T('provider_model'), field('model', { maxlength: 128, required: true, placeholder: MODEL_HINTS[f.kind] })),
        h('label', null, T('provider_key'), field('api_key', { type: 'password', maxlength: 255, autocomplete: 'off', placeholder: f.kind === 'anthropic' ? 'ANTHROPIC_API_KEY' : T('optional') })),
        h('label', null, T('provider_worker'), select(workers, String(f.worker_id || 0), (v) => { f.worker_id = Number(v); })),
        h('label', null, T('provider_weight'), field('weight', { type: 'number', min: 1, max: 100 })),
        h('label', null, T('provider_concurrent'), field('max_concurrent', { type: 'number', min: 1, max: 64 })),
        h('label', null, T('provider_timeout'), field('timeout_s', { type: 'number', min: 5, max: 300 }))),
      h('label', { class: 'cp-wide' }, T('provider_options'), h('textarea', { rows: 2, maxlength: 2000, placeholder: f.kind === 'anthropic' ? '{"effort": "low"}' : '{"temperature": 0.7, "num_ctx": 8192}',
        oninput: (e) => { f.options = e.target.value; } }, f.options || '')),
      h('p', { class: 'muted small' }, T('provider_hint_' + f.kind)),
      h('div', { class: 'cp-row' },
        h('button', { type: 'submit', class: 'cp-btn primary' }, T('save')),
        h('button', { type: 'button', class: 'cp-btn', onclick: () => { state.providerForm = null; renderServer(); } }, T('cancel'))));
  }

  async function saveProvider(p) {
    const body = { ...p, weight: Number(p.weight), max_concurrent: Number(p.max_concurrent), timeout_s: Number(p.timeout_s) };
    const done = await run(() => api('POST', '/api/companions/server/providers', body), T('provider_saved', { name: p.name }));
    if (done && !state.providerForm) openServer(true);
    return done;
  }

  async function deleteProvider(p) {
    if (!confirm(T('confirm_delete_provider', { name: p.name }))) return;
    if (await run(() => api('DELETE', `/api/companions/server/providers/${p.id}`), T('provider_deleted'))) openServer(true);
  }

  async function testProvider(p, button) {
    button.disabled = true;
    button.textContent = T('testing');
    try {
      const r = await api('POST', `/api/companions/server/providers/${p.id}/test`);
      toast(T('test_ok', { ms: r.ms, say: r.say || '…', brain: r.brain === 'local' ? T('this_server') : r.brain }));
    } catch (err) {
      toast(errorText(err), true);
    }
    openServer(true);
  }

  function workersCard() {
    const sv = state.server;
    const name = h('input', { type: 'text', maxlength: 64, placeholder: T('worker_name') });
    const origin = window.location.origin;
    return h('div', { class: 'card' },
      h('div', { class: 'label' }, T('workers')),
      h('p', { class: 'muted small' }, T('workers_hint')),
      sv.workers.length ? h('ul', { class: 'cp-list' }, sv.workers.map((w) => h('li', null,
        h('div', null, h('span', { class: 'dot ' + (w.online ? 'ok' : '') }), ' ', h('strong', null, w.name),
          h('div', { class: 'muted small' }, w.last_seen ? T('worker_seen', { when: w.last_seen, info: w.info }) : T('worker_never'))),
        h('button', { type: 'button', class: 'cp-btn danger', onclick: async () => {
          if (!confirm(T('confirm_delete_worker', { name: w.name }))) return;
          if (await run(() => api('DELETE', `/api/companions/server/workers/${w.id}`), T('worker_deleted'))) openServer(true);
        } }, T('delete'))))) : h('p', { class: 'muted' }, T('no_workers')),
      state.newToken ? h('div', { class: 'cp-token' },
        h('p', null, T('worker_token_once')),
        h('pre', null, `docker build -t forever-companions-brain docker/companions/brain\n`
          + `docker run -d --name companions-brain --restart unless-stopped --add-host=host.docker.internal:host-gateway \\\n`
          + `  -e BRAIN_URL=${origin} -e BRAIN_TOKEN=${state.newToken} forever-companions-brain`),
        h('p', { class: 'muted small' }, T('worker_after')),
        h('button', { type: 'button', class: 'cp-btn', onclick: () => { state.newToken = null; renderServer(); } }, T('done'))) : null,
      h('form', { class: 'cp-row', onsubmit: async (e) => {
        e.preventDefault();
        if (!name.value.trim()) return;
        const r = await run(() => api('POST', '/api/companions/server/workers', { name: name.value.trim() }), T('worker_created'));
        if (r) { state.newToken = r.token; openServer(true); }
      } }, name, h('button', { type: 'submit', class: 'cp-btn primary' }, T('add_worker'))));
  }

  start();
})();
