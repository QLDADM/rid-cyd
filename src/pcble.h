// Bluetooth do telefonu i komputera (usługa jak w Heltec, więc obsługuje je ten sam kod strony i agenta).
//  - telefon (NUS_TX / NUS_RX): strona podglądu dostaje linie JSON jak z Heltec:
//      {"d":{...}} dron w zasięgu (co 1 s), {"s":{...}} stan płytki (co 5 s), {"tags":{...}} oznaczenia;
//    polecenia: TAG <id> own|foreign|-, TIME <unix>, HIST (CYD nie ma historii - od razu hend 0).
//  - komputer (PC_TX / PC_RX): ten sam protokół co po USB (hostout.h). Uwaga: Windows nie paruje się
//    z ESP32 w CYD (błąd po PIN-ie), więc agent łączy się z CYD kablem; kanał zostaje na przyszłość.
// Parowanie z PIN-em (6 cyfr, losowany przy pierwszym starcie, widoczny w Ustawieniach i na górnym
// pasku w trakcie parowania). Do 3 połączeń naraz: telefony i komputer.
// Dołączany z main.cpp po oznaczeniach i sdlog.h, przed ui.h (NimBLE uruchamia radioBegin).
#pragma once
#include "nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h"  // nazwa urządzenia

#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define PC_TX "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"
#define PC_RX "6E400005-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_MAX_CONN 3  // CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define PHONE_DRONE_MS 1000
#define PHONE_STATUS_MS 5000
#define PHONE_DRONE_MAX_AGE_MS 60000

static uint8_t droneLastCh(const DroneInfo &d);  // sensors.h: kanał ostatniej ramki Wi-Fi drona
static bool sensOnline(int s);  // sensors.h: czujnik C5 nadaje (stan idzie na tablet)

static NimBLEServer *pcServer = nullptr;
static NimBLECharacteristic *pcTx = nullptr, *phoneTx = nullptr;
static volatile uint16_t pcConn = BLE_HS_CONN_HANDLE_NONE;
static uint32_t pcPin = 0;
static char pcName[16];
static uint32_t hostCmdMs = 0;  // ostatnie polecenie od agenta (USB albo BLE)
static volatile bool phoneSubscribed = false, phoneWantTags = false, blePairing = false;
static volatile bool bleUiDirty = false;
static volatile bool pairScanHold = false;  // skanowanie dronów wstrzymane na czas parowania  // zmiana z telefonu - pętla odświeża ekran
static char phoneCmd[64];
static volatile bool phoneCmdReady = false;
static volatile uint16_t bleConn[BLE_MAX_CONN] = {BLE_HS_CONN_HANDLE_NONE, BLE_HS_CONN_HANDLE_NONE,
                                                  BLE_HS_CONN_HANDLE_NONE};
static volatile bool bleConnSec[BLE_MAX_CONN];

static bool pcSecure(const ble_gap_conn_desc *d) { return d->sec_state.encrypted && d->sec_state.authenticated; }

// parowanie w toku (połączenie bez szyfrowania) - wtedy PIN na górnym pasku
static void bleRecalc() {
  bool pairing = false;
  for (int i = 0; i < BLE_MAX_CONN; i++)
    if (bleConn[i] != BLE_HS_CONN_HANDLE_NONE && !bleConnSec[i]) pairing = true;
  blePairing = pairing;
}

static void bleConnSet(uint16_t h, bool sec) {
  int free = -1;
  for (int i = 0; i < BLE_MAX_CONN; i++) {
    if (bleConn[i] == h) {
      bleConnSec[i] = sec;
      bleRecalc();
      return;
    }
    if (free < 0 && bleConn[i] == BLE_HS_CONN_HANDLE_NONE) free = i;
  }
  if (free >= 0) {
    bleConn[free] = h;
    bleConnSec[free] = sec;
  }
  bleRecalc();
}

static void pcOff() {
  pcConn = BLE_HS_CONN_HANDLE_NONE;
  hostBleOn = false;
  hostBleReset();
}

class PcServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, ble_gap_conn_desc *desc) override {
    bool sec = pcSecure(desc);
    bleConnSet(desc->conn_handle, sec);
    if (!sec) {  // nowe połączenie: przy ciągłym skanowaniu radio gubiło pakiety parowania
      pairScanHold = scanHold = true;
      NimBLEDevice::getScan()->stop();
    }
  }
  void onDisconnect(NimBLEServer *, ble_gap_conn_desc *desc) override {
    for (int i = 0; i < BLE_MAX_CONN; i++)
      if (bleConn[i] == desc->conn_handle) bleConn[i] = BLE_HS_CONN_HANDLE_NONE;
    if (desc->conn_handle == pcConn) pcOff();
    bleRecalc();
    phoneSubscribed = phoneTx->getSubscribedCount() > 0;
  }
  uint32_t onPassKeyRequest() override { return pcPin; }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    // tylko parowanie z PIN-em (MITM) - zwykłe "Just Works" nie wystarcza
    bool sec = pcSecure(desc);
    bleConnSet(desc->conn_handle, sec);
    if (!sec) NimBLEDevice::getServer()->disconnect(desc->conn_handle);
    else phoneWantTags = true;
  }
};

class PcTxCb : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *, ble_gap_conn_desc *desc, uint16_t sub) override {
    if (sub && pcSecure(desc)) {
      hostBleReset();
      pcConn = desc->conn_handle;
      hostBleOn = true;
    } else if (desc->conn_handle == pcConn) {
      pcOff();
    }
  }
};

class PcRxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *desc) override {
    if (!pcSecure(desc) || desc->conn_handle != pcConn) return;
    std::string v = c->getValue();
    hostBleReceive((const uint8_t *)v.data(), v.size());
  }
};

class PhoneTxCb : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *c, ble_gap_conn_desc *, uint16_t sub) override {
    phoneSubscribed = c->getSubscribedCount() > 0;
    if (sub) phoneWantTags = true;
  }
};

class PhoneRxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, ble_gap_conn_desc *desc) override {
    if (phoneCmdReady || !pcSecure(desc)) return;  // poprzednie polecenie nieprzetworzone / bez PIN-u
    std::string v = c->getValue();
    size_t n = min(v.size(), sizeof(phoneCmd) - 1);
    memcpy(phoneCmd, v.data(), n);
    while (n && (phoneCmd[n - 1] == '\n' || phoneCmd[n - 1] == '\r')) n--;
    phoneCmd[n] = 0;
    phoneCmdReady = true;
  }
};

// kawałek bufora kanału PC przez BLE (wywoływane z pętli i z hostOut przy pełnym buforze)
static void hostBleDrain() {
  if (!hostBleOn || !pcTx) return;
  size_t avail = (hostBleOutHead + HOST_BLE_OUT - hostBleOutTail) % HOST_BLE_OUT;
  if (!avail) return;
  uint16_t mtu = pcServer->getPeerMTU(pcConn);
  size_t n = min(min(avail, (size_t)(mtu > 3 ? mtu - 3 : 20)), (size_t)(HOST_BLE_OUT - hostBleOutTail));
  pcTx->setValue(hostBleOut + hostBleOutTail, n);
  pcTx->notify();
  hostBleOutTail = (hostBleOutTail + n) % HOST_BLE_OUT;
  delay(8);  // NimBLE 1.4 nie zgłasza przepełnienia kolejki powiadomień
}

// najmniejsze MTU wśród telefonów - powiadomienie idzie do wszystkich naraz
static size_t phoneChunk() {
  uint16_t mtu = 0;
  for (uint16_t h : pcServer->getPeerDevices()) {
    if (h == pcConn) continue;
    uint16_t m = pcServer->getPeerMTU(h);
    if (m && (!mtu || m < mtu)) mtu = m;
  }
  return mtu > 3 ? mtu - 3 : 20;
}

static void phoneSend(const char *line) {
  size_t len = strlen(line), chunk = phoneChunk();
  for (size_t i = 0; i < len && phoneSubscribed; i += chunk) {
    phoneTx->setValue((const uint8_t *)line + i, min(chunk, len - i));
    phoneTx->notify();  // tylko do połączeń zaszyfrowanych (NimBLE)
    delay(8);
  }
}

static void phoneSendTags() {
  char out[1200];
  size_t n = snprintf(out, sizeof(out), "{\"tags\":{");
  bool first = true;
  for (int i = 0; i < MAX_TAGS && n < sizeof(out); i++) {
    if (!tags[i].status) continue;
    n += snprintf(out + n, sizeof(out) - n, "%s\"%s\":\"%s\"", first ? "" : ",", tags[i].id,
                  tags[i].status == TAG_OWN ? "own" : "foreign");
    first = false;
  }
  if (n < sizeof(out)) snprintf(out + n, sizeof(out) - n, "}}\n");
  phoneSend(out);
}

