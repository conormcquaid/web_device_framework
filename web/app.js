'use strict';

/* ── Auth ─────────────────────────────────────────────────────────────── */

let authHeader = sessionStorage.getItem('authHeader') || '';

async function apiFetch(url, options = {}) {
  const headers = Object.assign({}, options.headers || {});
  if (authHeader) headers['Authorization'] = authHeader;
  const resp = await fetch(url, { ...options, headers });

  if (resp.status === 401) {
    const user = prompt('Username:') || '';
    const pass = prompt('Password:') || '';
    authHeader = 'Basic ' + btoa(user + ':' + pass);
    sessionStorage.setItem('authHeader', authHeader);
    headers['Authorization'] = authHeader;
    return fetch(url, { ...options, headers });
  }
  return resp;
}

async function apiJSON(url, options = {}) {
  const resp = await apiFetch(url, options);
  if (!resp.ok) throw new Error(resp.status);
  return resp.json();
}

/* ── Navigation ───────────────────────────────────────────────────────── */

const burger   = document.getElementById('burger');
const drawer   = document.getElementById('nav-drawer');
const backdrop = document.getElementById('nav-backdrop');

function openDrawer() {
  drawer.classList.add('open');
  backdrop.classList.remove('hidden');
  drawer.setAttribute('aria-hidden', 'false');
}

function closeDrawer() {
  drawer.classList.remove('open');
  backdrop.classList.add('hidden');
  drawer.setAttribute('aria-hidden', 'true');
}

burger.addEventListener('click', openDrawer);
backdrop.addEventListener('click', closeDrawer);

document.querySelectorAll('[data-page]').forEach(link => {
  link.addEventListener('click', e => {
    e.preventDefault();
    closeDrawer();
    const target = link.dataset.page;
    document.querySelectorAll('main section').forEach(s => s.classList.add('hidden'));
    document.getElementById('page-' + target).classList.remove('hidden');
    if (target === 'wifi') loadWifiPage();
    if (target === 'led-config') loadLedConfig();
  });
});

/* ── Status bar (polls /api/info every 10 s) ──────────────────────────── */

const statusBar = document.getElementById('status-bar');
const navTitle  = document.getElementById('nav-title');

async function refreshInfo() {
  try {
    const info = await apiJSON('/api/info');
    navTitle.textContent = info.device_name || 'ESP32 Device';
    statusBar.textContent =
      `WiFi: ${info.wifi_state || '?'} · IP: ${info.ip || 'N/A'} · ` +
      `Heap: ${Math.round(info.free_heap / 1024)} KB · ` +
      `Uptime: ${formatUptime(info.uptime_s)}`;
  } catch (e) {
    statusBar.textContent = 'Device unreachable';
  }
}

function formatUptime(s) {
  if (!s) return '0s';
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return h ? `${h}h ${m}m` : m ? `${m}m ${sec}s` : `${sec}s`;
}

refreshInfo();
setInterval(refreshInfo, 10000);

/* ── WebSocket / event log ─────────────────────────────────────────────── */

const eventLog = document.getElementById('event-log');

function logEvent(text) {
  const p = document.createElement('p');
  p.textContent = new Date().toLocaleTimeString() + ' ' + text;
  eventLog.prepend(p);
  while (eventLog.children.length > 100) eventLog.removeChild(eventLog.lastChild);
}

function connectWS() {
  const ws = new WebSocket('ws://' + location.host + '/ws');

  ws.addEventListener('open', () => logEvent('[connected]'));
  ws.addEventListener('close', () => {
    logEvent('[disconnected — retrying in 5s]');
    setTimeout(connectWS, 5000);
  });
  ws.addEventListener('message', e => {
    let msg;
    try { msg = JSON.parse(e.data); } catch { logEvent(e.data); return; }
    logEvent(JSON.stringify(msg));
    if (msg.event === 'led_state_changed') applyLedState(msg.data);
  });
}

connectWS();

/* ── LED controls ─────────────────────────────────────────────────────── */

const ledColor      = document.getElementById('led-color');
const ledWLabel     = document.getElementById('led-w-label');
const ledW          = document.getElementById('led-w');
const ledWValue     = document.getElementById('led-w-value');
const ledBrightness = document.getElementById('led-brightness');
const ledBrValue    = document.getElementById('led-brightness-value');
const ledToggle     = document.getElementById('led-toggle');
let   ledOn         = false;
let   ledIsRGBW     = false;

function setLedProto(proto) {
  ledIsRGBW = proto === 'RGBW' || proto === 'GRBW';
  ledWLabel.classList.toggle('hidden', !ledIsRGBW);
}

