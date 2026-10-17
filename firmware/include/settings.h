// Board pins and factory defaults. WiFi credentials live in secrets.h.
// Station, products, minimum minutes, brightness and refresh interval can be
// changed at runtime on the device's web page; these are only the defaults.
#pragma once

#define FW_VERSION "1.4.1"

// ---- GeekMagic SmallTV Pro hardware -------------------------------------
#define TFT_SCLK 18
#define TFT_MOSI 23
#define TFT_DC   2
#define TFT_RST  4
#define TFT_CS   -1         // tied to GND on the PCB
#define TFT_BL   25         // backlight, active-low
#define TFT_BL_INVERTED true
#define TOUCH_PIN 32        // capacitive pad on top of the case (T9)

// ---- Defaults (editable on the web page) ------------------------------------
#define DEFAULT_STATION_ID    "900068201"   // S+U Tempelhof (Berlin)
#define DEFAULT_STATION_NAME  "Tempelhof"
#define DEFAULT_PRODUCTS      3             // 1 = S-Bahn, 2 = U-Bahn, 4 = tram, 8 = bus, 64 = regional
#define DEFAULT_MIN_MINUTES   1             // hide departures leaving sooner than this
#define DEFAULT_BRIGHTNESS    80            // percent
#define DEFAULT_REFRESH_SEC   15            // fetch interval

// ---- Fixed behaviour ---------------------------------------------------------
#define LOOKAHEAD_MIN  60
#define STALE_SECONDS  90           // red dot when last good fetch is older
#define EXPIRE_SECONDS 900          // drop data entirely when older than this

// ---- Touch button / views ----------------------------------------------------
#define TOUCH_RATIO     0.88f       // touched below 88% of the baseline (measured: rest ~104, tap 73-85, hover 94-99)
#define LONG_PRESS_MS   800         // long press = back to the first view
#define INFO_TIMEOUT_MS 60000UL     // info view returns to the departures after 1 min
#define BOOT_INFO_MS    8000UL      // info view shown after start-up

// ---- Data source -----------------------------------------------------------
// transport.rest: community API for BVG/VBB timetables, no key needed. Tried in order.
#define REST_PRIMARY  "https://v6.bvg.transport.rest"
#define REST_FALLBACK "https://v6.vbb.transport.rest"
// Set to e.g. "http://192.168.1.20:5000/api/compact" to use the Python dev server instead
// (station / products / minimum minutes then come from the server's config.py).
#define SERVER_URL ""

// ---- Misc ------------------------------------------------------------------
#define HOSTNAME "abfahrten"        // http://abfahrten.local
#define SETUP_AP_NAME  "Abfahrtstafel-Setup"  // own WiFi when the configured one is unreachable
#define SETUP_RETRY_MS (10UL * 60 * 1000)    // leave setup mode and retry after 10 min if unused
#define TZ_INFO  "CET-1CEST,M3.5.0,M10.5.0/3"
