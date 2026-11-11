"""Driving scenario test: every scenario of scenarios.sh runs on the target from start to end, and drives are
interrupted (fault, lost signal, frozen display, stop, switch, replay) on the way.

While a scenario runs, the vehicle data the cluster accepted is read back with UDS (22 0200) about once a second
and compared with what the virtual ECU sent. cluster_gateway ramps towards new values (speed 100 km/h per second,
coolant 20 degC per second) and the display reports its data every 0.2 s, so a read value counts as right when it
lies within the speeds sent in the 1.5 s before the read, +-2 km/h. Every mode and warning-lamp set that was sent
for at least 2 s must have been read back at least once, and at the end the fuel, coolant, mode and lamps must be
the ones sent. A normal drive must leave no DTC; an interrupted one exactly the DTCs of its interruption, and the
read-back values must follow again afterwards.
It takes about 11 minutes and needs qt_cluster, cluster_gateway and cluster_diag to run on the target.

On the Pi the frames go over Wi-Fi. A stall of 0.5 s is what cluster_gateway takes for a lost source (U0401) and the
display for a lost link (U0100), and a home Wi-Fi stalls that long about twice a minute (measured 2026-11-11: 4 times
over 0.5 s in 2 minutes, up to 708 ms). So on the Pi only, a ping runs during every drive: when the only unexpected
DTCs are U0100 and U0401 and the ping stalled for 450 ms or more, the result is shown as WIFI, named in the summary,
and not counted as a failure. The limits themselves stay as they are.
"""
import contextlib
import os
import subprocess
import threading
import time

import send_signal as ss

HERE = os.path.dirname(os.path.abspath(__file__))
ON_WIFI = os.environ.get("QNX_ARCH", "x86_64") == "aarch64le"
WIFI_DTCS = frozenset({"U0100", "U0401"})   # what a stall of the frames gives
STALL_MS = 450                              # a ping stall this long can let 0.5 s pass without a frame
SPEED_TOL = 2.0             # km/h around the speeds sent shortly before a read
WINDOW = 1.5                # s of sent values a read is compared with
READ_EVERY = 0.7            # s between two read-backs
MIN_MATCH = 0.95            # share of the read values that must match
MIN_READ = 0.7              # share of the reads that must get an answer (Wi-Fi on the Pi)
STEADY = 2.0                # s a mode or lamp set must have been sent to be expected in the read-back
START = {"speed": 0, "mode": "comfort", "rpm": None, "gear": None, "warn": [], "fuel": 60, "coolant": 90}
VALUE_KEYS = ("speed", "rpm", "gear", "mode", "warn", "fuel", "coolant")


def lamps(bits):
    return tuple(sorted(n for n, b in ss.WARN.items() if n != "check_engine" and bits & b))


@contextlib.contextmanager
def drive_values(ecu):
    """The ECU sends from a standstill while the block runs; afterwards the user's values come back."""
    st = ecu.state()
    saved = {k: st["values"][k] for k in VALUE_KEYS}
    ecu.stop_scenario()
    ecu.stop_replay()
    ecu.clear_fault()
    ecu.set_signals(START)
    ecu.set_sending(True)
    try:
        yield
    finally:
        ecu.stop_scenario()
        ecu.stop_replay()
        ecu.clear_fault()
        ecu.set_signals(saved)
        ecu.set_sending(st["sending"])


class Sent:
    """Samples what the ECU sends, 20 times a second, in a thread of its own."""

    def __init__(self, ecu):
        self.ecu, self.rows, self.stop = ecu, [], threading.Event()
        self.thread = threading.Thread(target=self.loop, daemon=True)

    def loop(self):
        while not self.stop.is_set():
            s = self.ecu.state()
            v = s["values"]
            self.rows.append((time.monotonic(), s["current"]["speed"] if s["sending"] else None, v["mode"],
                              tuple(sorted(v["warn"])), float(v["fuel"]), float(v["coolant"])))
            time.sleep(0.05)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join()

    def speeds(self, t0, t1):
        return [r[1] for r in self.rows if t0 <= r[0] <= t1 and r[1] is not None]

    def steady(self, col):
        """Values of column col that were sent for at least STEADY seconds in a row."""
        out, start, cur = set(), None, None
        for r in self.rows:
            if r[col] != cur:
                cur, start = r[col], r[0]
            elif r[0] - start >= STEADY:
                out.add(cur)
        return out


