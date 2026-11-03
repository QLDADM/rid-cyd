# RID CYD: wykrywacz dronów Remote ID z ekranem dotykowym 2,8" (wersja 1.6)

Firmware dla płytki **ESP32-2432S028**, znanej jako „Cheap Yellow Display” (CYD). Zamienia ją w samodzielny
wykrywacz dronów z kolorowym ekranem dotykowym:

- Odbiera Remote ID przez **Wi-Fi Beacon**, **Wi-Fi NAN** i **BLE 4**. BLE 5 Long Range nie działa, bo ESP32 na CYD ma Bluetooth 4.2.
- Pokazuje **listę dronów** i **szczegóły** (pozycja, wysokość, prędkość, operator). Radar i mapa są na tablecie.
- Wykrywa też **drony Wi-Fi bez Remote ID** (DJI Tello, Parrot, zabawkowe drony z kamerą Wi-Fi) po nazwie sieci albo producencie.
- Ma zakładkę **Czujniki** ze stanem czujników ESP32-C5 i zajętością kanałów Wi-Fi 2,4 / 5 GHz. Ekran gaśnie sam po minucie.
- Może być **centralą systemu w walizce**: przyjmuje wykrycia z dwóch czujników ESP32-C5 z antenami
  (2,4 i 5 GHz) przez przewody. Opis niżej.
