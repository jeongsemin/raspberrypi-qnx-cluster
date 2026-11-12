"""End-to-end test of the whole chain: a fault on the CAN frames (or a frozen display) must end up as the right DTC.

For every case the test clears the DTC memory, checks that good frames leave it empty, injects the fault, waits,
reads the DTC list with UDS and checks the codes and the snapshot of the first one. The expected codes are what
the system does today (found by running every fault on the VM); a change of the DTC logic shows up here.
It takes about 80 seconds and needs qt_cluster, cluster_gateway and cluster_diag to run on the VM.
"""
import contextlib
import os
import shlex
import subprocess
import time

from app_scenario import STALL_MS, WIFI_DTCS, WifiWatch

HERE = os.path.dirname(os.path.abspath(__file__))
VENV_PY = os.path.join(os.path.expanduser("~"), "uds-venv", "bin", "python")
SPEED = 50.0

# name, how, seconds to wait after the start, DTCs that must be there, DTCs that must not be there
CASES = [
    ("crc", "fault", 8, {"U0401"}, set()),
    ("burst", "fault", 5, {"U0401"}, set()),
    ("repeat", "fault", 8, {"U0401"}, set()),
    ("lost", "fault", 8, set(), {"U0401", "U0100", "U1A04"}),      # every 3rd frame lost: the counter check tolerates it
    ("wrongseq", "fault", 4, {"U0401"}, set()),
    ("silent", "fault", 5, {"U0100"}, {"U1A04"}),
    ("freeze", "freeze", 3, {"U1A04"}, set()),                      # qt_cluster held for 4 s: no heartbeat
]

# On the Pi the frames go over Wi-Fi. "lost" drops every 3rd frame on purpose, and the receiver accepts up to
# E2E_MAX_GAP (3) lost frames in a row; a short Wi-Fi stall on top of that goes past the limit and gives a real
# U0401. That is the network, not the DTC logic, so on the Pi this case gets one more try (the first one is logged).
# A Wi-Fi stall of 0.5 s can also give U0100/U0401 before any fault is injected (seen once on 2026-11-11 in "crc").
# So on the Pi a ping runs during every case, and a case whose only failure is that check, with only U0100/U0401
# and a stall of STALL_MS or more, also gets one more try. The limits themselves stay as they are.
RETRY_ON_WIFI = {"lost"}
PRECHECK = "정상 프레임만 보낼 때 DTC가 없다"
ON_WIFI = os.environ.get("QNX_ARCH", "x86_64") == "aarch64le"
VALUE_KEYS = ("speed", "rpm", "gear", "mode", "warn", "fuel", "coolant")
TEST_VALUES = {"speed": SPEED, "mode": "comfort", "rpm": None, "gear": None, "warn": [], "fuel": 60, "coolant": 90}


@contextlib.contextmanager
def test_values(ecu):
    """The ECU sends the test values while the block runs. Afterwards, also when a test stops early or raises,
    the values and the sending state of the user come back."""
    st = ecu.state()
    saved = {k: st["values"][k] for k in VALUE_KEYS}
    ecu.stop_scenario()
    ecu.stop_replay()
    ecu.clear_fault()
    ecu.set_signals(TEST_VALUES)
    ecu.set_sending(True)
    try:
        yield
    finally:
        ecu.clear_fault()
        ecu.set_signals(saved)
        ecu.set_sending(st["sending"])


def run_cross(app, log):
    """uds_crosscheck.py expects exactly U0100 and U0401 in the DTC memory. Make them here, so the result does not
    depend on what an earlier test left behind, then run the script (it restarts cluster_diag at its end)."""
    ecu, uds = app.ecu, app.uds
    log("교차 확인 준비: DTC 메모리를 비우고 burst 결함으로 U0100과 U0401을 만듭니다.")
    with test_values(ecu):
        time.sleep(2.5)
        cleared = uds.clear("all")
        if not cleared["ok"]:
            log("DTC 메모리를 지우지 못했습니다: " + cleared["text"])
            return 1
        time.sleep(2.5)
        ecu.inject("burst")
        time.sleep(6)
        ecu.clear_fault()
        r = uds.read_dtcs()
        got = {d["code"] for d in r["dtcs"]} if r["ok"] else None
        log("준비 후 DTC: %s" % (", ".join(sorted(got)) if got else "읽지 못함"))
    if got != {"U0100", "U0401"}:
        log("준비 실패: U0100과 U0401 두 개가 있어야 합니다.")
        return 1
    log("")
    p = subprocess.Popen(["bash", "-c", "%s uds_crosscheck.py %s" % (shlex.quote(VENV_PY), shlex.quote(app.ip))], cwd=HERE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, text=True, errors="replace")
    for line in p.stdout:
        log(line.rstrip("\n"))
    return p.wait()