class WifiWatch:
    """Pings the target ten times a second (on the Pi) and keeps the longest stall: a late reply or missing ones."""

    def __init__(self, ip):
        self.ip, self.max_ms, self.p = ip, 0.0, None

    def __enter__(self):
        if ON_WIFI:
            self.p = subprocess.Popen(["ping", "-n", "-i", "0.1", "-W", "1", self.ip], stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, text=True)
            threading.Thread(target=self.read, daemon=True).start()
        return self

    def read(self):
        last = None
        for line in self.p.stdout:
            if "icmp_seq=" not in line or "time=" not in line:
                continue
            seq = int(line.split("icmp_seq=")[1].split()[0])
            ms = float(line.split("time=")[1].split()[0])
            gap = 0.0 if last is None else max(0, seq - last - 1) * 100.0     # replies that never came
            self.max_ms = max(self.max_ms, ms, gap + ms)
            last = seq

    def __exit__(self, *exc):
        if self.p:
            self.p.terminate()
            self.p.wait()


def run(app, log):
    log("주행 시나리오 시험: 시나리오를 대상에서 끝까지 돌리며 계기판이 받은 값을 되읽고, 주행 중 끼어들기를 시험합니다 (약 11분).")
    with drive_values(app.ecu):
        return Test(app, log).all()


