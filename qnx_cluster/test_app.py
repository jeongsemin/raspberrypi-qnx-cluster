#!/usr/bin/env python3
"""Self-test of the test console (cluster_app.py) on the PC. No target needed: a fake gateway listens on a
local UDP port, and the web server is asked over a local TCP port.  Run: python3 test_app.py"""
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

import app_ecu
import app_uds
import cluster_app
import send_signal as ss

failures = 0


def check(what, ok):
    global failures
    failures += not ok
    print("  %s  %s" % ("PASS" if ok else "FAIL", what))


def frames_from(sock, seconds):
    """All (can_id, data) frames that arrive within `seconds`."""
    out, end = [], time.monotonic() + seconds
    sock.settimeout(0.05)
    while time.monotonic() < end:
        try:
            d = sock.recvfrom(64)[0]
        except socket.timeout:
            continue
        can_id, dlc = struct.unpack_from("<IB", d)
        out.append((can_id, d[8:8 + dlc]))
    return out


def drain(sock):
    sock.setblocking(False)
    try:
        while True:
            sock.recvfrom(64)
    except (BlockingIOError, OSError):
        pass


def crc_ok(can_id, data):
    return ss.can_crc(can_id, data) == data[7]


def test_decoders():
    v = app_uds.decode_vehicle(bytes([0x01, 0xF4, 0x09, 0xC4, 0x82, 0x78, 0x03, 0x01, 0x00, 0x05]))
    check("vehicle data decodes", v == {"speed": 50.0, "rpm": 2500, "coolant": 90, "fuel": 60.0, "gear": 3, "mode": "COMFORT", "warnings": 5})
    r = bytes([0x59, 0x02, 0xFF, 0xC4, 0x01, 0x00, 0x2F, 0xC1, 0x00, 0x00, 0x24])
    d = app_uds.decode_dtcs(r)
    check("DTC list decodes", [x["code"] for x in d] == ["U0401", "U0100"] and d[0]["confirmed"] and not d[1]["confirmed"] and d[0]["failing"])
    check("DTC flags are named", "confirmedDTC" in d[0]["flags"] and d[0]["info"].startswith("게이트웨이"))
    s = app_uds.decode_snapshot(bytes([0x59, 0x04, 0xC4, 0x01, 0x00, 0x2F, 0x01, 0x01, 0x02, 0x00, 0x01, 0xF4, 0x09, 0xC4, 0x82, 0x78, 0x03, 0x01, 0x00, 0x05]))
    check("snapshot decodes", s["code"] == "U0401" and s["records"][0]["vehicle"]["gear"] == 3)
    check("scenarios.sh is read", {"city", "highway", "overheat", "lowfuel", "coldstart", "warnings", "modes"} <= set(cluster_app.App.__init__.__globals__["app_ecu"].load_scenarios(cluster_app.SCENARIO_FILE)))


