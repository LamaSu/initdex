// initdex v0.1.0 - catch the people you meet at init(), badge to badge.
// Runs from the app1 slot; the init() conference firmware stays untouched in app0.
// Hold badges close, both press KEY A within 6 s: Gotcha. Catches live in NVS ("initdex").
// USB: line-delimited JSON (hello, dex, set_profile, wanted, boot_init, clear_dex).
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "esp_littlefs.h"
#include <ArduinoJson.h>
#include <vector>
#include <string>

#define INITDEX_VER "0.1.0"
static const char* PACK_URL = "https://lamasu.github.io/initdex/";
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t CHANNEL = 1;
static const int RSSI_CATCH = -66;       // a throw must be this strong (badges close)
static const uint32_t THROW_MS = 6000;   // both press A inside this window
static const uint32_t BEACON_MS = 1200;
static const uint32_t NEARBY_MS = 15000;
static const int TZ_OFFSET_S = -7 * 3600;  // PDT, for display only
static const int MAX_DEX = 60;
static const int MAX_SEEN = 32;
static const int MAX_WANTED = 24;
static const int RXQ = 16;

struct __attribute__((packed)) Card {
  char name[40];
  char company[32];
  char net[12];
  char handle[48];
};
struct __attribute__((packed)) Pkt {
  char magic[4];  // "IDX1"
  uint8_t type;   // 1 beacon, 2 throw
  uint8_t ver;
  uint16_t caught;
  Card card;
};
struct __attribute__((packed)) Catch {
  uint8_t mac[6];
  uint32_t t;  // unix seconds UTC, 0 if the clock is unknown
  Card card;
};
struct Seen {
  bool used;
  uint8_t mac[6];
  Card card;
  uint16_t caught;
  int rssi;
  uint32_t lastSeen, lastThrow, caughtAt, wildAt;
  int throwRssi;
  bool wanted;
};
struct Rx {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t data[sizeof(Pkt)];
};

static Card me;
static const char* meSource = "default";
static uint8_t myMac[6];
static Catch dex[MAX_DEX];
static int dexCount = 0;
static Seen seen[MAX_SEEN];
static std::vector<std::string> wanted;
static Preferences prefs;
static M5Canvas canvas(&M5.Display);
static lgfx::LovyanGFX* G = nullptr;
static bool useCanvas = false, radioOk = false;
static int W = 466, H = 466, CX = 233, CY = 233;

enum Page { RADAR, DEX, ME, CONFIRM_BOOT };
static Page page = RADAR;
static bool throwing = false;
static uint32_t throwStart = 0, lastBeacon = 0, lastThrowSend = 0, lastDraw = 0, bothHeldSince = 0;
static uint32_t celebrateUntil = 0, gotAwayUntil = 0, bannerUntil = 0;
static Card celebCard;
static bool celebAgain = false, dirty = true;
static int dexView = 0;
static char banner[64];

static Rx rxq[RXQ];
static volatile uint8_t rxHead = 0, rxTail = 0;
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

