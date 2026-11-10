#!/usr/bin/env python3
"""Sends signals to cluster_gateway on the QNX target over UDP. Run it in the Ubuntu (WSL) terminal.

Text signals (one datagram, the gateway holds the values):
    python3 send_signal.py 192.168.122.146 speed=120 rpm=3000 gear=3 mode=sport warn=check,left
Interactive text (one command per line, empty line or Ctrl+D quits):
    python3 send_signal.py 192.168.122.146 -i

Virtual CAN frames. This simulates an ECU: it sends the frames 20 times a second, each with an alive
counter and a CRC (cluster_can.h), until --seconds have passed or you press Ctrl+C:
    python3 send_signal.py 192.168.122.146 --can speed=120 rpm=3000 gear=3 mode=sport
    python3 send_signal.py 192.168.122.146 --can speed=120 --seconds 30 --fault crc
  --hz N         frames per second (default 20)
  --seconds N    how long to send (default 15)
  --fault NAME   from t=6 s to t=12 s: crc (every 5th frame corrupted), burst (all frames corrupted for 2 s),
                 repeat (every 5th frame repeats the previous counter), lost (every 3rd frame not sent),
                 wrongseq (the counter jumps by 100 once), silent (nothing is sent for 2 s)
A driving profile instead of a fixed speed (with --can). Items are separated by spaces:
    hold:SPEED:SECONDS   keep about SPEED km/h; the value wanders smoothly within +-3 km/h (--jitter N)
    ramp:SPEED:SECONDS   change to SPEED km/h in a straight line over SECONDS
    set:key=value[:key=value]   change mode, warn, fuel or coolant at that moment (warn=check,oil is fine)
  The gear and the rpm follow the speed unless you give gear= or rpm=. --seconds defaults to the profile length.
    python3 send_signal.py 192.168.122.146 --can mode=comfort --profile "ramp:30:3 hold:30:4 ramp:50:12"
One raw frame, exactly the bytes you give (hex): the gateway rejects it unless counter and CRC are right.
    python3 send_signal.py 192.168.122.146 --can-raw 0x100 10 27 b8 0b 03 00 07 31
Check the CRC code against the C test vectors:
    python3 send_signal.py --selftest
Optional: --port 5555
"""
import random
import select
import socket
import struct
import sys
import time

MODES = {"eco": 0, "comfort": 1, "sport": 2}
WARN = {"check": 1, "check_engine": 1, "battery": 2, "oil": 4, "seatbelt": 8, "left": 16, "right": 32}
DATA_ID = {0x100: 0xA1, 0x101: 0xA2}   # mixed into the CRC, never sent (cluster_can.h)
FAULTS = ("crc", "burst", "repeat", "lost", "wrongseq", "silent")


def crc8_update(crc, data):
    # CRC-8/SAE-J1850: poly 0x1D, no reflection (cluster_e2e.h); init 0xFF and xor-out 0xFF are applied by the caller
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1D) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def can_crc(can_id, d):
    return crc8_update(crc8_update(0xFF, d[:7]), [DATA_ID[can_id]]) ^ 0xFF


def protect(can_id, d5, counter):
    """5 signal bytes -> 8 data bytes with reserved byte, counter and CRC."""
    d = bytes(d5) + bytes([0, counter & 0xFF])
    return d + bytes([can_crc(can_id, d)])


def can_frame(can_id, data, dlc=None):
    # Linux struct can_frame: id (u32 LE), dlc (u8), 3 padding bytes, 8 data bytes
    return struct.pack("<IB3x8s", can_id, len(data) if dlc is None else dlc, bytes(data).ljust(8, b"\0"))


def signal_bytes(kv):
    gear = 0 if kv.get("gear", "0").upper() == "N" else int(kv.get("gear", 0))
    motion = struct.pack("<HHB", round(float(kv.get("speed", 0)) * 100), round(float(kv.get("rpm", 800))), gear)
    w = kv.get("warn", "none")
    warn = 0 if w == "none" else (int(w, 0) if w[0].isdigit() else sum(WARN[x] for x in w.split(",")))
    status = struct.pack("<BBBH", round(float(kv.get("fuel", 60)) * 2), round(float(kv.get("coolant", 90)) + 40),
                         MODES[kv.get("mode", "comfort")], warn)
    return motion, status


SHIFT_UP = (15, 35, 60, 90, 120)   # km/h at which gear 1 -> 2, 2 -> 3, ...; it shifts back 6 km/h lower


def auto_gear(speed, prev=0):
    """Gear for a speed. `prev` (the gear before) gives a 6 km/h hysteresis, so a speed that wanders
    around a shift point does not make the gear flip back and forth."""
    if speed < 0.5:
        return 0
    gear = prev if prev > 0 else 1
    while gear < 6 and speed >= SHIFT_UP[gear - 1]:
        gear += 1
    while gear > 1 and speed < SHIFT_UP[gear - 2] - 6:
        gear -= 1
    return gear