class Test:
    def __init__(self, app, log):
        self.app, self.ecu, self.uds, self.log = app, app.ecu, app.uds, log
        self.passed = self.failed = 0
        self.wifi = []                           # drives whose only extra DTCs came with a Wi-Fi stall (Pi)
        self.watch = None
        self.label = ""

    def check(self, what, ok, detail=""):
        self.passed += bool(ok)
        self.failed += not ok
        self.log("  %s  %s%s" % ("PASS" if ok else "FAIL", what, ("  (" + detail + ")") if detail and not ok else ""))
        return ok

    def no_dtc(self, what, got, allowed=frozenset()):
        """Checks that `got` has nothing but `allowed`; on the Pi, U0100/U0401 with a Wi-Fi stall are shown as WIFI."""
        extra = None if got is None else set(got) - set(allowed)
        if extra and ON_WIFI and extra <= WIFI_DTCS and self.watch and self.watch.max_ms >= STALL_MS:
            self.wifi.append(self.label)
            self.log("  WIFI  %s  (DTC: %s, Wi-Fi가 %.0fms 멈춤: 파이에서는 실패로 세지 않음)"
                     % (what, ", ".join(sorted(extra)), self.watch.max_ms))
            return True
        return self.check(what, extra == set(), "DTC: %s" % (", ".join(sorted(extra)) if extra else "읽지 못함"))

    def codes(self):
        r = self.uds.read_dtcs()
        return {d["code"] for d in r["dtcs"]} if r["ok"] else None

    def vehicle(self):
        r = self.uds.vehicle()
        return r["vehicle"] if r["ok"] else None

    def fresh_start(self):
        """Standstill, no fault, empty DTC memory. Returns False when the DTC memory is not empty with good frames."""
        self.ecu.stop_scenario()
        self.ecu.stop_replay()
        self.ecu.clear_fault()
        self.ecu.set_signals(START)
        self.ecu.set_sending(True)
        time.sleep(2.0)
        cleared = self.uds.clear("all")
        time.sleep(2.0)
        if not self.check("시작: DTC 메모리를 지운다", cleared["ok"], cleared["text"]):
            return False
        ok = self.no_dtc("시작: 정상 프레임만 보낼 때 DTC가 없다", self.codes())
        if ok and self.wifi and self.wifi[-1] == self.label:   # a stall before the drive: start again from clean
            self.uds.clear("all")
            time.sleep(1.0)
        return ok

    def follow(self, sent, seconds, label):
        """Reads back for `seconds` and counts how many reads match what was sent. Returns (reads, matches, misses)."""
        reads = matches = asked = 0
        misses = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            t = time.monotonic()
            v = self.vehicle()
            asked += 1
            if v is not None:
                window = sent.speeds(t - WINDOW, time.monotonic())
                if window:
                    reads += 1
                    if min(window) - SPEED_TOL <= v["speed"] <= max(window) + SPEED_TOL:
                        matches += 1
                    else:
                        misses.append("%.1f (보낸 값 %.1f~%.1f)" % (v["speed"], min(window), max(window)))
            time.sleep(max(0.0, READ_EVERY - (time.monotonic() - t)))
        return asked, reads, matches, misses

    def judge_follow(self, label, asked, reads, matches, misses):
        self.log("      %s: 되읽기 %d번 중 응답 %d번, 속도 일치 %d번" % (label, asked, reads, matches))
        self.check("%s: 되읽기에 응답한다 (%d%% 이상)" % (label, MIN_READ * 100), asked and reads >= MIN_READ * asked,
                   "%d/%d" % (reads, asked))
        self.check("%s: 계기판이 받은 속도가 보낸 속도를 따라간다 (%d%% 이상)" % (label, MIN_MATCH * 100),
                   reads and matches >= MIN_MATCH * reads, "어긋남: " + ", ".join(misses[:4]))

    def wait_scenario_end(self, name, limit):
        end = time.monotonic() + limit
        while time.monotonic() < end:
            st = self.ecu.state()
            if st["scenario"] is None:
                return st["scenario_done"] == name
            time.sleep(0.2)
        return False

    def settled_values(self):
        """The fuel, coolant, mode and lamps sent now, and the ones read back after the gateway caught up."""
        time.sleep(1.5)
        v = self.ecu.state()["values"]
        return v, self.vehicle()

    # ---- the scenarios of scenarios.sh ----
    def scenario(self, name):
        text = self.app.scenarios[name]
        length = ss.Profile(ss.parse_profile(text)).duration
        self.log("")
        self.log("[시나리오 %s] %.0f초" % (name, length))
        self.label = "시나리오 " + name
        with WifiWatch(self.app.ip) as self.watch:
            self.drive(name, text, length)
        self.watch = None

    def drive(self, name, text, length):
        if not self.fresh_start():
            return
        with Sent(self.ecu) as sent:
            self.ecu.start_scenario(name, text)
            res = self.follow(sent, length - 0.5, name)
            ended = self.wait_scenario_end(name, 10)
            v, got = self.settled_values()
        self.judge_follow(name, *res)
        self.check("시나리오가 끝까지 돌았다", ended)
        if got is None:
            self.check("끝난 뒤 값을 되읽을 수 있다", False)
        else:
            self.check("끝난 뒤 모드가 같다 (%s)" % v["mode"], got["mode"].lower() == v["mode"], got["mode"])
            self.check("끝난 뒤 경고등이 같다 (%s)" % (",".join(v["warn"]) or "없음"),
                       lamps(got["warnings"]) == tuple(sorted(v["warn"])), ",".join(lamps(got["warnings"])) or "없음")
            self.check("끝난 뒤 연료가 같다 (%.1f%%)" % v["fuel"], abs(got["fuel"] - v["fuel"]) <= 0.5, "%.1f" % got["fuel"])
            self.check("끝난 뒤 냉각수가 같다 (%.0f도)" % v["coolant"], abs(got["coolant"] - v["coolant"]) <= 1, "%d" % got["coolant"])
        # every mode and lamp set that was on for a while must have reached the cluster
        modes_sent, lamps_sent = sent.steady(2), sent.steady(3)
        self.log("      보낸 모드: %s, 보낸 경고등 조합: %d개" % (", ".join(sorted(modes_sent)), len(lamps_sent)))
        self.check("2초 넘게 보낸 모드와 경고등 조합이 모두 계기판에 닿았다", self.seen_ok(sent, modes_sent, lamps_sent))
        self.no_dtc("정상 주행 뒤 DTC가 없다", self.codes())

    def seen_ok(self, sent, modes_sent, lamps_sent):
        # the modes and lamps read back are collected by follow() through self.seen
        return modes_sent <= self.seen_modes and lamps_sent <= self.seen_lamps

    # ---- drives with an interruption ----
    def interrupted(self, label, scenario, before, action, after, must, must_not=frozenset()):
        """Runs `scenario`, waits `before` s, calls action(), reads back for `after` s and checks the DTCs, then that
        the read-back follows the sent speed again for 6 s."""
        self.log("")
        self.log("[끼어들기: %s] 시나리오 %s, %.0f초 뒤" % (label, scenario, before))
        self.label = "끼어들기 " + label
        with WifiWatch(self.app.ip) as self.watch:
            self.interrupted_drive(scenario, before, action, after, must, must_not)
        self.watch = None

    def interrupted_drive(self, scenario, before, action, after, must, must_not):
        if not self.fresh_start():
            return
        with Sent(self.ecu) as sent:
            self.ecu.start_scenario(scenario, self.app.scenarios[scenario])
            res = self.follow(sent, before, "끼어들기 전")
            note = action()
            time.sleep(after)
            got = self.codes()
            self.ecu.clear_fault()
            if note:
                self.log("      " + note)
            self.log("      읽은 DTC: %s" % (", ".join(sorted(got)) if got else "없음" if got == set() else "읽지 못함"))
            if got is not None:
                for c in sorted(must):
                    self.check("읽은 DTC에 %s 포함" % c, c in got)
                for c in sorted(must_not):
                    self.check("읽은 DTC에 %s 없음" % c, c not in got)
                if not must:
                    self.no_dtc("DTC가 생기지 않았다", got)
            else:
                self.check("DTC를 읽을 수 있다", False)
            time.sleep(2.0)                      # the gateway catches up with what is sent now
            res2 = self.follow(sent, 6.0, "끼어든 뒤")
        self.judge_follow("끼어들기 전", *res)
        self.judge_follow("끼어든 뒤", *res2)

    def all(self):
        self.seen_modes, self.seen_lamps = set(), set()
        vehicle = self.vehicle

        def recording_vehicle():
            v = vehicle()
            if v is not None:
                self.seen_modes.add(v["mode"].lower())
                self.seen_lamps.add(lamps(v["warnings"]))
            return v
        self.vehicle = recording_vehicle

        time.sleep(2.0)                          # the display reports the vehicle data once it receives frames
        if self.vehicle() is None and self.vehicle() is None:
            self.check("cluster_diag가 차량 값(22 0200)에 응답한다", False, "응답 없음: 대상 프로그램이 모두 실행 중인지 보세요")
            return 1
        for name in self.app.scenarios:
            self.seen_modes, self.seen_lamps = set(), set()
            self.scenario(name)

        ecu = self.ecu

        def freeze():
            try:
                r = subprocess.run(["bash", "run_diag.sh", self.app.ip, "freeze", "4"], cwd=HERE, capture_output=True,
                                   text=True, timeout=30)
                return "qt_cluster 4초 얼림" if r.returncode == 0 else "얼리지 못함: " + (r.stdout + r.stderr).strip()[-120:]
            except subprocess.TimeoutExpired:
                return "얼리지 못함: 30초 안에 끝나지 않음"

        def silent():
            ecu.set_sending(False)
            time.sleep(3.0)
            ecu.set_sending(True)
            return "3초 동안 전송을 멈췄다가 다시 보냄"

        def stop():
            ecu.stop_scenario()
            return "시나리오 중지: 속도는 손으로 정한 값(%.0f)으로" % ecu.state()["values"]["speed"]

        def switch():
            ecu.start_scenario("city", self.app.scenarios["city"])
            return "highway 도중 city 시작"

        def replay():
            # a recording of the running drive, replayed, and the replay replaced by a scenario
            ecu.start_record()
            time.sleep(6.0)
            name = ecu.stop_record("scenario-test")
            ecu.start_replay(name)
            time.sleep(4.0)
            ecu.start_scenario("modes", self.app.scenarios["modes"])
            st = ecu.state()
            os.remove(os.path.join(HERE, "recordings", name + ".json"))
            return "6초 기록 → 재생 4초 → 재생 중 modes 시작 (재생 %s, 시나리오 %s)" % (
                "멈춤" if st["replay"] is None else "계속", st["scenario"]["name"] if st["scenario"] else "없음")

        self.interrupted("crc 결함", "city", 10, lambda: (ecu.inject("crc"), "crc 결함 6초")[1], 8, {"U0401"})
        self.interrupted("전송 끊김", "highway", 12, silent, 2, {"U0100"}, {"U1A04"})
        self.interrupted("계기판 얼림", "highway", 12, freeze, 1, {"U1A04"})
        self.interrupted("시나리오 중지", "highway", 15, stop, 4, frozenset())
        self.interrupted("시나리오 바꾸기", "highway", 15, switch, 4, frozenset())
        self.interrupted("기록, 재생, 재생 중 시나리오", "city", 5, replay, 3, frozenset())

        self.ecu.stop_scenario()
        self.uds.clear("all")
        self.log("")
        if self.wifi:
            self.log("Wi-Fi 끊김과 함께 U0100/U0401이 생겨 실패로 세지 않은 것: %s" % ", ".join(self.wifi))
        self.log("주행 시나리오 시험: 통과 %d개, 실패 %d개" % (self.passed, self.failed))
        return 1 if self.failed else 0
