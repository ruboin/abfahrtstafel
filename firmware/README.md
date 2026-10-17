# Firmware – technical reference

Installation and everyday use are described in the [main README](../README.md). This page
is for people who want to change the firmware.

## Build

```bash
pip install platformio
cp include/secrets.example.h include/secrets.h   # then set OTA_PASS
pio run                                           # -> .pio/build/smalltv_pro/firmware.bin
```

Upload to a board that already runs this firmware:

```bash
curl -u admin:<OTA_PASS> -F "update=@.pio/build/smalltv_pro/firmware.bin" http://abfahrten.local/update
```

## Files

| File | Content |
|---|---|
| `src/main.cpp` | Everything on the device: display, touch button, views, fetching, web server, WiFi setup, first-start cleanup |
| `include/settings.h` | Pins, defaults, timing, touch sensitivity, data source, host name |
| `include/webpage.h` | Settings page, WiFi page, shared stylesheet, passing-trains script, logo |
| `include/secrets.example.h` | Template for `secrets.h` (update login, optional WiFi) – `secrets.h` is git-ignored |
| `partitions_smalltv_pro.csv` | Same flash layout as the stock firmware: 2 × 2.125 MB app slots, 3.7 MB filesystem |
| `platformio.ini` | `espressif32@6.9.0` (Arduino core 2.0.17), Arduino_GFX 1.4.9, U8g2, ArduinoJson 7 |

## Settings (`include/settings.h`)

| Define | Default | Meaning |
|---|---|---|
| `DEFAULT_STATION_ID` / `DEFAULT_STATION_NAME` | `900068201` / `Tempelhof` | Stop on first start (changeable on the settings page) |
| `DEFAULT_PRODUCTS` | `3` | Bitmask: 1 S-Bahn, 2 U-Bahn, 4 tram, 8 bus, 16 ferry, 32 long-distance, 64 regional |
| `DEFAULT_MIN_MINUTES` | `1` | Hide departures leaving sooner than this |
| `DEFAULT_REFRESH_SEC` | `15` | Fetch interval |
| `TOUCH_RATIO` | `0.88` | A touch counts when the reading drops below 88 % of the resting value |
| `REST_PRIMARY` / `REST_FALLBACK` | `v6.bvg.transport.rest` / `v6.vbb.transport.rest` | Data source |
| `SERVER_URL` | empty | Use the dev server's `/api/compact` instead of transport.rest |
| `HOSTNAME` | `abfahrten` | mDNS name → `http://abfahrten.local` |
| `SETUP_AP_NAME` | `Abfahrtstafel-Setup` | WiFi name of the setup hotspot |

Station IDs can be looked up at
`https://v6.bvg.transport.rest/locations?query=<name>&results=5`.

## HTTP API

| Route | Method | |
|---|---|---|
| `/` | GET | Settings page |
| `/wifi` | GET | WiFi page |
| `/update` | GET/POST | Firmware upload (basic auth `OTA_USER` / `OTA_PASS`) |
| `/api/status` | GET | Version, errors, failures in a row, touch values, departures |
| `/api/settings` | GET/POST | Settings as JSON; POST accepts any subset of the fields |
| `/api/search?q=` | GET | Station search via transport.rest |
| `/api/wifi` | GET/POST | Saved network (never the password) / save a new network and restart |
| `/api/wifi/forget` | POST | Delete the WiFi completely and restart into setup mode |
| `/api/wifi/scan` | GET | Visible networks |

## How it's put together

- **Two tasks.** `fetchTask` (core 0) loads departures from transport.rest every refresh
  interval. After a failure it retries after 3, 6, 12, 24 s and alternates between the two
  servers. The Arduino loop (core 1) handles the display, the touch button and the web
  server. Shared data is guarded by `DataLock`.
- **Rendering** keeps a cache key per row and only redraws what changed. The loading
  animation is drawn into a small off-screen `Arduino_Canvas` so it doesn't flicker.
- **Touch**: GPIO 32 is polled every 20 ms. The baseline is taken at power-up, rises quickly
  and sinks only very slowly, so a hand hovering near the button doesn't make light taps
  stop counting. Debounce is 2 samples, and there's hysteresis on release.
- **Storage**: settings live in NVS (`Preferences`, namespace `board`). Saving or deleting the
  WiFi erases and rewrites the NVS partition, because NVS only marks overwritten entries
  as deleted.
- **First start over the stock firmware**: the other OTA slot, the filesystem partition
  and NVS are erased once (`wipeStockFirmware`).
- **Safety**: three crash resets in a row (counted in RTC memory) → safe mode with only
  the setup hotspot and `/update`. Three power-ups within 5 s each → setup hotspot.