def auto_rpm(speed, gear):
    per_kmh = (95, 95, 60, 42, 32, 25, 20)   # rpm per km/h in each gear (a rough made-up gearbox)
    return min(7000, 800 + speed * per_kmh[gear])


def parse_profile(spec):
    """'ramp:30:3 hold:30:4 set:mode=sport' -> [(kind, a, b), ...]"""
    segs = []
    for item in spec.split():
        parts = item.split(":")
        if parts[0] in ("hold", "ramp") and len(parts) == 3:
            segs.append((parts[0], float(parts[1]), float(parts[2])))
        elif parts[0] == "set" and len(parts) >= 2:
            segs.append(("set", dict(p.split("=", 1) for p in parts[1:]), 0.0))
        else:
            raise ValueError(f"bad profile item '{item}' (hold:SPEED:SECONDS, ramp:SPEED:SECONDS or set:key=value)")
    return segs


class Profile:
    """Speed over time. Call speed(t) with growing t: the wander in a hold is a small state machine."""

    def __init__(self, segs, start_speed=0.0, jitter=3.0):
        self.items = []                       # (t_start, kind, a, b, speed at the start)
        t, v = 0.0, start_speed
        for kind, a, b in segs:
            self.items.append((t, kind, a, b, v))
            if kind != "set":
                v, t = a, t + b
        self.duration = t
        self.jitter = jitter
        self.wander = 0.0
        self.target = 0.0
        self.next_target = 0.0
        self.last_t = 0.0
        self.settings = {}
        self.start_speed = start_speed

    def speed(self, t):
        nominal, active = self.start_speed, None
        for ts, kind, a, b, v0 in self.items:
            if t < ts:
                break
            if kind == "set":
                self.settings.update(a)
                continue
            if t < ts + b:
                active = kind
                nominal = a if kind == "hold" else v0 + (a - v0) * (t - ts) / b
                break
            nominal = a
        dt, self.last_t = max(0.0, t - self.last_t), t
        if active == "hold" and self.jitter > 0:
            if t >= self.next_target:         # pick a new offset every 0.5-1.2 s and drift towards it
                self.target = random.uniform(-self.jitter, self.jitter)
                self.next_target = t + random.uniform(0.5, 1.2)
            step = max(-3.0 * dt, min(3.0 * dt, self.target - self.wander))   # at most 3 km/h per second
            self.wander += step
        else:
            self.wander *= 0.9                # no wander while ramping: fade it out
        return max(0.0, nominal + self.wander)


def selftest():
    assert crc8_update(0xFF, b"123456789") ^ 0xFF == 0x4B, "CRC-8/SAE-J1850 check value"
    prof = Profile(parse_profile("ramp:30:3 hold:30:4 ramp:50:12 set:mode=sport:warn=check,oil"), 0.0, 3.0)
    seen = {}
    for i in range(0, 20 * 20 + 1):
        t = i / 20
        seen[i] = prof.speed(t)
    assert abs(seen[30] - 15.0) < 0.01, f"ramp midpoint {seen[30]}"
    assert all(27 <= seen[i] <= 33 for i in range(60, 140)), "hold stays within +-3 km/h"
    assert max(seen[i] for i in range(60, 140)) - min(seen[i] for i in range(60, 140)) > 0.5, "hold wanders"
    assert abs(seen[19 * 20] - 50) < 0.5, f"ramp end {seen[19 * 20]}"
    assert prof.settings == {"mode": "sport", "warn": "check,oil"}, prof.settings
    assert [auto_gear(s) for s in (0, 10, 20, 40, 70, 100, 130)] == [0, 1, 2, 3, 4, 5, 6]
    # hysteresis: gear 5 starts at 90 km/h but is only left below 84 km/h
    assert auto_gear(91, 4) == 5 and auto_gear(88, 5) == 5 and auto_gear(85, 5) == 5 and auto_gear(83, 5) == 4
    print("selftest: profile and gear table ok")
    d = protect(0x100, [0x10, 0x27, 0xB8, 0x0B, 0x03], 7)
    assert d[7] == 0x31, f"CAN 0x100 vector: {d[7]:#x}"
    e = protect(0x101, [0x3C, 0x82, 0x01, 0x24, 0x00], 9)
    assert e[7] == 0x58, f"CAN 0x101 vector: {e[7]:#x}"
    print("selftest ok: the CRC code matches the C test vectors (0x4B, 0x31, 0x58)")


class Replies:
    def __init__(self, sock):
        self.sock = sock
        self.ok = self.err = 0
        self.last_err = ""

    def drain(self):
        while select.select([self.sock], [], [], 0)[0]:
            text = self.sock.recvfrom(512)[0].decode(errors="replace").strip()
            if text.startswith("ok"):
                self.ok += 1
            else:
                self.err += 1
                self.last_err = text


