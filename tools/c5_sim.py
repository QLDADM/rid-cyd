"""Udaje dwa czujniki ESP32-C5 podłączone do CYD - test centrali bez płytek C5.

Wysyła do CYD przez kabel USB polecenia "C5 <1|2> <linia>", a CYD traktuje je tak,
jakby linia przyszła przewodem z czujnika (GPIO35 / GPIO27). Sprawdza całą drogę:
sumy kontrolne, dekodowanie Remote ID, drony Wi-Fi, pasma 2,4 i 5 GHz, stan czujników
na ekranie, przekazanie na tablet i do agenta.

    pip install pyserial
    python tools/c5_sim.py --port COM5            (agent RID Decoder musi być wtedy zamknięty)

Co wysyła:
  C5 nr 1 (2,4 GHz): dron Remote ID przez Wi-Fi Beacon (kanał 6), dron Wi-Fi bez RID (TELLO),
                     $HB co 5 s, $CS 2,4 GHz co 10 s
  C5 nr 2 (5 GHz):   dron Remote ID przez Wi-Fi NAN (kanał 149), $HB, $CS 5 GHz
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
            + struct.pack("<HHH", alt_enc(108 + height), alt_enc(110 + height), alt_enc(height))
            + bytes([(4 << 4) | 10, (3 << 4) | 2]) + struct.pack("<H", int(secs * 10)) + bytes([5, 0]))


def system(op_lat, op_lon) -> bytes:
    return (bytes([0x42, (1 << 2) | 1]) + struct.pack("<ii", int(op_lat * 1e7), int(op_lon * 1e7))
            + struct.pack("<H", 1) + bytes([0]) + struct.pack("<HH", 0, 0) + bytes([(1 << 4) | 2])
            + struct.pack("<H", alt_enc(110)) + struct.pack("<I", int(time.time() - EPOCH_2019)) + b"\x00")


def operator_id(op: str) -> bytes:
    return bytes([0x52, 0]) + pad(op, 20) + b"\x00" * 3


def pack(msgs: list[bytes]) -> bytes:
    return bytes([0xF2, 25, len(msgs)]) + b"".join(msgs)


# ---- linie czujnika (format z PROJEKT_wykrywanie_dronow.md)
def nmea(body: str) -> str:
    x = 0
    for c in body.encode("ascii"):
        x ^= c
    return f"${body}*{x:02X}"


def ssid_enc(s: str) -> str:
    return "".join(c if 0x21 <= ord(c) <= 0x7E and c not in ",*%" else f"%{ord(c):02X}" for c in s)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", required=True, help="port CYD, np. COM5")
    p.add_argument("--lat", type=float, default=52.392)
    p.add_argument("--lon", type=float, default=20.783)
    p.add_argument("--time", type=int, default=0, help="ile sekund (0 = do Ctrl+C)")
    a = p.parse_args()

    s = serial.Serial(a.port, 921600, timeout=0.2)
    s.dtr = False  # bez resetu płytki (jak agent)
    s.rts = False
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
            # dron 1: Wi-Fi Beacon 2,4 GHz z C5 nr 1, krąży 300 m od punktu
            ang = 0.05 * t
            lat1 = a.lat + 300 * math.cos(ang) / 111320
            lon1 = a.lon + 300 * math.sin(ang) / (111320 * math.cos(math.radians(a.lat)))
            msgs = [basic_id("C5TEST-24-0001"), location(lat1, lon1, 60 + 10 * math.sin(t / 8), 15, (math.degrees(ang) + 90) % 360),
                    system(a.lat - 0.001, a.lon - 0.001), operator_id("POLc5test0000001-abc")]
            ie = bytes([0xFA, 0x0B, 0xBC, 0x0D, n & 0xFF]) + pack(msgs)
            send(1, f"RID,1,{up},WIFI,60:60:1f:c5:00:01,{-58 - random.randint(0, 6)},6,2G4,{ie.hex()}")
            # dron 2: NAN 5 GHz z C5 nr 2, 700 m na północny wschód
            ang2 = -0.03 * t
            lat2 = a.lat + 0.005 + 200 * math.cos(ang2) / 111320
            lon2 = a.lon + 0.004 + 200 * math.sin(ang2) / (111320 * math.cos(math.radians(a.lat)))
            msgs2 = [basic_id("C5TEST-5G-0002"), location(lat2, lon2, 110, 9, (math.degrees(ang2) - 90) % 360),
                     system(a.lat + 0.004, a.lon + 0.003)]
            si = bytes([n & 0xFF]) + pack(msgs2)
            send(2, f"RID,2,{up},NAN,9c:00:00:c5:00:02,{-70 - random.randint(0, 6)},149,5G,{si.hex()}")
            # dron Wi-Fi bez Remote ID (nazwa sieci z przecinkiem sprawdza kodowanie %XX)
            if n % 3 == 0:
                send(1, f"AP,1,{up},60:60:1f:aa:bb:cc,{-50 - random.randint(0, 5)},11,2G4,{ssid_enc('TELLO-5A3F2B,test')}")
            if t - last_hb >= 5:
                last_hb = t
                send(1, f"HB,1,{up},6,2G4,{n},0.1")
                send(2, f"HB,2,{up},149,5G,{n},0.1")
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
