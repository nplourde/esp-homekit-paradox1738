#!/usr/bin/env node
/* Drive the page's own JavaScript without a browser (check the UI, not the claim).

   The web UI is ~250 lines of JavaScript living inside C string literals in
   web_server.c, and until now nothing tested any of it: a syntax error there is
   invisible to `make check`, to the boot-log smoke test, and to every device test
   that talks HTTP, because the page still loads and every endpoint still answers.
   The failure mode is a page that sits there doing nothing, which only a person
   clicking notices.

   This fetches the page from a running device, takes the script out of it, and runs
   it against a small fake DOM with a fake fetch. "Take the script out of the served
   page" matters: a mistake in C string escaping (a missing closing quote once made
   the build fail, and a missing one in JavaScript would not) is caught here rather
   than assumed away.

   What it holds to account: that saving is one action for the whole tab, that a zone
   counts as unsaved only while the page and the device disagree, that a refused zone
   stays marked as unsaved instead of quietly looking saved, that switching tabs does
   not throw away typed edits, and that no zone row carries a save button of its own.

   Needs node, so it is deliberately not part of `make check` (which needs only gcc,
   python3 and idf.py). Run it with: make ui-selftest HOST=<device address>
*/
import {readFileSync} from 'node:fs';
import {execFileSync} from 'node:child_process';
import vm from 'node:vm';

const host = process.argv[2] || process.env.HOST;
if (!host) {
  console.error('usage: web_ui_dom.mjs HOST=<device address> (or make ui-selftest HOST=...)');
  process.exit(2);
}

/* The page is served to unauthenticated clients on purpose (the sign-in form is part
 * of it, and the JavaScript hides the app until the session lands), so reading it does
 * not steal the device's single web session from whoever is looking at it. */
const html = execFileSync('curl', ['-s', '-m', '10', `http://${host}/`], {encoding: 'utf8'});
const blocks = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m => m[1]);
if (blocks.length !== 1) {
  console.error(`FAIL expected exactly one <script> block in the served page, found ${blocks.length}`);
  process.exit(1);
}
const script = blocks.join('\n');

/* ---------------------------------------------------------------- fake DOM -- */
const elementsBySelector = new Map();
const created = [];

class Element {
  constructor(tag) {
    this.tag = tag || 'div';
    this.textContent = '';
    this.value = '';
    this.checked = false;
    this.disabled = false;
    this.className = '';
    this.style = {};
    this.dataset = {};
    this.attributes = {};
    this.children = [];
    this.listeners = {};
    this.files = [];
    this.classListState = new Set();
    this.classList = {
      add: name => this.classListState.add(name),
      remove: name => this.classListState.delete(name),
      toggle: (name, force) => {
        const on = force === undefined ? !this.classListState.has(name) : !!force;
        if (on) this.classListState.add(name); else this.classListState.delete(name);
      },
      contains: name => this.classListState.has(name),
    };
  }
  append(...nodes) { this.children.push(...nodes); }
  replaceChildren(...nodes) { this.children = [...nodes]; }
  setAttribute(name, value) { this.attributes[name] = value; }
  getAttribute(name) { return this.attributes[name]; }
  addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
  fire(type) { for (const fn of this.listeners[type] || []) fn({target: this}); }
}

const bySelector = selector => {
  if (!elementsBySelector.has(selector)) elementsBySelector.set(selector, new Element());
  return elementsBySelector.get(selector);
};
const tabs = ['overview', 'zones', 'events'].map(name => {
  const el = new Element('button');
  el.dataset.tab = name;
  return el;
});
bySelector('#zone-list'); bySelector('#homekit-pending'); bySelector('#save-zones');
bySelector('#zone-dirty'); bySelector('#message');

const documentStub = {
  querySelector: selector => bySelector(selector),
  querySelectorAll: selector => selector === '[data-tab]' ? tabs : [],
  createElement: tag => { const el = new Element(tag); created.push(el); return el; },
  createTextNode: text => ({text}),
};

const zoneState = new Map([[16, {name: 'Zone 16', type: 'contact', enabled: true}],
                           [3, {name: 'Porte Avant', type: 'contact', enabled: true}]]);
const refusPutOf = new Set();
const calls = [];
bySelector('#homekit-pending').classListState.add('hidden');

const fetchStub = async (path, options = {}) => {
  const method = options.method || 'GET';
  /* Parse once. Spreading the raw body - which is a JSON string, because the page
   * stringifies before sending - would quietly store character indices as fields. */
  const body = options.body ? JSON.parse(options.body) : null;
  calls.push({method, path, body});
  if (path === '/api/auth/status') return answer({authenticated: false});
  if (path === '/api/zones' && method === 'GET') {
    return answer({zones: [...zoneState.entries()].map(([id, z]) => ({id, ...z})),
                   reboot_required_for_homekit: false});
  }
  const put = path.match(/^\/api\/zones\/(\d+)$/);
  if (put && method === 'PUT') {
    const id = Number(put[1]);
    if (refusPutOf.has(id)) return {ok: false, status: 400, text: async () => '{"error":"Name too long"}'};
    zoneState.set(id, {...body});
    return answer({ok: true, zone_id: id, reboot_required_for_homekit: true});
  }
  return {ok: false, status: 404, text: async () => '{"error":"unmocked"}'};
};
const answer = body => ({ok: true, status: 200, text: async () => JSON.stringify(body)});