def run_ecu(sock, dest, kv, hz, seconds, fault, profile=None):
    motion, status = signal_bytes(kv)
    counters = {0x100: 0, 0x101: 0}
    prev = {}
    n = 0
    rep = Replies(sock)
    t0 = time.monotonic()
    nxt = t0
    last_report = -1
    seq_jumped = False
    gear_now = 0
    print(f"sending CAN 0x100 and 0x101 at {hz} Hz for {seconds} s" + (f", fault {fault} at t=6..12 s" if fault else "") + " (Ctrl+C stops)")
    try:
        while True:
            t = time.monotonic() - t0
            if t > seconds:
                break
            if profile:
                now_kv = dict(kv)
                speed = profile.speed(t)
                now_kv.update(profile.settings)
                now_kv["speed"] = speed
                if "gear" not in now_kv:
                    gear_now = auto_gear(speed, gear_now)
                    now_kv["gear"] = str(gear_now)              # signal_bytes() takes the same strings as the command line
                if "rpm" not in now_kv:
                    now_kv["rpm"] = str(round(auto_rpm(speed, int(now_kv["gear"]) if now_kv["gear"].isdigit() else 1)))
                motion, status = signal_bytes(now_kv)
            active = 6 <= t < 12
            n += 1
            skip = fault == "silent" and 6 <= t < 8
            for can_id, d5 in ((0x100, motion), (0x101, status)):
                if fault == "lost" and active and n % 3 == 0:
                    counters[can_id] = (counters[can_id] + 1) & 0xFF   # a frame lost on the wire
                    continue
                if fault == "wrongseq" and active and not seq_jumped and can_id == 0x100:
                    counters = {k: (v + 100) & 0xFF for k, v in counters.items()}
                    seq_jumped = True
                data = protect(can_id, d5, counters[can_id])
                counters[can_id] = (counters[can_id] + 1) & 0xFF
                if fault == "crc" and active and n % 5 == 0:
                    data = data[:7] + bytes([data[7] ^ 0x01])            # corrupt after the CRC
                if fault == "burst" and 6 <= t < 8:
                    data = bytes([data[0] ^ 0x01]) + data[1:]
                if fault == "repeat" and active and n % 5 == 0 and can_id in prev:
                    data = prev[can_id]                                  # same counter again
                prev[can_id] = data
                if not skip:
                    sock.sendto(can_frame(can_id, data), dest)
            rep.drain()
            if int(t) != last_report:
                last_report = int(t)
                extra = f"  last error: {rep.last_err}" if rep.err else ""
                drive = f" speed={speed:5.1f} gear={now_kv['gear']} rpm={now_kv['rpm']}" if profile else ""
                print(f"t={int(t):3d}s{drive} replies ok={rep.ok} err={rep.err}{extra}")
            nxt += 1.0 / hz
            time.sleep(max(0.0, nxt - time.monotonic()))
    except KeyboardInterrupt:
        print("stopped")
    rep.drain()
    print(f"done: replies ok={rep.ok} err={rep.err}")


def send_once(sock, dest, payload):
    sock.sendto(payload, dest)
    try:
        print(sock.recvfrom(512)[0].decode().strip())
    except socket.timeout:
        print("(no reply: is cluster_gateway running and the IP correct?)")


def option(args, name, default, cast):
    if name in args:
        i = args.index(name)
        value = cast(args[i + 1])
        del args[i:i + 2]
        return value
    return default


def main():
    args = sys.argv[1:]
    if args and args[0] == "--selftest":
        selftest()
        return 0
    port = option(args, "--port", 5555, int)
    hz = option(args, "--hz", 20, float)
    seconds = option(args, "--seconds", None, float)
    profile_spec = option(args, "--profile", None, str)
    jitter = option(args, "--jitter", 3.0, float)
    fault = option(args, "--fault", None, str)
    if fault is not None and fault not in FAULTS:
        print(f"unknown fault '{fault}'. valid: {' '.join(FAULTS)}")
        return 2
    if len(args) < 2:
        print(__doc__)
        return 2
    dest = (args[0], port)
    rest = args[1:]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)
    if rest[0] == "-i":
        print("type e.g. speed=120 rpm=3000 gear=3   (empty line quits)")
        for line in sys.stdin:
            if not line.strip():
                break
            send_once(sock, dest, line.strip().encode())
    elif rest[0] == "--can":
        kv = dict(p.split("=", 1) for p in rest[1:])
        profile = None
        if profile_spec:
            try:
                profile = Profile(parse_profile(profile_spec), float(kv.get("speed", 0)), jitter)
            except ValueError as e:
                print(e)
                return 2
        if seconds is None:
            seconds = profile.duration if profile else 15
        run_ecu(sock, dest, kv, hz, seconds, fault, profile)
    elif rest[0] == "--can-raw":
        data = bytes(int(x, 16) for x in rest[2:])
        send_once(sock, dest, can_frame(int(rest[1], 0), data))
    else:
        send_once(sock, dest, " ".join(rest).encode())
    return 0


if __name__ == "__main__":
    sys.exit(main())
