// Web pages served by the board:
//   WEB_PAGE     settings          http://<device>/
//   WIFI_PAGE    WiFi setup        http://<device>/wifi and the setup hotspot
//   STYLE_CSS    shared styles     /style.css
//   VEHICLES_JS  passing trains    /vehicles.js
// The Python dev server (app.py) serves the same strings for testing in a browser.
#pragma once

// Builder logo (mark from since-1999-stripe-echo.svg): a small easter egg at the
// bottom of the settings page.
#define LOGO_SVG \
  "<svg viewBox=\"0 0 1080 1080\" role=\"img\" aria-label=\"Logo\"><defs><g id=\"mk\">" \
  "<path d=\"M0 0H750A250 250 0 0 1 750 500H500Z\"/>" \
  "<path d=\"M0 500H500L1000 1000H500A500 500 0 0 1 0 500Z\"/></g></defs>" \
  "<use href=\"#mk\" transform=\"translate(80 80)\" fill=\"#F2A93B\"/>" \
  "<use href=\"#mk\" transform=\"translate(40 40)\" fill=\"#E4572E\"/>" \
  "<use href=\"#mk\" fill=\"#0F4C55\"/></svg>"

static const char STYLE_CSS[] PROGMEM = R"CSS(
:root { --bg:#0a2260; --card:#10306f; --field:#0a1d52; --line:#2a4080; --text:#fff; --dim:#aab4dd;
        --acc:#ffd23f; --danger:#ff6b5b; }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--text);
       font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif; padding:16px; }
main { max-width:480px; margin:0 auto; }
.top { display:flex; align-items:center; justify-content:space-between; gap:12px; margin:4px 0 16px;
       padding:0 14px; }  /* line up with the text inside the cards */
h1 { font-size:20px; margin:0 0 2px; }
.sub { color:var(--dim); font-size:13px; }
.egg { width:22px; height:22px; margin:14px auto 0; opacity:.35; transition:opacity .3s, transform .3s; }
.egg:hover, .egg:active { opacity:1; transform:rotate(-8deg) scale(1.4); }
.egg svg { width:100%; height:100%; display:block; }
section { background:var(--card); border-radius:10px; padding:14px; margin-bottom:12px; }
h2 { font-size:13px; text-transform:uppercase; letter-spacing:.06em; color:var(--dim); margin:0 0 10px; }
label { display:block; margin:10px 0 4px; }
input[type=text], input[type=number], input[type=password] { width:100%; padding:9px 10px;
       border-radius:6px; border:1px solid var(--line); background:var(--field); color:var(--text); font-size:15px; }
input[type=range] { width:100%; }
.row { display:flex; justify-content:space-between; align-items:center; gap:10px; }
.chips { display:flex; flex-wrap:wrap; gap:8px; }
.chips label { margin:0; display:flex; gap:6px; align-items:center; background:var(--field);
               border:1px solid var(--line); border-radius:16px; padding:5px 11px; cursor:pointer; }
.list button { display:flex; justify-content:space-between; gap:8px; width:100%; text-align:left; margin-top:6px;
               padding:9px 10px; border-radius:6px; border:1px solid var(--line); background:var(--field);
               color:var(--text); font-size:14px; cursor:pointer; }
.list button:hover, .list button.sel { border-color:var(--acc); }
.save { width:100%; padding:12px; border:0; border-radius:8px; background:var(--acc); color:#000;
        font-weight:700; font-size:16px; cursor:pointer; margin-top:4px; }
.btn { flex:none; padding:7px 12px; border-radius:6px; border:1px solid var(--line); background:var(--field);
       color:var(--text); font-size:14px; text-decoration:none; cursor:pointer; }
.btn.danger { border-color:var(--danger); color:var(--danger); }
.hint { color:var(--dim); font-size:13px; margin:10px 0 0; }
#msg { text-align:center; min-height:22px; margin:8px 0; color:var(--dim); }
table { width:100%; border-collapse:collapse; }
td { padding:5px 0; border-top:1px solid var(--line); }
td.m { text-align:right; font-weight:700; white-space:nowrap; }
.badge { display:inline-block; min-width:36px; text-align:center; border-radius:4px; padding:1px 4px;
         font-weight:700; font-size:13px; margin-right:6px; }
