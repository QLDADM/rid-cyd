// Wykrywacz Remote ID na płytce ESP32-2432S028 ("Cheap Yellow Display").
//
// Odbiera ramki Open Drone ID (Wi-Fi Beacon, Wi-Fi NAN, BLE 4), dekoduje je na płytce
// i pokazuje na ekranie dotykowym; zapisuje ramki na kartę microSD. Przez USB (921600 bod)
// mówi tym samym protokołem co sensor Heltec, więc agent RID Decoder obsługuje go
// automatycznie (na stronie: "wykryto: CYD").
// Do tego drony Wi-Fi bez Remote ID (wifidrone.h), zajętość kanałów Wi-Fi i - gdy podłączony jest
// moduł nRF24L01 - analizator pasma 2,4 GHz wykrywający aparatury FPV (nrf24.h, zakładka "Pasmo").
// Agent może się też łączyć przez Bluetooth zamiast kabla (pcble.h), z tym samym protokołem.
//
// Polecenia przez port szeregowy: ID, CH 0..13, TIME <unix>, PING, TAG <id> own|foreign|-,
// TAGCLR, TAGS, TAGPEND, TAGPENDCLR, DUMP (pusty - zapis jest na karcie SD).

#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>

#define PIN_LED_R 4
#define PIN_LED_G 16
#define PIN_LED_B 17
#define PIN_SPEAKER 26
#define CH_BACKLIGHT 0
#define CH_SPEAKER 2

enum Transport : uint8_t { T_BEACON, T_NAN, T_BLE };
static const char *TRANSPORT_NAMES[] = {"wifi_beacon", "wifi_nan", "ble"};

struct Frame {
  uint8_t transport;
  uint8_t channel;
  int8_t rssi;
  uint8_t mac[6];
  uint16_t len;
  uint8_t data[256];
};

static QueueHandle_t frameQueue;
static volatile uint32_t counts[3] = {0, 0, 0};
static volatile uint32_t dropped = 0;
static Preferences prefs;
static uint16_t bootId = 0;

#define FW_VERSION "1.6"  // wspólny numer dla Heltec, CYD i M5 - nowy tylko przy dużych zmianach
#include "drones.h"
#include "wifidrone.h"
#include "hostout.h"  // od tego miejsca Serial = USB + kanał PC przez BLE

static void enqueue(uint8_t transport, const uint8_t *mac, int8_t rssi, const uint8_t *data, size_t len);

// ---------------------------------------------------------------- oznaczenia (jak w Heltec)

#define MAX_TAG_PENDING 16
static TagEntry tagPending[MAX_TAG_PENDING];  // zmiany z ekranu do przeniesienia na stronę
static uint8_t tagPendingCount = 0;

static void tagsLoad() {
  if (prefs.getBytes("tags", tags, sizeof(tags)) != sizeof(tags)) memset(tags, 0, sizeof(tags));
}
static void tagsSave() { prefs.putBytes("tags", tags, sizeof(tags)); }

static void tagSet(const char *id, uint8_t st) {
  if (!id[0] || strlen(id) > 20) return;
  TagEntry *slot = nullptr;
  for (auto &t : tags) {
    if (t.status && !strcmp(t.id, id)) { slot = &t; break; }
    if (!t.status && !slot) slot = &t;
  }
  if (!slot) return;
  strlcpy(slot->id, id, sizeof(slot->id));
  slot->status = st;
}

static void tagRecordPending(const char *id, uint8_t st) {
  for (int i = 0; i < tagPendingCount; i++)
    if (!strcmp(tagPending[i].id, id)) { tagPending[i].status = st; return; }
  if (tagPendingCount < MAX_TAG_PENDING) {
    strlcpy(tagPending[tagPendingCount].id, id, sizeof(tagPending[0].id));
    tagPending[tagPendingCount++].status = st;
  }
}

static void printTags(bool pending) {
  Serial.print(pending ? "{\"tagpend\":{" : "{\"tags\":{");
  bool first = true;
  int count = pending ? tagPendingCount : MAX_TAGS;
  const TagEntry *list = pending ? tagPending : tags;
  for (int i = 0; i < count; i++) {
    if (!pending && !list[i].status) continue;
    const char *st = list[i].status == TAG_OWN ? "own" : list[i].status == TAG_FOREIGN ? "foreign" : "-";
    Serial.printf("%s\"%s\":\"%s\"", first ? "" : ",", list[i].id, st);
    first = false;
  }
  Serial.println("}}");
}

