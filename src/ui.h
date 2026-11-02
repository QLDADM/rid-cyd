// Interfejs na ekranie dotykowym CYD (poziomo 320x240):
//   pasek górny (stan) | treść | pasek zakładek: Lista / Radar / Pasmo / Ustawienia
//   Lista: do 5 dronów na stronę (ponowne dotknięcie "Lista" = następna strona), dotknięcie = szczegóły
//   Szczegóły: wszystkie dane + przyciski SWOJ / OBCY / BRAK / WSTECZ
//   Radar: drony i operatorzy w skali (bez pozycji własnej - płytka nie ma GPS)
//   Pasmo: zajętość kanałów Wi-Fi 2,4 GHz (z C5 nr 1, bez niego z radia CYD) i 5 GHz (z C5 nr 2), stan czujników
//   Ustawienia: kanał Wi-Fi, jasność, odwrócenie kolorów, dźwięk, zapis SD, kalibracja dotyku
//   Górny pasek: C1 / C2 = czujniki C5 (zielony działa, czerwony zamilkł, szary nie był podłączony)
// Dołączany z main.cpp po drones.h, radio.h, sdlog.h, pcble.h, sensors.h, touch.h.
#pragma once

#include <TFT_eSPI.h>

#define C_BG 0x0000
#define C_PANEL 0x18E3
#define C_LINE 0x3186
#define C_TEXT 0xFFFF
#define C_MUTED 0x8C71
#define C_ACCENT 0x3D7F
#define C_OWN 0x2E89
#define C_FOREIGN 0xF8A2
#define C_UNKNOWN 0xFDA0
#define TOP_H 20
#define TAB_Y 210
#define ROW_H 37
#define ROWS 5
#define UI_DRONE_AGE_MS 300000UL  // lista: drony z ostatnich 5 min
#define UI_STALE_MS 15000

enum Screen : uint8_t { SCR_LIST, SCR_DETAIL, SCR_RADAR, SCR_SETTINGS, SCR_CALIB, SCR_BAND };

static TFT_eSPI tft;
static TFT_eSprite rowSpr(&tft);
static Screen screen = SCR_LIST;
static uint8_t listPage = 0;
static char selId[21] = "";  // wybrany dron (ID albo MAC)
static uint8_t selMac[6];
static uint8_t brightIdx = 0;
static const uint8_t BRIGHT[] = {255, 150, 70, 20};
static bool invertColors = false, soundOn = true;
static bool uiDirty = true;
static uint8_t calStep = 0;
static int calRaw[6];  // 3 punkty kalibracji: (20,20), (300,20), (20,220)
static const int CAL_X[3] = {20, 300, 20}, CAL_Y[3] = {20, 20, 220};

static uint16_t tagColor(uint8_t t) { return t == TAG_OWN ? C_OWN : t == TAG_FOREIGN ? C_FOREIGN : C_UNKNOWN; }

static int listDrones(DroneInfo **out) {
  uint32_t now = millis();
  int n = 0;
  for (auto &d : drones)
    if (d.used && now - d.lastMs < UI_DRONE_AGE_MS) out[n++] = &d;
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && out[j]->lastMs > out[j - 1]->lastMs; j--) {
      DroneInfo *t = out[j];
      out[j] = out[j - 1];
      out[j - 1] = t;
    }
  return n;
}

static DroneInfo *selectedDrone() {
  for (auto &d : drones)
    if (d.used && (selId[0] ? !strcmp(d.id, selId) : macIn(d, selMac))) return &d;
  return nullptr;
}

static String ageText(uint32_t ms) {
  uint32_t s = (millis() - ms) / 1000;
  return s < 120 ? String(s) + " s" : String(s / 60) + " min";
}

static String clockText() {
  uint32_t now = unixNow();
  if (!now) return "--:--";
  time_t t = now;
  struct tm tmv;
  localtime_r(&t, &tmv);
  char b[8];
  strftime(b, sizeof(b), "%H:%M", &tmv);
  return b;
}

