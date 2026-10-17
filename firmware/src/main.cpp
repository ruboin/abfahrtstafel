// Departure board for the GeekMagic SmallTV Pro (ESP32, 240x240 ST7789).
//
// Shows the next departures for one stop (default: S+U Tempelhof) catch.it-style:
// line badge, destination, minutes. Data comes from transport.rest, the community API
// for BVG/VBB timetables (or from the Python dev server, see SERVER_URL). Loading runs in
// a background task, so the screen, the button and the web pages never wait for it.
//
// Touch button on top: tap = next view (all / U-Bahn / S-Bahn / info),
//                      long press = back to the first view.
//
// Web UI:  http://abfahrten.local/        settings page
//          http://abfahrten.local/wifi    change or delete the WiFi
//          http://abfahrten.local/update  firmware upload (OTA)
// No WiFi at boot -> own hotspot "Abfahrtstafel-Setup" with a WiFi setup page.
// Same when the board is unplugged 3x within a few seconds after power-up.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <HTTPUpdateServer.h>
#include <Preferences.h>
#include <U8g2lib.h>  // fonts only
#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_attr.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <qrcode.h>
#include <time.h>

#include "secrets.h"
#include "settings.h"
#include "webpage.h"

// --------------------------------------------------------------------------- layout

constexpr int W = 240, H = 240;
constexpr int HEADER_H = 30;
constexpr int ROWS = 6;
constexpr int ROW_H = (H - HEADER_H) / ROWS;  // 35
constexpr int PAD = 8;
constexpr int BADGE_W = 38, BADGE_H = 22;
constexpr int MIN_W = 34;                     // space for the minutes column
constexpr int DIR_X = PAD + BADGE_W + 6;
constexpr int DIR_MAX_W = W - PAD - MIN_W - 4 - DIR_X;

constexpr uint16_t rgb(uint32_t hex) {
  return ((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F);
}
constexpr uint16_t COL_BG = rgb(0x0A2260);
constexpr uint16_t COL_HEAD = rgb(0xCFD8FF);
constexpr uint16_t COL_LINE = rgb(0x2A4080);
constexpr uint16_t COL_TEXT = rgb(0xFFFFFF);
constexpr uint16_t COL_DIM = rgb(0x8894C0);
constexpr uint16_t COL_ACC = rgb(0xFFD23F);
constexpr uint16_t COL_RED = rgb(0xEE3333);
constexpr uint16_t COL_WHITE = rgb(0xFFFFFF);
constexpr uint16_t COL_BLACK = 0;

#define FONT_SMALL u8g2_font_helvB08_tf
#define FONT_HEAD  u8g2_font_helvB10_tf
#define FONT_ROW   u8g2_font_helvB12_tf  // Latin-1, has umlauts and ß
#define FONT_MIN   u8g2_font_helvB24_tn

// --------------------------------------------------------------------------- products

struct Product {
  uint8_t bit;      // VBB product bit
  const char *api;  // transport.rest name
  const char *label;
  const char *badge;
  uint32_t color;
};

// Order = order of the views on the touch button.
static const Product PRODUCTS[] = {
    {2, "subway", "U-Bahn", "U", 0x115D91},  {1, "suburban", "S-Bahn", "S", 0x008D4F},
    {4, "tram", "Tram", "Tram", 0xBE1414},     {8, "bus", "Bus", "Bus", 0x95276E},
    {64, "regional", "Regio", "RE", 0xE30613}, {32, "express", "Fernverkehr", "ICE", 0x6E6E6E},
    {16, "ferry", "Fähre", "F", 0x0080BA},
};

const Product *productByBit(uint8_t bit) {
  for (auto &p : PRODUCTS)
    if (p.bit == bit) return &p;
  return nullptr;
}

uint8_t productByApiName(const char *name) {
  for (auto &p : PRODUCTS)
    if (name && strcmp(p.api, name) == 0) return p.bit;
  return 0;
}

// --------------------------------------------------------------------------- settings

struct Config {
  String stationId;
  String stationName;
  int products;     // product bitmask, see PRODUCTS
  int minMinutes;
  int brightness;   // percent
  int refreshSec;
} cfg;

Preferences prefs;

void loadConfig() {
  prefs.begin("board", true);
  cfg.stationId = prefs.getString("station", DEFAULT_STATION_ID);
  cfg.stationName = prefs.getString("name", DEFAULT_STATION_NAME);
  cfg.products = prefs.getInt("products", DEFAULT_PRODUCTS);
  cfg.minMinutes = prefs.getInt("minMin", DEFAULT_MIN_MINUTES);
  cfg.brightness = prefs.getInt("bright", DEFAULT_BRIGHTNESS);
  cfg.refreshSec = prefs.getInt("refresh", DEFAULT_REFRESH_SEC);
  prefs.end();
}

void saveConfig() {
  prefs.begin("board", false);
  prefs.putString("station", cfg.stationId);
  prefs.putString("name", cfg.stationName);
  prefs.putInt("products", cfg.products);
  prefs.putInt("minMin", cfg.minMinutes);
  prefs.putInt("bright", cfg.brightness);
  prefs.putInt("refresh", cfg.refreshSec);
  prefs.end();
}

// --------------------------------------------------------------------------- shared data
// Departures are written by the fetch task and read by the UI; guard with DataLock.
// The lock also covers cfg.stationId / cfg.products / cfg.refreshSec, which the fetch
// task reads.

struct Departure {
  char line[8];
  char dir[64];
  time_t when;
  uint16_t color;
  uint8_t product;  // product bit, see PRODUCTS
};

constexpr int MAX_DEPS = 30;

SemaphoreHandle_t dataMutex;
struct DataLock {
  DataLock() { xSemaphoreTake(dataMutex, portMAX_DELAY); }
  ~DataLock() { xSemaphoreGive(dataMutex); }
};

Departure deps[MAX_DEPS];
int depCount = 0;
time_t lastOk = 0;         // time of the last successful fetch
String lastError = "";
int failures = 0;          // failed fetches in a row
uint32_t settingsGen = 0;  // bumped when station or products change
TaskHandle_t fetchTaskHandle = nullptr;

void requestFetch() {
  if (fetchTaskHandle) xTaskNotifyGive(fetchTaskHandle);
}

// --------------------------------------------------------------------------- display

// ST7789 on this board has no CS line, so it needs SPI mode 3.
class SmallTvPanel : public Arduino_ST7789 {
 public:
  explicit SmallTvPanel(Arduino_DataBus *bus)
      : Arduino_ST7789(bus, TFT_RST, 0 /*rotation*/, true /*IPS*/, W, H) {}
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    _override_datamode = SPI_MODE3;
    return Arduino_ST7789::begin(speed);
  }
};