// ---------- small helpers ----------
static void copyField(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  size_t n = strlen(src);
  if (n > cap - 1) {
    n = cap - 1;
    while (n > 0 && (((uint8_t)src[n]) & 0xC0) == 0x80) --n;  // never split a UTF-8 sequence
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}
#define COPY(dst, s) copyField((dst), sizeof(dst), (s))

static void terminate(Card& c) {
  c.name[sizeof(c.name) - 1] = 0;
  c.company[sizeof(c.company) - 1] = 0;
  c.net[sizeof(c.net) - 1] = 0;
  c.handle[sizeof(c.handle) - 1] = 0;
}

static String macStr(const uint8_t* m) {
  char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static uint32_t nowUnix() {
  if (!M5.Rtc.isEnabled()) return 0;
  auto dt = M5.Rtc.getDateTime();
  if (dt.date.year < 2025 || dt.date.year > 2099) return 0;
  return (uint32_t)(daysFromCivil(dt.date.year, dt.date.month, dt.date.date) * 86400 +
                    dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds);
}

static void clockText(uint32_t t, char* out, size_t cap) {
  if (!t) { snprintf(out, cap, "caught tonight"); return; }
  int64_t local = (int64_t)t + TZ_OFFSET_S;
  int mins = (int)((local % 86400 + 86400) % 86400 / 60);
  int h = mins / 60, m = mins % 60;
  snprintf(out, cap, "caught %d:%02d %s", (h % 12) ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
}

static bool iequals(const char* a, const std::string& b) {
  size_t n = strlen(a);
  if (n != b.size()) return false;
  for (size_t i = 0; i < n; ++i)
    if (tolower((uint8_t)a[i]) != tolower((uint8_t)b[i])) return false;
  return true;
}

static std::string trimmed(const char* s) {
  std::string v(s ? s : "");
  size_t b = v.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  size_t e = v.find_last_not_of(" \t\r\n");
  return v.substr(b, e - b + 1);
}

static bool isWanted(const char* name) {
  for (auto& w : wanted)
    if (iequals(name, w)) return true;
  return false;
}

static void buzz(int pulses, int ms) {
  for (int i = 0; i < pulses; ++i) {
    M5.Power.setVibration(170);
    delay(ms);
    M5.Power.setVibration(0);
    if (i + 1 < pulses) delay(70);
  }
}

static void chirp(const float* notes, int count, int ms) {
  for (int i = 0; i < count; ++i) {
    M5.Speaker.tone(notes[i], ms);
    delay(ms + 15);
  }
}

static void setBanner(const char* text, uint32_t ms) {
  COPY(banner, text);
  bannerUntil = millis() + ms;
  dirty = true;
}

// ---------- profile: reuse the init() badge profile (read-only) ----------
static bool loadInitProfile(Card& c) {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/badgefs";
  conf.partition_label = "ffat";
  conf.format_if_mount_failed = false;
  conf.dont_mount = false;
  conf.read_only = true;  // never write the user's storage
  if (esp_vfs_littlefs_register(&conf) != ESP_OK) return false;
  bool ok = false;
  FILE* f = fopen("/badgefs/conference-manual-v1.bin", "rb");
  if (f) {
    uint8_t h[24];
    if (fread(h, 1, 24, f) == 24 && memcmp(h, "INITCF01", 8) == 0) {
      uint16_t ver = h[8] | (h[9] << 8);
      uint16_t hdr = h[10] | (h[11] << 8);
      uint32_t meta = h[16] | (h[17] << 8) | ((uint32_t)h[18] << 16) | ((uint32_t)h[19] << 24);
      if (ver >= 1 && ver <= 3 && hdr >= 24 && hdr <= 512 && meta > 0 && meta <= 4096 && fseek(f, hdr, SEEK_SET) == 0) {
        std::vector<uint8_t> m(meta);
        if (fread(m.data(), 1, meta, f) == meta) {
          std::string fields[5];
          int count = (ver == 3) ? 4 : (ver == 1 ? 4 : 5);
          size_t used = 0;
          bool good = true;
          for (int i = 0; i < count; ++i) {
            if (used + 2 > meta) { good = false; break; }
            size_t n = m[used] | (m[used + 1] << 8);
            used += 2;
            if (n > meta - used) { good = false; break; }
            fields[i].assign((const char*)m.data() + used, n);
            used += n;
          }
          if (good) {
            COPY(c.name, fields[0].c_str());
            if (ver == 3) {
              COPY(c.net, fields[1].c_str());
              COPY(c.handle, fields[2].c_str());
              COPY(c.company, fields[3].c_str());
            } else {
              static const char* slots[3] = {"github", "x", "linkedin"};
              for (int i = 0; i < 3; ++i) {
                if (fields[1 + i].empty()) continue;
                std::string url = fields[1 + i];
                while (!url.empty() && url.back() == '/') url.pop_back();
                size_t cut = url.find_last_of('/');
                COPY(c.net, slots[i]);
                COPY(c.handle, (cut == std::string::npos ? url : url.substr(cut + 1)).c_str());
                break;
              }
              if (ver == 2) COPY(c.company, fields[4].c_str());
            }
            ok = c.name[0] != 0;
          }
        }
      }
    }
    fclose(f);
  }
  esp_vfs_littlefs_unregister("ffat");
  return ok;
}

static void loadProfile() {
  memset(&me, 0, sizeof me);
  if (prefs.isKey("card") && prefs.getBytes("card", &me, sizeof me) == sizeof me && me.name[0]) {
    terminate(me);
    meSource = "override";
    return;
  }
  memset(&me, 0, sizeof me);
  if (loadInitProfile(me)) {
    meSource = "init";
    return;
  }
  memset(&me, 0, sizeof me);
  char buf[24];
  snprintf(buf, sizeof buf, "Trainer %02X%02X", myMac[4], myMac[5]);
  COPY(me.name, buf);
  meSource = "default";
}

// ---------- dex + wanted persistence ----------
static void loadDex() {
  dexCount = 0;
  int n = prefs.isKey("n") ? prefs.getInt("n", 0) : 0;
  if (n > MAX_DEX) n = MAX_DEX;
  for (int i = 0; i < n; ++i) {
    char k[8];
    snprintf(k, sizeof k, "c%d", i);
    if (prefs.isKey(k) && prefs.getBytes(k, &dex[dexCount], sizeof(Catch)) == sizeof(Catch)) {
      terminate(dex[dexCount].card);
      dexCount++;
    }
  }
}

static bool saveCatch(int idx) {
  char k[8];
  snprintf(k, sizeof k, "c%d", idx);
  bool ok = prefs.putBytes(k, &dex[idx], sizeof(Catch)) == sizeof(Catch);
  if (ok) prefs.putInt("n", dexCount);
  return ok;
}

static void refreshWantedFlags() {
  for (auto& s : seen)
    if (s.used) s.wanted = isWanted(s.card.name);
}

static void loadWanted() {
  wanted.clear();
  if (!prefs.isKey("want")) return;
  String all = prefs.getString("want", "");
  int start = 0;
  while (start < (int)all.length() && (int)wanted.size() < MAX_WANTED) {
    int nl = all.indexOf('\n', start);
    if (nl < 0) nl = all.length();
    std::string w = trimmed(all.substring(start, nl).c_str());
    if (!w.empty()) wanted.push_back(w);
    start = nl + 1;
  }
}

// ---------- radio ----------
static void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != (int)sizeof(Pkt) || memcmp(data, "IDX1", 4) != 0) return;
  portENTER_CRITICAL(&rxMux);
  uint8_t next = (rxHead + 1) % RXQ;
  if (next != rxTail) {
    Rx& r = rxq[rxHead];
    memcpy(r.mac, info->src_addr, 6);
    r.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : -100;
    memcpy(r.data, data, sizeof(Pkt));
    rxHead = next;
  }
  portEXIT_CRITICAL(&rxMux);
}

static void sendPkt(uint8_t type) {
  if (!radioOk) return;
  Pkt p;
  memcpy(p.magic, "IDX1", 4);
  p.type = type;
  p.ver = 1;
  p.caught = dexCount;
  p.card = me;
  esp_now_send(BCAST, (const uint8_t*)&p, sizeof p);
}

static Seen* findSeen(const uint8_t* mac) {
  Seen* freeSlot = nullptr;
  Seen* oldest = nullptr;
  for (auto& s : seen) {
    if (s.used && !memcmp(s.mac, mac, 6)) return &s;
    if (!s.used && !freeSlot) freeSlot = &s;
    if (s.used && (!oldest || s.lastSeen < oldest->lastSeen)) oldest = &s;
  }
  Seen* slot = freeSlot ? freeSlot : oldest;
  memset(slot, 0, sizeof(Seen));
  slot->used = true;
  memcpy(slot->mac, mac, 6);
  slot->rssi = -100;
  return slot;
}

static void wildAlert(const Seen& s) {
  char text[64];
  snprintf(text, sizeof text, "A wild %s appeared!", s.card.name);
  setBanner(text, 4500);
  static const float notes[] = {988, 1319};
  chirp(notes, 2, 70);
  buzz(2, 90);
  JsonDocument ev;
  ev["event"] = "wild";
  ev["name"] = s.card.name;
  serializeJson(ev, Serial);
  Serial.println();
}

static void processRx() {
  for (;;) {
    Rx r;
    bool have = false;
    portENTER_CRITICAL(&rxMux);
    if (rxTail != rxHead) {
      r = rxq[rxTail];
      rxTail = (rxTail + 1) % RXQ;
      have = true;
    }
    portEXIT_CRITICAL(&rxMux);
    if (!have) break;
    const Pkt* p = (const Pkt*)r.data;
    uint32_t now = millis();
    Seen* s = findSeen(r.mac);
    bool fresh = s->lastSeen && now - s->lastSeen < 5000;
    s->card = p->card;
    terminate(s->card);
    s->caught = p->caught;
    s->rssi = fresh ? (s->rssi * 3 + r.rssi) / 4 : r.rssi;
    s->lastSeen = now;
    if (p->type == 2) {
      s->lastThrow = now;
      s->throwRssi = r.rssi;
    }
    s->wanted = isWanted(s->card.name);
    if (s->wanted && (s->wildAt == 0 || now - s->wildAt > 300000)) {
      s->wildAt = now;
      wildAlert(*s);
    }
    dirty = true;
  }
}

// ---------- catching ----------
static void celebrate(const Card& c, bool again) {
  celebCard = c;
  celebAgain = again;
  celebrateUntil = millis() + 3200;
  dirty = true;
  static const float notes[] = {1319, 1568, 2093};
  chirp(notes, 3, 80);
  buzz(1, 220);
}

static void doCatch(Seen& s) {
  s.caughtAt = millis() ? millis() : 1;
  int idx = -1;
  for (int i = 0; i < dexCount; ++i)
    if (!memcmp(dex[i].mac, s.mac, 6)) { idx = i; break; }
  bool again = idx >= 0;
  if (!again) {
    if (dexCount >= MAX_DEX) {
      setBanner("Dex is full", 2500);
      return;
    }
    idx = dexCount++;
    memcpy(dex[idx].mac, s.mac, 6);
    dex[idx].t = nowUnix();
  }
  dex[idx].card = s.card;
  if (!saveCatch(idx)) setBanner("Saved in memory only", 2500);
  dexView = idx;
  celebrate(s.card, again);
  JsonDocument ev;
  ev["event"] = "caught";
  ev["name"] = s.card.name;
  ev["again"] = again;
  serializeJson(ev, Serial);
  Serial.println();
}

static int caughtThisThrow() {
  int n = 0;
  for (auto& s : seen)
    if (s.used && s.caughtAt && s.caughtAt >= throwStart) n++;
  return n;
}

static void checkCatches() {
  if (!throwing) return;
  uint32_t now = millis();
  for (auto& s : seen) {
    if (!s.used || !s.lastThrow) continue;
    if (now - s.lastThrow > 1500) continue;            // their throw is live
    if (s.throwRssi < RSSI_CATCH) continue;             // and close by
    if (s.caughtAt && s.caughtAt >= throwStart) continue;  // once per throw
    doCatch(s);
  }
}

static int nearbyList(Seen** out, int cap) {
  uint32_t now = millis();
  int n = 0;
  for (auto& s : seen) {
    if (!s.used || now - s.lastSeen > NEARBY_MS) continue;
    int i = n < cap ? n++ : cap - 1;
    if (i == cap - 1 && n == cap && out[i] && out[i]->rssi >= s.rssi) continue;
    out[i] = &s;
    while (i > 0 && out[i - 1]->rssi < out[i]->rssi) {
      Seen* t = out[i - 1];
      out[i - 1] = out[i];
      out[i] = t;
      --i;
    }
  }
  return n;
}

static int countNearby() {
  uint32_t now = millis();
  int n = 0;
  for (auto& s : seen)
    if (s.used && now - s.lastSeen <= NEARBY_MS) n++;
  return n;
}

static void bootInit() {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
  if (p && esp_ota_set_boot_partition(p) == ESP_OK) {
    G->fillScreen(TFT_BLACK);
    G->setFont(&fonts::FreeSansBold18pt7b);
    G->setTextDatum(middle_center);
    G->setTextColor(TFT_WHITE);
    G->drawString("Back to init()...", CX, CY);
    if (useCanvas) canvas.pushSprite(0, 0);
    delay(500);
    ESP.restart();
  }
  setBanner("Couldn't switch back", 3000);
}

// ---------- drawing ----------
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return M5.Display.color565(r, g, b); }

