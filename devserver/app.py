"""Dev server for the departure board (GeekMagic SmallTV Pro, 240x240).

  GET /               240x240 board preview, shown 2x (?zoom=1 for real size)
  GET /loading        the board's loading screens with the animated S-Bahn
  GET /device         the board's settings page (from firmware/include/webpage.h)
  GET /wifi           the board's WiFi page (with harmless example data)
  GET /api/departures full JSON
  GET /api/compact    tiny JSON, usable by the firmware via SERVER_URL
Optional query param on the board routes: ?rows=N
Run from this folder: python app.py  ->  http://localhost:5000
"""
import logging
import re
from datetime import datetime
from pathlib import Path

from flask import Flask, Response, jsonify, render_template_string, request

import bvg_client
import config

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
app = Flask(__name__)


def _rows():
    return request.args.get("rows", default=config.MAX_ROWS, type=int)


_ABBREV = [("Platz", "Pl."), ("platz", "pl."), ("Straße", "Str."), ("straße", "str."),
           ("Bahnhof", "Bhf"), ("bahnhof", "bhf")]


def short(text, n=config.DIR_MAX_CHARS):
    """Shorten a direction so it fits the 240 px screen, e.g. 'Kurt-Schumacher-Pl.'"""
    if len(text) <= n:
        return text
    for long, abbr in _ABBREV:
        text = text.replace(long, abbr)
    return text if len(text) <= n else text[:n - 1].rstrip(" -") + "."


@app.get("/api/departures")
def api_departures():
    return jsonify(bvg_client.get_board(max_rows=_rows()))


@app.get("/api/compact")
def api_compact():
    board = bvg_client.get_board(max_rows=_rows())
    resp = jsonify({
        "t": datetime.now().strftime("%H:%M"),
        "s": board["stale"],
        "r": config.REFRESH_SECONDS,
        "d": [[d["line"], short(d["direction"]), d["minutes"], d["color"]]
              for d in board["departures"]],
    })
    resp.headers["Cache-Control"] = "no-store"
    return resp


PAGE = """<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta http-equiv="refresh" content="{{ refresh }}">
<title>Abfahrten</title>
<style>
  body { background:#e9e7e2; display:flex; flex-direction:column; align-items:center;
         justify-content:center; min-height:100vh; margin:0;
         font-family:Poppins,"Segoe UI",Arial,sans-serif; }
  .device { background:#111; padding:{{ 14 * zoom }}px; border-radius:{{ 10 * zoom }}px;
            box-shadow:0 8px 24px #0004; }
  .scale { width:{{ w * zoom }}px; height:{{ h * zoom }}px; overflow:hidden; }
  .screen { width:{{ w }}px; height:{{ h }}px; transform:scale({{ zoom }}); transform-origin:0 0;
            background:linear-gradient(160deg,#0b1b4a,#0a2a6a); color:#fff; }
  .head { height:{{ header_h }}px; display:flex; align-items:center; justify-content:space-between;
          padding:0 8px; box-sizing:border-box; font-size:13px; font-weight:600;
          border-bottom:1px solid #ffffff30; color:#cfd8ff; }
  .dot { display:inline-block; width:7px; height:7px; border-radius:50%; background:#e33;
         margin-left:5px; vertical-align:middle; }
  .row { height:{{ row_h }}px; display:flex; align-items:center; gap:6px; padding:0 8px;
         box-sizing:border-box; }
  .row + .row { border-top:1px solid #ffffff14; }
  .badge { width:38px; height:22px; line-height:22px; text-align:center; font-weight:700;
           font-size:14px; border-radius:3px; flex:none; }
  .suburban .badge { border-radius:11px; }
  .dir { flex:1; font-size:14px; font-weight:600; white-space:nowrap; overflow:hidden;
         text-overflow:ellipsis; }
  .min { width:30px; text-align:right; font-size:22px; font-weight:800; flex:none; }
  .empty { padding-top:80px; text-align:center; opacity:.7; font-size:14px; }
  .meta { margin-top:14px; color:#555; font-size:12px; }
</style></head><body>
<div class="device"><div class="scale"><div class="screen">
  <div class="head">
    <span>{{ station }}{% if stale %}<span class="dot" title="Daten veraltet"></span>{% endif %}</span>
    <span>{{ clock }}</span>
  </div>
{% for d in deps %}
  <div class="row {{ d.product }}">
    <span class="badge" style="background:{{ d.color }}">{{ d.line }}</span>
    <span class="dir">{{ short(d.direction) }}</span>
    <span class="min">{{ d.minutes }}</span>
  </div>
{% else %}
  <div class="empty">Keine Abfahrten</div>
{% endfor %}
</div></div></div>
<div class="meta">{{ w }}&times;{{ h }} px &middot; {{ zoom }}x &middot; updated {{ updated }}
{% if stale %}<span style="color:#b00"> &middot; API-Fehler, alte Daten</span>{% endif %}
&middot; <a href="/api/departures">JSON</a> &middot; <a href="/api/compact">compact</a></div>
</body></html>"""