Arduino_DataBus *bus = new Arduino_HWSPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED, &SPI);
Arduino_GFX *gfx = new SmallTvPanel(bus);

constexpr int BL_CHANNEL = 0;

void setupBacklight() {
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(TFT_BL, BL_CHANNEL);
}

void applyBrightness() {
  int level = constrain(cfg.brightness, 0, 100) * 255 / 100;
  ledcWrite(BL_CHANNEL, TFT_BL_INVERTED ? 255 - level : level);
}

int textWidth(const String &s) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s.c_str(), 0, 0, &x1, &y1, &w, &h);
  return w;
}

void drawText(const String &s, int x, int y, const uint8_t *font, uint16_t color) {
  gfx->setFont(font);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

void drawCentered(const String &s, int y, uint16_t color, const uint8_t *font = FONT_ROW,
                  int cx = W / 2) {
  gfx->setFont(font);
  drawText(s, cx - textWidth(s) / 2, y, font, color);
}

// Shorten text (in the current font) to maxW pixels: abbreviate, then cut with ".".
String fitText(String s, int maxW) {
  if (textWidth(s) <= maxW) return s;
  s.replace("Platz", "Pl.");
  s.replace("platz", "pl.");
  s.replace("Straße", "Str.");
  s.replace("straße", "str.");
  if (textWidth(s) <= maxW) return s;
  while (s.length() > 1) {
    // drop one UTF-8 character
    int i = s.length() - 1;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
    s.remove(i);
    s.trim();
    while (s.endsWith("-")) s.remove(s.length() - 1);
    if (textWidth(s + ".") <= maxW) return s + ".";
  }
  return s;
}

// Loading animation: a small S-Bahn (colours of the website's trains) shuttling on a
// rail, like at a terminus. Drawn in an off-screen canvas so it moves without flicker.
// Always at the same place; busyY < 0 = not on screen, the loop animates it while shown.
constexpr int TRAIN_X = 20, TRAIN_Y = 170, TRAIN_W = 200, TRAIN_H = 28;
constexpr int TRAIN_LEN = 62;  // two cars of 30 px + 2 px coupling
constexpr uint16_t COL_RAIL = rgb(0x4A5A90);
Arduino_Canvas *trainCanvas = nullptr;
int busyY = -1;

void drawTrainCar(Arduino_GFX *g, int x, bool cabLeft, bool cabRight) {
  g->fillRoundRect(x, 9, 30, 14, 4, rgb(0xA6182C));  // red body
  g->fillRoundRect(x, 9, 30, 6, 3, rgb(0xE3A72F));   // ochre roof band ...
  g->fillRect(x + 1, 12, 28, 3, rgb(0xE3A72F));      // ... with a straight lower edge
  for (int i = 0; i < 4; i++) g->fillRect(x + 4 + i * 6, 13, 4, 4, rgb(0x1D2533));  // windows
  if (cabLeft) g->fillRect(x + 1, 12, 2, 6, rgb(0x1D2533));
  if (cabRight) g->fillRect(x + 27, 12, 2, 6, rgb(0x1D2533));
  g->fillCircle(x + 7, 23, 2, rgb(0x222222));        // wheels on the rail
  g->fillCircle(x + 23, 23, 2, rgb(0x222222));
}

void drawBusy(bool force = false) {
  static int lastX = -1;
  if (busyY < 0 || !trainCanvas) return;
  // 2.2 s drive with gentle start/stop, 0.4 s halt at each end
  const uint32_t drive = 2200, halt = 400;
  uint32_t t = millis() % (2 * (drive + halt));
  float p = t < drive ? t / (float)drive
          : t < drive + halt ? 1.0f
          : t < 2 * drive + halt ? 1.0f - (t - drive - halt) / (float)drive
          : 0.0f;
  int x = (int)((1 - cosf(p * PI)) / 2 * (TRAIN_W - TRAIN_LEN));
  if (x == lastX && !force) return;
  lastX = x;

  Arduino_GFX *c = trainCanvas;
  c->fillScreen(COL_BG);
  for (int rx = 0; rx < TRAIN_W; rx += 9) c->fillRect(rx, 25, 6, 2, COL_RAIL);
  drawTrainCar(c, x, true, false);
  drawTrainCar(c, x + 32, false, true);
  c->fillCircle(x + 31, 5, 4, rgb(0x008D4F));  // green S sign on the roof
  c->fillRect(x + 30, 3, 3, 1, COL_WHITE);     // tiny "S" stroke
  c->fillRect(x + 29, 4, 1, 1, COL_WHITE);
  c->fillRect(x + 30, 5, 2, 1, COL_WHITE);
  c->fillRect(x + 32, 6, 1, 1, COL_WHITE);
  c->fillRect(x + 29, 7, 3, 1, COL_WHITE);
  trainCanvas->flush();
}

void showBusy() {
  busyY = TRAIN_Y;
  drawBusy(true);
}

// Header bar used by all screens: title left, small text right, divider line.
void drawBar(const String &left, const String &right = "") {
  gfx->fillRect(0, 0, W, HEADER_H, COL_BG);
  gfx->setFont(FONT_HEAD);
  int rw = right.length() ? textWidth(right) : 0;
  drawText(fitText(left, W - 2 * PAD - rw - 10), PAD, 20, FONT_HEAD, COL_HEAD);
  if (rw) drawText(right, W - PAD - rw, 20, FONT_HEAD, COL_DIM);
  gfx->drawFastHLine(0, HEADER_H - 1, W, COL_LINE);
}

// Full-screen message in the board's style, optionally with the loading train.
void messageScreen(const String &title, const String &l1, const String &l2 = "", bool busy = false) {
  busyY = -1;
  gfx->fillScreen(COL_BG);
  drawBar(title);
  drawCentered(l1, 104, COL_TEXT);
  if (l2.length()) drawCentered(l2, 128, COL_DIM, FONT_HEAD);
  if (busy) showBusy();
}

// QR code on a white tile, centred at (cx, cy), at most maxPx wide.
struct QrTarget {
  int cx, cy, maxPx;
} qrTarget;

void qrDraw(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q);
  int scale = max(1, qrTarget.maxPx / (n + 4));
  int size = (n + 4) * scale;
  int x0 = qrTarget.cx - size / 2, y0 = qrTarget.cy - size / 2;
  gfx->fillRoundRect(x0, y0, size, size, 4, COL_WHITE);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (esp_qrcode_get_module(q, x, y))
        gfx->fillRect(x0 + (x + 2) * scale, y0 + (y + 2) * scale, scale, scale, COL_BLACK);
}