static void text(const char* s, int y, const lgfx::IFont* font, uint16_t color) {
  G->setFont(font);
  G->setTextColor(color);
  G->setTextDatum(middle_center);
  int dy = abs(y - CY);
  int r = CX - 6;
  int chord = dy < r ? (int)(2 * sqrtf((float)(r * r - dy * dy))) - 48 : 0;
  String str(s);
  if (G->textWidth(str.c_str()) > chord) {
    while (str.length() > 1 && G->textWidth((str + "...").c_str()) > chord) str.remove(str.length() - 1);
    str += "...";
  }
  G->drawString(str.c_str(), CX, y);
}

static void pokeball(int x, int y, int r) {
  G->fillCircle(x, y, r, TFT_WHITE);
  G->fillArc(x, y, r, 0, 180, 360, TFT_RED);
  G->fillRect(x - r, y - r / 9, 2 * r, 2 * (r / 9), TFT_BLACK);
  G->fillCircle(x, y, r / 3, TFT_BLACK);
  G->fillCircle(x, y, r / 4, TFT_WHITE);
  G->drawCircle(x, y, r, rgb(90, 90, 90));
}

static void handleLine(const Card& c, char* out, size_t cap) {
  if (c.handle[0] && c.net[0]) snprintf(out, cap, "@%s  %s", c.handle, c.net);
  else if (c.handle[0]) snprintf(out, cap, "@%s", c.handle);
  else out[0] = 0;
}