#include "radio.h"

static void enqueue(uint8_t transport, const uint8_t *mac, int8_t rssi, const uint8_t *data, size_t len) {
  if (len > sizeof(Frame::data)) return;
  Frame f;
  f.transport = transport;
  f.channel = transport == T_BLE ? 0 : currentChannel;
  f.rssi = rssi;
  memcpy(f.mac, mac, 6);
  f.len = len;
  memcpy(f.data, data, len);
  if (xQueueSend(frameQueue, &f, 0) == pdTRUE) counts[transport]++;
  else dropped++;
}

#include "nrf24.h"
#include "sdlog.h"
#include "pcble.h"
#include "touch.h"
#include "ui.h"

// ---------------------------------------------------------------- sygnalizacja

static void led(bool r, bool g, bool b) {  // dioda RGB aktywna stanem niskim
  digitalWrite(PIN_LED_R, !r);
  digitalWrite(PIN_LED_G, !g);
  digitalWrite(PIN_LED_B, !b);
}

static uint32_t ledUntil = 0;

static void beep(uint16_t hz, uint16_t ms) {
  if (!soundOn) return;
  ledcWriteTone(CH_SPEAKER, hz);
  delay(ms);
  ledcWriteTone(CH_SPEAKER, 0);
}

static void alertNewDrone(const DroneInfo &d) {
  uint8_t tag = tagStatus(d.id);
  if (tag == TAG_OWN) {
    led(false, true, false);
  } else {
    led(true, false, false);
    beep(1800, 80);
    delay(60);
    beep(1800, 80);
  }
  ledUntil = millis() + 5000;
  uiDirty = true;
}

static void applyDisplay() {
  if (!nrfPresent) ledcWrite(CH_BACKLIGHT, BRIGHT[brightIdx]);  // z nRF24 pin podświetlenia jest jego CSN
  tft.invertDisplay(invertColors);
}

static void saveSettings() {
  prefs.putUChar("bright", brightIdx);
  prefs.putBool("inv", invertColors);
  prefs.putBool("sound", soundOn);
  prefs.putBool("sd", sdEnabled);
  prefs.putBool("ble", bleEnabled);
  prefs.putBytes("tcal", &touchCal, sizeof(touchCal));
}

// ---------------------------------------------------------------- USB (protokół jak Heltec)

static void printFrame(const Frame &f) {
  static const char hex[] = "0123456789abcdef";
  static char line[700];
  int n = snprintf(line, sizeof(line),
                   "{\"t\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,\"ch\":%u,\"dev\":\"cyd\",\"d\":\"",
                   TRANSPORT_NAMES[f.transport], f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5], f.rssi,
                   f.channel);
  for (size_t i = 0; i < f.len && n < (int)sizeof(line) - 4; i++) {
    line[n++] = hex[f.data[i] >> 4];
    line[n++] = hex[f.data[i] & 0x0F];
  }
  line[n++] = '"';
  line[n++] = '}';
  line[n] = 0;
  Serial.println(line);
}

static unsigned nvsFree() {  // wolne wpisy w pamięci ustawień (tam też trafiają parowania Bluetooth)
  nvs_stats_t st;
  return nvs_get_stats(nullptr, &st) == ESP_OK ? (unsigned)st.free_entries : 0;
}

static void printStatus() {
  Serial.printf("{\"status\":{\"dev\":\"cyd\",\"beacon\":%u,\"nan\":%u,\"ble\":%u,\"dropped\":%u,\"ch\":%u,"
                "\"all_beacon\":%u,\"all_ble\":%u,\"ble_on\":%s,\"sd\":%s,\"sd_lines\":%u,\"sd_free_mb\":%u,\"boot\":%u,\"up\":%u,"
                "\"wifi_drone\":%u,\"nrf24\":%s,\"pin\":%u,\"ble_name\":\"%s\",\"pc_ble\":%s,\"bonds\":%d,\"nvs_free\":%u}}\n",
                counts[T_BEACON], counts[T_NAN], counts[T_BLE], dropped, currentChannel, allBeacon, allBle,
                bleEnabled ? "true" : "false", sdOk ? (sdEnabled ? "true" : "false") : "null", sdLines, sdFreeMB(), bootId, millis() / 1000,
                wdSeen, nrfPresent ? "true" : "false", pcPin, pcName, hostBleOn ? "true" : "false", NimBLEDevice::getNumBonds(), nvsFree());
}