function applyLedState(data) {
  if (!data) return;
  if (typeof data.on === 'boolean') ledOn = data.on;
  if (data.r !== undefined) {
    ledColor.value = '#' +
      [data.r, data.g, data.b].map(v => v.toString(16).padStart(2, '0')).join('');
  }
  if (data.w !== undefined) {
    ledW.value = data.w;
    ledWValue.textContent = data.w;
  }
  if (data.brightness !== undefined) {
    ledBrightness.value = data.brightness;
    ledBrValue.textContent = data.brightness;
  }
  ledToggle.textContent = ledOn ? 'Turn Off' : 'Turn On';
}

function currentLedPayload() {
  const hex = ledColor.value;
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  const w = ledIsRGBW ? parseInt(ledW.value, 10) : 0;
  return { r, g, b, w };
}

async function sendLedColor() {
  await apiFetch('/api/led', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(currentLedPayload()),
  });
  ledOn = true;
  ledToggle.textContent = 'Turn Off';
}

async function loadLedState() {
  try {
    const [state, cfg] = await Promise.all([
      apiJSON('/api/led'),
      apiJSON('/api/led/config'),
    ]);
    setLedProto(cfg.proto);
    applyLedState(state);
  } catch (e) { /* ignore */ }
}

ledColor.addEventListener('input', sendLedColor);

ledW.addEventListener('input', () => {
  ledWValue.textContent = ledW.value;
  sendLedColor();
});

ledBrightness.addEventListener('input', async () => {
  const brightness = parseInt(ledBrightness.value, 10);
  ledBrValue.textContent = brightness;
  await apiFetch('/api/led', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ brightness }),
  });
});

ledToggle.addEventListener('click', async () => {
  if (ledOn) {
    await apiFetch('/api/led', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ on: false }),
    });
    ledOn = false;
    ledToggle.textContent = 'Turn On';
  } else {
    await sendLedColor();
  }
});

loadLedState();

/* ── WiFi Config page ─────────────────────────────────────────────────── */

const wifiList = document.getElementById('wifi-creds-list');

async function loadWifiPage() {
  wifiList.innerHTML = '<em>Loading…</em>';
  try {
    const data = await apiJSON('/api/wifi/creds');
    const creds = data.creds || [];
    if (creds.length === 0) {
      wifiList.innerHTML = '<em>No saved networks</em>';
      return;
    }
    const table = document.createElement('table');
    table.innerHTML = '<thead><tr><th>SSID</th><th></th></tr></thead>';
    const tbody = document.createElement('tbody');
    creds.forEach(({ ssid }) => {
      const tr = document.createElement('tr');
      tr.innerHTML = `<td>${escapeHtml(ssid)}</td>
        <td><button class="outline" data-remove="${escapeHtml(ssid)}">Remove</button></td>`;
      tbody.appendChild(tr);
    });
    table.appendChild(tbody);
    wifiList.replaceChildren(table);

    wifiList.querySelectorAll('[data-remove]').forEach(btn => {
      btn.addEventListener('click', async () => {
        await apiFetch('/api/wifi/creds', {
          method: 'DELETE',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ ssid: btn.dataset.remove }),
        });
        loadWifiPage();
      });
    });
  } catch (e) {
    wifiList.innerHTML = '<em>Failed to load</em>';
  }
}

document.getElementById('wifi-add-form').addEventListener('submit', async e => {
  e.preventDefault();
  const ssid = document.getElementById('wifi-ssid').value.trim();
  const pwd  = document.getElementById('wifi-pwd').value;
  if (!ssid) return;
  await apiFetch('/api/wifi/creds', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ssid, pwd }),
  });
  e.target.reset();
  loadWifiPage();
});

document.getElementById('wifi-connect-btn').addEventListener('click', async () => {
  await apiFetch('/api/wifi/connect', { method: 'POST' });
  logEvent('[reconnect requested]');
});

/* ── LED Config page ──────────────────────────────────────────────────── */

async function loadLedConfig() {
  try {
    const cfg = await apiJSON('/api/led/config');
    document.getElementById('led-gpio').value  = cfg.gpio;
    document.getElementById('led-count').value = cfg.count;
    document.getElementById('led-proto').value = cfg.proto;
  } catch (e) { /* ignore */ }
}

document.getElementById('led-config-form').addEventListener('submit', async e => {
  e.preventDefault();
  const gpio  = parseInt(document.getElementById('led-gpio').value, 10);
  const count = parseInt(document.getElementById('led-count').value, 10);
  const proto = document.getElementById('led-proto').value;
  await apiFetch('/api/led/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ gpio, count, proto }),
  });
  setLedProto(proto);
  logEvent(`[LED config saved: gpio=${gpio} count=${count} proto=${proto}]`);
});

function escapeHtml(str) {
  return str.replace(/[&<>"']/g, c =>
    ({ '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;' }[c]));
}