void drawQr(const String &text, int cx, int cy, int maxPx) {
  qrTarget = {cx, cy, maxPx};
  esp_qrcode_config_t c = {};
  c.display_func = qrDraw;
  c.max_qrcode_version = 6;
  c.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  esp_qrcode_generate(&c, text.c_str());
}

// --------------------------------------------------------------------------- fetching

static const char *const REST_BASES[] = {REST_PRIMARY, REST_FALLBACK};

uint16_t lineColor(const char *line, uint8_t product) {
  static const struct { const char *name; uint32_t hex; } table[] = {
      {"U1", 0x7DAD4C}, {"U2", 0xDA421E}, {"U3", 0x16683D}, {"U4", 0xF0D722},
      {"U5", 0x7E5330}, {"U6", 0x8C6DAB}, {"U7", 0x528DBA}, {"U8", 0x224F86},
      {"U9", 0xF3791D}, {"S1", 0xDE4DA4}, {"S2", 0x005F27}, {"S25", 0x005F27},
      {"S26", 0x005F27}, {"S3", 0x0A4C99}, {"S41", 0xAD5937}, {"S42", 0xCB6418},
      {"S45", 0xCD9C53}, {"S46", 0xCD9C53}, {"S47", 0xCD9C53}, {"S5", 0xEB7405},
      {"S7", 0x816DA6}, {"S75", 0x816DA6}, {"S8", 0x66AA22}, {"S85", 0x66AA22},
      {"S9", 0x992746},
  };
  for (auto &e : table)
    if (strcmp(e.name, line) == 0) return rgb(e.hex);
  const Product *p = productByBit(product);
  return p ? rgb(p->color) : rgb(0x555555);
}

uint16_t parseHexColor(const char *s) {
  if (!s || s[0] != '#') return rgb(0x555555);
  return rgb(strtoul(s + 1, nullptr, 16));
}

String colorHex(uint16_t c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", (c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3);
  return buf;
}

// "S+U Kurt-Schumacher-Platz (Berlin)" -> "Kurt-Schumacher-Platz"
String cleanName(String s) {
  if (s.startsWith("Ringbahn")) return "Ring";
  if (s.startsWith("S+U ")) s.remove(0, 4);
  else if (s.startsWith("S ") || s.startsWith("U ")) s.remove(0, 2);
  s.replace(" (Berlin)", "");
  int b = s.indexOf(" [");
  if (b >= 0) {
    int e = s.indexOf(']', b);
    if (e > b) s.remove(b, e - b + 1);
  }
  s.replace(" Bhf", "");
  s.trim();
  return s;
}

// days since 1970-01-01 (H. Hinnant's days_from_civil)
long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// "2026-10-17T11:40:00+02:00" -> epoch seconds
time_t parseIso(const char *s) {
  int y, mo, d, h, mi, se, oh = 0, om = 0;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  const char *tz = s + 19;
  while (*tz && *tz != '+' && *tz != '-' && *tz != 'Z') tz++;  // skip fractional seconds
  long off = 0;
  if ((*tz == '+' || *tz == '-') && sscanf(tz + 1, "%d:%d", &oh, &om) >= 1)
    off = (oh * 3600L + om * 60L) * (*tz == '-' ? -1 : 1);
  return (time_t)(daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + se - off);
}

