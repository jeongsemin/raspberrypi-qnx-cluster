#!/usr/bin/env python3
"""Test console for the cluster on the QNX target: a small web page that sends the signals, reads the
diagnostics and runs the checks. Run it in the Ubuntu (WSL) terminal, then open the address in the Windows browser.

    python3 cluster_app.py 192.168.122.146          the page is at http://localhost:8765
    python3 cluster_app.py                          the IP used last time (saved in ~/.cluster_console.json)
    python3 cluster_app.py 192.168.122.146 --port 9000
    python3 cluster_app.py --idle-exit 120          stop by itself when no page has asked for 120 s (used by ClusterConsole.exe)

What it does (everything goes to the target over UDP, the same way as send_signal.py and uds_tester.py):
  Drive     a virtual ECU: sliders for speed, rpm, gear, mode, warning lamps, fuel and coolant; the driving
            scenarios of scenarios.sh; faults on the CAN frames (crc, burst, repeat, lost, wrongseq, silent);
            a graph of what was sent and what the cluster read back; recording and replay of a drive
  Diagnose  the DTC list with decoded status bits, the snapshot of a DTC, the vehicle data, unlock, ECU reset, clear
  Checks    unit tests, the cross-check with udsoncan, check_env.sh, the end-to-end test (fault -> DTC), the VM
            actions of run_diag.sh / run_qt.sh, and a report of what was run (saved in reports/)

It listens on 127.0.0.1 only, and a request that changes something must carry the header X-Cluster-App.
cluster_diag cannot tell two testers apart: the page warns when another tester or sender runs in WSL.
"""
import json
import os
import re
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import app_e2e
import app_ecu
import app_jobs
import app_uds

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIO_FILE = os.path.join(HERE, "qt_cluster", "scenarios.sh")
REPORTS = os.path.join(HERE, "reports")
CONFIG = os.path.expanduser("~/.cluster_console.json")
DEFAULT_IP = "192.168.122.146"
OTHER_TESTERS = re.compile(r"uds_tester\.py|uds_crosscheck\.py|send_signal\.py")