- Pozwala oznaczać drony jako **swój / obcy**. Przy nowym obcym dronie piszczy i świeci na czerwono.
- Zapisuje wykryte drony na kartę **microSD**.
- Łączy się przez Bluetooth z [podglądem na telefonie](https://qldadm.github.io/dron-detector/), także razem z płytką Heltec.
- Przez USB działa jako sensor agenta RID Decoder na komputerze (opcjonalnie).

## Projekt Dron Detector

Ten firmware jest częścią projektu **Dron Detector** - pasywnego systemu wykrywania dronów. CYD jest w nim centralą systemu w walizce. Wszystkie części tylko **odbierają** sygnały - nic nie nadają w stronę drona.

| Część | Rola | Gdzie |
|---|---|---|
| **Strona podglądu** (telefon, tablet, komputer) | lista dronów, mapa, radar, widok 3D, wysokość nad terenem (AGL), pasma Wi-Fi | [dron-detector](https://github.com/QLDADM/dron-detector), **https://qldadm.github.io/dron-detector/** |
| **Heltec V3** | samodzielny wykrywacz Remote ID, alarmy LoRa do aplikacji Meshtastic | [dron-detector/firmware](https://github.com/QLDADM/dron-detector/tree/main/firmware) |
| **CYD** (ESP32-2432S028) | centrala systemu w walizce: ekran dotykowy, karta SD, czujniki ESP32-C5, Bluetooth do tabletu | [rid-cyd](https://github.com/QLDADM/rid-cyd) |
| **ESP32-C5** (2 szt., anteny zewnętrzne) | czujniki Remote ID na Wi-Fi 2,4 GHz i 5 GHz, podłączone przewodem do CYD | firmware w przygotowaniu |
| **M5StickC Plus2** | osobisty alarm w kieszeni, tryb szukania drona po sile sygnału | [rid-m5](https://github.com/QLDADM/rid-m5) |
| **Quansheng UV-K5** | wykrywanie łączy sterowania ELRS / Crossfire (433 / 868 / 915 MHz) i wideo FPV 1,2 GHz | [dron-k5](https://github.com/QLDADM/dron-k5) |

Firmware Heltec, CYD i M5 ma wspólny numer wersji (1.6), a strona podglądu własny (1.0).

---

## Wgranie krok po kroku (bez instalowania programów)

### Co jest potrzebne

- Płytka **ESP32-2432S028 (CYD)**.
- **Kabel USB do transmisji danych.** Część kabli służy tylko do ładowania i z nimi komputer nie widzi płytki.
  Jeśli nic się nie dzieje, najpierw zmień kabel.
- Komputer z przeglądarką **Google Chrome** albo **Microsoft Edge**. Firefox i Safari nie obsługują tej metody.

### Krok 1. Sprawdź, którą wersję płytki masz

CYD występuje w dwóch wersjach z różnymi ekranami, więc są dwa pliki:

| Płytka | Plik |
|---|---|
| jedno gniazdo **micro-USB** | `rid-cyd-ili9341-full.bin` |
| dwa gniazda: **micro-USB i USB-C** | `rid-cyd-st7789-full.bin` |

Jeśli po wgraniu ekran zostaje biały albo kolory są dziwne, wgraj drugi plik.

### Krok 2. Pobierz plik firmware

1. Wejdź na https://github.com/QLDADM/rid-cyd/releases.
2. Przy najnowszej wersji (np. **v1.6**) w sekcji **Assets** kliknij plik dla swojej płytki.
3. Plik trafi do folderu **Pobrane**. Nie otwieraj go i nie zmieniaj mu nazwy.

### Krok 3. Podłącz płytkę

1. Podłącz CYD kablem USB do komputera. Przy płytce z dwoma gniazdami użyj **micro-USB**.
2. Zamknij programy, które mogą używać płytki: agenta RID Decoder, Arduino, monitor portu.
   Port może naraz używać tylko jeden program.
3. **Windows:** CYD ma zwykle układ USB **CH340**. Jeśli w kroku 5 przeglądarka nie pokazuje żadnego portu,
   zainstaluj sterownik CH340 ze strony producenta: https://www.wch-ic.com/downloads/CH341SER_EXE.html.
   Potem odłącz i podłącz płytkę ponownie.

### Krok 4. Otwórz programator w przeglądarce

1. W Chrome albo Edge wejdź na **https://espressif.github.io/esptool-js/**.
2. Zostaw **puste** pole wyboru **WebUSB (CH340)** oraz **Show Debug log**.
3. W sekcji **Program** zostaw **Baudrate: 921600**.

### Krok 5. Połącz się z płytką

1. Kliknij **Connect**.
2. Wybierz port podobny do **„USB-SERIAL CH340 (COM5)”**. Numer COM może być inny. Kliknij **Połącz**.
3. Po kilku sekundach w konsoli na dole pojawi się **„ESP32”**, a w sekcji Program **pojawi się tabela z polem
   „Flash Address”**. Przed połączeniem tej tabeli nie ma.

**Jeśli połączenie się nie udaje:** przytrzymaj przycisk **BOOT** na płytce, krótko naciśnij **RST**, puść **BOOT**,
odśwież stronę (F5) i kliknij **Connect** jeszcze raz.

### Krok 6. Wgraj firmware

1. W polu **Flash Address** wpisz **`0x0`** (zero, iks, zero).
2. Kliknij **Wybierz plik** w tym samym wierszu i wskaż pobrany plik `.bin`.
3. Kliknij **Program** i poczekaj, aż pasek dojdzie do 100% (około 30–60 s). **Nie odłączaj kabla w trakcie.**
4. Naciśnij na płytce **RST** albo odłącz i podłącz kabel.

### Krok 7. Pierwsze uruchomienie

Na ekranie pojawi się lista dronów i zakładki na dole. Jeśli dotyk trafia obok, w **Ustawieniach** uruchom
**kalibrację dotyku** (trzy krzyżyki). Jeśli kolory są w negatywie, zmień tam **Odwróć kolory**.

Wgranie pliku `full.bin` kasuje ustawienia płytki (kalibracja dotyku, jasność, oznaczenia).
Pliki na karcie SD zostają.

---

## Ekran

- **Lista**: do 5 dronów na stronę (ponowne dotknięcie „Lista” pokazuje następną stronę). Kolorowy pasek oznacza
  swój (zielony), obcy (czerwony) albo nieoznaczony (żółty). Wiersz zawiera ID, RSSI, wysokość, prędkość,
  odległość do operatora i czas od ostatniego odbioru. Dotknięcie wiersza otwiera szczegóły.
- **Szczegóły**: dane drona i operatora, także klasa UE, prędkość pionowa, numer operatora, maksymalna wysokość,
  odległość dron–operator i **Odbiór** (CYD / C5-1 / C5-2), oraz przyciski **SWOJ / OBCY / BRAK / WSTECZ**.
- **Czujniki**: stan czujników C5 nr 1 i nr 2 dużym napisem (**DZIAŁA / CISZA / BRAK**, kanał, liczba ramek Remote ID,
  błędne linie, a przy braku podpowiedź, do którego złącza podłączyć przewód) i małe wykresy pasm Wi-Fi 2,4 / 5 GHz
  (czerwona ramka = kanał, na którym przez ostatnie 15 s słychać drona). Gdy któryś czujnik zamilknie, napis „Czujniki”
  na dolnym pasku robi się czerwony. Pełne pasma i radar są na tablecie - CYD nie ma GPS, więc radaru już nie ma. Stan
  czujników C5.
- **Górny pasek**: liczba dronów (w trakcie parowania PIN), czujniki **C1 / C2**, Bluetooth, karta SD, kanał i godzina.
- **Ustawienia**: kanał Wi-Fi (skakanie albo stały 6), jasność, odwrócenie kolorów, dźwięk (głośnik na złączu SPEAK),
  zapis na kartę SD, **Bluetooth wł./wył.** (tryb „tylko Wi-Fi”), **wygaszanie ekranu** (po 1 min / po 5 min / nigdy) i **kalibracja dotyku**.

Ekran gaśnie domyślnie po minucie bez dotyku (mniej prądu i ciepła w zamkniętej walizce). Pierwsze dotknięcie
tylko go budzi; budzi go też nowy dron i parowanie z PIN-em. Nasłuch, przekazywanie danych na tablet i alarm działają cały czas.

Dioda RGB świeci przy nowym dronie: czerwono przy obcym lub nieoznaczonym, zielono przy swoim.

## Bluetooth: telefon i komputer

CYD ogłasza się jako **RID-CYD-XXXX**. Połączenie wymaga sparowania 6-cyfrowym **PIN-em**, który widać
w **Ustawieniach** (wiersz Bluetooth) i na górnym pasku w trakcie parowania. Naraz można połączyć do 3 urządzeń.

- **Telefon:** strona https://qldadm.github.io/dron-detector/ (Android: Chrome, iPhone: Bluefy), przycisk **Połącz**
  albo **Dodaj płytkę**. Widać drony, stan płytki i oznaczenia, także razem z dronami z Heltec. CYD nie ma historii
  lotów na telefonie, bo zapisuje ramki na kartę SD.
- **Komputer:** agent RID Decoder łączy się z CYD kablem USB. Przez Bluetooth łączy się z komputerem tylko Heltec,
  bo Windows nie paruje się z układem ESP32 w CYD.
- Tryb **Bluetooth: wyl. (tylko Wi-Fi)** w Ustawieniach wyłącza też te połączenia.

## Czujniki ESP32-C5: CYD jako centrala systemu w walizce

CYD przyjmuje ramki z dwóch czujników **ESP32-C5** z zewnętrznymi antenami. Każdy czujnik słucha jednego pasma
i wysyła wykrycia przewodem (tylko TX czujnika → RX CYD, 115200 bod, 3,3 V):

| Czujnik | Pasmo | C5 | CYD |
|---|---|---|---|
| **C5 nr 1** | Wi-Fi 2,4 GHz | GPIO9 (TX) | **P3 IO35** |
| **C5 nr 2** | Wi-Fi 5 GHz | GPIO9 (TX) | **CN1 IO27** |
| oba | | G | GND (wspólna masa obowiązkowa) |

Każdy czujnik ma osobne zasilanie USB. Pinów 5 V nie łączyć.

- Ramki Remote ID z czujników trafiają do tego samego dekodera co z radia CYD: lista, alarmy, oznaczenia, karta SD,
  telefon i agent działają tak samo. W szczegółach drona wiersz **Odbiór** mówi, kto go słyszał (np. `CYD+C5-1`).
- Na górnym pasku **C1** i **C2**: zielony = czujnik działa, czerwony = zamilkł, szary = nie był podłączony.
- Zakładka **Czujniki**: 2,4 GHz z C5 nr 1, 5 GHz z C5 nr 2 (CYD nie podaje własnej zajętości kanałów) i stan obu czujników
  (kanał, liczba ramek Remote ID, błędne linie). Te same dane idą na tablet i do agenta (panele pasm na stronach).
- Na tablet CYD wysyła też kanał, na którym słyszy drona Remote ID (pole `ch`), więc panel pasm na stronie zaznacza go na czerwono.
- Format linii czujnika (z sumą kontrolną jak w NMEA) jest opisany w `src/sensors.h`.
- Obsługa modułu nRF24L01 została usunięta: piny IO27 i IO35 służą teraz czujnikom, a regulacja jasności ekranu działa zawsze.

**Test bez czujników:** polecenie USB `C5 <1|2> <linia>` podaje linię tak, jakby przyszła przewodem.
Skrypt `tools/c5_sim.py` udaje oba czujniki (drony Remote ID na 2,4 i 5 GHz, dron Wi-Fi, pasma, stan):

Najprościej: zamknij agenta RID Decoder i kliknij dwukrotnie `tools/test_c5.bat`. Skrypt sam znajdzie port CYD
(układ CH340), w razie potrzeby doinstaluje `pyserial` i wyśle drony testowe wokół rynku w Nowym Wiśniczu:
100 m na NE (C5 nr 1, kanał 6), 500 m na SSW (C5 nr 2, 5 GHz, kanał 44), 3 km na WNW (C5 nr 1, kanał 1)
i drona Wi-Fi TELLO (kanał 11). Ctrl+C kończy test. Z wiersza poleceń:

```powershell
python tools/c5_sim.py                                   # port szukany sam
python tools/c5_sim.py --port COM11 --lat 50.06 --lon 19.94 --time 600   # inny port, inne miejsce, 10 min
```

## Karta SD

Pliki `/rid/rid-RRRRMMDD.jsonl` (gdy płytka zna czas) albo `/rid/boot-NNNN.jsonl`. Jedna linia na ramkę,
najwyżej 1 ramka danego typu z drona na 3 s:
`{"t":unix,"boot":N,"up":s,"tr":"ble","mac":"..","rssi":-70,"ch":6,"src":"cyd","d":"<hex>"}`
(`src`: `cyd` = radio płytki, `c5-1` / `c5-2` = czujnik).

---

## Dla zaawansowanych

### Wgranie z wiersza poleceń

```powershell
pip install esptool
esptool --chip esp32 --baud 921600 write_flash 0x0 rid-cyd-ili9341-full.bin
```

### Budowanie ze źródeł

```powershell
pip install platformio
pio run -e cyd -t upload        # płytka z jednym micro-USB (ekran ILI9341)
pio run -e cyd2usb -t upload    # płytka z micro-USB i USB-C (ekran ST7789)
```

Wgranie przez PlatformIO nie kasuje ustawień.

### Protokół USB (921600 bod)

Linie JSON: ramki `{"t":...,"dev":"cyd","src":"cyd|c5-1|c5-2"}`, status (z polami `c5`, `c5_rid`, `c5_bad`),
drony Wi-Fi `{"wd":{...}}`, pasma `{"cs":{"b":"2G4|5G","src":..,"c":[kanały],"r":[ramek/s],"p":[dBm]}}`.
Polecenia: `ID`, `CH <n>`, `TIME <unix>`, `TAG <id> own|foreign|-`, `TAGS`, `TAGCLR`, `TAGPEND`, `PING`,
`TRAW` (surowe odczyty dotyku przez 20 s), `C5 <1|2> <linia>` (test czujnika bez płytki C5).
