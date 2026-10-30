// Analizator pasma 2,4 GHz na module nRF24L01 (opcjonalny), podłączanym do złączy CYD:
//   CN1: GND -> GND, 3.3V -> VCC i CE, IO22 -> SCK, IO27 -> MOSI
//   P3:  IO35 -> MISO, IO21 -> CSN (IRQ niepodłączone)
// Płytka sprawdza obecność modułu przy starcie i co 5 s, więc po podłączeniu działa bez wgrywania.
//
// CE jest na stałe w stanie wysokim (brak wolnego pinu), więc kanał przełączamy bitem PRIM_RX:
// PRIM_RX=0 (Standby-II przy pustej kolejce nadawczej) -> RF_CH -> PRIM_RX=1 (odbiór po 130 us) -> RPD.
// RPD = sygnał powyżej ok. -64 dBm na kanale 1 MHz, czyli widać bliskie nadajniki (kilkadziesiąt metrów).
// IO21 steruje też podświetleniem ekranu: przy podłączonym module podświetlenie jest stale pełne
// (CSN w spoczynku = stan wysoki) i jasność z ustawień nie działa.
//
// Aparatury FPV 2,4 GHz (ELRS, FrSky, Flysky) skaczą po całym paśmie krótkimi impulsami, a Wi-Fi zajmuje
// stałe bloki ~20 kanałów. Liczymy więc kanały z rzadką aktywnością poza kanałami zajętymi przez Wi-Fi
// (statystyka z radio.h) - wynik jak w detektorze UV-K5: +3 za okno z rozproszoną aktywnością, alarm od 6.
// Dołączany z main.cpp po radio.h i ui.h.
#pragma once

#define NRF_SCK 22
#define NRF_MOSI 27
#define NRF_MISO 35
#define NRF_CSN 21  // = TFT_BL
#define NRF_CH 126  // 2400..2525 MHz
#define NRF_WINDOW_MS 1000
#define NRF_PROBE_MS 5000
#define NRF_HOP_MIN 6      // tyle rozproszonych kanałów w oknie = okno aktywne
#define NRF_HOP_SPREAD 30  // ...rozrzuconych na co najmniej tylu kanałach (MHz)
#define NRF_SPORADIC 40    // kanał zajęty w <= 40% przebiegów = impulsy, nie stała nośna
#define NRF_SCORE_MAX 12

static volatile bool nrfPresent = false;
static uint8_t nrfLevel[NRF_CH];  // zajętość kanału w ostatnim oknie, % przebiegów z sygnałem
static uint8_t nrfHold[NRF_CH];   // maksimum zanikające powoli (wykres)
static uint8_t nrfScore = 0, nrfDistinct = 0;
static bool nrfAlarm = false;
static volatile bool nrfAlarmEdge = false;  // nowy alarm - sygnał w pętli głównej
static volatile bool nrfWindowReady = false;
static uint16_t nrfSweepsLast = 0;

static void applyDisplay();

static inline void nrfDelay() { ets_delay_us(1); }  // ~500 kHz: luźne przewody dupont

static uint8_t nrfXfer(uint8_t out) {
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(NRF_MOSI, (out >> i) & 1);
    nrfDelay();
    digitalWrite(NRF_SCK, HIGH);
    in = (in << 1) | digitalRead(NRF_MISO);
    nrfDelay();
    digitalWrite(NRF_SCK, LOW);
  }
  return in;
}

static uint8_t nrfCmd(uint8_t cmd, uint8_t val, bool withData = true) {
  digitalWrite(NRF_CSN, LOW);
  nrfDelay();
  uint8_t r = nrfXfer(cmd);
  if (withData) r = nrfXfer(val);
  digitalWrite(NRF_CSN, HIGH);
  nrfDelay();
  return r;
}

static uint8_t nrfRead(uint8_t reg) { return nrfCmd(reg & 0x1F, 0xFF); }
static void nrfWrite(uint8_t reg, uint8_t v) { nrfCmd(0x20 | (reg & 0x1F), v); }

static void nrfTakePins() {
  ledcDetachPin(NRF_CSN);  // pin podświetlenia przechodzi na CSN (stan wysoki = ekran świeci)
  pinMode(NRF_CSN, OUTPUT);
  digitalWrite(NRF_CSN, HIGH);
  pinMode(NRF_SCK, OUTPUT);
  digitalWrite(NRF_SCK, LOW);
  pinMode(NRF_MOSI, OUTPUT);
  pinMode(NRF_MISO, INPUT);
}

static void nrfReleasePins() {
  ledcAttachPin(NRF_CSN, CH_BACKLIGHT);  // z powrotem regulacja jasności
  applyDisplay();
}

