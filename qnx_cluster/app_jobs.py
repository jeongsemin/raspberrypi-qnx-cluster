"""Test runner and VM actions of the test app. Only commands from the fixed list below can be started: the
page sends a name, never a command line."""
import os
import shlex
import subprocess
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~")

# Target: the QEMU VM by default. QNX_ARCH=aarch64le selects the Raspberry Pi 4 (QNX 8.0 quick start image), where
# root cannot log in over SSH, so qnxuser is used. run_qt.sh and run_diag.sh read the same variables.
ARCH = os.environ.get("QNX_ARCH", "x86_64")
SSH_USER = os.environ.get("QNX_USER") or ("qnxuser" if ARCH == "aarch64le" else "root")
TARGET_NAME = "파이" if ARCH == "aarch64le" else "VM"      # shown in the page, the job titles and the report


def unit(src, libs=""):
    return ("d=$(mktemp -d) && gcc -Wall -Wextra -o $d/t %s %s && $d/t; rc=$?; rm -rf $d; exit $rc" % (src, libs))


def check_env_path():
    for p in (os.path.join(HERE, "..", "check_env.sh"), os.path.join(HOME, "qnx-everywhere-projects", "check_env.sh")):
        if os.path.exists(p):
            return os.path.normpath(p)
    return os.path.normpath(os.path.join(HERE, "..", "check_env.sh"))


# name -> (title, group, command with {ip}, needs the target (UDS port is shared), minutes hint)
JOBS = {
    "unit_e2e": ("단위 시험: E2E", "tests", unit("test_e2e.c", "-lm"), False),
    "unit_dem": ("단위 시험: DTC 논리", "tests", unit("test_dem.c"), False),
    "unit_isotp": ("단위 시험: ISO-TP", "tests", unit("test_isotp.c"), False),
    "unit_uds": ("단위 시험: UDS 서비스", "tests", unit("test_uds.c"), False),
    "self_signal": ("자체 시험: send_signal.py", "tests", "python3 send_signal.py --selftest", False),
    "self_uds": ("자체 시험: uds_tester.py", "tests", "python3 uds_tester.py --selftest", False),
    "self_app": ("자체 시험: 이 앱", "tests", "python3 test_app.py", False),
    "env16": ("check_env.sh 16 (환경 전체 점검)", "target", "bash " + shlex.quote(check_env_path()) + " 16 {ip}", True),
    "diag_start": (TARGET_NAME + ": cluster_diag 시작", "vm", "bash run_diag.sh {ip}", False),
    "diag_stop": (TARGET_NAME + ": cluster_diag 중지", "vm", "bash run_diag.sh {ip} stop", False),
    "diag_log": (TARGET_NAME + ": DTC 로그 보기", "vm", "bash run_diag.sh {ip} log", False),
    "diag_dump": (TARGET_NAME + ": DTC 메모리 파일 보기", "vm", "bash run_diag.sh {ip} dump", False),
    "diag_wipe": (TARGET_NAME + ": DTC 메모리 파일 삭제", "vm", "bash run_diag.sh {ip} clear", False),
    "gw_start": (TARGET_NAME + ": cluster_gateway 복사 후 시작", "vm",
                 "ssh -n -o BatchMode=yes " + SSH_USER + "@{ip} 'slay -f cluster_gateway; true'; "
                 "scp -q -o BatchMode=yes " + ARCH + "/cluster_gateway " + SSH_USER + "@{ip}:/tmp/ && "
                 "ssh -n -o BatchMode=yes " + SSH_USER + "@{ip} '(nohup /tmp/cluster_gateway > /tmp/g.log 2>&1 &)' && echo 'cluster_gateway started'", False),
    "gw_stop": (TARGET_NAME + ": cluster_gateway 중지", "vm", "ssh -n -o BatchMode=yes " + SSH_USER + "@{ip} 'slay -f cluster_gateway; true'; echo stopped", False),
    "freeze": (TARGET_NAME + ": qt_cluster를 4초 얼리기 (하트비트 DTC U1A04)", "vm", "bash run_diag.sh {ip} freeze 4", False),
    "qt_start": (TARGET_NAME + ": qt_cluster 복사 후 시작", "vm", "bash qt_cluster/run_qt.sh {ip}", False),
    "qt_stop": (TARGET_NAME + ": qt_cluster 중지", "vm", "bash qt_cluster/run_qt.sh {ip} stop", False),
}


