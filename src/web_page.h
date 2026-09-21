// The setup website, served from flash. One page; every value from the device
// or a WiFi scan is inserted as text, never as HTML.
#pragma once

static const char WEB_PAGE[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>claude-status</title>
<style>
:root{--bg:#000;--card:#0e0e11;--edge:#222228;--text:#f2f2f5;--dim:#8a8a96;--accent:#d97757;--good:#2ee59d;--warn:#ffb020;--bad:#ff4b4b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:560px;margin:0 auto;padding:20px 16px 48px}
h1{font-size:22px;margin:0}h2{font-size:17px;margin:0 0 12px}
.sub{color:var(--dim);font-size:14px;margin:2px 0 20px}
.card{background:var(--card);border:1px solid var(--edge);border-radius:14px;padding:16px;margin:0 0 14px}
label{display:block;font-size:13px;color:var(--dim);margin:12px 0 4px}
input,select{width:100%;padding:11px 12px;border-radius:10px;border:1px solid var(--edge);background:#16161b;color:var(--text);font-size:16px}
input[type=checkbox]{width:auto;margin-right:8px}
.check{display:flex;align-items:center;color:var(--text);font-size:15px;margin-top:14px}
button,.btn{display:inline-block;border:0;border-radius:10px;padding:11px 16px;font-size:15px;font-weight:600;background:var(--accent);color:#111;text-decoration:none;cursor:pointer;margin:14px 8px 0 0}
button.alt,.btn.alt{background:#23232a;color:var(--text)}button.bad{background:#3a1616;color:#ff8a8a}
button:disabled{opacity:.5}
.row{display:flex;justify-content:space-between;align-items:baseline;gap:8px}
.badge{font-size:12px;color:var(--dim);border:1px solid var(--edge);border-radius:999px;padding:1px 8px}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}.chip{font-size:13px;background:#18181e;border-radius:8px;padding:3px 8px}
.err{color:var(--bad);font-size:14px;margin-top:8px}.ok{color:var(--good);font-size:14px;margin-top:8px}
.muted{color:var(--dim);font-size:14px}.nets{display:flex;flex-direction:column;gap:6px;margin-top:10px}
.net{display:flex;justify-content:space-between;padding:10px 12px;border-radius:10px;background:#16161b;cursor:pointer;border:1px solid transparent}
.net.sel{border-color:var(--accent)}.step{font-weight:600;margin-top:14px}
.pin{letter-spacing:.4em;font-size:22px;text-align:center}.acct+.acct{border-top:1px solid var(--edge);margin-top:14px;padding-top:14px}
</style></head><body><main id="app"><p class="muted">Loading...</p></main>
<script>
const app = document.getElementById('app');
let S = null, timer = null;

function el(tag, props, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (k === 'text') e.textContent = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'class') e.className = v;
    else e.setAttribute(k, v);
  }
  for (const c of kids) if (c) e.append(c);
  return e;
}
async function api(path, body) {
  const opt = body === undefined ? {} :
    {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)};
  const r = await fetch(path, opt);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
  return j;
}
function note(parent, text, bad) {
  parent.querySelectorAll(':scope > .err, :scope > .ok').forEach(n => n.remove());
  if (text) parent.append(el('div', {class: bad ? 'err' : 'ok', text}));
}
async function load() {
  try { S = await api('/api/state'); render(); }
  catch (e) { app.replaceChildren(el('p', {class: 'err', text: 'Lost contact with the device: ' + e.message})); }
}

function render() {
  const head = [el('h1', {text: 'Claude Status'}),
    el('p', {class: 'sub', text: S.mode === 'setup' ? 'First-time setup' : S.hostname + '  -  ' + S.ssid})];
  let body;
  if (S.mode === 'setup') body = [wifiCard(true)];
  else if (!S.authed) body = [pinCard()];
  else if (S.pending) { app.replaceChildren(...head); showSignin(S.pending.alias, S.pending.url, true); return; }
  else body = [accountsCard(), addCard(), deviceCard()];
  app.replaceChildren(...head, ...body.filter(Boolean));
  clearTimeout(timer);
  if (S.mode === 'lan' && S.authed) timer = setTimeout(load, 20000);
}

function wifiCard(setup) {
  const c = el('div', {class: 'card'}, el('h2', {text: setup ? 'Connect to your WiFi' : 'Change WiFi'}));
  const nets = el('div', {class: 'nets'});
  const ssid = el('input', {value: S.ssid || '', autocapitalize: 'off', autocorrect: 'off'});
  const pass = el('input', {type: 'password', autocomplete: 'off'});
  const host = el('input', {value: S.hostname, autocapitalize: 'off'});
  const hosting = el('input', {type: 'checkbox'}); hosting.checked = setup ? true : S.hosting;
  const scan = el('button', {class: 'alt', text: 'Scan for networks', onclick: async () => {
    scan.disabled = true; scan.textContent = 'Scanning...';
    try {
      const r = await api('/api/wifi/scan');
      nets.replaceChildren(...r.networks.sort((a, b) => b.rssi - a.rssi).map(n => {
        const row = el('div', {class: 'net', onclick: () => {
          ssid.value = n.ssid; nets.querySelectorAll('.net').forEach(x => x.classList.remove('sel'));
          row.classList.add('sel'); pass.focus(); }},
          el('span', {text: n.ssid}), el('span', {class: 'muted', text: (n.open ? 'open  ' : '') + n.rssi + ' dBm'}));
        return row; }));
    } catch (e) { note(c, e.message, true); }
    scan.disabled = false; scan.textContent = 'Scan again';
  }});
  const save = el('button', {text: 'Save and restart', onclick: async () => {
    save.disabled = true;
    try {
      await api('/api/wifi', {ssid: ssid.value, password: pass.value, hostname: host.value, hosting: hosting.checked});
      c.replaceChildren(el('h2', {text: 'Restarting...'}),
        el('p', {text: 'The display is joining ' + ssid.value + '. Any accounts already on it are kept.'}),
        el('p', {class: 'muted', text: 'To add accounts, scan the new code on the display to join its ' +
          'hotspot again (its password changes on every restart), or press and hold the display for the menu.'}));
    } catch (e) { note(c, e.message, true); save.disabled = false; }
  }});
  c.append(scan, nets, el('label', {text: 'Network'}), ssid, el('label', {text: 'Password'}), pass,
    el('label', {text: 'Device name on your network'}), host,
    el('label', {class: 'check'}, hosting, 'Keep this page available on my network (address shown on the display)'), save);
  return c;
}

function pinCard() {
  const c = el('div', {class: 'card'}, el('h2', {text: 'Enter the PIN from the display'}),
    el('p', {class: 'muted', text: 'Changes need a PIN that only someone looking at the display can see.'}));
  const pin = el('input', {class: 'pin', inputmode: 'numeric', maxlength: '6', placeholder: '000000'});
  const show = el('button', {class: 'alt', text: 'Show PIN on display', onclick: async () => {
    try { await api('/api/pin/request', {}); note(c, 'Check the display.'); pin.focus(); }
    catch (e) { note(c, e.message, true); }
  }});
  const go = el('button', {text: 'Unlock', onclick: async () => {
    try { await api('/api/pin/verify', {pin: pin.value.trim()}); load(); }
    catch (e) { note(c, e.message, true); }
  }});
  pin.addEventListener('keydown', e => { if (e.key === 'Enter') go.click(); });
  c.append(show, el('label', {text: 'PIN'}), pin, go);
  return c;
}

function accountsCard() {
  const c = el('div', {class: 'card'}, el('h2', {text: 'Accounts'}));
  if (!S.accounts.length) c.append(el('p', {class: 'muted', text: 'No accounts yet. Add one below.'}));
  for (const a of S.accounts) {
    const box = el('div', {class: 'acct'},
      el('div', {class: 'row'}, el('strong', {text: a.alias}), el('span', {class: 'badge', text: a.plan})),
      el('div', {class: 'muted', text: a.email || 'email unknown'}),
      a.meters.length ? el('div', {class: 'chips'}, ...a.meters.map(m =>
        el('span', {class: 'chip', text: m.label + ' ' + m.pct + '%' + (m.shared ? ' (shared)' : '')}))) : null,
      a.error ? el('div', {class: 'err', text: a.error}) : null);
    box.append(
      el('button', {class: 'alt', text: 'Sign in again', onclick: () => signin(box, a.alias, a.models)}),
      el('button', {class: 'bad', text: 'Remove', onclick: async () => {
        if (!confirm('Remove ' + a.alias + ' from the display?')) return;
        try { await api('/api/account/remove', {alias: a.alias}); load(); } catch (e) { note(box, e.message, true); }
      }}));
    c.append(box);
  }
  return c;
}

function addCard() {
  if (S.accounts.length >= S.maxAccounts) return null;
  const c = el('div', {class: 'card'}, el('h2', {text: 'Add a Claude account'}));
  const alias = el('input', {placeholder: 'personal', autocapitalize: 'off', maxlength: '23'});
  const models = el('input', {placeholder: 'Opus, Fable', autocapitalize: 'off'});
  c.append(el('label', {text: 'Name on the display'}), alias,
    el('label', {text: 'Model meters to show (leave empty for every limit)'}), models,
    el('button', {text: 'Continue', onclick: () => signin(c, alias.value.trim(), models.value.replace(/\s*,\s*/g, ',').trim())}));
  return c;
}

async function signin(host, alias, models) {
  let r;
  try { r = await api('/api/signin/start', {alias, models}); } catch (e) { note(host, e.message, true); return; }
  showSignin(alias, r.url, false);
}

// Browsers only allow the clipboard API on HTTPS, so fall back to selecting
// the link in a box and copying that.
function copyText(input, host) {
  input.focus(); input.select(); input.setSelectionRange(0, 99999);
  let ok = false;
  try { ok = document.execCommand('copy'); } catch (e) {}
  note(host, ok ? 'Link copied.' : 'Select the link above and copy it.', !ok);
}

function showSignin(alias, url, resumed) {
  clearTimeout(timer);
  const code = el('input', {placeholder: 'Paste the code here', autocapitalize: 'off', autocorrect: 'off'});
  const link = el('input', {value: url, readonly: 'readonly'});
  const box = el('div', {class: 'card'},
    el('h2', {text: (resumed ? 'Finish signing in: ' : 'Sign in: ') + alias}),
    el('div', {class: 'step', text: '1. Approve on claude.ai'}),
    el('p', {class: 'muted', text: 'Sign in as the account you want shown as "' + alias +
      '". If this browser is signed in to a different Claude account, use a private window.'}),
    el('a', {class: 'btn', href: url, target: '_blank', rel: 'noopener', text: 'Open claude.ai'}));
  box.append(
    el('p', {class: 'muted', text: "If claude.ai won't load, your phone is sending everything through the " +
      "board's hotspot, which has no internet. Copy the link, switch back to your usual network, open it " +
      'there and approve, copy the code it shows, then rejoin the hotspot and reopen this page. It picks up here.'}),
    link, el('button', {class: 'alt', text: 'Copy link', onclick: () => copyText(link, box)}),
    el('div', {class: 'step', text: '2. Paste the code it shows'}), code);
  const finish = el('button', {text: 'Finish', onclick: async () => {
    finish.disabled = true;
    try {
      const f = await api('/api/signin/finish', {code: code.value});
      box.replaceChildren(el('h2', {text: 'Added ' + alias}),
        el('p', {class: 'ok', text: 'Signed in as ' + (f.email || 'unknown') + '. The display updates in a few seconds.'}));
      setTimeout(load, 6000);
    } catch (e) { note(box, e.message, true); finish.disabled = false; }
  }});
  box.append(finish, el('button', {class: 'alt', text: 'Cancel', onclick: () => { S.pending = null; render(); }}));
  app.replaceChildren(app.children[0], app.children[1], box);
  window.scrollTo(0, 0);
}

function deviceCard() {
  const where = S.via === 'hotspot'
    ? "Connected through the board's hotspot (" + S.hotspot + '). It turns off after 15 idle minutes.'
    : 'Address: ' + S.url + '  or  ' + S.mdns;
  const c = el('div', {class: 'card'}, el('h2', {text: 'Device'}), el('p', {class: 'muted', text: where}),
    el('p', {class: 'muted', text: 'Press and hold the display for its menu: this page, change WiFi, restart.'}));
  const num = (v, min, max) => el('input', {type: 'number', min: String(min), max: String(max), value: v});
  const refresh = num(S.refreshMinutes, 1, 240), bright = num(S.brightness, 5, 100);
  const sleep = num(S.sleepMinutes, 0, 1440), dim = num(S.dimMinutes, 0, 120);
  const secs = num(S.pageSeconds, 0, 3600);
  const hosting = el('input', {type: 'checkbox'}); hosting.checked = S.hosting;
  const save = el('button', {text: 'Save', onclick: async () => {
    try {
      await api('/api/settings', {refreshMinutes: +refresh.value, brightness: +bright.value,
        sleepMinutes: +sleep.value, dimMinutes: +dim.value, pageSeconds: +secs.value, hosting: hosting.checked});
      note(c, 'Saved.'); load();
    } catch (e) { note(c, e.message, true); }
  }});
  const wifi = el('button', {class: 'alt', text: 'Change WiFi', onclick: () => { c.after(wifiCard(false)); wifi.remove(); }});
  c.append(el('label', {text: 'Check usage every (minutes)'}), refresh,
    el('label', {text: 'Brightness (%)'}), bright,
    el('label', {text: 'Dim out after this many idle minutes (0 = never)'}), sleep,
    el('label', {text: 'Fade length before the screen turns off (minutes)'}), dim,
    el('label', {text: 'Seconds per page, with 3+ accounts (0 = only by touch)'}), secs,
    el('label', {class: 'check'}, hosting, 'Keep this page available after a restart'), save, wifi,
    el('button', {class: 'alt', text: 'Restart', onclick: async () => { await api('/api/reboot', {}); note(c, 'Restarting...'); }}),
    el('button', {class: 'bad', text: 'Erase everything', onclick: async () => {
      if (!confirm('Erase WiFi, accounts and logins, and restart into first-time setup?')) return;
      await api('/api/reset', {}); note(c, 'Erased. The display restarts into setup mode.');
    }}));
  return c;
}

load();
</script></body></html>)HTML";
