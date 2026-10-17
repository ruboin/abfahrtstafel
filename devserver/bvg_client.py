"""Fetch and normalise departures for one stop.

Primary source: VBB's HAFAS "mgate" backend (the one behind fahrinfo.vbb.de / VBB app).
Fallback:       the community REST wrapper v6.bvg.transport.rest (often overloaded).
"""
import logging
import re
import time
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo

import requests

import config

log = logging.getLogger("bvg")
BERLIN = ZoneInfo("Europe/Berlin")

_session = requests.Session()
_session.headers["User-Agent"] = "abfahrten-board/0.1"

_cache = {"ts": 0.0, "data": None, "source": None}

# HAFAS product bitmask used by VBB
_PRODUCT_BITS = {"suburban": 1, "subway": 2, "tram": 4, "bus": 8,
                 "ferry": 16, "express": 32, "regional": 64}
_CLS_TO_PRODUCT = {v: k for k, v in _PRODUCT_BITS.items()}


class ApiError(Exception):
    pass


# --------------------------------------------------------------------------- HAFAS

def _hafas_request(method, req):
    body = {
        "lang": "de",
        "svcReqL": [{"meth": method, "req": req}],
        "client": {"type": "IPA", "id": "VBB", "name": "vbbPROD", "v": "4010300"},
        "ext": "VBB.1",
        "ver": "1.33",
        "auth": {"type": "AID", "aid": "hafas-vbb-apps"},
    }
    r = _session.post(config.HAFAS_URL, json=body, timeout=config.HTTP_TIMEOUT)
    r.raise_for_status()
    data = r.json()
    if data.get("err", "OK") != "OK":
        raise ApiError(f"HAFAS error {data.get('err')}: {data.get('errTxt')}")
    svc = data["svcResL"][0]
    if svc.get("err", "OK") != "OK":
        raise ApiError(f"HAFAS {method} error {svc.get('err')}: {svc.get('errTxt')}")
    return svc["res"]


def _hafas_time(date_str, time_str):
    """'20261002' + '124300' (or '01003000' = +1 day) -> aware datetime."""
    if not time_str:
        return None
    day_offset = 0
    if len(time_str) > 6:
        day_offset, time_str = int(time_str[:-6]), time_str[-6:]
    base = datetime.strptime(date_str + time_str, "%Y%m%d%H%M%S")
    return (base + timedelta(days=day_offset)).replace(tzinfo=BERLIN)


def _fetch_hafas():
    mask = sum(bit for name, bit in _PRODUCT_BITS.items() if config.PRODUCTS.get(name))
    res = _hafas_request("StationBoard", {
        "type": "DEP",
        "stbLoc": {"type": "S", "lid": f"A=1@L={config.STATION_ID}@"},
        "dur": config.DURATION_MIN,
        "maxJny": 40,
        "jnyFltrL": [{"type": "PROD", "mode": "INC", "value": mask}],
    })
    prods = res.get("common", {}).get("prodL", [])
    out = []
    for j in res.get("jnyL", []):
        stop = j.get("stbStop", {})
        prod = prods[j["prodX"]] if j.get("prodX") is not None and j["prodX"] < len(prods) else {}
        planned = _hafas_time(j["date"], stop.get("dTimeS"))
        realtime = _hafas_time(j["date"], stop.get("dTimeR"))
        platform = (stop.get("dPltfR") or stop.get("dPltfS") or {}).get("txt")
        out.append({
            "line": (prod.get("name") or "?").replace(" ", ""),
            "product": _CLS_TO_PRODUCT.get(prod.get("cls"), ""),
            "direction": j.get("dirTxt"),
            "when": realtime or planned,
            "planned": planned,
            "realtime": realtime is not None,
            "cancelled": bool(stop.get("dCncl")),
            "platform": platform,
        })
    return out


# --------------------------------------------------------------------------- REST fallback

def _fetch_rest():
    params = {"duration": config.DURATION_MIN, "results": 40, "remarks": "false"}
    params.update({k: str(v).lower() for k, v in config.PRODUCTS.items()})
    last_err = None
    for base in config.REST_BASES:
        try:
            r = _session.get(f"{base}/stops/{config.STATION_ID}/departures",
                             params=params, timeout=config.HTTP_TIMEOUT)
            r.raise_for_status()
            res = r.json()
            break
        except (requests.RequestException, ValueError) as e:
            last_err = f"{base}: {e}"
            log.warning("REST request failed: %s", last_err)
    else:
        raise ApiError(last_err)

    deps = res.get("departures", []) if isinstance(res, dict) else res
    out = []
    for d in deps:
        line = d.get("line") or {}
        when = datetime.fromisoformat(d["when"]) if d.get("when") else None
        planned = datetime.fromisoformat(d["plannedWhen"]) if d.get("plannedWhen") else None
        out.append({
            "line": line.get("name", "?").replace(" ", ""),
            "product": line.get("product", ""),
            "direction": d.get("direction"),
            "when": when or planned,
            "planned": planned,
            "realtime": when is not None,
            "cancelled": bool(d.get("cancelled")),
            "platform": d.get("platform") or d.get("plannedPlatform"),
        })
    return out