class Jobs:
    def __init__(self, get_ip, on_exclusive, native=None):
        self.get_ip = get_ip
        self.on_exclusive = on_exclusive      # called with True/False around jobs that use the UDS port themselves
        self.native = native or {}            # name -> (title, group, function(log) -> exit status), run inside this program
        self.items = []                       # newest last
        self.lock = threading.Lock()
        self.next_id = 1

    def catalog(self):
        return ([{"name": k, "title": v[0], "group": v[1], "exclusive": v[3]} for k, v in JOBS.items()] +
                [{"name": k, "title": v[0], "group": v[1], "exclusive": True} for k, v in self.native.items()])

    def start(self, name):
        if name not in JOBS and name not in self.native:
            raise ValueError("없는 작업입니다: " + name)
        with self.lock:
            if any(j["state"] == "running" for j in self.items):
                raise RuntimeError("다른 작업이 아직 실행 중입니다")
            if name in self.native:
                title, exclusive, cmd = self.native[name][0], True, None
            else:
                title, _, cmd, exclusive = JOBS[name]
            job = {"id": self.next_id, "name": name, "title": title, "state": "running", "rc": None, "lines": [],
                   "started": time.time(), "secs": 0.0, "exclusive": exclusive}
            self.next_id += 1
            self.items.append(job)
            del self.items[:-12]
        if cmd is None:
            threading.Thread(target=self.run_native, args=(job, self.native[name][2]), daemon=True).start()
        else:
            threading.Thread(target=self.run, args=(job, cmd.replace("{ip}", self.get_ip())), daemon=True).start()
        return job["id"]

    def run_native(self, job, func):
        self.on_exclusive(True)
        try:
            job["rc"] = func(lambda text: job["lines"].append(text))
        except Exception as e:          # a bug in a test must show up as a failed job, not kill the thread silently
            job["lines"].append("예외: %r" % (e,))
            job["rc"] = 1
        finally:
            job["secs"] = round(time.time() - job["started"], 1)
            job["state"] = "passed" if job["rc"] == 0 else "failed"
            self.on_exclusive(False)

    def run(self, job, cmd):
        if job["exclusive"]:
            self.on_exclusive(True)
        try:
            p = subprocess.Popen(["bash", "-c", cmd], cwd=HERE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, text=True, errors="replace", bufsize=1)
            for line in p.stdout:
                job["lines"].append(line.rstrip("\n"))
                del job["lines"][:-2000]
            job["rc"] = p.wait()
        except OSError as e:
            job["lines"].append(str(e))
            job["rc"] = 127
        finally:
            job["secs"] = round(time.time() - job["started"], 1)
            job["state"] = "passed" if job["rc"] == 0 else "failed"
            if job["exclusive"]:
                self.on_exclusive(False)

    def get(self, job_id, start=0):
        for j in self.items:
            if j["id"] == job_id:
                return {**{k: j[k] for k in ("id", "name", "title", "state", "rc", "secs")}, "lines": j["lines"][start:], "next": len(j["lines"])}
        return None

    def report_html(self, ip):
        """The jobs run so far, with their output, as one self-contained HTML page."""
        import html
        done = [j for j in self.items if j["state"] != "running"]
        rows = "".join("<tr><td class='%s'>%s</td><td>%s</td><td>%s s</td></tr>" % (
            j["state"], "통과" if j["state"] == "passed" else "실패", html.escape(j["title"]), j["secs"]) for j in done)
        bodies = "".join("<h2>%s <small>(%s, rc %s)</small></h2><pre>%s</pre>" % (
            html.escape(j["title"]), "통과" if j["state"] == "passed" else "실패", j["rc"], html.escape("\n".join(j["lines"]))) for j in done)
        return ("<!doctype html><html lang='ko'><meta charset='utf-8'><title>시험 보고서</title>"
                "<style>body{font:14px system-ui,sans-serif;max-width:960px;margin:24px auto;padding:0 16px}"
                "table{border-collapse:collapse;width:100%%}td{border-bottom:1px solid #ccc;padding:4px 8px}"
                ".passed{color:#1a7f37;font-weight:600}.failed{color:#cf222e;font-weight:600}"
                "pre{background:#f4f5f7;padding:8px;overflow:auto;white-space:pre-wrap;font-size:12px}small{font-weight:400;color:#666}</style>"
                "<h1>클러스터 시험 보고서</h1><p>대상 %s: %s<br>만든 때: %s<br>통과 %d개, 실패 %d개</p>"
                "<table>%s</table>%s</html>" % (TARGET_NAME, html.escape(ip), time.strftime("%Y-%m-%d %H:%M:%S"),
                                              sum(j["state"] == "passed" for j in done), sum(j["state"] == "failed" for j in done), rows, bodies))

    def summary(self):
        return [{k: j[k] for k in ("id", "name", "title", "state", "rc", "secs")} for j in self.items]


class VmStatus:
    """Which programs run on the VM, and the end of the diag log. One ssh call every few seconds."""
    PROGRAMS = ("cluster_gateway", "qt_cluster", "cluster_diag", "cluster_display", "cluster_supervisor", "speed_source")

    def __init__(self, get_ip):
        self.get_ip = get_ip
        self.info = {"reachable": None, "running": {}, "diag_log": [], "age": None}
        self.misses = 0                      # failed polls in a row: one slow answer of the VM is not "unreachable"
        self.stop = False
        self.thread = threading.Thread(target=self.loop, daemon=True)

    def start(self):
        self.thread.start()

    def poll(self):
        cmd = "pidin ar; echo ===LOG; tail -n 8 /tmp/diag.log 2> /dev/null"
        try:
            r = subprocess.run(["ssh", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", SSH_USER + "@" + self.get_ip(), cmd],
                               capture_output=True, text=True, timeout=25)
            failed = r.returncode != 0 and not r.stdout
        except (subprocess.TimeoutExpired, OSError):
            failed = True
        if failed:
            self.misses += 1
            if self.misses >= 2:
                self.info.update(reachable=False)
            return
        self.misses = 0
        procs, _, log = r.stdout.partition("===LOG")
        self.info.update(reachable=True, age=time.time(), diag_log=[x for x in log.strip().splitlines()],
                         running={p: any(p in line for line in procs.splitlines()) for p in self.PROGRAMS})

    def loop(self):
        while not self.stop:
            self.poll()
            time.sleep(5.0)
