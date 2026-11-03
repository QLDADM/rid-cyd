// Czujniki ESP32-C5 podłączone przewodem (system w walizce, PROJEKT_wykrywanie_dronow.md):
//   C5 nr 1 (Wi-Fi 2,4 GHz): TX (GPIO9) -> CYD GPIO35 (P3), UART1
//   C5 nr 2 (Wi-Fi 5 GHz):   TX (GPIO9) -> CYD GPIO27 (CN1), UART2
// Tylko odbiór, 115200 8N1, wspólna masa. Każdy czujnik wysyła linie ASCII z sumą XOR jak w NMEA
// (XOR bajtów między '$' a '*', dwie cyfry hex):
//   $RID,<nr>,<uptime_ms>,WIFI|NAN,<mac>,<rssi>,<kanal>,2G4|5G,<hex>*XX
//        ramka Remote ID: dla WIFI treść Vendor IE od OUI (FA0BBC 0D licznik pack), dla NAN Service Info
//        (licznik pack) - dokładnie to, co radio.h daje do enqueue()
//   $AP,<nr>,<uptime_ms>,<mac>,<rssi>,<kanal>,2G4|5G,<ssid>*XX
//        sieć drona Wi-Fi bez Remote ID (wifidrone.h); w ssid znaki spoza 0x21-0x7E oraz , * % jako %XX
//   $HB,<nr>,<uptime_ms>,<kanal>[,<pasmo>,<ramek_rid>,<wersja>]*XX       co 5 s
//   $CS,<nr>,2G4|5G,<kanal>:<ramek_na_s>:<szczyt_dbm>;...*XX               co ~10 s, -128 = brak sygnału
// Ramki RID trafiają do tej samej kolejki co z własnego radia (Frame.src = 1 / 2), więc dekodowanie, lista,
// alarmy, zapis SD, telefon i agent działają bez zmian. Numer czujnika wynika z wejścia, nie z pola <nr>.
// Test bez czujników: polecenie USB "C5 <1|2> <linia>" podaje linię tak, jakby przyszła z przewodu
// (skrypt tools/c5_sim.py udaje oba czujniki).
// Dołączany z main.cpp po radio.h i pcble.h, przed ui.h.
#pragma once

#define SENS_N 2
#define SENS_BAUD 115200
#define SENS_ONLINE_MS 15000  // bez linii dłużej = czujnik offline
#define SENS_LINE 760         // $RID z 256 B danych to ok. 600 znaków
#define CS_MAX 32             // kanałów w jednym paśmie (5 GHz w PL: do ok. 25)
#define CS_FRESH_MS 30000     // tyle ważne są dane pasma z czujnika

struct SensorState {
  HardwareSerial *port;
  int8_t rxPin;
  char line[SENS_LINE];
  uint16_t n;
  bool overflow;
  uint32_t lastMs;  // ostatnia poprawna linia (0 = jeszcze nic)
  uint32_t upMs;    // czas pracy czujnika z $HB
  uint8_t ch;       // kanał z $HB
  char band[4];
  char fw[12];
  uint32_t lines, bad, rid, ap;
};
static SensorState sens[SENS_N] = {{&Serial1, 35}, {&Serial2, 27}};

struct BandData {
  uint8_t n;
  uint8_t ch[CS_MAX];
  uint16_t rate[CS_MAX];
  int8_t peak[CS_MAX];
  uint32_t ms;  // 0 = brak danych
  uint8_t src;  // czujnik 1 / 2
};
static BandData band24, band5;
static uint32_t band24Sent = 0, band5Sent = 0;

static bool sensOnline(int s) { return sens[s].lastMs && millis() - sens[s].lastMs < SENS_ONLINE_MS; }
static bool bandFresh(const BandData &b) { return b.ms && millis() - b.ms < CS_FRESH_MS; }
// wykres pasma tylko od działającego czujnika - po odłączeniu C5 (albo końcu testu) znika od razu
static bool bandLive(const BandData &b) { return bandFresh(b) && b.src && sensOnline(b.src - 1); }

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// sprawdza sumę i ucina linię na '*'
static bool sensChecksum(char *s) {
  char *star = strrchr(s, '*');
  if (!star || hexVal(star[1]) < 0 || hexVal(star[2]) < 0) return false;
  uint8_t x = 0;
  for (char *p = s + 1; p < star; p++) x ^= (uint8_t)*p;
  bool ok = x == ((hexVal(star[1]) << 4) | hexVal(star[2]));
  *star = 0;
  return ok;
}

static int splitFields(char *s, char **f, int max) {
  int n = 0;
  f[n++] = s;
  for (char *p = s; *p && n < max; p++)
    if (*p == ',') {
      *p = 0;
      f[n++] = p + 1;
    }
  return n;
}

static bool parseMac(const char *s, uint8_t *mac) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) mac[i] = v[i];
  return true;
}