static void handleSerialCommand() {
  static char buf[64];
  static size_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (n < sizeof(buf) - 1) buf[n++] = c;
      continue;
    }
    buf[n] = 0;
    if (n) hostCmdMs = millis();
    if (!strcasecmp(buf, "ID")) {
      Serial.println("{\"info\":\"RID sensor CYD\",\"fw\":\"" FW_VERSION "\",\"dev\":\"cyd\"}");
      printStatus();
    } else if (!strncasecmp(buf, "TIME ", 5)) {
      uint32_t t = strtoul(buf + 5, nullptr, 10);
      if (t > 1700000000UL) unixBase = t - millis() / 1000;
    } else if (!strncasecmp(buf, "TAG ", 4)) {
      char *st = strchr(buf + 4, ' ');
      if (st) {
        *st++ = 0;
        tagSet(buf + 4, !strcasecmp(st, "own") ? TAG_OWN : !strcasecmp(st, "foreign") ? TAG_FOREIGN : TAG_NONE);
        tagsSave();
        uiDirty = true;
      }
    } else if (!strcasecmp(buf, "TAGCLR")) {
      memset(tags, 0, sizeof(tags));
      tagsSave();
      uiDirty = true;
    } else if (!strcasecmp(buf, "TAGS")) {
      printTags(false);
    } else if (!strcasecmp(buf, "TAGPEND")) {
      printTags(true);
    } else if (!strcasecmp(buf, "BONDCLR")) {  // usunięcie sparowanych telefonów i komputerów
      NimBLEDevice::deleteAllBonds();
      Serial.println("{\"info\":\"sparowane urzadzenia usuniete\",\"dev\":\"cyd\"}");
    } else if (!strcasecmp(buf, "TAGPENDCLR")) {
      tagPendingCount = 0;
    } else if (!strcasecmp(buf, "TCAL")) {  // kalibracja dotyku na żądanie
      screen = SCR_CALIB;
      calStep = 0;
      drawCalib();
    } else if (!strcasecmp(buf, "TRAW")) {  // diagnostyka dotyku: surowe odczyty przez 20 s
      for (int i = 0; i < 100; i++) {
        int irq = digitalRead(T_IRQ);
        digitalWrite(T_CS, LOW);
        int z1 = touchXfer(0xB0), z2 = touchXfer(0xC0), x = touchXfer(0xD0), y = touchXfer(0x90);
        digitalWrite(T_CS, HIGH);
        Serial.printf("{\"traw\":{\"irq\":%d,\"z1\":%d,\"z2\":%d,\"z\":%d,\"x\":%d,\"y\":%d}}\n", irq, z1, z2,
                      z1 + 4095 - z2, x, y);
        delay(200);
      }
    } else if (!strcasecmp(buf, "DUMP")) {  // pamięć wewnętrzna nie jest używana - zapis na SD
      Serial.printf("{\"dump_begin\":{\"records\":0,\"boot\":%u,\"up\":%u,\"fs\":false,\"boots\":{}}}\n", bootId, millis() / 1000);
      Serial.println("{\"dump_end\":{\"records\":0}}");
    } else if (n >= 3 && !strncasecmp(buf, "CH", 2)) {
      int ch = atoi(buf + 2);
      if (ch >= 0 && ch <= 13) fixedChannel = ch;
    }
    n = 0;
  }
}

// ---------------------------------------------------------------- dotyk

