// Odbiór Open Drone ID przez BLE 4 (jak w firmware Heltec).
// ESP32 na CYD ma Bluetooth 4.2 - bez BLE 5 Long Range (Coded PHY).
// CYD nie słucha Wi-Fi: Wi-Fi 2,4 GHz i 5 GHz odbierają czujniki C5 nr 1 i nr 2 (sensors.h),
// a całe radio CYD jest dla Bluetooth (skan dronów, tablet, komputer).
// Dołączany z main.cpp po definicji Frame/enqueue i po wifidrone.h.
#pragma once

#include <NimBLEDevice.h>

static volatile uint32_t allBle = 0;

class OdidScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *dev) override {
    allBle++;
    const uint8_t *p = dev->getPayload();
    size_t len = dev->getPayloadLength();
    size_t i = 0;
    while (i + 1 < len) {
      uint8_t ln = p[i];
      if (ln == 0 || i + 1 + ln > len) break;
      uint8_t type = p[i + 1];
      const uint8_t *d = p + i + 2;
      // Service Data 16-bit UUID 0xFFFA, app code 0x0D
      if (type == 0x16 && ln >= 5 && d[0] == 0xFA && d[1] == 0xFF && d[2] == 0x0D) {
        const uint8_t *a = dev->getAddress().getNative();
        uint8_t mac[6] = {a[5], a[4], a[3], a[2], a[1], a[0]};
        enqueue(T_BLE, mac, dev->getRSSI(), d + 2, ln - 3);
        return;
      }
      i += 1 + ln;
    }
  }
};

static volatile bool scanHold = false;  // konfiguracja serwera BLE - stop() woła restartScan, który by wznowił skan

static void restartScan(NimBLEScanResults) {
  if (!scanHold) NimBLEDevice::getScan()->start(0, restartScan, false);
}

static void radioBegin() {
  wdBegin();  // drony Wi-Fi bez Remote ID przychodzą z czujników C5 (linie $AP)
  NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new OdidScanCallbacks(), true);
  scan->setActiveScan(false);
  scan->setDuplicateFilter(false);
  scan->setMaxResults(0);
  scan->setInterval(100);
  scan->setWindow(100);  // ciągły skan - radio nie jest dzielone z Wi-Fi
  scan->start(0, restartScan, false);  // Bluetooth działa zawsze
}