footer { text-align:center; font-size:13px; color:var(--dim); margin:16px 0 100px; }
a { color:var(--acc); }
[hidden] { display:none !important; }

/* passing traffic */
#track { position:fixed; left:0; right:0; bottom:0; height:84px; pointer-events:none; overflow:hidden; }
/* track bed under the rail, with a faint gravel texture */
#track::before { content:""; position:absolute; left:0; right:0; bottom:0; height:14px;
                 border-top:1px solid var(--line);
                 background:radial-gradient(#ffffff14 1px, transparent 1.6px) 0 0/7px 7px, var(--card); }
#track::after { content:""; position:absolute; left:0; right:0; bottom:14px; height:3px;
                background:repeating-linear-gradient(90deg,#5a6aa0 0 14px,transparent 14px 22px); opacity:.6; }
.vehicle { position:absolute; bottom:17px; left:0; will-change:transform; }
.vehicle svg { display:block; animation:bob .35s ease-in-out infinite alternate; }
.vehicle.rtl svg { transform:scaleX(-1); }
@keyframes bob { from { translate:0 0; } to { translate:0 -1px; } }
@media (prefers-reduced-motion: reduce) { #track { display:none; } }
)CSS";

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Abfahrtstafel</title>
<link rel="stylesheet" href="/style.css">
</head><body><main>
<header class="top">
  <div><h1>Abfahrtstafel</h1><div class="sub" id="status">lade...</div></div>
</header>

<section>
  <h2>Nächste Abfahrten</h2>
  <table id="deps"></table>
</section>

<section>
  <h2>Station</h2>
  <div>Aktuell: <b id="stationName"></b> <small id="stationId" style="color:var(--dim)"></small></div>
  <label for="q">Andere Station suchen</label>
  <input type="text" id="q" placeholder="z.B. Hermannplatz" autocomplete="off">
  <div id="results" class="list"></div>
</section>

<section>
  <h2>Verkehrsmittel</h2>
  <div class="chips">
    <label><input type="checkbox" value="1"> S-Bahn</label>
    <label><input type="checkbox" value="2"> U-Bahn</label>
    <label><input type="checkbox" value="4"> Tram</label>
    <label><input type="checkbox" value="8"> Bus</label>
    <label><input type="checkbox" value="64"> Regio</label>
  </div>
</section>

<section>
  <h2>Anzeige</h2>
  <label for="minMinutes">Mindestens Minuten bis Abfahrt</label>
  <input type="number" id="minMinutes" min="0" max="30">
  <label for="refresh">Aktualisieren alle (Sekunden)</label>
  <input type="number" id="refresh" min="10" max="300">
  <div class="row"><label for="brightness">Helligkeit</label><span id="bval"></span></div>
  <input type="range" id="brightness" min="5" max="100">
  <p class="hint">Taste oben am Gerät: tippen = nächste Ansicht (Alle · je Verkehrsmittel · Info),
     lang drücken = zurück zur Übersicht.</p>
</section>

<button class="save" id="save">Speichern</button>
<div id="msg"></div>

<section>
  <h2>WLAN</h2>
  <div class="row"><span>Verbunden mit <b id="wifiName">…</b></span><a class="btn" href="/wifi">Ändern / Löschen</a></div>
</section>

<footer><a href="/update">Firmware-Update</a> &middot; <a href="/api/status">Status (JSON)</a>
  <div class="egg">)HTML" LOGO_SVG R"HTML(</div>
</footer>
</main>
<script>
const $ = s => document.querySelector(s);
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const boxes = [...document.querySelectorAll('.chips input')];
let station = {id:'', name:''};

function setProducts(mask) { boxes.forEach(b => b.checked = (mask & +b.value) > 0); }
function getProducts() { return boxes.reduce((m, b) => b.checked ? m | +b.value : m, 0); }
function showStation() { $('#stationName').textContent = station.name; $('#stationId').textContent = station.id; }

async function load() {
  const [s, w] = await Promise.all([fetch('/api/settings').then(r => r.json()),
                                    fetch('/api/wifi').then(r => r.json())]);
  $('#wifiName').textContent = w.ssid || '–';
  station = {id: s.stationId, name: s.stationName};
  showStation();
  setProducts(s.products);
  $('#minMinutes').value = s.minMinutes;
  $('#refresh').value = s.refreshSec;
  $('#brightness').value = s.brightness;
  $('#bval').textContent = s.brightness + ' %';
}

async function status() {
  try {
    const s = await (await fetch('/api/status')).json();
    const age = s.last_ok_age_s;
    $('#status').textContent = (age < 0 ? 'noch keine Daten' : 'aktualisiert vor ' + age + ' s')
      + ' · WLAN ' + s.rssi + ' dBm' + (s.error ? ' · Fehler: ' + s.error : '');
    $('#deps').innerHTML = s.departures.filter(d => d.in_s >= 0).slice(0, 8).map(d =>
      `<tr><td><span class="badge" style="background:${esc(d.color)}">${esc(d.line)}</span>${esc(d.dir)}</td>` +
      `<td class="m">${Math.floor(d.in_s / 60)} min</td></tr>`).join('') ||
      '<tr><td>Keine Abfahrten</td></tr>';
  } catch (e) { $('#status').textContent = 'Gerät nicht erreichbar'; }
}

let timer;
$('#q').addEventListener('input', e => {
  clearTimeout(timer);
  const q = e.target.value.trim();
  if (q.length < 3) { $('#results').innerHTML = ''; return; }
  timer = setTimeout(async () => {
    $('#results').innerHTML = '<small>suche...</small>';
    const r = await fetch('/api/search?q=' + encodeURIComponent(q));
    const list = r.ok ? await r.json() : [];
    $('#results').innerHTML = '';
    if (!list.length) $('#results').innerHTML = '<small>nichts gefunden</small>';
    list.forEach(r => {
      const b = document.createElement('button');
      b.textContent = r.name;
      b.onclick = () => {
        station = {id: r.id, name: r.label};
        showStation();
        const rail = r.products & 7;              // prefer S/U/Tram if the stop has them
        setProducts(rail || r.products);
        $('#results').innerHTML = ''; $('#q').value = '';
        $('#msg').textContent = 'Station gewählt – jetzt speichern.';
      };
      $('#results').appendChild(b);
    });
  }, 400);
});

$('#brightness').addEventListener('input', e => $('#bval').textContent = e.target.value + ' %');
$('#brightness').addEventListener('change', e => post({brightness: +e.target.value}, 'Helligkeit gesetzt'));

async function post(body, okMsg) {
  const r = await fetch('/api/settings', {method: 'POST', headers: {'Content-Type': 'application/json'},
                                          body: JSON.stringify(body)});
  $('#msg').textContent = r.ok ? okMsg : 'Fehler: ' + await r.text();
  if (r.ok) setTimeout(status, 3000);
}

$('#save').onclick = () => {
  if (!getProducts()) { $('#msg').textContent = 'Mindestens ein Verkehrsmittel wählen.'; return; }
  post({stationId: station.id, stationName: station.name, products: getProducts(),
        minMinutes: +$('#minMinutes').value, refreshSec: +$('#refresh').value,
        brightness: +$('#brightness').value}, 'Gespeichert ✓');
};

load(); status(); setInterval(status, 5000);
</script>
<script src="/vehicles.js"></script>
</body></html>)HTML";

static const char WIFI_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WLAN · Abfahrtstafel</title>
<link rel="stylesheet" href="/style.css">
</head><body><main>
<header class="top">
  <div><h1>WLAN</h1><div class="sub" id="status">Abfahrtstafel</div></div>
</header>

<section>
  <h2>Gespeichert</h2>
  <div class="row"><span id="saved">…</span><button class="btn" id="forget" hidden>Löschen</button></div>
  <p class="hint">Löschen entfernt Name und Passwort vollständig vom Gerät.
     Danach startet es im Einrichtungsmodus (WLAN „Abfahrtstafel-Setup“).</p>
</section>

<section>
  <h2>Neues WLAN</h2>
  <div id="nets" class="list"><small>suche Netzwerke ...</small></div>
  <label for="ssid">Name</label>
  <input type="text" id="ssid" placeholder="WLAN-Name" autocomplete="off">
  <label for="pass">Passwort</label>
  <input type="password" id="pass" placeholder="Passwort">
  <p class="hint">Nur 2,4-GHz-Netze. Ein gespeichertes WLAN wird dabei ersetzt.</p>
  <button class="save" id="save" style="margin-top:12px">Speichern &amp; neu starten</button>
</section>

<div id="msg"></div>
<footer id="back" hidden><a href="/">← Einstellungen</a></footer>
</main>
<script>
const $ = s => document.querySelector(s);
const bars = r => r > -60 ? '▂▄▆█' : r > -70 ? '▂▄▆' : r > -80 ? '▂▄' : '▂';

fetch('/api/wifi').then(r => r.json()).then(w => {
  $('#saved').textContent = w.ssid || 'Kein WLAN gespeichert';
  $('#forget').hidden = !w.ssid;
  $('#back').hidden = w.setup;
  $('#status').textContent = w.setup ? 'Einrichtungsmodus' : w.connected ? 'Verbunden mit ' + w.ssid : 'Abfahrtstafel';
}).catch(() => {});

fetch('/api/wifi/scan').then(r => r.json()).then(list => {
  $('#nets').innerHTML = list.length ? '' : '<small>keine gefunden</small>';
  list.sort((a, b) => b.rssi - a.rssi).forEach(n => {
    const b = document.createElement('button');
    b.innerHTML = '<span></span><span>' + bars(n.rssi) + (n.open ? '' : ' 🔒') + '</span>';
    b.firstChild.textContent = n.ssid;
    b.onclick = () => {
      document.querySelectorAll('#nets button').forEach(x => x.classList.remove('sel'));
      b.classList.add('sel'); $('#ssid').value = n.ssid; $('#pass').focus();
    };
    $('#nets').appendChild(b);
  });
}).catch(() => $('#nets').innerHTML = '<small>Suche fehlgeschlagen – Namen eingeben</small>');

$('#save').onclick = async () => {
  const ssid = $('#ssid').value.trim();
  if (!ssid) { $('#msg').textContent = 'Bitte ein Netzwerk wählen.'; return; }
  const r = await fetch('/api/wifi', {method: 'POST', headers: {'Content-Type': 'application/json'},
                                      body: JSON.stringify({ssid, pass: $('#pass').value})});
  $('#msg').textContent = r.ok ? 'Gespeichert – das Gerät startet neu und verbindet sich mit „' + ssid + '“.'
                               : 'Fehler: ' + await r.text();
};

// two-step delete: first tap asks, second tap (within 4 s) deletes
let armed;
$('#forget').onclick = async () => {
  const b = $('#forget');
  if (!armed) {
    b.textContent = 'Wirklich löschen?'; b.classList.add('danger');
    armed = setTimeout(() => { armed = null; b.textContent = 'Löschen'; b.classList.remove('danger'); }, 4000);
    return;
  }
  clearTimeout(armed);
  const r = await fetch('/api/wifi/forget', {method: 'POST'});
  $('#msg').textContent = r.ok ? 'WLAN gelöscht – das Gerät startet im Einrichtungsmodus neu.'
                               : 'Fehler: ' + await r.text();
  if (r.ok) { b.hidden = true; $('#saved').textContent = 'Kein WLAN gespeichert'; }
};
</script>
<script src="/vehicles.js"></script>
</body></html>)HTML";