static void drawBanner() {
  if (millis() >= bannerUntil) return;
  G->fillRoundRect(48, 188, W - 96, 90, 18, TFT_YELLOW);
  text(banner, 233, &fonts::FreeSansBold12pt7b, TFT_BLACK);
}

static void drawRadar() {
  char line[80];
  text("initdex", 64, &fonts::FreeSansBold18pt7b, TFT_YELLOW);
  snprintf(line, sizeof line, "caught %d  |  nearby %d", dexCount, countNearby());
  text(line, 106, &fonts::FreeSans12pt7b, TFT_WHITE);
  Seen* list[4] = {nullptr, nullptr, nullptr, nullptr};
  int n = nearbyList(list, 4);
  if (n > 4) n = 4;
  if (n == 0) {
    pokeball(CX, 228, 72);
    text(radioOk ? "No trainers nearby yet" : "Radio off", 334, &fonts::FreeSans12pt7b, rgb(150, 150, 150));
  } else {
    for (int i = 0; i < n; ++i) {
      Seen* s = list[i];
      int y = 162 + i * 46;
      int bars = s->rssi > -55 ? 4 : s->rssi > -65 ? 3 : s->rssi > -75 ? 2 : 1;
      bool caught = false;
      for (int k = 0; k < dexCount; ++k)
        if (!memcmp(dex[k].mac, s->mac, 6)) { caught = true; break; }
      snprintf(line, sizeof line, "%s%s", s->wanted ? "* " : "", s->card.name);
      G->setFont(&fonts::FreeSans12pt7b);
      G->setTextDatum(middle_left);
      G->setTextColor(s->wanted ? TFT_YELLOW : (caught ? rgb(140, 200, 255) : TFT_WHITE));
      String nm(line);
      while (nm.length() > 1 && G->textWidth(nm.c_str()) > 250) nm.remove(nm.length() - 1);
      G->drawString(nm.c_str(), 88, y);
      for (int b = 0; b < 4; ++b) {
        int bh = 6 + b * 6;
        G->fillRect(352 + b * 9, y + 12 - bh, 6, bh, b < bars ? TFT_GREEN : rgb(60, 60, 60));
      }
    }
  }
  text(radioOk ? "badges close + both press A" : "restart the badge", 374, &fonts::FreeSans9pt7b, rgb(150, 150, 150));
  text("B: dex & me", 404, &fonts::FreeSans9pt7b, rgb(90, 90, 90));
  drawBanner();
}