def load_config():
    try:
        with open(CONFIG, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_config(cfg):
    try:
        with open(CONFIG, "w", encoding="utf-8") as f:
            json.dump(cfg, f)
    except OSError:
        pass


def valid_ip(ip):
    parts = ip.split(".")
    return len(parts) == 4 and all(p.isascii() and p.isdigit() and 0 <= int(p) <= 255 for p in parts)


class App:
    def __init__(self, ip):
        self.ip = ip
        self.ecu = app_ecu.Ecu(ip)
        self.uds = app_uds.Uds(ip)
        native = {"e2e": ("종단 시험: 결함을 넣고 기대한 DTC가 생기는지 확인 (약 80초)", "target", lambda log: app_e2e.run(self, log)),
                  "cross": ("udsoncan / can-isotp 교차 확인 (약 40초, DTC를 스스로 만들고 cluster_diag를 다시 시작함)", "target",
                            lambda log: app_e2e.run_cross(self, log))}
        self.jobs = app_jobs.Jobs(lambda: self.ip, self.exclusive, native)
        self.vm = app_jobs.VmStatus(lambda: self.ip)
        self.scenarios = app_ecu.load_scenarios(SCENARIO_FILE)
        self.last_request = time.monotonic()
        self._conflicts = ([], 0.0)

    def start(self):
        self.ecu.start()
        self.uds.start()
        self.vm.start()

    def exclusive(self, on):
        """A job that talks to cluster_diag itself: the page must stay off the UDS port meanwhile."""
        self.uds.paused = on

    def set_ip(self, ip):
        if not valid_ip(ip):
            raise ValueError("IPv4 주소가 아닙니다")
        self.ip = ip
        self.ecu.set_ip(ip)
        self.uds.set_ip(ip)
        save_config({**load_config(), "ip": ip})

    def conflicts(self):
        """Other programs in WSL that talk to the same gateway or the same diagnostic server (checked every 3 s)."""
        found, at = self._conflicts
        if time.monotonic() - at < 3.0:
            return found
        found = []
        try:
            out = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True, text=True, timeout=3).stdout
            for line in out.splitlines()[1:]:
                pid, _, args = line.strip().partition(" ")
                if not pid.isdigit() or int(pid) == os.getpid() or not args.strip():
                    continue
                if args.lstrip().startswith(("ps ", "bash -c", "/bin/sh -c")) or "ps -eo" in args:
                    continue
                if "cluster_app.py" in args and "python" in args:
                    found.append("cluster_app.py (PID %s): 같은 게이트웨이에 보내는 두 번째 콘솔" % pid)
                elif OTHER_TESTERS.search(args) and "python" in args:
                    if self.uds.paused and "uds_" in args:
                        continue            # the cross-check started by a job of this console
                    found.append("%s (PID %s)" % (OTHER_TESTERS.search(args).group(0), pid))
        except (OSError, subprocess.TimeoutExpired):
            pass
        self._conflicts = (found, time.monotonic())
        return found

    def state(self):
        self.last_request = time.monotonic()
        u = self.uds
        try:
            reports = sorted(f for f in os.listdir(REPORTS) if f.endswith(".html"))[-8:]
        except OSError:
            reports = []
        return {"ip": self.ip, "ecu": self.ecu.state(),
                "uds": {**u.info, "refresh": u.refresh_on, "keepalive": u.keepalive, "paused": u.paused,
                        "log": u.log[-25:]},
                "vm": self.vm.info, "jobs": self.jobs.summary(), "conflicts": self.conflicts(), "reports": reports}

    def save_report(self):
        os.makedirs(REPORTS, exist_ok=True)
        name = "report-%s.html" % time.strftime("%Y%m%d-%H%M%S")
        with open(os.path.join(REPORTS, name), "w", encoding="utf-8") as f:
            f.write(self.jobs.report_html(self.ip))
        return name

    def do_ecu(self, d):
        a = d.get("action")
        if a == "set":
            self.ecu.set_signals(d.get("values", {}))
        elif a == "sending":
            self.ecu.set_sending(d.get("on"))
        elif a == "scenario":
            name = d.get("name")
            if name not in self.scenarios:
                raise ValueError("없는 시나리오입니다")
            self.ecu.start_scenario(name, self.scenarios[name])
        elif a == "stop_scenario":
            self.ecu.stop_scenario()
            self.ecu.stop_replay()
        elif a == "fault":
            self.ecu.inject(d.get("name"))
        elif a == "clear_fault":
            self.ecu.clear_fault()
        elif a == "record_start":
            self.ecu.start_record()
        elif a == "record_stop":
            name = "rec-" + time.strftime("%Y%m%d-%H%M%S")
            return {"ok": True, "saved": self.ecu.stop_record(name)}
        elif a == "replay":
            self.ecu.start_replay(str(d.get("name", "")))
        else:
            raise ValueError("없는 동작입니다")
        return {"ok": True}

    def do_uds(self, d):
        a, u = d.get("action"), self.uds
        if u.paused:
            raise RuntimeError("시험이 cluster_diag를 쓰는 중입니다. 끝날 때까지 기다리세요")
        if a == "session":
            return u.session(d.get("kind"))
        if a == "tester_present":
            return u.tester_present()
        if a == "dtcs":
            return u.read_dtcs()
        if a == "snapshot":
            return u.snapshot(str(d.get("code")))
        if a == "vehicle":
            return u.vehicle()
        if a == "did":
            return u.read_did(int(str(d.get("did")), 16))
        if a == "clear":
            return u.clear(str(d.get("what", "all")))
        if a == "unlock":
            return u.unlock()
        if a == "reset":
            return u.reset()
        if a == "raw":
            return u.raw(str(d.get("bytes", "")))
        if a == "refresh":
            u.refresh_on = bool(d.get("on"))
            return {"ok": True}
        if a == "keepalive":
            u.keepalive = bool(d.get("on"))
            return {"ok": True}
        raise ValueError("없는 동작입니다")