String urlEncode(const String &s) {
  String out;
  char buf[4];
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// GET from transport.rest and stream-parse with a filter. Starts with the server that
// answered last time and falls back to the other one.
bool restGet(const String &path, JsonDocument &filter, JsonDocument &doc, String &err) {
  static int preferred = 0;
  for (int i = 0; i < 2; i++) {
    int idx = (preferred + i) % 2;
    const char *base = REST_BASES[idx];
    WiFiClientSecure tls;
    tls.setInsecure();  // public timetable data, no secrets involved
    tls.setHandshakeTimeout(15);
    HTTPClient http;
    http.useHTTP10(true);  // no chunked encoding, so we can stream-parse
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    http.setUserAgent("abfahrten-board/" FW_VERSION);
    if (!http.begin(tls, String(base) + path)) {
      err = "http.begin failed";
      continue;
    }
    int code = http.GET();
    if (code != 200) {
      err = String(base + 8) + ": ";
      if (code > 0) err += "HTTP " + String(code);
      else err += HTTPClient::errorToString(code);
      http.end();
      continue;
    }
    doc.clear();
    DeserializationError e =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (e) {
      err = String(base + 8) + ": JSON " + e.c_str();
      continue;
    }
    preferred = idx;
    return true;
  }
  return false;
}

void sortDepartures(Departure *d, int n) {
  std::sort(d, d + n, [](const Departure &a, const Departure &b) { return a.when < b.when; });
}

bool fetchRest(const String &station, int products, Departure *out, int &n, String &err) {
  String path = "/stops/" + station + "/departures?duration=" + LOOKAHEAD_MIN +
                "&results=" + MAX_DEPS + "&remarks=false&linesOfStops=false&language=de";
  for (auto &p : PRODUCTS) path += String("&") + p.api + ((products & p.bit) ? "=true" : "=false");

  JsonDocument filter;
  JsonObject f = filter["departures"][0].to<JsonObject>();
  f["when"] = true;
  f["plannedWhen"] = true;
  f["direction"] = true;
  f["cancelled"] = true;
  f["line"]["name"] = true;
  f["line"]["productName"] = true;
  f["line"]["product"] = true;

  JsonDocument doc;
  if (!restGet(path, filter, doc, err)) return false;

  n = 0;
  for (JsonObject j : doc["departures"].as<JsonArray>()) {
    if (n >= MAX_DEPS) break;
    if (j["cancelled"] | false) continue;
    time_t when = parseIso(j["when"] | (const char *)nullptr);
    if (!when) when = parseIso(j["plannedWhen"] | (const char *)nullptr);
    if (!when) continue;

    uint8_t product = productByApiName(j["line"]["product"].as<const char *>());
    String line = j["line"]["name"] | "?";
    line.replace(" ", "");
    // some S/U trips come without the letter ("46" instead of "S46")
    if ((product == 1 || product == 2) && isdigit((unsigned char)line[0]))
      line = String(j["line"]["productName"] | "") + line;
    Departure &d = out[n++];
    strlcpy(d.line, line.c_str(), sizeof(d.line));
    strlcpy(d.dir, cleanName(j["direction"] | "?").c_str(), sizeof(d.dir));
    d.when = when;
    d.product = product;
    d.color = lineColor(d.line, d.product);
  }
  sortDepartures(out, n);
  return true;
}

// Python dev server: {"d":[["U6","Alt-Mariendorf",3,"#8C6DAB"], ...]}
bool fetchServer(Departure *out, int &n, String &err) {
  WiFiClient client;
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(8000);
  if (!http.begin(client, SERVER_URL)) {
    err = "http.begin failed";
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    err = "Server HTTP " + String(code);
    http.end();
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e) {
    err = String("JSON: ") + e.c_str();
    return false;
  }
  time_t now = time(nullptr);
  n = 0;
  for (JsonArray r : doc["d"].as<JsonArray>()) {
    if (n >= MAX_DEPS) break;
    Departure &d = out[n++];
    strlcpy(d.line, r[0] | "?", sizeof(d.line));
    strlcpy(d.dir, r[1] | "?", sizeof(d.dir));
    d.when = now + (r[2] | 0) * 60 + 30;  // server rounds down; assume mid-minute
    d.product = d.line[0] == 'S' ? 1 : d.line[0] == 'U' ? 2 : 0;
    d.color = parseHexColor(r[3] | "");
  }
  sortDepartures(out, n);
  return true;
}

// Background task: fetch every refresh interval; after a failure retry soon and back off
// (3, 6, 12, 24 s ... up to the refresh interval). requestFetch() wakes it up early.
void fetchTask(void *) {
  static Departure fresh[MAX_DEPS];
  uint32_t waitMs = 0;
  for (;;) {
    if (waitMs) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
    waitMs = 1000;
    if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000) continue;  // need WiFi + clock

    String station;
    int products;
    uint32_t gen;
    {
      DataLock lock;
      station = cfg.stationId;
      products = cfg.products;
      gen = settingsGen;
    }
    int n = 0;
    String err;
    bool ok = strlen(SERVER_URL) ? fetchServer(fresh, n, err)
                                 : fetchRest(station, products, fresh, n, err);

    DataLock lock;
    if (gen != settingsGen) continue;  // settings changed meanwhile: fetch again right away
    if (ok) {
      memcpy(deps, fresh, n * sizeof(Departure));
      depCount = n;
      lastOk = time(nullptr);
      lastError = "";
      failures = 0;
      waitMs = cfg.refreshSec * 1000UL;
    } else {
      lastError = err;
      failures++;
      waitMs = min<uint32_t>(cfg.refreshSec * 1000UL, 3000UL << min(failures - 1, 3));
      Serial.printf("Fetch failed (%d): %s\n", failures, err.c_str());
    }
  }
}

// --------------------------------------------------------------------------- views

// A view is a product filter (0 = all products) or the info screen.
constexpr uint8_t VIEW_ALL = 0, VIEW_INFO = 0xFF;
uint8_t views[2 + sizeof(PRODUCTS) / sizeof(PRODUCTS[0])] = {VIEW_ALL};
int viewCount = 1, viewIdx = 0;
uint32_t viewSince = 0, viewTimeout = 0;  // info view returns to the first view after viewTimeout

String rowCache[ROWS];
String screenCache;

void invalidateScreen() {
  screenCache = "";
  for (auto &r : rowCache) r = "";
}

// All, then one view per enabled product (only if more than one is enabled), then info.
void buildViews() {
  uint8_t current = views[viewIdx];
  int enabled = 0;
  for (auto &p : PRODUCTS) enabled += (cfg.products & p.bit) ? 1 : 0;
  viewCount = 0;
  views[viewCount++] = VIEW_ALL;
  if (enabled > 1)
    for (auto &p : PRODUCTS)
      if (cfg.products & p.bit) views[viewCount++] = p.bit;
  views[viewCount++] = VIEW_INFO;
  viewIdx = 0;
  for (int i = 0; i < viewCount; i++)
    if (views[i] == current) viewIdx = i;
}

void setView(int i, uint32_t timeoutMs = INFO_TIMEOUT_MS) {
  viewIdx = i;
  viewSince = millis();
  viewTimeout = timeoutMs;
  busyY = -1;
  invalidateScreen();
  gfx->fillScreen(COL_BG);
}

int infoViewIndex() { return viewCount - 1; }

// --------------------------------------------------------------------------- board rendering

Departure snap[MAX_DEPS];  // the UI's copy of the shared departures

// Small product logo in the header of a filtered view: U square, S circle, others a pill.
int drawProductBadge(uint8_t bit, int right) {
  const Product *p = productByBit(bit);
  if (!p) return right;
  gfx->setFont(FONT_HEAD);
  int tw = textWidth(p->badge);
  int w = max(18, tw + 8);
  int x = right - w;
  if (bit == 1) gfx->fillCircle(x + w / 2, 15, 9, rgb(p->color));
  else gfx->fillRoundRect(x, 6, w, 18, bit == 2 ? 3 : 9, rgb(p->color));
  drawText(p->badge, x + (w - tw) / 2, 20, FONT_HEAD, COL_TEXT);
  return x;
}