@app.get("/")
def index():
    board = bvg_client.get_board(max_rows=_rows())
    rows = max(_rows(), 1)
    return render_template_string(
        PAGE, deps=board["departures"], updated=board["updated"], stale=board["stale"],
        refresh=config.REFRESH_SECONDS, station=config.STATION_LABEL,
        clock=datetime.now().strftime("%H:%M"), short=short,
        w=config.DISPLAY_W, h=config.DISPLAY_H, header_h=config.HEADER_H,
        row_h=(config.DISPLAY_H - config.HEADER_H) // rows,
        zoom=request.args.get("zoom", default=2, type=int),
    )


# --------------------------------------------------------------------------
# Device settings page, served from the firmware's webpage.h so it can be tested
# without the SmallTV. The /api/settings, /api/status and /api/search routes mimic
# the firmware; changes apply to this server (and therefore to the preview at /).

WEBPAGE_H = Path(__file__).resolve().parent.parent / "firmware" / "include" / "webpage.h"
_PRODUCT_BITS = {"suburban": 1, "subway": 2, "tram": 4, "bus": 8, "regional": 64}
_device = {"brightness": 80}


def _products_mask():
    return sum(bit for name, bit in _PRODUCT_BITS.items() if config.PRODUCTS.get(name))


def _settings():
    return {"stationId": config.STATION_ID, "stationName": config.STATION_LABEL,
            "products": _products_mask(), "minMinutes": config.MIN_MINUTES,
            "brightness": _device["brightness"], "refreshSec": config.REFRESH_SECONDS}


def _webpage(name):
    """The value of `static const char NAME[] PROGMEM = ...;` from webpage.h: raw string
    literals (R"X(...)X") concatenated with #define'd string macros such as LOGO_SVG."""
    src = WEBPAGE_H.read_text(encoding="utf-8")
    macros = {
        m.group(1): "".join(s.replace('\\"', '"') for s in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2)))
        for m in re.finditer(r"#define (\w+) \\\n((?:[^\n]*\\\n)*[^\n]*\n)", src)
    }
    i = src.index(f"{name}[] PROGMEM =") + len(f"{name}[] PROGMEM =")
    out = []
    while True:
        while src[i].isspace():
            i += 1
        if src[i] == ";":
            return "".join(out)
        if src.startswith('R"', i):
            paren = src.index("(", i)
            delim = src[i + 2:paren]
            end = src.index(")" + delim + '"', paren)
            out.append(src[paren + 1:end])
            i = end + len(delim) + 2
        else:
            ident = re.match(r"\w+", src[i:]).group(0)
            out.append(macros[ident])
            i += len(ident)


@app.get("/device")
def device_page():
    return _webpage("WEB_PAGE")


@app.get("/wifi")
def device_wifi_page():
    return _webpage("WIFI_PAGE")


@app.get("/style.css")
def device_css():
    return Response(_webpage("STYLE_CSS"), mimetype="text/css")


@app.get("/vehicles.js")
def device_js():
    return Response(_webpage("VEHICLES_JS"), mimetype="application/javascript")