static void drawTopBar(int count) {
  tft.fillRect(0, 0, 320, TOP_H, C_PANEL);
  tft.setTextColor(C_ACCENT, C_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("RID CYD", 4, 2, 2);
  if (blePairing) {  // telefon albo komputer się paruje - PIN do wpisania
    tft.setTextColor(C_ACCENT, C_PANEL);
    tft.drawString("PIN " + String(pcPin), 76, 2, 2);
  } else {
    tft.setTextColor(C_TEXT, C_PANEL);
    tft.drawString("Drony: " + String(count), 76, 2, 2);
  }
  tft.setTextColor(C_TEXT, C_PANEL);
  tft.setTextDatum(TR_DATUM);
  tft.setTextDatum(TL_DATUM);
  for (int s = 0; s < SENS_N; s++) {  // czujniki C5
    tft.setTextColor(sensOnline(s) ? C_OWN : sens[s].lastMs ? C_FOREIGN : C_LINE, C_PANEL);
    tft.drawString("C" + String(s + 1), 156 + s * 22, 2, 2);
  }
  tft.setTextColor(C_TEXT, C_PANEL);
  tft.setTextDatum(TR_DATUM);
  String right = String(bleEnabled ? (hostBleOn ? "PC " : "") : "WiFi ") + (sdOk ? (sdEnabled ? "SD " : "sd- ") : "") + "CH" + String(currentChannel) +
                 (fixedChannel ? " " : "* ") + clockText();
  tft.drawString(right, 316, 2, 2);
}

static const Screen TAB_SCREENS[] = {SCR_LIST, SCR_RADAR, SCR_BAND, SCR_SETTINGS};

static void drawTabs() {
  static const char *TABS[] = {"Lista", "Radar", "Pasmo", "Ustawienia"};
  for (int i = 0; i < 4; i++) {
    bool sel = screen == TAB_SCREENS[i];
    tft.fillRect(i * 80, TAB_Y, 79, 30, sel ? C_ACCENT : C_PANEL);
    tft.setTextColor(sel ? C_BG : C_TEXT);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(TABS[i], i * 80 + 40, TAB_Y + 15, 2);
  }
}

static void drawButtonRow(const char *const *labels, const uint16_t *colors, int n) {
  int w = 320 / n;
  for (int i = 0; i < n; i++) {
    tft.fillRect(i * w, TAB_Y, w - 1, 30, colors[i]);
    tft.setTextColor(C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(labels[i], i * w + w / 2, TAB_Y + 15, 2);
  }
}

// ---------------------------------------------------------------- lista

static void drawList() {
  DroneInfo *list[MAX_DRONES];
  int n = listDrones(list);
  int pages = max(1, (n + ROWS - 1) / ROWS);
  if (listPage >= pages) listPage = 0;
  uint32_t now = millis();
  for (int r = 0; r < ROWS; r++) {
    int i = listPage * ROWS + r;
    rowSpr.fillSprite(C_BG);
    if (i < n) {
      const DroneInfo &d = *list[i];
      uint8_t tag = tagStatus(d.id);
      bool stale = now - d.lastMs > UI_STALE_MS;
      rowSpr.fillRect(0, 2, 6, ROW_H - 4, tagColor(tag));
      rowSpr.setTextColor(stale ? C_MUTED : C_TEXT, C_BG);
      rowSpr.setTextDatum(TL_DATUM);
      String id = d.id[0] ? String(d.id) : "bez ID";
      rowSpr.drawString(id, 12, 2, 2);
      rowSpr.setTextDatum(TR_DATUM);
      rowSpr.drawString(String(d.rssi) + " dBm", 316, 2, 2);
      String l2 = "wys " + (isnan(d.height) ? String("-") : String((int)d.height) + " m");
      if (!isnan(d.speed)) l2 += "  " + String(d.speed, 0) + " m/s";
      if (d.hasOp && d.hasLoc) l2 += "  op " + String((int)distM(d.lat, d.lon, d.opLat, d.opLon)) + " m";
      if (!d.hasLoc) l2 += "  brak pozycji";
      if (d.wifiKind) l2 = String("Wi-Fi: ") + d.wifiKind + "  kanal " + String(d.wifiCh);
      rowSpr.setTextColor(stale ? C_MUTED : C_MUTED, C_BG);
      rowSpr.setTextDatum(TL_DATUM);
      rowSpr.drawString(l2, 12, 19, 2);
      rowSpr.setTextDatum(TR_DATUM);
      rowSpr.drawString(ageText(d.lastMs), 316, 19, 2);
      rowSpr.drawFastHLine(0, ROW_H - 1, 320, C_LINE);
    } else if (i == 0) {
      rowSpr.setTextColor(C_MUTED, C_BG);
      rowSpr.setTextDatum(MC_DATUM);
      rowSpr.drawString("Brak dronow w zasiegu - nasluch trwa", 160, ROW_H / 2, 2);
    }
    rowSpr.pushSprite(0, TOP_H + 2 + r * ROW_H);
  }
  if (pages > 1) {
    tft.setTextColor(C_MUTED, C_BG);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(String(listPage + 1) + "/" + String(pages), 316, TAB_Y - 12, 1);
  }
}

// ---------------------------------------------------------------- szczegóły

static void detailLine(int row, const String &text, uint16_t color = C_TEXT) {
  int y = TOP_H + 4 + row * 23;
  tft.fillRect(0, y, 320, 22, C_BG);
  tft.setTextColor(color, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(text, 6, y + 3, 2);
}

static void drawDetail() {
  DroneInfo *d = selectedDrone();
  if (!d) {
    screen = SCR_LIST;
    return;
  }
  static const char *STATUS[] = {"nieznany", "na ziemi", "w powietrzu", "AWARIA"};
  uint8_t tag = tagStatus(d->id);
  static const char *LABELS[] = {"SWOJ", "OBCY", "BRAK", "WSTECZ"};
  static const uint16_t COLORS[] = {C_OWN, C_FOREIGN, C_MUTED, C_ACCENT};
  if (d->wifiKind) {  // dron Wi-Fi bez Remote ID
    const uint8_t *m = d->macs[0];
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    detailLine(0, String(d->id), C_ACCENT);
    detailLine(1, String("Oznaczenie: ") + (tag == TAG_OWN ? "SWOJ" : tag == TAG_FOREIGN ? "OBCY" : "nieoznaczony"), tagColor(tag));
    detailLine(2, String("Dron Wi-Fi: ") + d->wifiKind);
    detailLine(3, "RSSI " + String(d->rssi) + " dBm, kanal " + String(d->wifiCh) + ", " + ageText(d->lastMs) + " temu");
    detailLine(4, String("MAC ") + mac);
    detailLine(5, "Rozpoznany po nazwie sieci / producencie,", C_MUTED);
    detailLine(6, "nie nadaje Remote ID - brak pozycji.", C_MUTED);
    detailLine(7, "Widziany od " + ageText(d->firstMs) + " temu", C_MUTED);
    drawButtonRow(LABELS, COLORS, 4);
    return;
  }
  detailLine(0, d->id[0] ? String(d->id) : String("bez ID"), C_ACCENT);
  String l1 = String("Oznaczenie: ") + (tag == TAG_OWN ? "SWOJ" : tag == TAG_FOREIGN ? "OBCY" : "nieoznaczony");
  if (d->euClass >= 1 && d->euClass <= 7) l1 += "  C" + String(d->euClass - 1);
  if (!isnan(d->vspeed)) l1 += "  pion " + String(d->vspeed, 1) + " m/s";
  detailLine(1, l1, tagColor(tag));
  detailLine(2, "Status: " + String(STATUS[d->status & 3]) + "   RSSI " + String(d->rssi) + " dBm, " + ageText(d->lastMs) +
                    " temu");
  String l3 = "Wys: " + (isnan(d->height) ? String("-") : String(d->height, 0) + " m");
  if (!isnan(d->altGeo)) l3 += " (npm " + String(d->altGeo, 0) + " m)";
  if (!isnan(d->speed)) l3 += "  " + String(d->speed, 1) + " m/s";
  if (d->track >= 0) l3 += "  kurs " + String(d->track);
  detailLine(3, l3);
  detailLine(4, d->hasLoc ? "Dron: " + String(d->lat * 1e-7, 6) + ", " + String(d->lon * 1e-7, 6) : String("Dron: brak pozycji"));
  detailLine(5, d->hasOp ? "Operator: " + String(d->opLat * 1e-7, 6) + ", " + String(d->opLon * 1e-7, 6) +
                               (d->hasLoc ? "  " + String((int)distM(d->lat, d->lon, d->opLat, d->opLon)) + " m" : String(""))
                         : String("Operator: brak pozycji"));
  String l6 = d->opId[0] ? String("Nr operatora: ") + d->opId : String("Nr operatora: brak");
  if (!isnan(d->maxHeight)) l6 += "  maks " + String(d->maxHeight, 0) + " m";
  detailLine(6, l6);
  detailLine(7, "Odbior: " + viaText(*d) + ", widziany od " + ageText(d->firstMs) + " temu", C_MUTED);
  drawButtonRow(LABELS, COLORS, 4);
}

// ---------------------------------------------------------------- radar

static void drawRadar() {
  tft.fillRect(0, TOP_H, 320, TAB_Y - TOP_H, C_BG);
  DroneInfo *list[MAX_DRONES];
  int n = listDrones(list);
  int32_t minLat = INT32_MAX, maxLat = INT32_MIN, minLon = INT32_MAX, maxLon = INT32_MIN;
  int pts = 0;
  for (int i = 0; i < n; i++) {
    const DroneInfo &d = *list[i];
    if (d.hasLoc) { minLat = min(minLat, d.lat); maxLat = max(maxLat, d.lat); minLon = min(minLon, d.lon); maxLon = max(maxLon, d.lon); pts++; }
    if (d.hasOp) { minLat = min(minLat, d.opLat); maxLat = max(maxLat, d.opLat); minLon = min(minLon, d.opLon); maxLon = max(maxLon, d.opLon); pts++; }
  }
  tft.setTextDatum(MC_DATUM);
  if (!pts) {
    tft.setTextColor(C_MUTED, C_BG);
    tft.drawString("Brak pozycji dronow i operatorow", 160, 115, 2);
    return;
  }
  // skala: metry na piksel, obszar 300x170, co najmniej 200 m
  float cosLat = cosf((minLat + maxLat) * 0.5e-7f * DEG_TO_RAD);
  float wM = (maxLon - minLon) * 1e-7f * 111320.0f * cosLat, hM = (maxLat - minLat) * 1e-7f * 111320.0f;
  float span = max(max(wM / 300.0f, hM / 170.0f), 200.0f / 170.0f) * 1.25f;  // m/px z marginesem
  int32_t cLat = (minLat + maxLat) / 2, cLon = (minLon + maxLon) / 2;
  auto px = [&](int32_t lat, int32_t lon, int &x, int &y) {
    x = 160 + (int)((lon - cLon) * 1e-7f * 111320.0f * cosLat / span);
    y = 115 - (int)((lat - cLat) * 1e-7f * 111320.0f / span);
  };
  for (int i = 0; i < n; i++) {
    const DroneInfo &d = *list[i];
    int dx = 0, dy = 0, ox = 0, oy = 0;
    if (d.hasLoc) px(d.lat, d.lon, dx, dy);
    if (d.hasOp) px(d.opLat, d.opLon, ox, oy);
    if (d.hasLoc && d.hasOp) tft.drawLine(dx, dy, ox, oy, C_LINE);
    if (d.hasOp) tft.fillRect(ox - 3, oy - 3, 7, 7, C_UNKNOWN);
    if (d.hasLoc) {
      uint16_t c = tagColor(tagStatus(d.id));
      if (d.track >= 0) {  // trójkąt w kierunku lotu
        float a = d.track * DEG_TO_RAD;
        tft.fillTriangle(dx + 9 * sinf(a), dy - 9 * cosf(a), dx + 6 * sinf(a + 2.5f), dy - 6 * cosf(a + 2.5f),
                         dx + 6 * sinf(a - 2.5f), dy - 6 * cosf(a - 2.5f), c);
      } else {
        tft.fillCircle(dx, dy, 5, c);
      }
      tft.setTextColor(C_TEXT, C_BG);
      tft.setTextDatum(ML_DATUM);
      String sid = d.id[0] ? String(d.id + max(0, (int)strlen(d.id) - 6)) : String("?");
      tft.drawString(sid + (isnan(d.height) ? "" : " " + String((int)d.height) + "m"), dx + 10, dy, 1);
    }
  }
  // podziałka
  int barM = span * 80 > 1000 ? 500 : span * 80 > 400 ? 200 : span * 80 > 150 ? 100 : 50;
  int barPx = barM / span;
  tft.drawFastHLine(8, TAB_Y - 10, barPx, C_TEXT);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextDatum(BL_DATUM);
  tft.drawString(String(barM) + " m", 10, TAB_Y - 12, 1);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("N^  kwadrat = operator", 316, TOP_H + 4, 1);
}

// ---------------------------------------------------------------- pasmo: Wi-Fi 2,4 GHz i 5 GHz

static uint16_t rssiColor(int8_t r) { return r >= -55 ? C_FOREIGN : r >= -70 ? C_UNKNOWN : C_OWN; }

// słupki kanałów, skala logarytmiczna 1 / 10 / 100 / 1000 ramek/s, kolor = najsilniejszy sygnał
static void drawBandBars(int top, int hMax, const uint8_t *ch, const uint16_t *rate, const int8_t *peak, int n, int curCh) {
  if (n <= 0) return;
  int w = min(24, 312 / n), x0 = 4 + (312 - w * n) / 2, base = top + hMax;
  tft.setTextDatum(TC_DATUM);
  for (int i = 0; i < n; i++) {
    int x = x0 + i * w;
    int h = rate[i] ? min(hMax, (int)(log10f(rate[i] + 1) * hMax / 3)) : 0;
    tft.drawRect(x, top, w - 2, hMax, C_LINE);
    if (h) tft.fillRect(x + 1, base - h, w - 4, h, peak[i] > -128 ? rssiColor(peak[i]) : C_MUTED);
    if (w >= 18 || i % 2 == 0) {  // przy wąskich słupkach (5 GHz) co drugi numer kanału
      tft.setTextColor(ch[i] == curCh ? C_ACCENT : C_MUTED, C_BG);
      tft.drawString(String(ch[i]), x + (w - 2) / 2, base + 2, 1);
    }
  }
}

static void bandTitle(int y, const String &left, const String &right, uint16_t color = C_TEXT) {
  tft.setTextColor(color, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(left, 4, y, 1);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(right, 316, y, 1);
}

static String sensStatusText(int s) {
  const SensorState &st = sens[s];
  String t = "C5 nr " + String(s + 1) + (s ? " (5 GHz, GPIO27): " : " (2,4 GHz, GPIO35): ");
  if (!st.lastMs) return t + "nie podlaczony";
  if (!sensOnline(s)) return t + "cisza od " + ageText(st.lastMs);
  return t + "kanal " + String(st.ch) + ", RID " + String(st.rid) + (st.bad ? ", bledy " + String(st.bad) : String(""));
}

static void drawBand() {
  static const uint8_t CH24[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  tft.fillRect(0, TOP_H, 320, TAB_Y - TOP_H, C_BG);
  uint32_t now = millis();
  // 2,4 GHz: z C5 nr 1 (antena zewnętrzna), bez niego z własnego radia CYD
  if (bandFresh(band24) && band24.src) {
    bandTitle(TOP_H + 3, "Wi-Fi 2,4 GHz - C5 nr " + String(band24.src), String((now - band24.ms) / 1000) + " s temu");
    drawBandBars(TOP_H + 14, 46, band24.ch, band24.rate, band24.peak, band24.n, sens[band24.src - 1].ch);
  } else {
    bandTitle(TOP_H + 3, "Wi-Fi 2,4 GHz - radio CYD", "bez C5 nr 1");
    drawBandBars(TOP_H + 14, 46, CH24, chRate + 1, chRssi + 1, 13, currentChannel);
  }
  // 5 GHz: tylko z C5 nr 2 (ESP32 w CYD nie ma 5 GHz)
  int y5 = TOP_H + 76;
  tft.drawFastHLine(0, y5 - 3, 320, C_LINE);
  if (band5.ms) {
    bool fresh = bandFresh(band5);
    bandTitle(y5, "Wi-Fi 5 GHz - C5 nr " + String(band5.src), (fresh ? "" : "stare, ") + String((now - band5.ms) / 1000) + " s temu",
              fresh ? C_TEXT : C_MUTED);
    drawBandBars(y5 + 11, 46, band5.ch, band5.rate, band5.peak, band5.n, band5.src ? sens[band5.src - 1].ch : 0);
  } else {
    bandTitle(y5, "Wi-Fi 5 GHz", "");
    tft.setTextColor(C_MUTED, C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Brak danych - C5 nr 2 podlacza sie do GPIO27", 160, y5 + 34, 2);
  }
  // stan czujników
  int ys = TOP_H + 152;
  tft.drawFastHLine(0, ys - 3, 320, C_LINE);
  for (int s = 0; s < SENS_N; s++) bandTitle(ys + s * 12, sensStatusText(s), "", sensOnline(s) ? C_TEXT : C_MUTED);
  bandTitle(ys + 24, "Slupek: ramki/s (log), kolor: najsilniejszy sygnal", "");
}

// ---------------------------------------------------------------- ustawienia

#define SET_ROWS 7
#define SET_ROW_H 26
static String settingText(int i) {
  switch (i) {
    case 0: return String("Kanal Wi-Fi: ") + (fixedChannel ? "staly 6" : "skakanie");
    case 1: return "Jasnosc ekranu: " + String(BRIGHT[brightIdx] * 100 / 255) + "%";
    case 2: return String("Odwroc kolory: ") + (invertColors ? "tak" : "nie");
    case 3: return String("Dzwiek (glosnik): ") + (soundOn ? "wl." : "wyl.");
    case 5: return bleEnabled ? "Bluetooth: wl.  PIN " + String(pcPin) + (hostBleOn ? "  PC" : "") + (phoneSubscribed ? "  tel." : "") : String("Bluetooth: wyl. (tylko Wi-Fi)");
    case 4:
      return String("Zapis na karte SD: ") + (!sdOk ? "brak karty" : sdEnabled ? "wl. (" + String(sdLines) + " lin., " + String(sdFreeMB()) + " MB wolne)" : "wyl.");
    default: return "Kalibracja dotyku";
  }
}

static void drawSettings() {
  for (int i = 0; i < SET_ROWS; i++) {
    int y = TOP_H + 3 + i * SET_ROW_H;
    tft.fillRect(0, y, 320, SET_ROW_H - 2, C_PANEL);
    tft.setTextColor(C_TEXT, C_PANEL);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(settingText(i), 8, y + SET_ROW_H / 2 - 1, 2);
  }
  tft.setTextColor(C_MUTED, C_PANEL);  // wersja w wierszu kalibracji, po prawej
  tft.setTextDatum(MR_DATUM);
  tft.drawString("wersja " FW_VERSION, 312, TOP_H + 3 + (SET_ROWS - 1) * SET_ROW_H + SET_ROW_H / 2 - 1, 2);
}

// ---------------------------------------------------------------- kalibracja dotyku

static void drawCalib() {
  tft.fillScreen(C_BG);
  int x = CAL_X[calStep], y = CAL_Y[calStep];
  tft.drawLine(x - 10, y, x + 10, y, C_FOREIGN);
  tft.drawLine(x, y - 10, x, y + 10, C_FOREIGN);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Dotknij srodka krzyzyka", 160, 110, 2);
  tft.drawString("(" + String(calStep + 1) + " z 3)", 160, 130, 2);
}