void drawBoardHeader(const String &clock, bool stale) {
  uint8_t v = views[viewIdx];
  String key = cfg.stationName + "|" + clock + (stale ? "!" : "") + "|" + v;
  if (key == screenCache) return;
  screenCache = key;

  gfx->fillRect(0, 0, W, HEADER_H, COL_BG);
  gfx->setFont(FONT_HEAD);
  int clockX = W - PAD - textWidth(clock);
  drawText(clock, clockX, 20, FONT_HEAD, COL_HEAD);
  int right = v != VIEW_ALL ? drawProductBadge(v, clockX - 8) : clockX;
  gfx->setFont(FONT_HEAD);
  String label = fitText(cfg.stationName, right - PAD - 18);
  drawText(label, PAD, 20, FONT_HEAD, COL_HEAD);
  if (stale) gfx->fillCircle(PAD + textWidth(label) + 8, 15, 3, COL_RED);
  gfx->drawFastHLine(0, HEADER_H - 1, W, COL_LINE);
}

void drawRow(int i, const Departure *d, int minutes) {
  String key = d ? String(d->line) + "|" + d->dir + "|" + minutes : "-";
  if (key == rowCache[i]) return;
  rowCache[i] = key;

  int y0 = HEADER_H + i * ROW_H;
  gfx->fillRect(0, y0, W, ROW_H, COL_BG);
  if (i > 0) gfx->drawFastHLine(PAD, y0, W - 2 * PAD, COL_LINE);
  if (!d) return;

  // badge: pill for S-Bahn, square for the rest
  int by = y0 + (ROW_H - BADGE_H) / 2;
  gfx->fillRoundRect(PAD, by, BADGE_W, BADGE_H, d->product == 1 ? BADGE_H / 2 : 3, d->color);
  gfx->setFont(FONT_ROW);
  String line = fitText(d->line, BADGE_W - 2);
  drawText(line, PAD + (BADGE_W - textWidth(line)) / 2, by + 16, FONT_ROW, COL_TEXT);
  drawText(fitText(d->dir, DIR_MAX_W), DIR_X, by + 16, FONT_ROW, COL_TEXT);

  String m(minutes);
  gfx->setFont(FONT_MIN);
  drawText(m, W - PAD - textWidth(m), y0 + ROW_H / 2 + 12, FONT_MIN, COL_TEXT);
}

// Body while there is no (current) data yet: text + loading train, details after failures.
void renderLoading(int fails, const String &err) {
  String key = "loading";
  if (fails >= 2) key = "fail|" + err;
  if (rowCache[0] == key) return;
  for (auto &r : rowCache) r = "";
  rowCache[0] = key;

  gfx->fillRect(0, HEADER_H, W, H - HEADER_H, COL_BG);
  drawCentered("Abfahrten werden", 88, COL_TEXT);
  drawCentered("geladen", 110, COL_TEXT);
  if (fails >= 2) {
    drawCentered("Datenquelle antwortet nicht,", 138, COL_DIM, FONT_SMALL);
    drawCentered("neuer Versuch läuft", 152, COL_DIM, FONT_SMALL);
    gfx->setFont(FONT_SMALL);
    drawCentered(fitText(err, W - 2 * PAD), 224, COL_DIM, FONT_SMALL);
  }
  showBusy();
}

void renderBoard(const String &clock, time_t now) {
  int count, fails;
  time_t ok;
  String err;
  {
    DataLock lock;
    count = depCount;
    memcpy(snap, deps, count * sizeof(Departure));
    ok = lastOk;
    fails = failures;
    err = lastError;
  }
  bool haveData = ok && now - ok < EXPIRE_SECONDS;
  drawBoardHeader(clock, haveData && now - ok > STALE_SECONDS);
  if (!haveData) {
    renderLoading(fails, err);
    return;
  }
  busyY = -1;

  uint8_t filter = views[viewIdx];
  int row = 0;
  for (int k = 0; k < count && row < ROWS; k++) {
    if (filter != VIEW_ALL && !(snap[k].product & filter)) continue;
    long diff = (long)(snap[k].when - now);
    if (diff < 0) continue;
    int minutes = diff / 60;
    if (minutes < cfg.minMinutes) continue;
    drawRow(row++, &snap[k], minutes);
  }
  if (row == 0) {
    if (rowCache[0] != "empty") {
      gfx->fillRect(0, HEADER_H, W, H - HEADER_H, COL_BG);
      drawCentered("Keine Abfahrten", 130, COL_DIM);
      for (auto &r : rowCache) r = "";
      rowCache[0] = "empty";
    }
    return;
  }
  for (; row < ROWS; row++) drawRow(row, nullptr, 0);
}

// Label above a value, left column of the info screen.
void infoItem(int y, const char *label, const String &value) {
  drawText(label, PAD, y, FONT_SMALL, COL_DIM);
  gfx->setFont(FONT_ROW);
  drawText(fitText(value, 128), PAD, y + 18, FONT_ROW, COL_TEXT);
}

void renderInfo(const String &clock) {
  String ip = WiFi.localIP().toString();
  String ssid = WiFi.SSID();
  time_t ok;
  {
    DataLock lock;
    ok = lastOk;
  }
  char upd[6] = "-";
  if (ok > 1700000000) {
    struct tm lt;
    localtime_r(&ok, &lt);
    strftime(upd, sizeof(upd), "%H:%M", &lt);
  }
  String key = "info|" + clock + "|" + ip + "|" + ssid + "|" + cfg.stationName + "|" + upd;
  if (key == screenCache) return;
  screenCache = key;

  busyY = -1;
  gfx->fillScreen(COL_BG);
  drawBar("Info", clock);

  infoItem(48, "STATION", cfg.stationName);
  infoItem(92, "WLAN", ssid);
  infoItem(136, "ADRESSE", ip);
  drawText(HOSTNAME ".local", PAD, 172, FONT_HEAD, COL_DIM);
  drawText(String("Stand ") + upd + "  \xC2\xB7  v" FW_VERSION, PAD, 226, FONT_SMALL, COL_DIM);

  // right column: QR code to the settings page
  drawQr("http://" + ip + "/", 186, 82, 90);
  drawCentered("Einstellungen", 142, COL_DIM, FONT_SMALL, 186);
}