static bool sensRid(int s, char **f, int nf) {
  if (nf < 9) return false;
  static Frame fr;  // ~270 B - poza stosem zadania
  fr.transport = !strcmp(f[3], "NAN") ? T_NAN : !strcmp(f[3], "WIFI") ? T_BEACON : 0xFF;
  size_t hl = strlen(f[8]);
  if (fr.transport == 0xFF || !parseMac(f[4], fr.mac) || hl % 2 || hl / 2 > sizeof(fr.data)) return false;
  for (size_t i = 0; i < hl / 2; i++) {
    int h = hexVal(f[8][2 * i]), l = hexVal(f[8][2 * i + 1]);
    if (h < 0 || l < 0) return false;
    fr.data[i] = (h << 4) | l;
  }
  fr.len = hl / 2;
  fr.rssi = atoi(f[5]);
  fr.channel = atoi(f[6]);
  fr.src = s + 1;
  if (fr.transport == T_BEACON) wdNoteOdid(fr.mac);  // ten MAC ma Remote ID - nie jest "dronem Wi-Fi"
  if (xQueueSend(frameQueue, &fr, 0) == pdTRUE) counts[fr.transport]++;
  else dropped++;
  sens[s].rid++;
  return true;
}

static bool sensAp(int s, char **f, int nf) {
  if (nf < 8) return false;
  WdEvent e;
  if (!parseMac(f[3], e.mac) || wdIsOdid(e.mac)) return nf >= 8;
  char ssid[33];
  size_t k = 0;
  for (const char *p = f[7]; *p && k < 32; p++) {
    char c = *p;
    if (c == '%' && hexVal(p[1]) >= 0 && hexVal(p[2]) >= 0) {
      c = (hexVal(p[1]) << 4) | hexVal(p[2]);
      p += 2;
    }
    ssid[k++] = (c < 32 || c > 126 || c == '"' || c == '\\') ? '?' : c;  // jak wdCheck: bezpieczne w JSON
  }
  ssid[k] = 0;
  if (ssid[0]) strlcpy(e.ssid, ssid, sizeof(e.ssid));
  else snprintf(e.ssid, sizeof(e.ssid), "WIFI-%02X%02X%02X", e.mac[3], e.mac[4], e.mac[5]);
  e.kind = wdKindFor(e.mac, ssid);
  if (!e.kind) e.kind = "dron Wi-Fi";  // czujnik może mieć nowszą listę wzorców
  e.ch = atoi(f[5]);
  e.rssi = atoi(f[4]);
  if (xQueueSend(wdQueue, &e, 0) == pdTRUE) wdSeen++;
  sens[s].ap++;
  return true;
}

static bool sensHb(int s, char **f, int nf) {
  if (nf < 4) return false;
  sens[s].upMs = strtoul(f[2], nullptr, 10);
  sens[s].ch = atoi(f[3]);
  if (nf > 4) strlcpy(sens[s].band, f[4], sizeof(sens[s].band));
  if (nf > 6) strlcpy(sens[s].fw, f[6], sizeof(sens[s].fw));
  return true;
}

static bool sensCs(int s, char **f, int nf) {
  if (nf < 4) return false;
  bool is5 = !strcmp(f[2], "5G");
  if (!is5 && strcmp(f[2], "2G4")) return false;
  BandData b = {};
  for (char *p = f[3]; *p && b.n < CS_MAX;) {
    unsigned ch, rate;
    int peak;
    if (sscanf(p, "%u:%u:%d", &ch, &rate, &peak) != 3) return false;
    b.ch[b.n] = ch;
    b.rate[b.n] = min(rate, 65535u);
    b.peak[b.n++] = constrain(peak, -128, 0);
    p = strchr(p, ';');
    if (!p) break;
    p++;
  }
  b.src = s + 1;
  b.ms = millis();
  (is5 ? band5 : band24) = b;  // odczyt w pętli głównej; pojedyncze rozjechanie wiersza jest nieszkodliwe
  return true;
}

// jedna linia od czujnika s (0 / 1); l jest zmieniana w trakcie analizy
static void sensLine(int s, char *l) {
  SensorState &st = sens[s];
  while (*l && *l != '$') l++;  // śmieci na początku (np. po włączeniu zasilania)
  if (!*l || !sensChecksum(l)) {
    st.bad++;
    return;
  }
  char *f[10];
  int nf = splitFields(l + 1, f, 10);
  bool ok = !strcmp(f[0], "RID") ? sensRid(s, f, nf) : !strcmp(f[0], "AP") ? sensAp(s, f, nf)
          : !strcmp(f[0], "HB") ? sensHb(s, f, nf) : !strcmp(f[0], "CS") ? sensCs(s, f, nf) : false;
  if (!ok) {
    st.bad++;
    return;
  }
  st.lines++;
  st.lastMs = millis();
}

