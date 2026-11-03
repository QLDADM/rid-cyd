"""Udaje dwa czujniki ESP32-C5 podłączone do CYD - test centrali bez płytek C5.

Wysyła do CYD przez kabel USB polecenia "C5 <1|2> <linia>", a CYD traktuje je tak,
jakby linia przyszła przewodem z czujnika (GPIO35 / GPIO27). Sprawdza całą drogę:
sumy kontrolne, dekodowanie Remote ID, drony Wi-Fi, pasma 2,4 i 5 GHz, stan czujników
na ekranie, przekazanie na tablet i do agenta.

    pip install pyserial
    python tools/c5_sim.py                        (port CYD szukany sam; agent RID Decoder musi być zamknięty)
    python tools/c5_sim.py --port COM5 --lat 50.06 --lon 19.94 --time 600
    albo dwuklik na tools/test_c5.bat (Windows)

Co wysyła (drony wokół --lat/--lon, lista DRONES):
  C5 nr 1 (2,4 GHz): drony Remote ID przez Wi-Fi Beacon - 100 m na NE (kanał 6) i 3 km na WNW (kanał 1),
                     dron Wi-Fi bez RID (TELLO, kanał 11), $HB co 5 s, $CS 2,4 GHz co 10 s
  C5 nr 2 (5 GHz):   dron Remote ID przez Wi-Fi NAN - 500 m na SSW (kanał 44), $HB, $CS 5 GHz
  co 30 s jedną linię z błędną sumą - CYD ma ją odrzucić (licznik "bledy" w zakładce Pasmo).
"""
from __future__ import annotations

import argparse
import math
import random
import struct
import sys
import threading
import time
from datetime import datetime, timezone

import serial

# wysokość geodezyjna (nad elipsoidą WGS-84) terenu w Nowym Wiśniczu: 275 m n.p.m. (NMT GUGiK) + 38,6 m geoidy -
# AGL na stronach wychodzi wtedy realny
GROUND_HAE = 275 + 38.6
EPOCH_2019 = datetime(2019, 1, 1, tzinfo=timezone.utc).timestamp()
CH_5G = [36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140]


# ---- wiadomości Open Drone ID (jak agent/simulate.py w RID Decoder)
def pad(s: str, n: int) -> bytes:
    return s.encode("ascii")[:n].ljust(n, b"\x00")


def alt_enc(m: float) -> int:
    return int(round((m + 1000) / 0.5))


def basic_id(uas_id: str) -> bytes:
    return bytes([0x02, (1 << 4) | 2]) + pad(uas_id, 20) + b"\x00" * 3


def location(lat, lon, height, speed, track) -> bytes:
    ew = 1 if track >= 180 else 0
    flags = (2 << 4) | (ew << 1)
    secs = time.time() % 3600
    return (bytes([0x12, flags, int(track) - 180 * ew, int(round(speed / 0.25))]) + struct.pack("<b", 0)
            + struct.pack("<ii", int(lat * 1e7), int(lon * 1e7))
            + struct.pack("<HHH", alt_enc(GROUND_HAE - 2 + height), alt_enc(GROUND_HAE + height), alt_enc(height))
            + bytes([(4 << 4) | 10, (3 << 4) | 2]) + struct.pack("<H", int(secs * 10)) + bytes([5, 0]))


def system(op_lat, op_lon) -> bytes:
    return (bytes([0x42, (1 << 2) | 1]) + struct.pack("<ii", int(op_lat * 1e7), int(op_lon * 1e7))
            + struct.pack("<H", 1) + bytes([0]) + struct.pack("<HH", 0, 0) + bytes([(1 << 4) | 2])
            + struct.pack("<H", alt_enc(GROUND_HAE + 1.5)) + struct.pack("<I", int(time.time() - EPOCH_2019)) + b"\x00")


def operator_id(op: str) -> bytes:
    return bytes([0x52, 0]) + pad(op, 20) + b"\x00" * 3


def pack(msgs: list[bytes]) -> bytes:
    return bytes([0xF2, 25, len(msgs)]) + b"".join(msgs)


# ---- testowe drony: odległość [m] i kierunek [°] od punktu, mały okrąg wokół tego miejsca
DRONES = [
    dict(id="C5TEST-100M", dist=100, brg=45, orbit=20, w=0.08, h=40, node=1, tr="WIFI", ch=6, band="2G4",
         mac="60:60:1f:c5:01:00", rssi=-50),
    dict(id="C5TEST-500M", dist=500, brg=200, orbit=60, w=-0.05, h=90, node=2, tr="NAN", ch=44, band="5G",
         mac="9c:00:00:c5:05:00", rssi=-66),
    dict(id="C5TEST-3KM", dist=3000, brg=300, orbit=150, w=0.03, h=120, node=1, tr="WIFI", ch=1, band="2G4",
         mac="60:60:1f:c5:30:00", rssi=-82),
]


def offset(lat0: float, dist: float, brg: float) -> tuple[float, float]:
    """Przesunięcie (dlat, dlon) w stopniach o dist metrów w kierunku brg."""
    r = math.radians(brg)
    return dist * math.cos(r) / 111320, dist * math.sin(r) / (111320 * math.cos(math.radians(lat0)))


# ---- linie czujnika (format z PROJEKT_wykrywanie_dronow.md)
def nmea(body: str) -> str:
    x = 0
    for c in body.encode("ascii"):
        x ^= c
    return f"${body}*{x:02X}"