void render() {
  time_t now = time(nullptr);
  bool timeReady = now > 1700000000;  // NTP synced
  char clock[6] = "";
  if (timeReady) {
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(clock, sizeof(clock), "%H:%M", &lt);
  }

  if (views[viewIdx] == VIEW_INFO) {
    renderInfo(clock);
  } else if (!timeReady) {
    if (screenCache != "time") {
      messageScreen(cfg.stationName, "Uhrzeit wird geladen", "", true);
      screenCache = "time";
    }
  } else {
    if (screenCache == "time") {
      busyY = -1;
      gfx->fillScreen(COL_BG);
      invalidateScreen();
    }
    renderBoard(clock, now);
  }
}

// --------------------------------------------------------------------------- touch button
// Capacitive pad on top of the case. The reading drops while touched; the baseline is
// taken at power-up (don't touch it then) and follows slow drift afterwards.

float touchBase = 0;
uint16_t touchLast = 0, touchMin = 0xFFFF, touchMinShown = 0;

enum TouchEvent { TOUCH_NONE, TOUCH_TAP, TOUCH_LONG };

void touchBegin() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += touchRead(TOUCH_PIN);
    delay(5);
  }
  touchBase = sum / 16.0f;
}

TouchEvent touchPoll() {
  static uint32_t lastRead = 0, downSince = 0, minSince = 0;
  static uint8_t hits = 0, misses = 0;
  static bool down = false, longFired = false;

  uint32_t now = millis();
  if (now - lastRead < 20) return TOUCH_NONE;
  lastRead = now;
  touchLast = touchRead(TOUCH_PIN);
  touchMin = min(touchMin, touchLast);  // lowest reading per 10 s, for tuning via /api/status
  if (now - minSince > 10000) {
    touchMinShown = touchMin;
    touchMin = 0xFFFF;
    minSince = now;
  }
  // hysteresis: a touch starts below base * TOUCH_RATIO and ends above base * (ratio + 0.04)
  bool pressed = touchLast < touchBase * (down ? TOUCH_RATIO + 0.04f : TOUCH_RATIO);

  if (!down) {
    if (!pressed) {
      // The baseline rises quickly but sinks only very slowly, and ignores readings that
      // look like an approaching finger - otherwise a hovering hand drags it (and the
      // threshold) down until light taps no longer count.
      if (touchLast > touchBase) touchBase += (touchLast - touchBase) * 0.05f;
      else if (touchLast > touchBase * 0.95f) touchBase += (touchLast - touchBase) * 0.002f;
      hits = 0;
      return TOUCH_NONE;
    }
    if (++hits < 2) return TOUCH_NONE;  // debounce
    down = true;
    downSince = now;
    longFired = false;
    misses = 0;
    return TOUCH_NONE;
  }
  if (pressed) {
    misses = 0;
    if (!longFired && now - downSince >= LONG_PRESS_MS) {
      longFired = true;
      return TOUCH_LONG;
    }
    return TOUCH_NONE;
  }
  if (++misses < 2) return TOUCH_NONE;
  down = false;
  hits = 0;
  return longFired ? TOUCH_NONE : TOUCH_TAP;
}

// --------------------------------------------------------------------------- web

WebServer server(80);
HTTPUpdateServer updater;

void sendJson(JsonDocument &doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void handleStatus() {
  JsonDocument doc;
  time_t now = time(nullptr);
  doc["version"] = FW_VERSION;
  doc["station"] = cfg.stationId;
  doc["source"] = strlen(SERVER_URL) ? SERVER_URL : REST_PRIMARY;
  doc["heap"] = ESP.getFreeHeap();
  doc["rssi"] = WiFi.RSSI();
  doc["view"] = views[viewIdx];
  doc["touch"]["raw"] = touchLast;
  doc["touch"]["base"] = (int)touchBase;
  doc["touch"]["min10s"] = touchMinShown;
  {
    DataLock lock;
    doc["last_ok_age_s"] = lastOk ? (long)(now - lastOk) : -1;
    doc["error"] = lastError;
    doc["failures"] = failures;
    JsonArray arr = doc["departures"].to<JsonArray>();
    for (int k = 0; k < depCount; k++) {
      JsonObject o = arr.add<JsonObject>();
      o["line"] = deps[k].line;
      o["dir"] = deps[k].dir;
      o["color"] = colorHex(deps[k].color);
      o["product"] = deps[k].product;
      o["in_s"] = (long)(deps[k].when - now);
    }
  }
  sendJson(doc);
}

void handleGetSettings() {
  JsonDocument doc;
  doc["stationId"] = cfg.stationId;
  doc["stationName"] = cfg.stationName;
  doc["products"] = cfg.products;
  doc["minMinutes"] = cfg.minMinutes;
  doc["brightness"] = cfg.brightness;
  doc["refreshSec"] = cfg.refreshSec;
  sendJson(doc);
}

// Accepts any subset of the fields from handleGetSettings.
void handlePostSettings() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    server.send(400, "text/plain", "invalid JSON");
    return;
  }
  String id = in["stationId"] | cfg.stationId.c_str();
  id.trim();
  if (id.length() < 6 || id.length() > 12) {
    server.send(400, "text/plain", "invalid station id");
    return;
  }
  bool changed = false;
  {
    DataLock lock;
    if (in["stationId"].is<const char *>()) {
      changed |= id != cfg.stationId;
      cfg.stationId = id;
      cfg.stationName = in["stationName"] | id.c_str();
    }
    if (in["products"].is<int>()) {
      int p = constrain(in["products"].as<int>(), 1, 127);
      changed |= p != cfg.products;
      cfg.products = p;
    }
    if (in["refreshSec"].is<int>()) cfg.refreshSec = constrain(in["refreshSec"].as<int>(), 10, 300);
    if (changed) {
      depCount = 0;
      lastOk = 0;
      failures = 0;
      lastError = "";
      settingsGen++;
    }
  }
  if (in["minMinutes"].is<int>()) cfg.minMinutes = constrain(in["minMinutes"].as<int>(), 0, 30);
  if (in["brightness"].is<int>()) cfg.brightness = constrain(in["brightness"].as<int>(), 5, 100);

  saveConfig();
  applyBrightness();
  buildViews();
  invalidateScreen();
  if (changed) requestFetch();
  handleGetSettings();
}