static void drawThrow() {
  char line[64];
  uint32_t now = millis();
  int wob = ((now / 170) % 2) ? 16 : -16;
  pokeball(CX + wob, 206, 82);
  text("Throwing...", 326, &fonts::FreeSansBold18pt7b, TFT_WHITE);
  int left = (int)((THROW_MS - (now - throwStart) + 999) / 1000);
  if (left < 0) left = 0;
  snprintf(line, sizeof line, "%ds  -  hold badges close", left);
  text(line, 370, &fonts::FreeSans12pt7b, rgb(150, 150, 150));
  drawBanner();
}

static void drawCelebrate() {
  char line[80];
  if (celebAgain) text("Caught again!", 110, &fonts::FreeSansBold18pt7b, TFT_YELLOW);
  else text("Gotcha!", 110, &fonts::FreeSansBold24pt7b, TFT_YELLOW);
  text(celebCard.name, 198, &fonts::FreeSansBold18pt7b, TFT_WHITE);
  if (celebCard.company[0]) text(celebCard.company, 244, &fonts::FreeSans12pt7b, rgb(170, 170, 170));
  handleLine(celebCard, line, sizeof line);
  if (line[0]) text(line, 284, &fonts::FreeSans12pt7b, rgb(140, 200, 255));
  snprintf(line, sizeof line, "dex: %d", dexCount);
  text(line, 346, &fonts::FreeSans12pt7b, TFT_GREEN);
}