# WiFi endpoints: harmless stand-ins, the dev server has no WiFi to configure.
@app.get("/api/wifi")
def device_wifi():
    return jsonify({"ssid": "MeinWLAN", "connected": True, "setup": False})


@app.get("/api/wifi/scan")
def device_wifi_scan():
    return jsonify([{"ssid": "MeinWLAN", "rssi": -48, "open": False},
                    {"ssid": "Nachbar-WLAN", "rssi": -77, "open": False}])


@app.post("/api/wifi")
@app.post("/api/wifi/forget")
def device_wifi_save():
    return "ok (dev server: nothing saved)"


@app.get("/api/settings")
def get_settings():
    return jsonify(_settings())


@app.post("/api/settings")
def post_settings():
    body = request.get_json(silent=True)
    if not isinstance(body, dict):
        return "invalid JSON", 400
    clamp = lambda v, lo, hi: max(lo, min(hi, int(v)))
    if "stationId" in body:
        sid = str(body["stationId"]).strip()
        if not 6 <= len(sid) <= 12:
            return "invalid station id", 400
        config.STATION_ID = sid
        config.STATION_LABEL = body.get("stationName") or sid
    if "products" in body:
        mask = clamp(body["products"], 1, 127)
        for name, bit in _PRODUCT_BITS.items():
            config.PRODUCTS[name] = bool(mask & bit)
    if "minMinutes" in body:
        config.MIN_MINUTES = clamp(body["minMinutes"], 0, 30)
    if "refreshSec" in body:
        config.REFRESH_SECONDS = clamp(body["refreshSec"], 10, 300)
    if "brightness" in body:
        _device["brightness"] = clamp(body["brightness"], 5, 100)
    bvg_client.invalidate_cache()
    return jsonify(_settings())


@app.get("/api/status")
def device_status():
    board = bvg_client.get_board(max_rows=12)
    return jsonify({
        "version": "dev",
        "station": config.STATION_ID,
        "last_ok_age_s": board["data_age_s"] if board["data_age_s"] is not None else -1,
        "error": board["error"] or "",
        "failures": 0,
        "rssi": -55,
        "departures": [{"line": d["line"], "dir": d["direction"], "color": d["color"],
                        "in_s": d["minutes"] * 60 + 30} for d in board["departures"]],
    })


@app.get("/api/search")
def device_search():
    q = request.args.get("q", "").strip()
    if len(q) < 2:
        return jsonify([])
    try:
        return jsonify(bvg_client.search_stations(q))
    except Exception as e:  # noqa: BLE001 - surface any upstream error to the page
        return str(e), 502


# --------------------------------------------------------------------------
# Preview of the board's loading screens (same geometry and timing as drawBusy() /
# renderLoading() / messageScreen() in firmware/src/main.cpp), 240x240 shown 2x.

