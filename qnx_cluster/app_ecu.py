"""The sender side of the test app: a virtual ECU that sends the CAN frames 0x100 and 0x101 to cluster_gateway.

It uses the CRC, the frame layout, the gear table and the profile code of send_signal.py. The difference is that
the signals can be changed while it runs (sliders in the app) and a fault can be switched on at any time.
"""
import json
import math
import os
import re
import shlex
import socket
import threading
import time

import send_signal as ss

FAULT_SECONDS = {"crc": 6, "burst": 2, "repeat": 6, "lost": 6, "wrongseq": 0, "silent": 2}
WARN_NAMES = ("check", "battery", "oil", "seatbelt", "left", "right")
DEFAULTS = {"speed": 0.0, "rpm": None, "gear": None, "mode": "comfort", "warn": [], "fuel": 60.0, "coolant": 90.0}
SAMPLE_KEYS = ("speed", "rpm", "gear", "mode", "warn", "fuel", "coolant")
MAX_RECORD_SAMPLES = 24000          # 20 minutes at 20 Hz
# what the CAN frames can carry (send_signal.signal_bytes): a value outside would stop the sender thread
LIMITS = {"speed": (0.0, 655.35), "rpm": (0.0, 65535.0), "gear": (0, 255), "fuel": (0.0, 127.5), "coolant": (-40.0, 215.0)}
RECORDINGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recordings")


def list_recordings():
    try:
        return sorted(f[:-5] for f in os.listdir(RECORDINGS) if f.endswith(".json"))
    except OSError:
        return []


def load_scenarios(path):
    """The profiles of scenarios.sh: name -> profile text. Read from the script so there is one list only."""
    text = open(path, encoding="utf-8").read()
    body = text[text.index("profile_of()"):]
    body = body[:body.index("*) return 1")].replace("\\\n", " ")
    out = {}
    for name, block in re.findall(r"^\s+(\w+)\)\s*\n\s*echo(.*?);;", body, re.S | re.M):
        out[name] = " ".join(shlex.split(block))
    return out


def bounded(k, v):
    """A number for signal k, inside what its CAN field can carry. Not a number (also NaN, inf): ValueError."""
    x = float(v)
    if not math.isfinite(x):
        raise ValueError("%s: 숫자가 아닙니다" % k)
    lo, hi = LIMITS[k]
    return min(max(x, lo), hi)