// Every now and then a U-Bahn, S-Bahn or bus drives along the bottom of the page.
static const char VEHICLES_JS[] PROGMEM = R"JS(
(() => {
if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;
const track = document.createElement('div');
track.id = 'track';
document.body.appendChild(track);

function windows(x0, n, y, w, h, gap, fill) {
  let s = '';
  for (let i = 0; i < n; i++) s += `<rect x="${x0 + i * (w + gap)}" y="${y}" width="${w}" height="${h}" rx="2" fill="${fill}"/>`;
  return s;
}
function wheels(xs, y, r) { return xs.map(x => `<circle cx="${x}" cy="${y}" r="${r}" fill="#222"/><circle cx="${x}" cy="${y}" r="${r/2.5}" fill="#777"/>`).join(''); }

// Trains have a cab at both ends: end cars are drawn with the nose on the right,
// the left one mirrored. So trains never need flipping and their signs stay readable.
const at = (x, inner, mirror) =>
  `<g transform="translate(${x},0)${mirror ? ' scale(-1,1)' : ''}">${inner}</g>`;

// BVG U-Bahn: yellow cars. End car 112 wide, middle car 108 wide.
function ubahnCar(nose) {
  return `<path d="M2 8 h${nose ? 98 : 106} ${nose ? 'q10 0 12 14 v18 q0 4 -4 4' : 'v36'} H2 q-2 0 -2 -2 V10 q0 -2 2 -2z" fill="#F7D117"/>
    <rect x="0" y="34" width="${nose ? 112 : 108}" height="4" fill="#3a3a3a"/>
    ${windows(8, nose ? 5 : 6, 13, 11, 13, 5, '#26324f')}
    ${nose ? '<rect x="94" y="13" width="14" height="12" rx="2" fill="#26324f"/><circle cx="108" cy="31" r="1.6" fill="#fff8c0"/>' : ''}
    ${wheels([18, 34, 76, 92], 44, 4)}`;
}
const UBAHN = () => `<svg width="332" height="50" viewBox="0 0 332 50">
  ${at(112, ubahnCar(true), true)}${at(112, ubahnCar(false))}${at(220, ubahnCar(true))}
  <rect x="158" y="0" width="16" height="9" rx="1.5" fill="#115D91"/><text x="166" y="7.5" font-size="8" font-weight="700" fill="#fff" text-anchor="middle" font-family="Arial">U6</text>
</svg>`;

// Berlin S-Bahn: ochre top, red bottom. Each car 118 wide.
function sbahnCar() {
  return `<path d="M2 6 h104 q12 0 14 16 v20 q0 4 -4 4 H2 q-2 0 -2 -2 V8 q0 -2 2 -2z" fill="#A6182C"/>
    <path d="M2 6 h104 q9 0 12 10 H0 V8 q0 -2 2 -2z" fill="#E3A72F"/>
    ${windows(8, 5, 12, 12, 12, 5, '#1d2533')}
    <rect x="0" y="27" width="118" height="2" fill="#E3A72F"/>
    <rect x="98" y="12" width="16" height="12" rx="2" fill="#1d2533"/><circle cx="114" cy="38" r="1.8" fill="#fff8c0"/>
    ${wheels([20, 36, 82, 98], 47, 4)}`;
}
const SBAHN = () => `<svg width="236" height="52" viewBox="0 0 236 52">
  ${at(118, sbahnCar(), true)}${at(118, sbahnCar())}
  <circle cx="118" cy="4" r="5" fill="#008D4F"/><text x="118" y="6.8" font-size="7.5" font-weight="700" fill="#fff" text-anchor="middle" font-family="Arial">S</text>
</svg>`;

// BVG double-decker bus
const BUS = () => `<svg width="120" height="64" viewBox="0 0 120 64">
  <path d="M4 4 h100 q10 0 12 12 v38 q0 3 -3 3 H4 q-3 0 -3 -3 V7 q0 -3 3 -3z" fill="#F7D117"/>
  ${windows(8, 5, 9, 15, 14, 4, '#26324f')}
  ${windows(8, 4, 31, 15, 14, 4, '#26324f')}
  <rect x="84" y="31" width="12" height="22" rx="1.5" fill="#26324f"/>
  <path d="M102 9 h4 q6 0 7 8 v6 h-11z" fill="#26324f"/>
  <path d="M102 30 h11 v14 h-11z" fill="#26324f"/>
  <rect x="1" y="50" width="115" height="3" fill="#3a3a3a"/>
  <rect x="100" y="5" width="12" height="3" rx="1" fill="#ff9a00"/>
  ${wheels([22, 92], 57, 6)}
</svg>`;

const VEHICLES = [UBAHN, UBAHN, SBAHN, SBAHN, BUS];
function drive() {
  const v = document.createElement('div');
  const rtl = Math.random() < .5;
  const kind = VEHICLES[Math.floor(Math.random() * VEHICLES.length)];
  v.className = 'vehicle' + (rtl && kind === BUS ? ' rtl' : '');  // only the bus has a front
  v.innerHTML = kind();
  track.appendChild(v);
  const w = v.firstElementChild.getAttribute('width');
  const from = rtl ? innerWidth : -w, to = rtl ? -w : innerWidth;
  const speed = 160 + Math.random() * 120;                 // px per second
  v.animate([{transform: `translateX(${from}px)`}, {transform: `translateX(${to}px)`}],
            {duration: (innerWidth + +w) / speed * 1000, easing: 'linear'})
   .onfinish = () => v.remove();
  setTimeout(drive, 12000 + Math.random() * 25000);
}
setTimeout(drive, 2500);
})();
)JS";