LOADING_PAGE = """<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<title>Ladeanimation</title>
<style>
  body { margin:0; min-height:100vh; display:flex; flex-direction:column; align-items:center;
         justify-content:center; gap:18px; background:#e9e7e2; font:14px system-ui,sans-serif; color:#333; }
  .device { background:#111; padding:28px; border-radius:22px; box-shadow:0 8px 24px #0004; }
  canvas { width:480px; height:480px; max-width:80vw; max-height:80vw; image-rendering:pixelated; display:block; }
  .btns { display:flex; gap:8px; flex-wrap:wrap; justify-content:center; }
  button { padding:8px 14px; border-radius:8px; border:1px solid #bbb; background:#fff; cursor:pointer; font:inherit; }
  button.on { background:#0a2260; color:#fff; border-color:#0a2260; }
  p { margin:0; color:#777; font-size:12px; }
</style></head><body>
<div class="device"><canvas id="c" width="240" height="240"></canvas></div>
<div class="btns">
  <button data-v="loading" class="on">Abfahrten werden geladen</button>
  <button data-v="fail">Datenquelle antwortet nicht</button>
  <button data-v="wifi">WLAN wird verbunden</button>
</div>
<p>240&times;240 Pixel, 2&times; vergrößert &middot; gleiche Geometrie und Zeiten wie in der Firmware</p>
<script>
const g = document.getElementById('c').getContext('2d');
const BG = '#0A2260', HEAD = '#CFD8FF', LINE = '#2A4080', TEXT = '#FFFFFF', DIM = '#8894C0', RAIL = '#4A5A90';
const F_ROW = 'bold 15px Helvetica, Arial, sans-serif', F_HEAD = 'bold 12px Helvetica, Arial, sans-serif',
      F_SMALL = 'bold 10px Helvetica, Arial, sans-serif';
let view = 'loading';
document.querySelectorAll('button').forEach(b => b.onclick = () => {
  view = b.dataset.v;
  document.querySelectorAll('button').forEach(x => x.classList.toggle('on', x === b));
});

const rect = (x, y, w, h, c) => { g.fillStyle = c; g.fillRect(x, y, w, h); };
const rrect = (x, y, w, h, r, c) => { g.fillStyle = c; g.beginPath(); g.roundRect(x, y, w, h, r); g.fill(); };
const circ = (x, y, r, c) => { g.fillStyle = c; g.beginPath(); g.arc(x + .5, y + .5, r + .5, 0, 7); g.fill(); };
const text = (s, y, c, f) => { g.font = f; g.fillStyle = c; g.textAlign = 'center'; g.fillText(s, 120, y); };

function bar(left, right) {
  g.font = F_HEAD; g.textAlign = 'left'; g.fillStyle = HEAD; g.fillText(left, 8, 20);
  if (right) { g.textAlign = 'right'; g.fillText(right, 232, 20); }
  rect(0, 29, 240, 1, LINE);
}

function car(x, cabLeft, cabRight) {
  rrect(x, 9, 30, 14, 4, '#A6182C');            // red body
  rrect(x, 9, 30, 6, 3, '#E3A72F');             // ochre roof band
  rect(x + 1, 12, 28, 3, '#E3A72F');
  for (let i = 0; i < 4; i++) rect(x + 4 + i * 6, 13, 4, 4, '#1D2533');
  if (cabLeft) rect(x + 1, 12, 2, 6, '#1D2533');
  if (cabRight) rect(x + 27, 12, 2, 6, '#1D2533');
  circ(x + 7, 23, 2, '#222'); circ(x + 23, 23, 2, '#222');
}

function train(t) {
  const drive = 2200, halt = 400;
  t %= 2 * (drive + halt);
  const p = t < drive ? t / drive : t < drive + halt ? 1 : t < 2 * drive + halt ? 1 - (t - drive - halt) / drive : 0;
  const x = Math.floor((1 - Math.cos(p * Math.PI)) / 2 * (200 - 62));
  g.save(); g.translate(20, 170);
  rect(0, 0, 200, 28, BG);
  for (let rx = 0; rx < 200; rx += 9) rect(rx, 25, 6, 2, RAIL);
  car(x, true, false); car(x + 32, false, true);
  circ(x + 31, 5, 4, '#008D4F');                 // green S sign
  [[30,3,3,1],[29,4,1,1],[30,5,2,1],[32,6,1,1],[29,7,3,1]].forEach(([a, b, w, h]) => rect(x + a, b, w, h, '#fff'));
  g.restore();
}

function frame(ts) {
  rect(0, 0, 240, 240, BG);
  if (view === 'wifi') {
    bar('Verbinde ...');
    text('MeinWLAN', 104, TEXT, F_ROW);
    text('WLAN wird verbunden', 128, DIM, F_HEAD);
  } else {
    bar('Tempelhof', new Date().toTimeString().slice(0, 5));
    text('Abfahrten werden', 88, TEXT, F_ROW);
    text('geladen', 110, TEXT, F_ROW);
    if (view === 'fail') {
      text('Datenquelle antwortet nicht,', 138, DIM, F_SMALL);
      text('neuer Versuch läuft', 152, DIM, F_SMALL);
      text('v6.vbb.transport.rest: HTTP 500', 224, DIM, F_SMALL);
    }
  }
  train(ts);
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
</script></body></html>"""


@app.get("/loading")
def loading_preview():
    return LOADING_PAGE


if __name__ == "__main__":
    app.run(host=config.HOST, port=config.PORT, debug=True)