def make_handler(app, port):
    allowed_hosts = ("localhost:%d" % port, "127.0.0.1:%d" % port)

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def reply(self, code, body, ctype="application/json"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def host_ok(self):
            return self.headers.get("Host", "") in allowed_hosts

        def do_GET(self):
            if not self.host_ok():
                return self.reply(403, {"error": "bad Host header"})
            url = urlparse(self.path)
            if url.path in ("/", "/index.html"):
                with open(os.path.join(HERE, "app_ui.html"), "rb") as f:
                    return self.reply(200, f.read(), "text/html; charset=utf-8")
            if url.path == "/api/state":
                return self.reply(200, app.state())
            if url.path == "/api/meta":
                return self.reply(200, {"scenarios": {k: round(app_ecu.ss.Profile(app_ecu.ss.parse_profile(v)).duration) for k, v in app.scenarios.items()},
                                        "faults": list(app_ecu.FAULT_SECONDS), "warn": list(app_ecu.WARN_NAMES),
                                        "modes": list(app_ecu.ss.MODES), "jobs": app.jobs.catalog(),
                                        "target": app_jobs.TARGET_NAME})
            if url.path == "/api/job":
                q = parse_qs(url.query)
                job = app.jobs.get(int(q.get("id", ["0"])[0]), int(q.get("from", ["0"])[0]))
                return self.reply(200 if job else 404, job or {"error": "no such job"})
            m = re.fullmatch(r"/reports/(report-\d{8}-\d{6}\.html)", url.path)
            if m and os.path.exists(os.path.join(REPORTS, m.group(1))):
                with open(os.path.join(REPORTS, m.group(1)), "rb") as f:
                    return self.reply(200, f.read(), "text/html; charset=utf-8")
            self.reply(404, {"error": "not found"})

        def do_POST(self):
            # a web page of another site can send a POST to localhost, but not with a custom header
            if not self.host_ok() or self.headers.get("X-Cluster-App") != "1":
                return self.reply(403, {"error": "forbidden"})
            try:
                n = int(self.headers.get("Content-Length", "0"))
                d = json.loads(self.rfile.read(n) or b"{}")
                path = urlparse(self.path).path
                if path == "/api/ecu":
                    return self.reply(200, app.do_ecu(d))
                if path == "/api/uds":
                    return self.reply(200, app.do_uds(d))
                if path == "/api/config":
                    app.set_ip(str(d.get("ip", "")))
                    return self.reply(200, {"ok": True})
                if path == "/api/job":
                    return self.reply(200, {"id": app.jobs.start(d.get("name"))})
                if path == "/api/report":
                    return self.reply(200, {"name": app.save_report()})
                self.reply(404, {"error": "not found"})
            except (ValueError, KeyError, TypeError, RuntimeError) as e:
                self.reply(400, {"error": str(e)})

    return Handler


def main(argv):
    port, idle = 8765, 0.0
    if "--port" in argv:
        i = argv.index("--port")
        port = int(argv[i + 1])
        del argv[i:i + 2]
    if "--idle-exit" in argv:
        i = argv.index("--idle-exit")
        idle = float(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) > 1 or (argv and not valid_ip(argv[0])):
        print(__doc__)
        return 2
    ip = argv[0] if argv else load_config().get("ip", DEFAULT_IP)
    if not valid_ip(ip):
        ip = DEFAULT_IP
    save_config({**load_config(), "ip": ip})
    app = App(ip)
    app.start()
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(app, port))
    if idle:
        def watchdog():
            while time.monotonic() - app.last_request < idle:
                time.sleep(2.0)
            server.shutdown()
        threading.Thread(target=watchdog, daemon=True).start()
    print("test console for %s: 브라우저에서 http://localhost:%d 를 여세요 (Ctrl+C로 종료)" % (ip, port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