def test_ecu():
    gw = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    gw.bind(("127.0.0.1", 0))
    ecu = app_ecu.Ecu("127.0.0.1", gw.getsockname()[1], hz=50)
    ecu.start()
    check("nothing is sent before Start", not frames_from(gw, 0.3))
    ecu.set_signals({"speed": 80, "mode": "sport", "warn": ["left", "oil"], "fuel": 40, "coolant": 100})
    ecu.set_sending(True)
    fr = frames_from(gw, 0.6)
    check("frames arrive at about the set rate", 40 <= len(fr) <= 80)
    check("every frame has a good CRC", all(crc_ok(i, d) for i, d in fr))
    first = [d for i, d in fr if i == 0x100][0]
    check("speed 80 km/h is in the frame", struct.unpack_from("<H", first)[0] == 8000)
    check("auto gear and rpm follow the speed", first[4] == 4 and struct.unpack_from("<H", first, 2)[0] == round(ss.auto_rpm(80, 4)))
    st = [d for i, d in fr if i == 0x101][0]
    check("status frame: fuel, coolant, mode, warnings", st[0] == 80 and st[1] == 140 and st[2] == 2 and struct.unpack_from("<H", st, 3)[0] == 20)
    seq = [d[6] for i, d in fr if i == 0x100]
    check("the alive counter counts up", all((b - a) & 0xFF == 1 for a, b in zip(seq, seq[1:])))
    ecu.set_signals({"gear": 2, "rpm": 3000})
    fr = frames_from(gw, 0.3)
    m = [d for i, d in fr if i == 0x100][-1]
    check("a fixed gear and rpm win over auto", m[4] == 2 and struct.unpack_from("<H", m, 2)[0] == 3000)

    ecu.inject("crc")
    fr = frames_from(gw, 0.8)
    bad = sum(not crc_ok(i, d) for i, d in fr)
    check("fault crc corrupts about every 5th frame", len(fr) // 8 <= bad <= len(fr) // 3)
    ecu.clear_fault()
    check("after the fault ends the frames are good again", all(crc_ok(i, d) for i, d in frames_from(gw, 0.4)))

    ecu.inject("silent")
    time.sleep(0.2)
    drain(gw)
    check("fault silent sends nothing", not frames_from(gw, 0.6))
    ecu.clear_fault()
    check("sending resumes after silent", len(frames_from(gw, 0.4)) > 5)

    before = [d[6] for i, d in frames_from(gw, 0.2) if i == 0x100]
    ecu.inject("wrongseq")
    fr = frames_from(gw, 0.5)
    seq = before[-1:] + [d[6] for i, d in fr if i == 0x100]
    check("fault wrongseq jumps the counter once", sum((b - a) & 0xFF != 1 for a, b in zip(seq, seq[1:])) == 1)

    ecu.start_scenario("test", "set:mode=eco ramp:60:0.5 hold:60:10")
    time.sleep(1.0)
    s = ecu.state()
    check("a scenario drives the speed", s["scenario"] is not None and 50 < s["current"]["speed"] < 70)
    ecu.stop_scenario()

    # a scenario's fuel, coolant and warn show in the values; its starting mode does not override the mode set by hand,
    # a value set by hand stays until the scenario changes it again, and a mode change later in the scenario is taken over
    ecu.set_signals({"mode": "comfort"})
    ecu.start_scenario("test2", "set:mode=eco:fuel=33:coolant=101:warn=check,oil hold:10:1 set:fuel=20 hold:10:1 set:mode=sport hold:10:5")
    time.sleep(0.5)
    v = ecu.state()["values"]
    check("a scenario's fuel, coolant and warn show in the values", v["fuel"] == 33 and v["coolant"] == 101 and v["warn"] == ["check", "oil"])
    check("the mode a scenario starts with does not override the mode set by hand", v["mode"] == "comfort")
    ecu.set_signals({"mode": "eco", "coolant": 80})
    drain(gw)
    time.sleep(0.4)
    v = ecu.state()["values"]
    check("a mode and coolant set by hand are not overwritten by the scenario", v["mode"] == "eco" and v["coolant"] == 80)
    time.sleep(0.9)
    v = ecu.state()["values"]
    check("the scenario's later fuel change is taken over, the hand-set mode stays", v["fuel"] == 20 and v["mode"] == "eco")
    time.sleep(0.6)
    check("a mode change later in the scenario is taken over", ecu.state()["values"]["mode"] == "sport")
    status = [d for i, d in frames_from(gw, 0.3) if i == 0x101]
    check("the frames carry the scenario's mode and fuel", bool(status) and status[-1][2] == 2 and status[-1][0] == 40)
    ecu.stop_scenario()
    ecu.set_sending(False)
    time.sleep(0.2)
    ecu.stop = True
    gw.close()


def test_record_replay():
    import tempfile
    app_ecu.RECORDINGS = tempfile.mkdtemp()
    gw = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    gw.bind(("127.0.0.1", 0))
    ecu = app_ecu.Ecu("127.0.0.1", gw.getsockname()[1], hz=50)
    ecu.start()
    ecu.set_signals({"speed": 33, "mode": "sport", "warn": ["oil"]})
    ecu.start_record()
    time.sleep(0.8)
    check("a recording collects samples", ecu.state()["recording"]["samples"] >= 25)
    name = ecu.stop_record("t1")
    check("the recording is saved and listed", name == "t1" and "t1" in app_ecu.list_recordings())
    ecu.set_signals({"speed": 120, "mode": "eco", "warn": []})
    drain(gw)
    ecu.start_replay("t1")
    fr = frames_from(gw, 0.4)
    m = [d for i, d in fr if i == 0x100][-1]
    st = [d for i, d in fr if i == 0x101][-1]
    check("a replay sends the recorded values, not the current ones", struct.unpack_from("<H", m)[0] == 3300 and st[2] == 2 and struct.unpack_from("<H", st, 3)[0] == 4)
    time.sleep(0.8)
    check("the replay ends and the speed stays at the last value", ecu.state()["replay"] is None and ecu.state()["values"]["speed"] == 33.0)
    try:
        ecu.start_replay("../x")
        check("a path as a recording name is refused", False)
    except ValueError:
        check("a path as a recording name is refused", True)
    ecu.stop = True
    gw.close()


def test_server():
    app = cluster_app.App("127.0.0.1")
    server = ThreadingHTTPServer(("127.0.0.1", 0), cluster_app.make_handler(app, 0))
    port = server.server_address[1]
    server.RequestHandlerClass = cluster_app.make_handler(app, port)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % port

    def call(path, body=None, headers=None, host=None):
        req = urllib.request.Request(base + path, data=None if body is None else json.dumps(body).encode(), headers=headers or {})
        if host:
            req.add_header("Host", host)
        try:
            with urllib.request.urlopen(req, timeout=3) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    code, body = call("/")
    check("the page is served", code == 200 and "클러스터 테스트 콘솔".encode() in body)
    code, body = call("/api/meta")
    meta = json.loads(body)
    check("meta lists scenarios, faults and jobs", code == 200 and "city" in meta["scenarios"] and "crc" in meta["faults"] and len(meta["jobs"]) >= 10)
    code, _ = call("/api/ecu", {"action": "set", "values": {"speed": 10}})
    check("a change without the header is refused", code == 403)
    code, _ = call("/api/ecu", {"action": "set", "values": {"speed": 10}}, {"X-Cluster-App": "1"}, host="evil.example")
    check("a wrong Host header is refused (DNS rebinding)", code == 403)
    code, _ = call("/api/state", host="evil.example")
    check("reading with a wrong Host header is refused", code == 403)
    code, _ = call("/api/ecu", {"action": "set", "values": {"speed": 33}}, {"X-Cluster-App": "1"})
    check("a change with the header works", code == 200 and app.ecu.kv["speed"] == 33)
    code, _ = call("/api/ecu", {"action": "scenario", "name": "nope"}, {"X-Cluster-App": "1"})
    check("an unknown scenario is a 400", code == 400)
    code, _ = call("/api/job", {"name": "rm -rf /"}, {"X-Cluster-App": "1"})
    check("a command line is not accepted as a job", code == 400)
    code, _ = call("/api/config", {"ip": "1.2.3; ls"}, {"X-Cluster-App": "1"})
    check("a bad IP is refused", code == 400)
    check("the end-to-end test is in the job list", any(j["name"] == "e2e" for j in meta["jobs"]))
    check("the driving scenario test is in the job list", any(j["name"] == "scenario" for j in meta["jobs"]))
    check("the cross-check, the environment check and the gateway jobs are in the job list",
          {"cross", "env16", "gw_start", "gw_stop"} <= {j["name"] for j in meta["jobs"]})
    import subprocess
    other = subprocess.Popen(["python3", "-c", "import time; time.sleep(8)", "uds_tester.py"])
    time.sleep(0.3)
    app._conflicts = ([], 0.0)
    check("another tester in WSL is reported", any("uds_tester.py" in c for c in app.conflicts()))
    other.kill()
    app._conflicts = ([], 0.0)
    # the console that runs this self-test (a job of the real console) is itself a cluster_app.py and is expected
    check("no warning when nothing else runs", [c for c in app.conflicts() if "cluster_app.py" not in c] == [])
    import tempfile
    cluster_app.CONFIG = os.path.join(tempfile.mkdtemp(), "c.json")
    code, _ = call("/api/config", {"ip": "10.1.2.3"}, {"X-Cluster-App": "1"})
    check("the IP is saved for the next start", code == 200 and cluster_app.load_config().get("ip") == "10.1.2.3")
    cluster_app.REPORTS = tempfile.mkdtemp()
    code, body = call("/api/report", {}, {"X-Cluster-App": "1"})
    rname = json.loads(body)["name"]
    code2, page = call("/reports/" + rname)
    check("a report is saved and can be opened", code == 200 and code2 == 200 and "시험 보고서".encode() in page)
    code3, _ = call("/reports/..%2Fcluster_app.py")
    check("a report path outside the folder is refused", code3 == 404)
    code, body = call("/api/state")
    st = json.loads(body)
    check("state has the parts the page needs", code == 200 and {"ecu", "uds", "vm", "jobs", "ip"} <= set(st))

    code, body = call("/api/job", {"name": "self_signal"}, {"X-Cluster-App": "1"})
    jid = json.loads(body)["id"]
    for _ in range(50):
        code, body = call("/api/job?id=%d" % jid)
        j = json.loads(body)
        if j["state"] != "running":
            break
        time.sleep(0.2)
    check("a job runs and its output is kept", j["state"] == "passed" and any("selftest ok" in x for x in j["lines"]))
    server.shutdown()
    app.ecu.stop = app.uds.stop = app.vm.stop = True


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    test_decoders()
    test_ecu()
    test_record_replay()
    test_server()
    print("test_app: %s" % ("all checks passed" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)