static void onTap(int x, int y) {
  if (screen == SCR_CALIB) {
    int rx, ry;
    if (!touchRaw(rx, ry)) return;
    calRaw[calStep * 2] = rx;
    calRaw[calStep * 2 + 1] = ry;
    if (++calStep < 3) {
      drawCalib();
      return;
    }
    // punkt 1 -> 2 zmienia tylko x ekranu: oś panelu, która zmieniła się bardziej, to oś X ekranu
    uint8_t swap = abs(calRaw[3] - calRaw[1]) > abs(calRaw[2] - calRaw[0]);
    int ax0 = swap ? calRaw[1] : calRaw[0], ax1 = swap ? calRaw[3] : calRaw[2];  // punkty 1 i 2
    int by0 = swap ? calRaw[0] : calRaw[1], by2 = swap ? calRaw[4] : calRaw[5];  // punkty 1 i 3
    float sx = (ax1 - ax0) / 280.0f, sy = (by2 - by0) / 200.0f;
    touchCal = {(int16_t)(ax0 - 20 * sx), (int16_t)(ax0 + 299 * sx), (int16_t)(by0 - 20 * sy), (int16_t)(by0 + 219 * sy), swap};
    saveSettings();
    screen = SCR_SETTINGS;
    tft.fillScreen(C_BG);
    uiDirty = true;
    return;
  }
  if (y >= TAB_Y) {  // dolny pasek
    if (screen == SCR_DETAIL) {
      int b = x / 80;
      DroneInfo *d = selectedDrone();
      if (b < 3 && d && d->id[0]) {
        uint8_t st = b == 0 ? TAG_OWN : b == 1 ? TAG_FOREIGN : TAG_NONE;
        tagSet(d->id, st);
        tagsSave();
        tagRecordPending(d->id, st);  // trafi na stronę RID Decoder przy podłączeniu do agenta
      } else if (b == 3) {
        screen = SCR_LIST;
      }
    } else {
      Screen next = TAB_SCREENS[min(x / 80, 3)];
      if (next == SCR_LIST && screen == SCR_LIST) listPage++;  // kolejna strona listy
      screen = next;
    }
    tft.fillRect(0, TOP_H, 320, TAB_Y - TOP_H, C_BG);
    uiDirty = true;
    return;
  }
  if (y < TOP_H) return;
  if (screen == SCR_LIST) {
    DroneInfo *list[MAX_DRONES];
    int n = listDrones(list);
    int i = listPage * ROWS + (y - TOP_H - 2) / ROW_H;
    if (i < n) {
      strlcpy(selId, list[i]->id, sizeof(selId));
      memcpy(selMac, list[i]->macs[0], 6);
      screen = SCR_DETAIL;
      tft.fillRect(0, TOP_H, 320, TAB_Y - TOP_H, C_BG);
      uiDirty = true;
    }
  } else if (screen == SCR_SETTINGS) {
    int i = (y - TOP_H - 3) / SET_ROW_H;
    if (i == 0) fixedChannel = fixedChannel ? 0 : 6;
    else if (i == 1 && !nrfPresent) brightIdx = (brightIdx + 1) % sizeof(BRIGHT);
    else if (i == 2) invertColors = !invertColors;
    else if (i == 3) soundOn = !soundOn;
    else if (i == 4 && sdOk) {
      sdEnabled = !sdEnabled;
      if (!sdEnabled && sdFile) sdFile.close();
    } else if (i == 5) {
      bleSetEnabled(!bleEnabled);
      pcAdvertise(bleEnabled);  // tryb "tylko Wi-Fi": bez połączenia z komputerem przez BLE
    } else if (i == 6) {
      screen = SCR_CALIB;
      calStep = 0;
      drawCalib();
      return;
    }
    applyDisplay();
    saveSettings();
    uiDirty = true;
  }
}

static void touchTick() {
  static bool wasDown = false;
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 25) return;
  lastPoll = millis();
  int x, y;
  bool down = touchRead(x, y);
  if (down && !wasDown) onTap(x, y);
  wasDown = down;
}

// ---------------------------------------------------------------- start i pętla

static void render() {
  DroneInfo *list[MAX_DRONES];
  int n = listDrones(list);
  if (screen == SCR_CALIB) return;
  drawTopBar(n);
  if (screen == SCR_LIST) drawList();
  else if (screen == SCR_DETAIL) drawDetail();
  else if (screen == SCR_RADAR) drawRadar();
  else if (screen == SCR_BAND) drawBand();
  else if (screen == SCR_SETTINGS) drawSettings();
  if (screen != SCR_DETAIL) drawTabs();
}