// Station search: [{"id":"900078101","name":"U Hermannplatz (Berlin)","label":"Hermannplatz","products":10}]
void handleSearch() {
  String q = server.arg("q");
  q.trim();
  JsonDocument result;
  JsonArray list = result.to<JsonArray>();
  if (q.length() >= 2) {
    JsonDocument filter;
    filter[0]["id"] = true;
    filter[0]["name"] = true;
    filter[0]["type"] = true;
    filter[0]["products"] = true;
    JsonDocument doc;
    String err;
    String path = "/locations?query=" + urlEncode(q) +
                  "&results=8&stops=true&addresses=false&poi=false&linesOfStops=false&language=de";
    if (!restGet(path, filter, doc, err)) {
      server.send(502, "text/plain", err);
      return;
    }
    for (JsonObject l : doc.as<JsonArray>()) {
      if (strcmp(l["type"] | "", "stop") != 0 && strcmp(l["type"] | "", "station") != 0) continue;
      int mask = 0;
      for (JsonPair kv : l["products"].as<JsonObject>())
        if (kv.value().as<bool>()) mask |= productByApiName(kv.key().c_str());
      JsonObject o = list.add<JsonObject>();
      o["id"] = l["id"];
      o["name"] = l["name"];
      o["label"] = cleanName(l["name"] | "?");
      o["products"] = mask;
    }
  }
  sendJson(result);
}

// --------------------------------------------------------------------------- boot safety
// Crash resets in a row are counted in RTC memory (survives resets, not power loss).
// After 3 crashes the board starts only the setup hotspot incl. /update, so a broken
// build can always be replaced without opening the case.

RTC_NOINIT_ATTR uint32_t crashMagic;
RTC_NOINIT_ATTR uint32_t crashCount;

bool crashLoop() {
  esp_reset_reason_t r = esp_reset_reason();
  bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                 r == ESP_RST_WDT;
  if (crashMagic != 0xC0FFEE42 || !crashed) {
    crashMagic = 0xC0FFEE42;
    crashCount = 0;
  }
  if (crashed) crashCount++;
  return crashCount >= 3;
}

// Unplugging the board 3x within a few seconds after power-up opens the WiFi setup,
// e.g. to switch networks when the board's IP is unknown.
bool powerCycleGesture() {
  prefs.begin("board", false);
  int n = esp_reset_reason() == ESP_RST_POWERON ? prefs.getInt("boots", 0) + 1 : 0;
  prefs.putInt("boots", n >= 3 ? 0 : n);
  prefs.end();
  return n >= 3;
}

void clearPowerCycles() {
  prefs.begin("board", false);
  prefs.putInt("boots", 0);
  prefs.end();
}

// --------------------------------------------------------------------------- storage
// After installing over the stock firmware, the flash still holds the old GeekMagic app
// in the other OTA slot, its files (web UI, pictures) in the filesystem partition and its
// settings in NVS. Wipe all of it once. The bootloader and partition table stay.

bool partitionBlank(const esp_partition_t *p) {
  uint32_t buf[64];
  if (esp_partition_read(p, 0, buf, sizeof(buf)) != ESP_OK) return true;
  for (uint32_t w : buf)
    if (w != 0xFFFFFFFF) return false;
  return true;
}

void drawProgress(int done, int total) {
  int w = 160, x = (W - w) / 2, y = 166;
  gfx->drawRoundRect(x, y, w, 8, 4, COL_LINE);
  if (done > 0) gfx->fillRoundRect(x, y, max(8, w * done / total), 8, 4, COL_ACC);
}

void erasePartition(const esp_partition_t *p, int &done, int total) {
  for (uint32_t off = 0; off < p->size; off += 0x10000) {
    esp_partition_erase_range(p, off, 0x10000);
    drawProgress(++done, total);
    delay(1);
  }
}

void wipeStockFirmware() {
  const esp_partition_t *fs =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  prefs.begin("board", true);
  bool wiped = prefs.getBool("wiped", false);
  prefs.end();
  if (wiped && (!fs || partitionBlank(fs))) return;

  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (other == esp_ota_get_running_partition()) other = nullptr;  // never erase ourselves
  int total = (other ? other->size : 0) / 0x10000 + (fs ? fs->size : 0) / 0x10000;
  int done = 0;
  messageScreen("Einrichtung", "Alte Software", "wird gelöscht ...");
  drawProgress(0, total);
  if (other) erasePartition(other, done, total);
  if (fs) erasePartition(fs, done, total);

  nvs_flash_deinit();  // old settings, including the stock firmware's WiFi
  nvs_flash_erase();
  nvs_flash_init();
  prefs.begin("board", false);
  prefs.putBool("wiped", true);
  prefs.end();
}

// Rewrites the settings storage from scratch with the given WiFi. NVS only marks
// overwritten entries as deleted, so an old password would otherwise stay in flash.
void rewriteStorage(const String &ssid, const String &pass) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  nvs_flash_deinit();
  nvs_flash_erase();
  nvs_flash_init();
  saveConfig();
  prefs.begin("board", false);
  prefs.putBool("wiped", true);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

// --------------------------------------------------------------------------- WiFi setup mode
// If the stored WiFi can't be joined at boot, the board opens its own access point
// with a captive portal to enter new credentials, so it never ends up unreachable.

DNSServer dns;
bool setupMode = false;

bool wifiConfigured(const String &ssid) {
  return ssid.length() > 0 && ssid != "your-wifi";
}

String wifiSsid() {
  prefs.begin("board", true);
  String s = prefs.getString("ssid", WIFI_SSID);
  prefs.end();
  return s;
}

String wifiPass() {
  prefs.begin("board", true);
  String s = prefs.getString("pass", WIFI_PASS);
  prefs.end();
  return s;
}

void handleWifiScan() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    bool dup = false;
    for (JsonVariant v : arr) dup |= v["ssid"] == WiFi.SSID(i);
    if (dup || WiFi.SSID(i).isEmpty()) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(doc);
}