static void phoneSendDrone(const DroneInfo &d, uint32_t now) {
  char line[480];
  size_t n = snprintf(line, sizeof(line), "{\"d\":{\"id\":\"%s\",\"mac\":\"%02x%02x%02x%02x%02x%02x\",\"rssi\":%d,\"age\":%u",
                      d.id, d.macs[0][0], d.macs[0][1], d.macs[0][2], d.macs[0][3], d.macs[0][4], d.macs[0][5],
                      d.rssi, (now - d.lastMs) / 1000);
#define PH_ADD(...) \
  if (n < sizeof(line)) n += snprintf(line + n, sizeof(line) - n, __VA_ARGS__)
  uint8_t tag = tagStatus(d.id);
  if (tag) { PH_ADD(",\"tag\":\"%s\"", tag == TAG_OWN ? "own" : "foreign"); }
  if (d.wifiKind) { PH_ADD(",\"wifi\":\"%s\",\"ch\":%u", d.wifiKind, d.wifiCh); }
  else if (uint8_t ch = droneLastCh(d)) { PH_ADD(",\"ch\":%u", ch); }  // kanał w panelu pasm na czerwono
  if (d.hasLoc) { PH_ADD(",\"lat\":%.7f,\"lon\":%.7f,\"st\":%u", d.lat * 1e-7, d.lon * 1e-7, d.status); }
  if (!isnan(d.height)) { PH_ADD(",\"h\":%.1f", d.height); }
  if (!isnan(d.altGeo)) { PH_ADD(",\"alt\":%.1f", d.altGeo); }
  if (!isnan(d.speed)) { PH_ADD(",\"spd\":%.1f", d.speed); }
  if (d.track >= 0) { PH_ADD(",\"trk\":%d", d.track); }
  if (d.hasOp) { PH_ADD(",\"olat\":%.7f,\"olon\":%.7f", d.opLat * 1e-7, d.opLon * 1e-7); }
  if (d.opId[0]) { PH_ADD(",\"opid\":\"%s\"", d.opId); }
  if (d.selfId[0]) { PH_ADD(",\"sid\":\"%s\"", d.selfId); }
  if (d.uaType) { PH_ADD(",\"ut\":%u", d.uaType); }
  if (d.euClass) { PH_ADD(",\"cl\":%u,\"cat\":%u", d.euClass, d.euCat); }
  if (!isnan(d.vspeed)) { PH_ADD(",\"vs\":%.1f", d.vspeed); }
  if (!isnan(d.altBaro)) { PH_ADD(",\"baro\":%.1f", d.altBaro); }
  if (!isnan(d.opAlt)) { PH_ADD(",\"oalt\":%.1f,\"olt\":%u", d.opAlt, d.opLocType); }
  if (!isnan(d.maxHeight)) { PH_ADD(",\"mh\":%.0f", d.maxHeight); }
  if (!isnan(d.maxSpeed)) { PH_ADD(",\"ms\":%.1f", d.maxSpeed); }
  PH_ADD("}}\n");
#undef PH_ADD
  phoneSend(line);
}

static void phoneHandleCommand(const char *cmd) {
  if (!strncasecmp(cmd, "TIME ", 5)) {
    uint32_t t = strtoul(cmd + 5, nullptr, 10);
    if (t > 1700000000UL) unixBase = t - millis() / 1000;
  } else if (!strcasecmp(cmd, "HIST")) {
    phoneSend("{\"hend\":0}\n");  // historia jest tylko w Heltec; CYD zapisuje ramki na kartę SD
  } else if (!strcasecmp(cmd, "HISTCLR")) {
    phoneSend("{\"hclr\":1}\n");
  } else if (!strncasecmp(cmd, "TAG ", 4)) {
    char buf[64];
    strlcpy(buf, cmd + 4, sizeof(buf));
    char *st = strchr(buf, ' ');
    if (!st) return;
    *st++ = 0;
    uint8_t s = !strcasecmp(st, "own") ? TAG_OWN : !strcasecmp(st, "foreign") ? TAG_FOREIGN : TAG_NONE;
    tagSet(buf, s);
    tagsSave();
    tagRecordPending(buf, s);  // trafi na stronę RID Decoder przez agenta
    phoneWantTags = true;
    bleUiDirty = true;
  }
}

static void pcAdvertise(bool on) {
  NimBLEAdvertising *a = NimBLEDevice::getAdvertising();
  if (on && !a->isAdvertising() && pcServer->getConnectedCount() < BLE_MAX_CONN) a->start();
  if (!on) {
    a->stop();
    for (uint16_t h : pcServer->getPeerDevices()) pcServer->disconnect(h);
  }
}