void setup() {
  Serial.begin(921600);
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);  // czas polski z automatyczną zmianą lato/zima
  tzset();
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  led(false, false, false);
  ledcSetup(CH_SPEAKER, 2000, 8);
  ledcAttachPin(PIN_SPEAKER, CH_SPEAKER);
  ledcWriteTone(CH_SPEAKER, 0);

  prefs.begin("ridcyd", false);
  bootId = prefs.getUShort("boot", 0) + 1;
  prefs.putUShort("boot", bootId);
  brightIdx = min<uint8_t>(prefs.getUChar("bright", 0), sizeof(BRIGHT) - 1);
  invertColors = prefs.getBool("inv", false);
  soundOn = prefs.getBool("sound", true);
  sdEnabled = prefs.getBool("sd", true);
  bleEnabled = prefs.getBool("ble", true);
  bool calibrated = prefs.getBytes("tcal", &touchCal, sizeof(touchCal)) == sizeof(touchCal);
  if (!calibrated) touchCal = {200, 3700, 240, 3800, 0};
  tagsLoad();

  tft.init();
  tft.setRotation(1);
  ledcSetup(CH_BACKLIGHT, 5000, 8);
  ledcAttachPin(TFT_BL, CH_BACKLIGHT);
  applyDisplay();
  tft.fillScreen(C_BG);
  rowSpr.setColorDepth(16);
  rowSpr.createSprite(320, ROW_H);
  touchBegin();
  sdBegin(bootId);

  if (!calibrated) {  // pierwszy start: bez kalibracji dotyk może trafiać w złe miejsca
    screen = SCR_CALIB;
    calStep = 0;
    drawCalib();
  }

  frameQueue = xQueueCreate(32, sizeof(Frame));
  radioBegin();
  pcBleBegin(bleEnabled);
  nrfBegin();  // moduł nRF24 - wykrywany sam, także po podłączeniu w trakcie pracy
  Serial.println("{\"info\":\"RID sensor CYD gotowy\",\"dev\":\"cyd\"}");
}

void loop() {
  static uint32_t lastDraw = 0, lastStatus = 0;
  Frame f;
  while (xQueueReceive(frameQueue, &f, pdMS_TO_TICKS(5)) == pdTRUE) {
    printFrame(f);
    sdLog(f);
    DroneInfo *d = parseFrame(f);
    if (d && !d->shown && (d->id[0] || millis() - d->firstMs > 5000)) {  // chwila na Basic ID
      d->shown = true;
      alertNewDrone(*d);
    }
  }
  WdEvent we;
  while (xQueueReceive(wdQueue, &we, 0) == pdTRUE) {  // dron Wi-Fi bez Remote ID
    DroneInfo *d = wdApply(we);
    if (!d->shown) {
      d->shown = true;
      alertNewDrone(*d);
    }
    wdPrint(Serial, *d);
  }
  static bool nrfWas = false;
  if (nrfPresent != nrfWas) {
    nrfWas = nrfPresent;
    Serial.printf("{\"info\":\"modul nRF24 %s\",\"dev\":\"cyd\"}\n", nrfPresent ? "wykryty - analizator 2,4 GHz" : "odlaczony");
    uiDirty = true;
  }
  if (nrfWindowReady) {  // okno 1 s analizatora 2,4 GHz -> agent (/api/rf, jak detektor UV-K5)
    nrfWindowReady = false;
    Serial.printf("{\"rf\":{\"band\":\"2G4\",\"score\":%u,\"distinct\":%u,\"peak_dbm\":-64,\"alarm\":%s},\"dev\":\"cyd\"}\n",
                  nrfScore, nrfDistinct, nrfAlarm ? "true" : "false");
    if (screen == SCR_BAND) uiDirty = true;
  }
  if (nrfAlarmEdge) {  // nowa aktywność aparatury 2,4 GHz
    nrfAlarmEdge = false;
    led(true, false, false);
    ledUntil = millis() + 5000;
    beep(1200, 120);
    delay(60);
    beep(1200, 120);
    delay(60);
    beep(1200, 120);
    uiDirty = true;
  }
  chSummaryTick();
  pcBleTick();
  static bool pairWas = false;
  if (blePairing != pairWas || bleUiDirty) {  // PIN na górnym pasku w trakcie parowania, zmiany z telefonu
    pairWas = blePairing;
    bleUiDirty = false;
    uiDirty = true;
  }
  handleSerialCommand();
  touchTick();
  sdTick();
  uint32_t now = millis();
  if (ledUntil && now > ledUntil) {
    ledUntil = 0;
    led(false, false, false);
  }
  if (uiDirty || now - lastDraw > 1000) {
    uiDirty = false;
    lastDraw = now;
    render();
  }
  if (now - lastStatus > 10000) {
    lastStatus = now;
    printStatus();
  }
}
