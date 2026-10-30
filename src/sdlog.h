// Zapis wykrytych ramek Remote ID na kartę microSD (VSPI: SCK 18, MISO 19, MOSI 23, CS 5).
// Plik dzienny /rid/rid-RRRRMMDD.jsonl (gdy znany czas) albo /rid/boot-NNNN.jsonl.
// Jedna linia JSON na ramkę, najwyżej 1 ramka danego typu z jednego MAC na SD_MIN_INTERVAL_MS:
//   {"t":<unix|0>,"boot":N,"up":s,"tr":"ble","mac":"..","rssi":-70,"d":"<hex>"}
// Dołączany z main.cpp po drones.h.
#pragma once

#include <SD.h>
#include <SPI.h>

#define SD_CS 5
#define SD_MIN_INTERVAL_MS 3000
#define SD_FLUSH_MS 5000

static SPIClass sdSpi(VSPI);
static bool sdOk = false, sdEnabled = true;
static File sdFile;
static char sdPath[32] = "";
static uint32_t sdLines = 0, sdLastFlush = 0;
static uint16_t sdBoot = 0;
static struct { uint8_t mac[6]; uint8_t tr, type; uint32_t ms; } sdSeen[32];

static void sdBegin(uint16_t boot) {
  sdBoot = boot;
  sdSpi.begin(18, 19, 23, SD_CS);
  sdOk = SD.begin(SD_CS, sdSpi, 20000000);
  if (sdOk) SD.mkdir("/rid");
}

static bool sdOpen() {
  if (!sdOk || !sdEnabled) return false;
  char p[32];
  uint32_t now = unixNow();
  if (now) {
    time_t t = now;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(p, sizeof(p), "/rid/rid-%Y%m%d.jsonl", &tmv);
  } else {
    snprintf(p, sizeof(p), "/rid/boot-%04u.jsonl", sdBoot);
  }
  if (sdFile && !strcmp(p, sdPath)) return true;
  if (sdFile) sdFile.close();
  strlcpy(sdPath, p, sizeof(sdPath));
  sdFile = SD.open(p, FILE_APPEND);
  if (!sdFile) sdOk = false;  // karta wyjęta albo uszkodzona
  return (bool)sdFile;
}

static void sdLog(const Frame &f) {
  if (!sdOk || !sdEnabled) return;
  // BLE 4: osobna wiadomość na reklamę - limit liczony per typ wiadomości
  uint8_t type = (f.transport == T_BLE && f.len >= 3) ? (f.data[2] >> 4) : 0xF;
  uint32_t now = millis();
  int slot = -1, oldest = 0;
  for (int i = 0; i < 32; i++) {
    if (sdSeen[i].tr == f.transport && sdSeen[i].type == type && !memcmp(sdSeen[i].mac, f.mac, 6)) { slot = i; break; }
    if (sdSeen[i].ms < sdSeen[oldest].ms) oldest = i;
  }
  if (slot >= 0 && now - sdSeen[slot].ms < SD_MIN_INTERVAL_MS) return;
  if (slot < 0) {
    slot = oldest;
    memcpy(sdSeen[slot].mac, f.mac, 6);
    sdSeen[slot].tr = f.transport;
    sdSeen[slot].type = type;
  }
  sdSeen[slot].ms = now;
  if (!sdOpen()) return;
  static const char hex[] = "0123456789abcdef";
  sdFile.printf("{\"t\":%u,\"boot\":%u,\"up\":%u,\"tr\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,\"d\":\"",
                unixNow(), sdBoot, millis() / 1000, TRANSPORT_NAMES[f.transport], f.mac[0], f.mac[1], f.mac[2],
                f.mac[3], f.mac[4], f.mac[5], f.rssi);
  for (size_t i = 0; i < f.len; i++) {
    sdFile.write(hex[f.data[i] >> 4]);
    sdFile.write(hex[f.data[i] & 0x0F]);
  }
  sdFile.print("\"}\n");
  sdLines++;
}

static void sdTick() {
  if (sdFile && millis() - sdLastFlush > SD_FLUSH_MS) {
    sdLastFlush = millis();
    sdFile.flush();  // wyjęcie karty traci najwyżej kilka ostatnich sekund
  }
}

static uint32_t sdFreeMB() { return sdOk ? (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024 * 1024)) : 0; }