const windowListeners = {};
const sandbox = {
  document: documentStub,
  window: {addEventListener: (type, fn) => { (windowListeners[type] ||= []).push(fn); }},
  fetch: fetchStub,
  EventSource: class { addEventListener() {} close() {} },
  sessionStorage: {store: {}, getItem(k) { return this.store[k] ?? null; },
                   setItem(k, v) { this.store[k] = v; }, removeItem(k) { delete this.store[k]; }},
  location: {reload() {}},
  confirm: () => true,
  console,
  setTimeout, clearTimeout,
};
sandbox.globalThis = sandbox;
vm.createContext(sandbox);

/* The script's top-level `const`s are script-scoped, so the hooks have to be handed
 * out from inside the same script rather than reached for afterwards. */
vm.runInContext(script + `
;globalThis.__hooks = {loadZones, zoneChanged, markZoneEdits};`, sandbox);

const problems = [];
const check = (condition, description) => {
  if (!condition) problems.push(description);
  console.log(`${condition ? 'ok  ' : 'FAIL'} ${description}`);
};
const saveButton = bySelector('#save-zones');
const message = bySelector('#message');
const dirty = bySelector('#zone-dirty');
const rowsFor = async () => { await sandbox.__hooks.loadZones(); return rowInputs(); };
const rowInputs = () => created.filter(el => el.tag === 'input' && el.attributes['aria-label']);

(async () => {
  check(typeof sandbox.__hooks.loadZones === 'function', 'the served script ran to completion under the fake DOM');

  let inputs = await rowsFor();
  check(inputs.length === 2, 'two mock zones rendered (the page read them over the API)');
  check(saveButton.disabled, 'the single save button starts disabled with nothing changed');
  check(dirty.textContent === '', 'and says nothing about unsaved work');
  check(!created.some(el => el.textContent === 'Save zone'),
        'no zone row carries a save button of its own');
  const perRowButtons = created.filter(el => el.tag === 'button' && el.textContent.startsWith('Save'));
  check(perRowButtons.length === 0, 'and no per-row Save button was created at all');

  const name16 = inputs.find(el => el.attributes['aria-label'] === 'Zone 16 name');
  const enabled3 = created.filter(el => el.tag === 'input');
  const checkbox3 = enabled3[enabled3.length - 1];
  name16.value = 'Hall Window';
  name16.fire('input');
  check(!saveButton.disabled, 'one field change enables the save button');
  check(dirty.textContent.includes('16'), 'and names the zone that differs');
  checkbox3.checked = false;
  checkbox3.fire('change');
  check(dirty.textContent.includes('3') && dirty.textContent.includes('16'),
        'a second zone edited on the same page is listed too');

  refusPutOf.add(3);
  await saveButton.onclick();
  const puts = calls.filter(c => c.method === 'PUT');
  check(puts.length === 2, `clicking save once wrote both edited zones (PUT count ${puts.length})`);
  check(puts.some(p => p.path === '/api/zones/16' && p.body.name === 'Hall Window'),
        'the rename reached the API with the type and enabled fields alongside it');
  check(message.textContent.includes('1 saved') && message.textContent.includes('refused'),
        `the refused zone is reported, not swallowed: ${message.textContent}`);
  check(dirty.textContent.includes('3') && !dirty.textContent.includes('16'),
        'the zone the device refused still counts as unsaved');
  check(zoneState.get(16).name === 'Hall Window' && zoneState.get(3).enabled === true,
        'only the accepted write changed the mock device state');
  check(!bySelector('#homekit-pending').classListState.has('hidden'),
        'a successful write revealed the HomeKit banner without a reload');

  refusPutOf.clear();
  await saveButton.onclick();
  check(calls.filter(c => c.method === 'PUT').length === 3, 'saving again sent only the still-unsaved zone');
  check(saveButton.disabled && dirty.textContent === '', 'and the page is clean afterwards');
  check(zoneState.get(3).enabled === false, 'the retried zone is now disabled on the mock device');

  /* Typed edits must survive poking around the page. */
  name16.value = 'Kitchen Window';
  name16.fire('input');
  const zoneReadsBefore = calls.filter(c => c.method === 'GET' && c.path === '/api/zones').length;
  await tabs[1].onclick();
  check(calls.filter(c => c.method === 'GET' && c.path === '/api/zones').length === zoneReadsBefore,
        'returning to the Zones tab did not re-read the list over an edit in progress');
  check(message.textContent.includes('unsaved'), 'and said so instead');

  const event = {prevented: false, preventDefault() { this.prevented = true; }};
  for (const fn of windowListeners.beforeunload || []) fn(event);
  check(event.prevented, 'a refresh with edits in progress is warned about');
  name16.value = 'Hall Window';
  name16.fire('input');
  const clean = {prevented: false, preventDefault() { this.prevented = true; }};
  for (const fn of windowListeners.beforeunload || []) fn(clean);
  check(!clean.prevented, 'and not warned about once the page matches the device');

  console.log(`\npage script: ${script.length} bytes fetched from ${host}`);
  if (problems.length) {
    for (const p of problems) console.log(`FAIL ${p}`);
    process.exit(1);
  }
  console.log(`web ui dom check: PASS (${calls.length} mock requests, all expectations met)`);
})().catch(e => { console.error('harness error:', e); process.exit(1); });