# transport.rest first, like the board. VBB redirects device clients of its internal
# endpoint to the official API, so HAFAS is only a fallback for this PC tool.
SOURCES = [("rest", _fetch_rest), ("hafas", _fetch_hafas)]


def _fetch_any():
    errors = []
    for _ in range(config.RETRIES + 1):
        for name, fn in SOURCES:
            try:
                return name, fn()
            except (ApiError, requests.RequestException, ValueError, KeyError) as e:
                errors.append(f"{name}: {e}")
                log.warning("Source %s failed: %s", name, e)
    raise ApiError(errors[-1] if errors else "unknown error")


# --------------------------------------------------------------------------- formatting

_CLEAN = [
    (re.compile(r"^Ringbahn\s+(S\s?4[12])"), "Ring"),
    (re.compile(r"^S\+U\s+"), ""),
    (re.compile(r"^[SU]\s+"), ""),
    (re.compile(r"\s*\(Berlin\)"), ""),
    (re.compile(r"\s*\[.*?\]"), ""),
    (re.compile(r"\s+Bhf\b"), ""),
]


def clean_direction(name):
    if not name:
        return "?"
    for pat, rep in _CLEAN:
        name = pat.sub(rep, name)
    return name.strip()


def _fmt(dt):
    return dt.astimezone(BERLIN).strftime("%H:%M") if dt else None


def _present(dep, now):
    when, planned = dep["when"], dep["planned"]
    minutes = int((when - now).total_seconds() // 60) if when else None
    delay = round((when - planned).total_seconds() / 60) if when and planned else 0
    return {
        "line": dep["line"],
        "product": dep["product"],
        "color": config.LINE_COLORS.get(dep["line"],
                                        config.DEFAULT_COLORS.get(dep["product"], "#555555")),
        "direction": clean_direction(dep["direction"]),
        "minutes": max(minutes, 0) if minutes is not None else None,
        "time": _fmt(when),
        "planned": _fmt(planned),
        "delay_min": delay,
        "cancelled": dep["cancelled"],
        "platform": dep["platform"],
        "realtime": dep["realtime"],
    }


def search_stations(query, limit=8):
    """Station search via transport.rest -> [{id, name, label, products}] (same as the board)."""
    params = {"query": query, "results": limit, "stops": "true", "addresses": "false",
              "poi": "false", "linesOfStops": "false", "language": "de"}
    last_err = None
    for base in config.REST_BASES:
        try:
            r = _session.get(f"{base}/locations", params=params, timeout=config.HTTP_TIMEOUT)
            r.raise_for_status()
            return [{"id": loc["id"], "name": loc["name"], "label": clean_direction(loc["name"]),
                     "products": sum(bit for name, bit in _PRODUCT_BITS.items()
                                     if (loc.get("products") or {}).get(name))}
                    for loc in r.json() if loc.get("type") in ("stop", "station")]
        except (requests.RequestException, ValueError) as e:
            last_err = f"{base}: {e}"
    raise ApiError(last_err)


def invalidate_cache():
    _cache["data"] = None


def get_board(max_rows=config.MAX_ROWS, include_cancelled=False):
    """Next departures as a dict. Cached; falls back to stale data when all sources fail."""
    now_ts = time.time()
    error = None

    if _cache["data"] is None or now_ts - _cache["ts"] > config.CACHE_SECONDS:
        try:
            _cache["source"], _cache["data"] = _fetch_any()
            _cache["ts"] = now_ts
        except ApiError as e:
            error = str(e)
            log.error("All sources failed: %s", e)

    raw = _cache["data"] or []
    age = now_ts - _cache["ts"] if _cache["data"] is not None else None
    if error and (age is None or age > config.STALE_MAX_SECONDS):
        raw = []

    now = datetime.now(timezone.utc)
    deps = [_present(d, now) for d in raw]
    deps = [d for d in deps if d["minutes"] is not None and d["minutes"] >= config.MIN_MINUTES]
    if not include_cancelled:
        deps = [d for d in deps if not d["cancelled"]]
    deps.sort(key=lambda d: d["minutes"])

    return {
        "station_id": config.STATION_ID,
        "source": _cache["source"],
        "updated": datetime.now(BERLIN).strftime("%H:%M:%S"),
        "stale": error is not None,
        "error": error,
        "data_age_s": round(age) if age is not None else None,
        "departures": deps[:max_rows],
    }