static void drawGotAway() {
  text("It got away...", 196, &fonts::FreeSansBold18pt7b, TFT_WHITE);
  text("both press A within 6 s", 252, &fonts::FreeSans12pt7b, rgb(170, 170, 170));
  text("with badges close together", 286, &fonts::FreeSans12pt7b, rgb(170, 170, 170));
}

static void drawDex() {
  char line[80];
  if (dexCount == 0) {
    text("Dex", 64, &fonts::FreeSansBold18pt7b, TFT_YELLOW);
    pokeball(CX, 214, 60);
    text("No catches yet", 314, &fonts::FreeSansBold12pt7b, TFT_WHITE);
    text("go say hi!", 350, &fonts::FreeSans12pt7b, rgb(150, 150, 150));
  } else {
    if (dexView >= dexCount) dexView = 0;
    const Catch& c = dex[dexView];
    snprintf(line, sizeof line, "Dex  %d/%d", dexView + 1, dexCount);
    text(line, 64, &fonts::FreeSansBold18pt7b, TFT_YELLOW);
    text(c.card.name, 176, &fonts::FreeSansBold18pt7b, TFT_WHITE);
    if (c.card.company[0]) text(c.card.company, 224, &fonts::FreeSans12pt7b, rgb(170, 170, 170));
    handleLine(c.card, line, sizeof line);
    if (line[0]) text(line, 264, &fonts::FreeSans12pt7b, rgb(140, 200, 255));
    clockText(c.t, line, sizeof line);
    text(line, 312, &fonts::FreeSans9pt7b, rgb(150, 150, 150));
  }
  text("A: next   B: me", 396, &fonts::FreeSans9pt7b, rgb(90, 90, 90));
}

static void drawMe() {
  char line[80];
  text(me.name, 58, &fonts::FreeSansBold12pt7b, TFT_WHITE);
  handleLine(me, line, sizeof line);
  text(line[0] ? line : me.company, 90, &fonts::FreeSans9pt7b, rgb(150, 150, 150));
  int q = 216;
  G->fillRoundRect(CX - q / 2 - 10, 114, q + 20, q + 20, 12, TFT_WHITE);
  G->qrcode(PACK_URL, CX - q / 2, 124, q, 3);
  text("scan: get initdex", 374, &fonts::FreeSans12pt7b, TFT_YELLOW);
  text("hold A+B 3s: back to init()", 406, &fonts::FreeSans9pt7b, rgb(90, 90, 90));
}