def ssid_enc(s: str) -> str:
    return "".join(c if 0x21 <= ord(c) <= 0x7E and c not in ",*%" else f"%{ord(c):02X}" for c in s)


def find_cyd_port() -> str | None:
    """Port CYD: układ USB CH340 (VID 1A86). Przy kilku albo żadnym - lista portów i podpowiedź --port."""
    from serial.tools import list_ports
    ports = list(list_ports.comports())
    ch340 = [p for p in ports if p.vid == 0x1A86 or "CH340" in (p.description or "")]
    if len(ch340) == 1:
        print(f"CYD na porcie {ch340[0].device} ({ch340[0].description})")
        return ch340[0].device
    print("Nie wiem, który port to CYD. Porty w komputerze:")
    for p in ports:
        print(f"  {p.device}: {p.description}")
    print("Uruchom ponownie z numerem portu, np.:  python c5_sim.py --port COM11")
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="port CYD, np. COM5 (bez podania: szukany sam - układ USB CH340)")
    p.add_argument("--lat", type=float, default=49.9180, help="środek testu (domyślnie rynek w Nowym Wiśniczu)")
    p.add_argument("--lon", type=float, default=20.4683)
    p.add_argument("--time", type=int, default=0, help="ile sekund (0 = do Ctrl+C)")
    a = p.parse_args()

    if not a.port:
        a.port = find_cyd_port()
        if not a.port:
            return 1
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 921600, 0.2
    s.dtr = False  # bez resetu płytki przy otwarciu (jak agent) - liczniki CYD zostają
    s.rts = False
    s.open()
    stop = threading.Event()

    def reader():
        buf = b""
        while not stop.is_set():
            buf += s.read(4096)
            *lines, buf = buf.split(b"\n")
            for ln in lines:
                t = ln.decode("utf-8", "replace").strip()
                if '"c5"' in t or "czujnik" in t or '"cs"' in t:
                    print("CYD:", t[:200])

    threading.Thread(target=reader, daemon=True).start()

    def send(node: int, body: str, bad: bool = False):
        line = nmea(body)
        if bad:
            line = line[:-2] + "00"
        s.write(f"C5 {node} {line}\n".encode("ascii"))
        time.sleep(0.03)  # CYD czyta USB w pętli razem z ekranem - bez pośpiechu

    t0 = time.time()
    n = 0
    last_hb = last_cs = last_bad = 0.0
    print(f"symuluję C5 nr 1 i nr 2 na {a.port} (Ctrl+C kończy)")
    try:
        while not stop.is_set():
            t = time.time() - t0
            up = int(t * 1000)
            # drony Remote ID w różnych odległościach i kierunkach od punktu (DRONES), każdy krąży po małym okręgu
            for i, dr in enumerate(DRONES):
                dlat, dlon = offset(a.lat, dr["dist"], dr["brg"])
                ang = dr["w"] * t + i
                clat, clon = offset(a.lat, dr["orbit"], math.degrees(ang))
                lat, lon = a.lat + dlat + clat, a.lon + dlon + clon
                olat, olon = offset(a.lat, 80, dr["brg"] + 150)  # operator ok. 80 m od drona
                track = (math.degrees(ang) + (90 if dr["w"] > 0 else -90)) % 360
                msgs = [basic_id(dr["id"]), location(lat, lon, dr["h"] + 5 * math.sin(t / 8 + i), abs(dr["w"]) * dr["orbit"], track),
                        system(a.lat + dlat + olat, a.lon + dlon + olon), operator_id(f"POLc5test{i:07d}-abc")]
                rssi = dr["rssi"] - random.randint(0, 6)
                if dr["tr"] == "WIFI":
                    data = bytes([0xFA, 0x0B, 0xBC, 0x0D, n & 0xFF]) + pack(msgs)
                else:  # NAN: licznik + Message Pack
                    data = bytes([n & 0xFF]) + pack(msgs)
                send(dr["node"], f"RID,{dr['node']},{up},{dr['tr']},{dr['mac']},{rssi},{dr['ch']},{dr['band']},{data.hex()}")
            # dron Wi-Fi bez Remote ID (nazwa sieci z przecinkiem sprawdza kodowanie %XX)
            if n % 3 == 0:
                send(1, f"AP,1,{up},60:60:1f:aa:bb:cc,{-50 - random.randint(0, 5)},11,2G4,{ssid_enc('TELLO-5A3F2B,test')}")
            if t - last_hb >= 5:
                last_hb = t
                send(1, f"HB,1,{up},6,2G4,{n},0.1")
                send(2, f"HB,2,{up},44,5G,{n},0.1")
            if t - last_cs >= 10:
                last_cs = t
                cs1 = ";".join(f"{c}:{random.choice([0, 3, 12, 40, 150])}:{random.randint(-90, -45)}" for c in range(1, 14))
                cs2 = ";".join(f"{c}:{random.choice([0, 0, 2, 8, 30])}:{random.randint(-92, -60)}" for c in CH_5G)
                send(1, f"CS,1,2G4,{cs1}")
                send(2, f"CS,2,5G,{cs2}")
            if t - last_bad >= 30:
                last_bad = t
                send(1, f"HB,1,{up},6,2G4,{n},0.1", bad=True)
            n += 1
            if a.time and t > a.time:
                break
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    stop.set()
    s.close()


if __name__ == "__main__":
    sys.exit(main())