class Ecu:
    def __init__(self, ip, port=5555, hz=20):
        self.dest = (ip, port)
        self.hz = hz
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)
        self.kv = dict(DEFAULTS)
        self.sending = False
        self.scenario = None          # (name, Profile, start time)
        self.scenario_done = ""
        self.rec = None               # list of samples while recording
        self.rec_full = False         # the recording reached MAX_RECORD_SAMPLES: it is kept, nothing more is added
        self.send_error = ""          # why the sender thread stopped sending (a bad value), shown in the page
        self.replay = None            # (name, samples, start time)
        self.fault = None             # (name, end time)
        self.followed = {}            # mode / warn / fuel / coolant as the running scenario or replay last set them (see follow)
        self.sc_first = False         # the next follow() is the start of a scenario
        self.sent = self.ok = self.err = 0
        self.last_err = ""
        self.cur = {"speed": 0.0, "rpm": 800, "gear": 0}
        self.stop = False
        self.counters = {0x100: 0, 0x101: 0}
        self.prev = {}
        self.n = 0
        self.gear_now = 0
        self.seq_jumped = True
        self.thread = threading.Thread(target=self.loop, daemon=True)

    def start(self):
        self.thread.start()

    def set_ip(self, ip):
        with self.lock:
            self.dest = (ip, self.dest[1])

    def set_signals(self, values):
        with self.lock:
            for k, v in values.items():
                if k == "speed" or k == "fuel" or k == "coolant":
                    self.kv[k] = bounded(k, v)
                elif k in ("rpm", "gear"):
                    self.kv[k] = None if v in (None, "", "auto") else ("N" if k == "gear" and str(v).upper() == "N" else bounded(k, v))
                elif k == "mode" and v in ss.MODES:
                    self.kv[k] = v
                elif k == "warn":
                    self.kv[k] = [w for w in v if w in WARN_NAMES]

    def set_sending(self, on):
        with self.lock:
            self.sending = bool(on)
            if on:
                self.send_error = ""

    def start_scenario(self, name, text, jitter=3.0):
        prof = ss.Profile(ss.parse_profile(text), self.kv["speed"], jitter)
        with self.lock:
            self.followed = {}
            self.sc_first = True
            self.scenario = (name, prof, time.monotonic())
            self.replay = None
            self.scenario_done = ""
            self.sending = True

    def stop_scenario(self):
        with self.lock:
            self.scenario = None

    def start_record(self):
        with self.lock:
            self.rec = []
            self.rec_full = False
            self.sending = True

    def stop_record(self, name):
        """Stops recording and saves the samples. Returns the name of the recording, or None when it was empty."""
        with self.lock:
            samples, self.rec = self.rec or [], None
        if not samples:
            return None
        os.makedirs(RECORDINGS, exist_ok=True)
        with open(os.path.join(RECORDINGS, name + ".json"), "w", encoding="utf-8") as f:
            json.dump({"hz": self.hz, "samples": samples}, f, separators=(",", ":"))
        return name

    def start_replay(self, name):
        if name not in list_recordings():
            raise ValueError("그런 기록이 없습니다")
        with open(os.path.join(RECORDINGS, name + ".json"), encoding="utf-8") as f:
            data = json.load(f)
        with self.lock:
            self.followed = {}
            self.scenario = None
            self.replay = (name, data["samples"], data.get("hz", self.hz), time.monotonic())
            self.scenario_done = ""
            self.sending = True

    def stop_replay(self):
        with self.lock:
            self.replay = None

    def inject(self, name):
        if name not in ss.FAULTS:
            raise ValueError("없는 결함입니다: " + name)
        with self.lock:
            self.fault = (name, time.monotonic() + FAULT_SECONDS[name])
            self.seq_jumped = False
            self.sending = True

    def clear_fault(self):
        with self.lock:
            self.fault = None

    def state(self):
        with self.lock:
            sc = self.scenario
            fault = self.fault
            now = time.monotonic()
            rp = self.replay
            return {
                "recording": None if self.rec is None else {"samples": len(self.rec), "seconds": round(len(self.rec) / self.hz, 1),
                                                            "full": self.rec_full},
                "send_error": self.send_error,
                "replay": None if not rp else {"name": rp[0], "t": round(now - rp[3], 1), "length": round(len(rp[1]) / rp[2], 1)},
                "recordings": list_recordings(),
                "sending": self.sending, "values": {**self.kv}, "current": dict(self.cur),
                "scenario": None if not sc else {"name": sc[0], "t": round(now - sc[2], 1), "length": round(sc[1].duration, 1)},
                "scenario_done": self.scenario_done,
                "fault": None if not fault else {"name": fault[0], "left": round(max(0.0, fault[1] - now), 1)},
                "sent": self.sent, "ok": self.ok, "err": self.err, "last_err": self.last_err, "hz": self.hz,
            }

    # ---- the sender thread ----
    def drain(self):
        while True:
            try:
                text = self.sock.recvfrom(512)[0].decode(errors="replace").strip()
            except (BlockingIOError, OSError):
                return
            if text.startswith("ok"):
                self.ok += 1
            else:
                self.err += 1
                self.last_err = text

    FOLLOWED = ("mode", "warn", "fuel", "coolant")

    def manual(self):
        """The manual values that are not the speed, in the form of a frame setting."""
        return {"fuel": str(self.kv["fuel"]), "coolant": str(self.kv["coolant"]), "mode": self.kv["mode"],
                "warn": ",".join(self.kv["warn"]) or "none"}

    def follow(self, src, mode_too=True):
        """A scenario or a replay sets mode, warn, fuel or coolant: take the value over into the manual values, so the page
        shows it and a slider moved meanwhile counts too. Only a CHANGE is taken over: a value set by hand stays until the
        scenario or the replay changes that value again. The mode is the driver's choice: with mode_too=False (the start of a
        scenario) the mode a scenario starts with is noted but not taken over. Caller holds the lock."""
        for k in self.FOLLOWED:
            v = src.get(k)
            if v is None or self.followed.get(k) == v:
                continue
            self.followed[k] = v
            if k == "mode" and not mode_too:
                continue
            try:
                if k == "mode":
                    if v in ss.MODES:
                        self.kv[k] = v
                elif k == "warn":
                    if v == "none":
                        self.kv[k] = []
                    elif v[:1].isdigit():
                        self.kv[k] = [w for w in WARN_NAMES if int(v, 0) & ss.WARN[w]]
                    else:
                        self.kv[k] = [w for w in v.split(",") if w in WARN_NAMES]
                else:
                    self.kv[k] = float(v)
            except ValueError:
                pass            # a bad value in a scenario must not stop the sender thread

    def effective(self, now):
        """Signals now: the manual values, over-ridden by a running scenario or a replay. Caller holds the lock."""
        if self.replay:
            name, samples, hz, t0 = self.replay
            i = int((now - t0) * hz)
            if i >= len(samples):
                self.replay = None
                self.scenario_done = name
                last = samples[-1]
                self.kv["speed"] = float(last["speed"])
            else:
                kv = dict(samples[i])
                self.follow(kv)
                kv.update(self.manual())
                self.gear_now = int(kv["gear"]) if kv["gear"].isdigit() else 0
                return kv
        sc_speed = None
        if self.scenario:
            name, prof, t0 = self.scenario
            t = now - t0
            if t > prof.duration:
                self.scenario = None
                self.scenario_done = name
                self.kv["speed"] = round(prof.speed(prof.duration), 1)     # stay where the scenario ended
            else:
                sc_speed = prof.speed(t)
                self.follow(prof.settings, mode_too=not self.sc_first)
                self.sc_first = False
        kv = {"speed": str(self.kv["speed"]), **self.manual()}
        if self.kv["gear"] is not None:
            kv["gear"] = str(self.kv["gear"]) if str(self.kv["gear"]).upper() == "N" else str(int(self.kv["gear"]))
        if self.kv["rpm"] is not None:
            kv["rpm"] = str(self.kv["rpm"])
        if sc_speed is not None:
            kv["speed"] = str(sc_speed)
            kv.update({k: v for k, v in prof.settings.items() if k not in self.FOLLOWED})
        speed = float(kv["speed"])
        if "gear" not in kv:
            self.gear_now = ss.auto_gear(speed, self.gear_now)
            kv["gear"] = str(self.gear_now)
        if "rpm" not in kv:
            g = int(kv["gear"]) if kv["gear"].isdigit() else 1
            kv["rpm"] = str(round(ss.auto_rpm(speed, g)))
        return kv

    def send_frames(self, kv, now):
        motion, status = ss.signal_bytes(kv)
        fault, until = self.fault if self.fault else (None, 0)
        if fault and now >= until and fault != "wrongseq":
            self.fault = fault = None
        if fault == "wrongseq" and self.seq_jumped:
            self.fault = fault = None
        self.n += 1
        for can_id, d5 in ((0x100, motion), (0x101, status)):
            if fault == "lost" and self.n % 3 == 0:
                self.counters[can_id] = (self.counters[can_id] + 1) & 0xFF
                continue
            if fault == "wrongseq" and not self.seq_jumped and can_id == 0x100:
                self.counters = {k: (v + 100) & 0xFF for k, v in self.counters.items()}
                self.seq_jumped = True
            data = ss.protect(can_id, d5, self.counters[can_id])
            self.counters[can_id] = (self.counters[can_id] + 1) & 0xFF
            if fault == "crc" and self.n % 5 == 0:
                data = data[:7] + bytes([data[7] ^ 0x01])
            if fault == "burst":
                data = bytes([data[0] ^ 0x01]) + data[1:]
            if fault == "repeat" and self.n % 5 == 0 and can_id in self.prev:
                data = self.prev[can_id]
            self.prev[can_id] = data
            if fault == "silent":
                continue
            try:
                self.sock.sendto(ss.can_frame(can_id, data), self.dest)
                self.sent += 1
            except OSError:
                pass

    def loop(self):
        nxt = time.monotonic()
        while not self.stop:
            now = time.monotonic()
            with self.lock:
                if self.sending:
                    try:
                        kv = self.effective(now)
                        if self.rec is not None and not self.rec_full:
                            self.rec.append({k: kv[k] for k in SAMPLE_KEYS})
                            self.rec_full = len(self.rec) >= MAX_RECORD_SAMPLES   # keep it; "stop" still saves it
                        self.cur = {"speed": float(kv["speed"]), "rpm": int(float(kv["rpm"])), "gear": kv["gear"]}
                        self.send_frames(kv, now)
                    except Exception as e:   # a bad value (e.g. from a recording file) must not end this thread
                        self.sending = False
                        self.scenario = self.replay = None
                        self.send_error = "전송을 멈췄습니다: %s" % (e,)
                self.drain()
            nxt += 1.0 / self.hz
            delay = nxt - time.monotonic()
            if delay < -1:                 # the thread was stalled: do not send a burst to catch up
                nxt = time.monotonic()
                delay = 0
            time.sleep(max(0.0, delay))