// Saved network (never the password) for the WiFi page.
void handleWifiGet() {
  JsonDocument doc;
  String ssid = wifiSsid();
  doc["ssid"] = wifiConfigured(ssid) ? ssid : "";
  doc["connected"] = WiFi.status() == WL_CONNECTED;
  doc["setup"] = setupMode;
  sendJson(doc);
}

void restartWith(const String &ssid, const String &pass, const String &msg) {
  server.send(200, "text/plain", "ok");
  delay(300);  // let the answer reach the phone before WiFi goes down
  messageScreen("WLAN", msg, "Neustart ...");
  rewriteStorage(ssid, pass);
  delay(1200);
  ESP.restart();
}

void handleWifiSave() {
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain")) || !in["ssid"].is<const char *>() ||
      strlen(in["ssid"]) == 0) {
    server.send(400, "text/plain", "ssid missing");
    return;
  }
  restartWith(in["ssid"].as<String>(), in["pass"] | "", "Gespeichert");
}

void handleWifiForget() {
  restartWith("", "", "WLAN gelöscht");
}

void startSetupMode(const char *title) {
  setupMode = true;
  WiFi.setAutoReconnect(false);  // stop hunting for the old network, it disturbs the hotspot
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);  // STA part only used for scanning
  WiFi.softAP(SETUP_AP_NAME);
  dns.start(53, "*", WiFi.softAPIP());

  busyY = -1;
  gfx->fillScreen(COL_BG);
  drawBar(title, WiFi.softAPIP().toString());
  drawQr("WIFI:T:nopass;S:" SETUP_AP_NAME ";;", W / 2, 100, 132);
  drawCentered("Mit dem Handy scannen", 188, COL_TEXT);
  drawCentered("oder WLAN verbinden mit", 207, COL_DIM, FONT_SMALL);
  drawCentered(SETUP_AP_NAME, 228, COL_ACC);
}

// --------------------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  bool crashed = crashLoop();
  dataMutex = xSemaphoreCreateMutex();

  setupBacklight();
  cfg.brightness = DEFAULT_BRIGHTNESS;
  applyBrightness();
  gfx->begin(40000000);
  gfx->setUTF8Print(true);
  gfx->fillScreen(COL_BG);
  trainCanvas = new Arduino_Canvas(TRAIN_W, TRAIN_H, gfx, TRAIN_X, TRAIN_Y);
  if (!trainCanvas->begin(GFX_SKIP_OUTPUT_BEGIN)) {  // 11 KB frame buffer
    delete trainCanvas;
    trainCanvas = nullptr;
  }

  wipeStockFirmware();
  loadConfig();
  applyBrightness();
  buildViews();
  bool gesture = powerCycleGesture();
  touchBegin();

  WiFi.persistent(false);  // credentials live in our settings, not in the WiFi driver
  WiFi.setHostname(HOSTNAME);
  updater.setup(&server, "/update", OTA_USER, OTA_PASS);
  server.on("/style.css", [] { server.send_P(200, "text/css", STYLE_CSS); });
  server.on("/vehicles.js", [] { server.send_P(200, "application/javascript", VEHICLES_JS); });
  server.on("/api/wifi/scan", handleWifiScan);
  server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/api/wifi", HTTP_GET, handleWifiGet);
  server.on("/api/wifi", HTTP_POST, handleWifiSave);

  String ssid = wifiSsid();
  bool connected = false;
  if (!crashed && !gesture && wifiConfigured(ssid)) {
    messageScreen("Verbinde ...", ssid, "WLAN wird verbunden", true);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), wifiPass().c_str());
    for (uint32_t t0 = millis(); millis() - t0 < 30000 && WiFi.status() != WL_CONNECTED;) {
      drawBusy();
      delay(30);
    }
    connected = WiFi.status() == WL_CONNECTED;
  }

  if (!connected) {
    startSetupMode(crashed ? "Sicherer Modus" : "WLAN einrichten");
    server.on("/", [] { server.send_P(200, "text/html", WIFI_PAGE); });
    // captive portal: send every unknown URL to the setup page
    server.onNotFound([] {
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
      server.send(302, "text/plain", "");
    });
    server.begin();
    return;
  }

  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  MDNS.begin(HOSTNAME);
  server.on("/", [] { server.send_P(200, "text/html", WEB_PAGE); });
  server.on("/wifi", [] { server.send_P(200, "text/html", WIFI_PAGE); });
  server.on("/api/status", handleStatus);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/settings", HTTP_POST, handlePostSettings);
  server.on("/api/search", handleSearch);
  server.begin();
  MDNS.addService("http", "tcp", 80);

  // loading runs on core 0 next to the WiFi stack; the UI loop keeps core 1
  xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, &fetchTaskHandle, 0);

  setView(infoViewIndex(), BOOT_INFO_MS);  // show where the board can be reached
}

void loop() {
  static uint32_t lastRender = 0, lastBusy = 0;
  static bool bootsCleared = false, crashCleared = false;

  server.handleClient();

  if (!bootsCleared && millis() > 5000) {  // ran long enough: not a power-cycle gesture
    clearPowerCycles();
    bootsCleared = true;
  }
  if (!crashCleared && millis() > 60000) {  // stable: reset the crash counter
    crashCount = 0;
    crashCleared = true;
  }

  if (setupMode) {
    dns.processNextRequest();
    // Nobody configuring it? Maybe the router was just down - try again.
    if (millis() > SETUP_RETRY_MS && WiFi.softAPgetStationNum() == 0) ESP.restart();
    delay(5);
    return;
  }

  TouchEvent ev = touchPoll();
  if (ev == TOUCH_TAP) setView((viewIdx + 1) % viewCount);
  else if (ev == TOUCH_LONG) setView(0);
  if (views[viewIdx] == VIEW_INFO && millis() - viewSince > viewTimeout) setView(0);

  if (ev != TOUCH_NONE || millis() - lastRender >= 500) {
    lastRender = millis();
    render();
  }
  if (busyY >= 0 && millis() - lastBusy >= 30) {
    lastBusy = millis();
    drawBusy();
  }
  delay(2);
}