static void drawConfirm() {
  text("Back to the", 186, &fonts::FreeSansBold18pt7b, TFT_WHITE);
  text("init() badge?", 228, &fonts::FreeSansBold18pt7b, TFT_WHITE);
  text("A: yes    B: no", 296, &fonts::FreeSans12pt7b, TFT_YELLOW);
  text("your catches stay saved", 336, &fonts::FreeSans9pt7b, rgb(150, 150, 150));
}

static void draw() {
  G->fillScreen(TFT_BLACK);
  uint32_t now = millis();
  if (page == CONFIRM_BOOT) drawConfirm();
  else if (now < celebrateUntil) drawCelebrate();
  else if (throwing) drawThrow();
  else if (now < gotAwayUntil) drawGotAway();
  else if (page == DEX) drawDex();
  else if (page == ME) drawMe();
  else drawRadar();
  if (useCanvas) canvas.pushSprite(0, 0);
}

// ---------- input ----------
static void onA() {
  uint32_t now = millis();
  if (now < celebrateUntil) { celebrateUntil = 0; dirty = true; return; }
  switch (page) {
    case RADAR:
      if (!throwing) {
        throwing = true;
        throwStart = now;
        lastThrowSend = 0;
        gotAwayUntil = 0;
        M5.Speaker.tone(880, 60);
      }
      break;
    case DEX:
      if (dexCount) dexView = (dexView + 1) % dexCount;
      break;
    case CONFIRM_BOOT:
      bootInit();
      break;
    default:
      break;
  }
  dirty = true;
}

static void onB() {
  celebrateUntil = 0;
  if (page == CONFIRM_BOOT) page = RADAR;
  else if (throwing) throwing = false;
  else page = page == RADAR ? DEX : page == DEX ? ME : RADAR;
  dirty = true;
}

// ---------- USB JSON ----------
static void reply(JsonDocument& d) {
  serializeJson(d, Serial);
  Serial.println();
}

static void cardJson(JsonObject o, const Card& c) {
  o["name"] = (const char*)c.name;
  o["company"] = (const char*)c.company;
  o["net"] = (const char*)c.net;
  o["handle"] = (const char*)c.handle;
}

static void handleCommand(const String& line) {
  JsonDocument in, out;
  if (deserializeJson(in, line)) {
    out["ok"] = false;
    out["error"] = "bad json";
    reply(out);
    return;
  }
  const char* cmd = in["cmd"] | "";
  if (!strcmp(cmd, "hello")) {
    out["ok"] = true;
    out["app"] = "initdex";
    out["ver"] = INITDEX_VER;
    out["mac"] = macStr(myMac);
    cardJson(out["me"].to<JsonObject>(), me);
    out["caught"] = dexCount;
    out["source"] = meSource;
    out["radio"] = radioOk;
  } else if (!strcmp(cmd, "dex")) {
    out["ok"] = true;
    JsonArray a = out["dex"].to<JsonArray>();
    for (int i = 0; i < dexCount; ++i) {
      JsonObject o = a.add<JsonObject>();
      cardJson(o, dex[i].card);
      o["mac"] = macStr(dex[i].mac);
      o["t"] = dex[i].t;
    }
  } else if (!strcmp(cmd, "set_profile")) {
    Card c;
    memset(&c, 0, sizeof c);
    COPY(c.name, trimmed(in["name"] | "").c_str());
    COPY(c.company, trimmed(in["company"] | "").c_str());
    COPY(c.net, trimmed(in["net"] | "").c_str());
    COPY(c.handle, trimmed(in["handle"] | "").c_str());
    if (!c.name[0]) {
      out["ok"] = false;
      out["error"] = "name required";
    } else {
      me = c;
      prefs.putBytes("card", &me, sizeof me);
      meSource = "override";
      out["ok"] = true;
      dirty = true;
    }
  } else if (!strcmp(cmd, "wanted")) {
    wanted.clear();
    String joined;
    JsonArray names = in["names"].as<JsonArray>();
    for (JsonVariant v : names) {
      std::string w = trimmed(v | "");
      if (w.empty() || (int)wanted.size() >= MAX_WANTED) continue;
      wanted.push_back(w);
      joined += w.c_str();
      joined += '\n';
    }
    prefs.putString("want", joined);
    refreshWantedFlags();
    out["ok"] = true;
    out["count"] = (int)wanted.size();
    dirty = true;
  } else if (!strcmp(cmd, "boot_init")) {
    out["ok"] = true;
    out["rebooting"] = true;
    reply(out);
    Serial.flush();
    delay(200);
    bootInit();
    return;
  } else if (!strcmp(cmd, "clear_dex")) {
    for (int i = 0; i < dexCount; ++i) {
      char k[8];
      snprintf(k, sizeof k, "c%d", i);
      prefs.remove(k);
    }
    dexCount = 0;
    dexView = 0;
    prefs.putInt("n", 0);
    out["ok"] = true;
    dirty = true;
  } else {
    out["ok"] = false;
    out["error"] = "unknown cmd";
  }
  reply(out);
}