// osobne zadanie: pętla główna potrafi stać ~0,3 s (dźwięk, ekran), a bufor UART szybko się zapełnia
static void sensorTask(void *) {
  for (;;) {
    for (int s = 0; s < SENS_N; s++) {
      SensorState &st = sens[s];
      while (st.port->available()) {
        char c = st.port->read();
        if (c == '\n' || c == '\r') {
          if (st.n && !st.overflow) {
            st.line[st.n] = 0;
            sensLine(s, st.line);
          }
          st.n = 0;
          st.overflow = false;
        } else if (st.n < SENS_LINE - 1) {
          st.line[st.n++] = c;
        } else {
          st.overflow = true;  // za długa linia - odrzucona w całości
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void sensorsBegin() {
  for (auto &s : sens) {
    s.port->setRxBufferSize(2048);
    s.port->begin(SENS_BAUD, SERIAL_8N1, s.rxPin, -1);  // bez TX: czujnik tylko nadaje
  }
  xTaskCreatePinnedToCore(sensorTask, "c5", 4096, nullptr, 2, nullptr, 1);
}

static const char *srcName(uint8_t src) { return src == 1 ? "c5-1" : src == 2 ? "c5-2" : "cyd"; }


// skąd jest dron: bit 0 = radio CYD, bit 1 = C5 nr 1, bit 2 = C5 nr 2, oraz kanał Wi-Fi ostatniej ramki
// (0 = BLE / brak); zerowane, gdy slot zajmie nowy dron
static uint8_t droneVia[MAX_DRONES], droneCh[MAX_DRONES];
static uint32_t droneViaFirst[MAX_DRONES];

static void droneNoteVia(const DroneInfo &d, uint8_t src, uint8_t ch) {
  int i = &d - drones;
  if (droneViaFirst[i] != d.firstMs) {
    droneViaFirst[i] = d.firstMs;
    droneVia[i] = droneCh[i] = 0;
  }
  droneVia[i] |= 1 << src;
  if (ch) droneCh[i] = ch;
}

// kanał Wi-Fi, na którym ostatnio słyszano drona (dron Wi-Fi bez RID: kanał jego sieci); 0 = nieznany
static uint8_t droneLastCh(const DroneInfo &d) {
  if (d.wifiKind) return d.wifiCh;
  int i = &d - drones;
  return droneViaFirst[i] == d.firstMs ? droneCh[i] : 0;
}

// czy na kanale słychać teraz drona (zakładka Pasmo: czerwony kanał)
static bool droneOnChannel(uint8_t ch, uint32_t maxAgeMs) {
  uint32_t now = millis();
  for (auto &d : drones)
    if (d.used && now - d.lastMs < maxAgeMs && droneLastCh(d) == ch) return true;
  return false;
}

static String viaText(const DroneInfo &d) {
  int i = &d - drones;
  uint8_t v = droneViaFirst[i] == d.firstMs ? droneVia[i] : 0;
  String s;
  if (v & 1) s += "CYD";
  if (v & 2) s += String(s.length() ? "+" : "") + "C5-1";
  if (v & 4) s += String(s.length() ? "+" : "") + "C5-2";
  return s.length() ? s : String("CYD");
}

// pasmo z czujnika -> telefon i agent: {"cs":{"b":"5G","c":[36,40,...],"r":[...],"p":[...],"src":"c5-2"}}
static void bandSend(const BandData &b, const char *name) {
  char line[600];
  size_t n = snprintf(line, sizeof(line), "{\"cs\":{\"b\":\"%s\",\"src\":\"%s\",\"c\":[", name, srcName(b.src));
  for (int i = 0; i < b.n && n < sizeof(line); i++) n += snprintf(line + n, sizeof(line) - n, "%s%u", i ? "," : "", b.ch[i]);
  if (n < sizeof(line)) n += snprintf(line + n, sizeof(line) - n, "],\"r\":[");
  for (int i = 0; i < b.n && n < sizeof(line); i++) n += snprintf(line + n, sizeof(line) - n, "%s%u", i ? "," : "", b.rate[i]);
  if (n < sizeof(line)) n += snprintf(line + n, sizeof(line) - n, "],\"p\":[");
  for (int i = 0; i < b.n && n < sizeof(line); i++) n += snprintf(line + n, sizeof(line) - n, "%s%d", i ? "," : "", b.peak[i]);
  if (n >= sizeof(line) - 5) return;
  snprintf(line + n, sizeof(line) - n, "]}}\n");
  Serial.print(line);
  if (phoneSubscribed) phoneSend(line);
}

// z pętli: nowe dane pasm do wysłania, zmiana stanu czujników.
// Wynik: SENS_NEW_BAND - nowe dane pasma (ekran Pasmo), SENS_STATE - czujnik się pojawił albo zniknął
#define SENS_NEW_BAND 1
#define SENS_STATE 2
static uint8_t sensorsTick() {
  uint8_t out = 0;
  if (band24.ms != band24Sent) {
    band24Sent = band24.ms;
    bandSend(band24, "2G4");
    out |= SENS_NEW_BAND;
  }
  if (band5.ms != band5Sent) {
    band5Sent = band5.ms;
    bandSend(band5, "5G");
    out |= SENS_NEW_BAND;
  }
  static bool was[SENS_N];
  for (int s = 0; s < SENS_N; s++) {
    bool on = sensOnline(s);
    if (on != was[s]) {
      was[s] = on;
      Serial.printf("{\"info\":\"czujnik C5 nr %d %s\",\"dev\":\"cyd\"}\n", s + 1, on ? "polaczony" : "bez sygnalu");
      out |= SENS_STATE;
    }
  }
  return out;
}