// zapis i odczyt RF_CH trzema wartościami + domyślna szerokość adresu - pływająca linia MISO tego nie przejdzie
static bool nrfProbe() {
  static const uint8_t VALS[] = {0x4C, 0x15, 0x6A};
  for (uint8_t v : VALS) {
    nrfWrite(0x05, v);
    if (nrfRead(0x05) != v) return false;
  }
  uint8_t aw = nrfRead(0x03);
  return aw >= 1 && aw <= 3;
}

static void nrfSetup() {
  nrfWrite(0x00, 0x00);          // wyłączony
  nrfWrite(0x01, 0x00);          // bez potwierdzeń
  nrfWrite(0x02, 0x01);          // potok 0
  nrfWrite(0x06, 0x0F);          // 2 Mb/s, wzmacniacz LNA
  nrfCmd(0xE1, 0, false);        // FLUSH_TX: pusta kolejka = Standby-II przy PRIM_RX=0 i CE=1
  nrfCmd(0xE2, 0, false);        // FLUSH_RX
  nrfWrite(0x00, 0x02);          // PWR_UP
  delay(5);
}

static void nrfWindow(const uint16_t *hits, uint16_t sweeps) {
  // kanały nRF zajęte przez sieci Wi-Fi (środek kanału Wi-Fi c = 2412 + 5(c-1) MHz, szerokość ~22 MHz)
  bool wifi[NRF_CH] = {};
  for (int c = 1; c <= 13; c++) {
    if (chRssi[c] < -85 || !chRate[c]) continue;
    int center = 12 + 5 * (c - 1);
    for (int k = max(0, center - 11); k <= min(NRF_CH - 1, center + 11); k++) wifi[k] = true;
  }
  int distinct = 0, lo = NRF_CH, hi = -1;
  for (int k = 0; k < NRF_CH; k++) {
    uint8_t lvl = sweeps ? hits[k] * 100 / sweeps : 0;
    nrfLevel[k] = lvl;
    nrfHold[k] = lvl > nrfHold[k] ? lvl : (nrfHold[k] > 3 ? nrfHold[k] - 3 : 0);
    if (!wifi[k] && lvl > 0 && lvl <= NRF_SPORADIC) {
      distinct++;
      lo = min(lo, k);
      hi = max(hi, k);
    }
  }
  bool active = distinct >= NRF_HOP_MIN && hi - lo >= NRF_HOP_SPREAD;
  if (active) nrfScore = min(NRF_SCORE_MAX, nrfScore + 3);
  else nrfScore -= min<int>(nrfScore, distinct ? 1 : 2);
  nrfDistinct = distinct;
  bool was = nrfAlarm;
  if (nrfScore >= 6) nrfAlarm = true;
  else if (nrfScore <= 2) nrfAlarm = false;
  if (nrfAlarm && !was) nrfAlarmEdge = true;
  nrfSweepsLast = sweeps;
  nrfWindowReady = true;
}

static void nrfTask(void *) {
  static uint16_t hits[NRF_CH];
  uint32_t lastProbe = 0;
  for (;;) {
    if (!nrfPresent) {
      if (lastProbe && millis() - lastProbe < NRF_PROBE_MS) {
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      lastProbe = millis();
      nrfTakePins();
      if (nrfProbe()) {
        nrfSetup();
        memset(hits, 0, sizeof(hits));
        nrfScore = 0;
        nrfPresent = true;
      } else {
        nrfReleasePins();
      }
      continue;
    }
    uint32_t start = millis();
    uint16_t sweeps = 0;
    memset(hits, 0, sizeof(hits));
    while (millis() - start < NRF_WINDOW_MS) {
      for (int k = 0; k < NRF_CH; k++) {
        nrfWrite(0x00, 0x02);  // PRIM_RX=0: postój
        nrfWrite(0x05, k);
        nrfWrite(0x00, 0x03);  // PRIM_RX=1: odbiór
        ets_delay_us(200);     // 130 us rozruchu + 40 us na pomiar RPD
        if (nrfRead(0x09) & 1) hits[k]++;
        if ((k & 15) == 15) vTaskDelay(1);  // czas dla pętli głównej (ekran, dotyk)
      }
      sweeps++;
    }
    // moduł wciąż odpowiada? (odłączony = odczyt nie zgadza się z ostatnio ustawionym kanałem)
    static uint8_t lost = 0;
    if (nrfRead(0x05) != NRF_CH - 1) {
      if (++lost >= 3) {
        lost = 0;
        nrfPresent = false;
        nrfScore = 0;
        nrfAlarm = false;
        memset(nrfLevel, 0, sizeof(nrfLevel));
        nrfReleasePins();
        lastProbe = millis();
        continue;
      }
    } else {
      lost = 0;
    }
    nrfWindow(hits, sweeps);
  }
}

static void nrfBegin() { xTaskCreatePinnedToCore(nrfTask, "nrf24", 3072, nullptr, 1, nullptr, 1); }