static String serialLine;
static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length()) {
        handleCommand(serialLine);
        serialLine = "";
      }
    } else if (serialLine.length() < 3000) {
      serialLine += c;
    }
  }
}

// ---------- main ----------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.setTxBufferSize(4096);  // a full dex reply must not be dropped
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  M5.Display.setBrightness(150);
  W = M5.Display.width();
  H = M5.Display.height();
  CX = W / 2;
  CY = H / 2;
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  useCanvas = canvas.createSprite(W, H) != nullptr;
  G = useCanvas ? (lgfx::LovyanGFX*)&canvas : (lgfx::LovyanGFX*)&M5.Display;

  esp_read_mac(myMac, ESP_MAC_WIFI_STA);
  prefs.begin("initdex", false);
  loadProfile();
  loadDex();
  loadWanted();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onRecv);
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof peer);
    memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    radioOk = esp_now_add_peer(&peer) == ESP_OK;
  }
  M5.Speaker.setVolume(110);
  randomSeed(esp_random());
  dirty = true;
}

void loop() {
  M5.update();
  uint32_t now = millis();
  processRx();
  handleSerial();

  bool a = M5.BtnA.isPressed(), b = M5.BtnB.isPressed();
  if (a && b) {
    if (!bothHeldSince) bothHeldSince = now ? now : 1;
    else if (now - bothHeldSince > 3000 && page != CONFIRM_BOOT) {
      page = CONFIRM_BOOT;
      throwing = false;
      dirty = true;
    }
  } else {
    bothHeldSince = 0;
    if (M5.BtnA.wasClicked()) onA();
    if (M5.BtnB.wasClicked()) onB();
  }
  if (M5.Touch.getCount()) {
    auto t = M5.Touch.getDetail();
    if (t.wasClicked()) {
      if (now < celebrateUntil) celebrateUntil = 0;
      else if (page == DEX && dexCount) dexView = (dexView + 1) % dexCount;
      dirty = true;
    }
  }

  if (throwing) {
    if (now - lastThrowSend >= 200) {
      sendPkt(2);
      lastThrowSend = now;
    }
    checkCatches();
    if (now - throwStart > THROW_MS) {
      throwing = false;
      if (caughtThisThrow() == 0) gotAwayUntil = now + 2200;
      dirty = true;
    }
  } else if (now - lastBeacon >= BEACON_MS + (uint32_t)random(0, 300)) {
    sendPkt(1);
    lastBeacon = now;
  }

  bool animating = throwing || now < bannerUntil || now < celebrateUntil || now < gotAwayUntil;
  static bool wasAnimating = false;
  if (dirty || (animating && now - lastDraw > 170) || (wasAnimating && !animating) ||
      (page == RADAR && now - lastDraw > 1000)) {
    draw();
    lastDraw = now;
    dirty = false;
  }
  wasAnimating = animating;
  delay(5);
}