static void pcBleBegin(bool advertise) {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  snprintf(pcName, sizeof(pcName), "RID-CYD-%02X%02X", mac[4], mac[5]);
  pcPin = prefs.getUInt("pin", 0);
  if (pcPin < 100000 || pcPin > 999999) {  // pierwszy start: losowy PIN
    pcPin = 100000 + esp_random() % 900000;
    prefs.putUInt("pin", pcPin);
  }
  // usługi GATT można rejestrować i uruchamiać tylko, gdy radio nic nie robi - przy trwającym skanowaniu
  // NimBLE kończy createServer / start serwera restartem płytki; skanowanie wraca na końcu
  scanHold = true;
  NimBLEDevice::getScan()->stop();
  ble_svc_gap_device_name_set(pcName);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM (PIN), Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  // wymiana kluczy szyfrowania i tożsamości: bez klucza tożsamości Windows kończył parowanie błędem
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityPasskey(pcPin);
  pcServer = NimBLEDevice::createServer();
  pcServer->setCallbacks(new PcServerCb());
  NimBLEService *svc = pcServer->createService(NUS_SERVICE);
  // dostęp tylko po parowaniu z PIN-em: subskrypcja i zapis wymagają szyfrowania z MITM
  phoneTx = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                                  NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
  phoneTx->setCallbacks(new PhoneTxCb());
  NimBLECharacteristic *phoneRx = svc->createCharacteristic(
      NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
  phoneRx->setCallbacks(new PhoneRxCb());
  pcTx = svc->createCharacteristic(PC_TX, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ |
                                              NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
  pcTx->setCallbacks(new PcTxCb());
  NimBLECharacteristic *rx = svc->createCharacteristic(
      PC_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
  rx->setCallbacks(new PcRxCb());
  svc->start();
  pcServer->start();  // sam start przy pierwszej reklamie trafiałby na trwające skanowanie
  scanHold = false;
  NimBLEDevice::getScan()->start(0, restartScan, false);
  // nazwa w głównej reklamie (iPhone/Bluefy nie zawsze czyta odpowiedź na skan), usługa w odpowiedzi
  NimBLEAdvertising *a = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData, respData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setName(pcName);
  respData.setCompleteServices(NimBLEUUID(NUS_SERVICE));
  a->setAdvertisementData(advData);
  a->setScanResponseData(respData);
  a->setScanResponse(true);
  a->setMinInterval(800);  // 500-625 ms: mało czasu radia zabranego nasłuchowi
  a->setMaxInterval(1000);
  pcAdvertise(advertise);
}

// z pętli: kanał PC, polecenia i dane dla telefonów, wznowienie reklamy
static void pcBleTick() {
  static uint32_t lastAdv = 0, lastDrones = 0, lastStatus = 0;
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) hostBleDrain();
  // po parowaniu (albo rozłączeniu) skanowanie dronów wraca po chwili
  static uint32_t pairDoneMs = 0;
  if (pairScanHold) {
    if (blePairing) pairDoneMs = now;
    else if (now - pairDoneMs > 3000) {
      pairScanHold = scanHold = false;
      NimBLEDevice::getScan()->start(0, restartScan, false);
    }
  }
  if (phoneCmdReady) {
    phoneHandleCommand(phoneCmd);
    phoneCmdReady = false;
  }
  if (now - lastAdv > 2000) {
    lastAdv = now;
    pcAdvertise(true);  // reklama także przy połączeniu: kilka telefonów i komputer naraz
  }
  if (!phoneSubscribed) return;
  if (phoneWantTags) {
    phoneWantTags = false;
    phoneSendTags();
  }
  if (now - lastDrones > PHONE_DRONE_MS) {
    lastDrones = now;
    for (auto &d : drones)
      if (d.used && now - d.lastMs < PHONE_DRONE_MAX_AGE_MS) phoneSendDrone(d, now);
  }
  if (now - lastStatus > PHONE_STATUS_MS) {
    lastStatus = now;
    char line[300];
    snprintf(line, sizeof(line),
             "{\"s\":{\"fw\":\"" FW_VERSION "\",\"dev\":\"cyd\",\"name\":\"%s\",\"bat\":-1,\"chg\":0,\"bmv\":0,\"mem\":%u,\"sd\":%s,"
             "\"c5\":[%s,%s],\"pc\":%s,\"mesh\":false,\"up\":%u,\"pend\":%u,\"hist\":0}}\n",
             pcName, sdLines, sdOk && sdEnabled ? "true" : "false", sensOnline(0) ? "true" : "false", sensOnline(1) ? "true" : "false",
             hostBleOn || (hostCmdMs && now - hostCmdMs < 30000) ? "true" : "false", now / 1000, tagPendingCount);
    phoneSend(line);
  }
}