def run(app, log):
    log("종단 시험: 결함을 넣고 기대한 DTC가 생기는지 확인합니다 (약 80초). 이 동안 ECU 값은 시험이 정합니다.")
    with test_values(app.ecu):
        return run_cases(app, log)


def run_cases(app, log):
    ecu, uds = app.ecu, app.uds
    passed = failed = 0

    def check(what, ok, detail=""):
        nonlocal passed, failed
        passed += bool(ok)
        failed += not ok
        log("  %s  %s%s" % ("PASS" if ok else "FAIL", what, ("  (" + detail + ")") if detail and not ok else ""))
        return ok

    def codes():
        r = uds.read_dtcs()
        return {d["code"] for d in r["dtcs"]} if r["ok"] else None

    time.sleep(2.5)
    if not check("cluster_diag가 UDS에 응답한다", codes() is not None, "응답 없음: cluster_diag가 실행 중인지 보세요"):
        return 1

    def one_case(name, how, wait, must, must_not):
        """Runs one case and returns its lines: (check text, ok, detail), or (log line, None, "")."""
        out = []

        def rec(what, ok, detail=""):
            out.append((what, bool(ok), detail))
            return ok

        cleared = uds.clear("all")
        rec("DTC 메모리를 지운다", cleared["ok"], cleared["text"])
        time.sleep(2.5)
        before = codes()
        rec(PRECHECK, before == set(), "남은 DTC: %s" % (", ".join(sorted(before)) if before else "읽지 못함"))
        out.append(("", "before", before))
        if how == "fault":
            ecu.inject(name)
        else:
            try:
                res = subprocess.run(["bash", "run_diag.sh", app.ip, "freeze", "4"], cwd=HERE, capture_output=True,
                                     text=True, timeout=30)
                err = "" if res.returncode == 0 else (res.stdout + res.stderr).strip()[-200:]
            except subprocess.TimeoutExpired:
                err = "30초 안에 끝나지 않았습니다 (ssh가 멈춤)"
            if err:
                rec("qt_cluster를 얼린다", False, err)
                return out
        time.sleep(wait)
        got = codes()
        if rec("DTC를 읽을 수 있다", got is not None):
            out.append(("      읽은 DTC: %s" % (", ".join(sorted(got)) or "없음"), None, ""))
            for c in sorted(must):
                rec("읽은 DTC에 %s 포함" % c, c in got)
            for c in sorted(must_not):
                rec("읽은 DTC에 %s 없음" % c, c not in got)
            if must and must <= got:
                code = sorted(must)[0]
                snap = uds.snapshot(code)
                recs = snap.get("snapshot", {}).get("records", [])
                veh = recs[0]["vehicle"] if recs and recs[0]["vehicle"] else None
                rec("스냅샷(%s)에 결함 순간의 차량 값이 있다" % code, veh is not None)
                if veh:
                    rec("스냅샷의 속도가 보낸 값(%.0f km/h) 근처다" % SPEED, abs(veh["speed"] - SPEED) <= 6, "속도 %.1f" % veh["speed"])
        ecu.clear_fault()
        time.sleep(1.0)
        return out

    def watched(case):
        """Runs one case with a ping on the Pi; returns its lines and the longest stall in ms."""
        with WifiWatch(app.ip) as watch:
            out = one_case(*case)
        before = next((d for _, ok, d in out if ok == "before"), None)
        return [o for o in out if o[1] != "before"], before, watch.max_ms

    def wifi_precheck(out, before, stall):
        """True when the only failure is the check before the fault, with only U0100/U0401, during a stall."""
        failed_what = [what for what, ok, _ in out if ok is False]
        return (failed_what == [PRECHECK] and before and set(before) <= WIFI_DTCS and stall >= STALL_MS)

    retried = []
    for case in CASES:
        name = case[0]
        log("")
        log("[%s]" % name)
        out, before, stall = watched(case)
        failed_any = any(ok is False for _, ok, _ in out)
        if ON_WIFI and failed_any and (name in RETRY_ON_WIFI or wifi_precheck(out, before, stall)):
            # the first try is not counted, but it is shown and named in the summary
            for what, ok, detail in out:
                if ok is False:
                    log("  재시도  첫 시도 실패: %s%s" % (what, ("  (" + detail + ")") if detail else ""))
            log("  재시도  파이는 Wi-Fi에서 프레임이 더 사라질 수 있어 한 번 더 시험합니다 (Wi-Fi 최대 멈춤 %.0fms)."
                % stall)
            retried.append(name)
            out, before, stall = watched(case)
        for what, ok, detail in out:
            if ok is None:
                log(what)
            else:
                check(what, ok, detail)

    log("")
    uds.clear("all")
    if retried:
        log("다시 시험한 결함: %s (첫 시도 실패, 다시 시험해 결과를 셈)" % ", ".join(retried))
    log("종단 시험: 통과 %d개, 실패 %d개" % (passed, failed))
    return 1 if failed else 0
