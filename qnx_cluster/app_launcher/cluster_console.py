"""Windows launcher of the cluster test console: asks for the target, checks the PC for what that target needs
(and installs what it can), prepares the target and shows the console.

    Raspberry Pi 4 (QNX 8.0, no monitor): rpi4/start_pi.sh starts qt_cluster, cluster_gateway and cluster_diag on
        the Pi, UltraVNC opens its virtual display, then the console (aimed at the Pi) opens.
    QEMU VM: start_env.sh boots the VM (when it is not running) and starts the same programs on it, then the console.

    ClusterConsole.exe                        ask (the target and IP chosen last time are filled in)
    ClusterConsole.exe --pi [--ip 192.168.3.138]
    ClusterConsole.exe --vm [--ip 192.168.122.146]
    ClusterConsole.exe --distro Ubuntu-24.04 --port 8765

The environment check runs before every start. Fixes that need no password run here (installing the projects from
the payload.zip inside this exe, building, copying Qt to the target); the ones that need sudo, a Pi password or
Windows administrator rights open a terminal or ask Windows; the QNX account, licence and images can only be
described: those items say what to do and open the step of the setup guide (SETUP_GUIDE_QEMU.md or
SETUP_GUIDE_PI.md on GitHub; the same files are in ~/qnx_cluster).

The choice is kept in %LOCALAPPDATA%\\ClusterConsole\\launcher.json. The console server (cluster_app.py) runs inside
WSL and is stopped when the window is closed, together with the UltraVNC window opened here. The programs on the Pi
and the VM keep running (stop them with "bash rpi4/start_pi.sh stop" or "bash start_env.sh stop").
Build the exe with build_exe.ps1 (needs Python, PyInstaller and pywebview on Windows).
"""
import argparse
import collections
import ctypes
import json
import os
import re
import shlex
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import webbrowser
import zipfile

import webview

NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
NEW_CONSOLE = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
BASE = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))   # bundled files (or this folder)
CONFIG = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "ClusterConsole", "launcher.json")
CLUSTER_DIR = "~/qnx_cluster"
QT_BUILD_DIR = "~/qnx-everywhere-projects/qt_build"
# the default VNC password that the QNX quick start image sets for vncserv (post_startup.sh)
VNC_PASSWORD = "qnxuser"
VNC_VIEWERS = (r"C:\Program Files\uvnc\UltraVNC\vncviewer.exe", r"C:\Program Files (x86)\uvnc\UltraVNC\vncviewer.exe",
               # older installers
               r"C:\Program Files\uvnc bvba\UltraVNC\vncviewer.exe", r"C:\Program Files (x86)\uvnc bvba\UltraVNC\vncviewer.exe")

TARGETS = {
    "pi": {"name": "라즈베리 파이", "ip": "192.168.3.138", "script": "rpi4/start_pi.sh", "arch": "aarch64le",
           # QNX_ARCH makes the console's jobs log in as qnxuser and use the aarch64le builds
           "env": "export QNX_ARCH=aarch64le && ", "vnc": True,
           "items": ["wsl", "vnc", "apt", "projects", "sdp", "qt", "build", "pi_net", "pi_ssh", "pi_vdisp", "pi_qt", "udsvenv"]},
    "vm": {"name": "QEMU VM", "ip": "192.168.122.146", "script": "start_env.sh", "arch": "x86_64", "env": "", "vnc": False,
           "items": ["wsl", "apt", "projects", "sdp", "qemu", "kvm", "vm_image", "qt", "build", "vm_ssh", "vm_qt", "udsvenv"]},
}

# id -> title, fix kind, fix (command or text), items it needs first, optional
#   auto: a WSL command run here, its output shown in the page    term: a WSL command in a terminal window (sudo, passwords)
#   admin: a Windows command with administrator rights            win: a Windows command in a console window
#   guide: a description of what to do by hand; the button opens that step of the setup guide (GUIDES)
ITEMS = {
    "wsl": ("WSL과 Ubuntu-24.04", "admin", "wsl.exe --install -d Ubuntu-24.04", [], False),
    "vnc": ("UltraVNC 뷰어 (파이 화면 보기)", "win",
            "winget install -e --id uvncbvba.UltraVNC --accept-package-agreements --accept-source-agreements", [], True),
    "apt": ("기본 도구 (python3, gcc, curl, ssh)", "term", "sudo apt update && sudo apt install -y python3 gcc curl openssh-client",
            ["wsl"], False),
    "projects": ("프로젝트 파일 (~/qnx-everywhere-projects, ~/qnx_cluster)", "auto", "INSTALL_PAYLOAD", ["wsl"], False),
    "sdp": ("QNX SDP 8.0과 라이선스", "guide",
            "직접 해야 합니다 (설치 가이드 3단계). myQNX 계정을 만들어 무료 비상업 라이선스를 받고, Ubuntu용 QNX "
            "Software Center를 설치해 SDP 8.0(com.qnx.qnx800)을 ~/qnx800 에 설치한 뒤, Software Center에서 로그인해 "
            "라이선스를 받습니다.",
            ["wsl"], False),
    "qt": ("QNX용 Qt 6.8.5 ({arch})", "term",
           "cd " + QT_BUILD_DIR + " && sudo apt update && sudo apt install -y build-essential cmake ninja-build pkg-config "
           "libgl1-mesa-dev libfontconfig1-dev libssl-dev && bash 1_download.sh && "
           "{{ [ -x ~/qt-build/host/bin/qmake ] || bash 2_host_qt.sh; }} && QNX_ARCH={arch} bash 3_qnx_qt.sh",
           ["projects", "sdp", "apt"], False),
    "build": ("계기판 프로그램 빌드 ({arch})", "auto",
              "cd " + CLUSTER_DIR + " && bash build.sh {arch} && bash qt_cluster/build_qt.sh {arch}", ["projects", "sdp", "qt"], False),
    "udsvenv": ("udsoncan, can-isotp (선택: 교차 확인에만 필요)", "term",
                "sudo apt install -y python3-venv && python3 -m venv ~/uds-venv && ~/uds-venv/bin/pip install udsoncan can-isotp",
                ["apt"], True),
    "pi_net": ("파이 네트워크 연결", "guide",
               "직접 해야 합니다 (설치 가이드 6~7단계). 라즈베리 파이 4에 QNX 8.0 이미지(QNX Software Center의 com.qnx.qnx800.quickstart.rpi4)를 SD 카드에 굽고, "
               "부트 파티션의 wpa_supplicant.conf 에 Wi-Fi 이름과 비밀번호를 넣은 뒤 부팅합니다(약 1분). 파이가 이 PC와 "
               "같은 공유기에 붙어 있어야 합니다. 그다음 '← 대상 다시 고르기'에서 'IP 찾기'를 누르면 파이의 IP를 찾아 줍니다.",
               [], False),
    "pi_ssh": ("파이 SSH 키 로그인 (qnxuser)", "term",
               "{{ [ -f ~/.ssh/id_ed25519 ] || ssh-keygen -t ed25519 -N '' -f ~/.ssh/id_ed25519; }} && "
               "echo '파이 비밀번호를 물으면 입력하세요 (기본값 qnxuser).' && "
               "{{ ssh-copy-id -o StrictHostKeyChecking=accept-new qnxuser@{ip} || "
               "echo '호스트 키가 바뀌었다는 오류라면 SD 카드를 새로 구운 경우입니다: ssh-keygen -R {ip} 를 실행한 뒤 다시 하세요.'; }}",
               ["pi_net", "apt"], False),
    "pi_vdisp": ("파이 가상 디스플레이 설정", "term",
                 "scp -q " + CLUSTER_DIR + "/rpi4/use_virtual_display.sh qnxuser@{ip}:/data/home/qnxuser/ && "
                 "echo 'sudo 비밀번호를 물으면 입력하세요 (기본값 qnxuser). 끝나면 파이가 다시 시작합니다(약 1분).' && "
                 "ssh -t qnxuser@{ip} 'sudo sh /data/home/qnxuser/use_virtual_display.sh && sudo shutdown'",
                 ["pi_ssh", "projects"], False),
    "pi_qt": ("파이에 Qt 배포 (/data/home/qnxuser/qt6)", "auto",
              "cd " + QT_BUILD_DIR + " && QNX_ARCH=aarch64le QNX_USER=qnxuser QT_QNX_PREFIX=/data/home/qnxuser/qt6 "
              "bash 4_deploy_qt.sh {ip}", ["pi_ssh", "qt", "projects"], False),
    "qemu": ("QEMU와 네트워크 브리지", "term",
             "sudo apt update && sudo apt install -y qemu-system qemu-system-x86 qemu-kvm libvirt-daemon-system libvirt-clients "
             "bridge-utils virt-manager qemu-system-gui qemu-system-modules-opengl && sudo systemctl enable --now libvirtd && "
             "{{ sudo virsh net-start default || true; }} && sudo virsh net-autostart default && sudo mkdir -p /etc/qemu && "
             "echo 'allow virbr0' | sudo tee /etc/qemu/bridge.conf && sudo chmod 640 /etc/qemu/bridge.conf && "
             "sudo chown root:kvm /etc/qemu/bridge.conf", ["wsl"], False),
    # /dev/kvm is root:kvm 0660 and comes back like that after every WSL restart: the kvm group keeps the access,
    # the chmod gives it now (the new group counts only in a new WSL session)
    "kvm": ("/dev/kvm 권한", "term", "sudo usermod -aG kvm $USER && sudo chmod 666 /dev/kvm && "
            "echo 'kvm 그룹에 넣었습니다. WSL을 다시 시작해도 다시 할 필요가 없습니다.'", ["qemu"], False),
    "vm_image": ("QNX QEMU 이미지", "guide",
                 "직접 해야 합니다 (설치 가이드 5단계). QNX Software Center에서 com.qnx.qnx800.quickstart.qemu 를 "
                 "설치하고, Ubuntu에서 cd ~/qnx800/images/qemu && ./unpack_qemu_image.sh 로 풉니다(수 분, 약 48GB).",
                 ["sdp"], False),
    "vm_ssh": ("VM SSH 키 로그인 (root)", "guide",
               "직접 해야 합니다 (설치 가이드 6단계). mkqnximage --run 으로 VM을 한 번 띄우고, 이 PC의 SSH 공개키를 "
               "VM의 /root/.ssh/authorized_keys 에 넣은 뒤, VM 화면의 QNX 터미널에서 root 키 로그인을 허용합니다. "
               "VM을 직접 띄우는 것은 이때 한 번만 하세요.",
               ["vm_image"], False),
    "vm_qt": ("VM에 Qt 배포 (/data/qt6)", "auto", "cd " + QT_BUILD_DIR + " && bash 4_deploy_qt.sh {ip}",
              ["vm_ssh", "qt", "projects"], False),
}
FIX_LABEL = {"auto": "자동으로 고치기", "term": "터미널에서 실행", "admin": "설치 (관리자 권한)", "win": "설치",
             "guide": "설치 가이드 열기"}

# guide item -> (guide, heading of its step); the button opens that heading of the guide on GitHub
GUIDE_URL = "https://github.com/jeongsemin/raspberrypi-qnx-cluster/blob/main/qnx_cluster/"
GUIDES = {
    "sdp": ("{guide}", "3. QNX 계정, 라이선스, SDP 8.0 (직접)"),
    "pi_net": ("SETUP_GUIDE_PI.md", "6. SD 카드 굽기 (직접)"),
    "vm_image": ("SETUP_GUIDE_QEMU.md", "5. VM 이미지 (직접)"),
    "vm_ssh": ("SETUP_GUIDE_QEMU.md", "6. VM 첫 부팅과 SSH (직접)"),
}


def gh_anchor(heading):
    """The id GitHub gives a heading: lower case, no punctuation, spaces to hyphens."""
    s = "".join(c for c in heading.lower() if c.isalnum() or c in " -_")
    return s.replace(" ", "-")


def guide_url(item, target):
    name, heading = GUIDES[item]
    name = name.format(guide="SETUP_GUIDE_PI.md" if target == "pi" else "SETUP_GUIDE_QEMU.md")
    return GUIDE_URL + name + "#" + urllib.parse.quote(gh_anchor(heading))

PAGE = """<!doctype html><html lang="ko"><meta charset="utf-8">
<style>
:root{--bg:#0d0f12;--card:#16191e;--line:#2a2f37;--text:#e6e6e6;--muted:#9aa3ad;--accent:#d7b46a;--ok:#3fb950;--bad:#ff7b72;--skip:#8b949e}
body{margin:0;font:15px system-ui,sans-serif;background:var(--bg);color:var(--text);display:flex;justify-content:center}
main{width:min(860px,92vw);margin:7vh 0 40px}h1{font-size:22px;font-weight:600;margin:0 0 6px}
p.sub{color:var(--muted);margin:0 0 22px}.cards{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:18px}
.card.last{border-color:var(--accent)}.card h2{font-size:17px;margin:0 0 6px}.card p{color:var(--muted);font-size:13px;margin:0 0 14px;min-height:54px}
label{font-size:13px;color:var(--muted)}input{width:100%;box-sizing:border-box;margin:4px 0 12px;padding:7px 9px;border-radius:6px;
border:1px solid var(--line);background:#0f1216;color:var(--text);font:14px ui-monospace,monospace}
button{padding:9px 14px;border:0;border-radius:6px;background:var(--accent);color:#1b1b1b;font-weight:600;font-size:14px;cursor:pointer}
button.ghost{background:transparent;color:var(--text);border:1px solid var(--line)}button:disabled{opacity:.4;cursor:default}
.card button{width:100%}.err{color:var(--bad);font-size:13px;min-height:18px}#step{color:var(--accent);margin:12px 0}
pre{background:var(--card);padding:12px;max-height:340px;min-height:80px;overflow:auto;font-size:12px;white-space:pre-wrap;border-radius:6px}
.row{display:grid;grid-template-columns:28px 1fr auto;gap:10px;align-items:start;padding:10px 0;border-bottom:1px solid var(--line)}
.st{font-weight:700;text-align:center}.PASS{color:var(--ok)}.FAIL{color:var(--bad)}.SKIP,.WAIT{color:var(--skip)}
.row .d{color:var(--muted);font-size:13px;margin-top:3px}.row code{display:block;color:var(--skip);font-size:11px;margin-top:4px;word-break:break-all}
.bar{display:flex;gap:10px;margin:18px 0}.row button{padding:6px 10px;font-size:13px}
.ipline{display:flex;gap:8px;align-items:center}.ipline input{flex:1}.card .ipline button{width:auto;margin:4px 0 12px;padding:7px 12px;white-space:nowrap}
#found{font-size:13px;color:var(--muted);margin:-4px 0 12px}#found a{color:var(--accent);cursor:pointer;text-decoration:underline}
@media (max-width:640px){.cards{grid-template-columns:1fr}}
</style>
<main id="choose"><h1>클러스터 테스트 콘솔</h1><p class="sub">어디에서 계기판을 띄울지 고르세요. 시작하기 전에 이 PC의 환경을 점검합니다.</p>
<div class="cards">
 <div class="card" id="card-pi"><h2>라즈베리 파이</h2>
  <p>QNX 8.0을 올린 라즈베리 파이 4. 계기판은 가상 디스플레이에 그려지고 UltraVNC 창으로 보입니다.</p>
  <label>파이 IP</label><div class="ipline"><input id="ip-pi"><button class="ghost" id="findbtn" onclick="findPi()">IP 찾기</button></div>
  <div id="found"></div><button onclick="go('pi')">파이로 시작</button><div class="err" id="err-pi"></div></div>
 <div class="card" id="card-vm"><h2>QEMU VM</h2>
  <p>WSL 안의 QNX 8.0 VM. 꺼져 있으면 부팅부터 합니다(1~3분).</p>
  <label>VM IP</label><input id="ip-vm"><button onclick="go('vm')">VM으로 시작</button><div class="err" id="err-vm"></div></div>
</div></main>
<main id="check" style="display:none"><h1 id="ctitle">환경 점검</h1><p class="sub" id="csub">점검하는 중...</p>
<div id="rows"></div>
<div class="bar"><button class="ghost" onclick="back()">← 대상 다시 고르기</button><button class="ghost" id="recheck" onclick="recheck()">다시 점검</button>
<button id="startbtn" onclick="startNow()" disabled>시작</button></div>
<pre id="flog" style="display:none"></pre></main>
<main id="loading" style="display:none"><h1 id="title"></h1><div id="step">준비하는 중...</div><pre id="log"></pre></main>
<script>
const CFG = __CFG__;
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const MARK = {PASS:"✓", FAIL:"✕", SKIP:"–", WAIT:"…"};
$("ip-pi").value = CFG.ip.pi; $("ip-vm").value = CFG.ip.vm; $("card-" + CFG.target).classList.add("last");
function show(id){ for (const m of ["choose","check","loading"]) $(m).style.display = m === id ? "block" : "none" }
function step(t){ $("step").textContent = t }
function line(t){ const l = $("log"); l.textContent += t + "\\n"; l.scrollTop = l.scrollHeight }
function flog(t){ const l = $("flog"); l.style.display = "block"; l.textContent += t + "\\n"; l.scrollTop = l.scrollHeight }
function showLoading(t){ show("loading"); $("title").textContent = t }
function checking(t){ show("check"); $("ctitle").textContent = t; $("csub").textContent = "점검하는 중..."; $("startbtn").disabled = true; $("recheck").disabled = true }
function renderChecks(items, failed){
  show("check");
  $("rows").innerHTML = items.map(i => "<div class='row'><div class='st " + i.state + "'>" + MARK[i.state] + "</div><div><div>" + esc(i.title) +
    "</div>" + (i.detail ? "<div class='d'>" + esc(i.detail) + "</div>" : "") +
    (i.state === "FAIL" && i.text ? "<div class='d'>" + esc(i.text) + "</div>" : "") +
    (i.state === "FAIL" && i.cmd ? "<code>" + esc(i.cmd) + "</code>" : "") + "</div><div>" +
    (i.state === "FAIL" && i.label ? "<button " + (i.blocked ? "disabled title='위의 항목을 먼저 해결하세요'" : "") +
      " onclick=\\"fix('" + i.id + "')\\">" + esc(i.label) + "</button>" : "") + "</div></div>").join("");
  $("csub").textContent = failed ? "필요한 항목 " + failed + "개가 아직 준비되지 않았습니다. 위에서부터 해결하고 '다시 점검'을 누르세요." +
    " 터미널 창이 열리는 항목은 그 창에서 끝까지 진행한 뒤 다시 점검하세요." : "모두 준비되었습니다.";
  $("startbtn").disabled = false; $("recheck").disabled = false;
  $("startbtn").textContent = failed ? "그래도 시작" : "시작";
}
async function go(t){ $("err-" + t).textContent = ""; const e = await pywebview.api.choose(t, $("ip-" + t).value.trim()); if (e) $("err-" + t).textContent = e; }
async function fix(id){ document.querySelectorAll(".row button").forEach(b => b.disabled = true); await pywebview.api.fix(id); }
async function findPi(){
  const b = $("findbtn"), f = $("found");
  b.disabled = true; f.textContent = "이 PC의 네트워크에서 라즈베리 파이를 찾는 중... (몇 초)";
  try {
    const r = await pywebview.api.find_pi();
    const where = r.nets.length ? r.nets.join(", ") : "(사설 네트워크 없음)";
    if (!r.found.length) f.textContent = where + " 에서 라즈베리 파이를 찾지 못했습니다. 파이가 켜져 있고 같은 Wi-Fi/공유기에 붙어 있는지 확인하세요.";
    else {
      if (r.found[0].qnx || r.found.length === 1) $("ip-pi").value = r.found[0].ip;
      f.innerHTML = r.found.map(x => "<a data-ip='" + esc(x.ip) + "'>" + esc(x.ip) + "</a> " + esc(x.kind)).join("<br>");
      f.querySelectorAll("a").forEach(a => a.onclick = () => { $("ip-pi").value = a.dataset.ip; });
    }
  } catch (e) { f.textContent = "찾지 못했습니다: " + e; }
  b.disabled = false;
}
function recheck(){ pywebview.api.recheck() }
function startNow(){ pywebview.api.start_now() }
function back(){ pywebview.api.back(); show("choose") }
</script></html>"""


def alert(text):
    ctypes.windll.user32.MessageBoxW(0, text, "클러스터 테스트 콘솔", 0x10)


def valid_ip(ip):
    return bool(re.fullmatch(r"(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})", ip or "")) and all(int(x) < 256 for x in ip.split("."))


def load_config():
    cfg = {"target": "pi", "ip": {k: v["ip"] for k, v in TARGETS.items()}}
    try:
        with open(CONFIG, encoding="utf-8") as f:
            saved = json.load(f)
        if saved.get("target") in TARGETS:
            cfg["target"] = saved["target"]
        for k in TARGETS:
            if valid_ip(saved.get("ip", {}).get(k, "")):
                cfg["ip"][k] = saved["ip"][k]
    except (OSError, ValueError, AttributeError):
        pass
    return cfg


def save_config(cfg):
    try:
        os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
        with open(CONFIG, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=1)
    except OSError:
        pass                                  # only a convenience; the launcher works without it


def answering(url):
    try:
        with urllib.request.urlopen(url + "/api/meta", timeout=1.5) as r:
            return r.status == 200
    except OSError:
        return False


def wsl_path(win_path):
    """C:\\Users\\x\\f -> /mnt/c/Users/x/f (WSL mounts the Windows drives there by default)."""
    p = os.path.abspath(win_path)
    return "/mnt/%s%s" % (p[0].lower(), p[2:].replace("\\", "/"))


def has_distro(distro):
    try:
        r = subprocess.run(["wsl", "-l", "-q"], capture_output=True, timeout=30, creationflags=NO_WINDOW, stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired):
        return False
    out = r.stdout.decode("utf-16-le", "ignore") if b"\x00" in r.stdout else r.stdout.decode("utf-8", "ignore")
    return r.returncode == 0 and distro in [x.strip() for x in out.splitlines()]


def vnc_viewer():
    return next((v for v in VNC_VIEWERS if os.path.exists(v)), None)


def payload_version():
    """The version of the projects in this exe (make_payload.py), or "" if there is no payload."""
    try:
        with zipfile.ZipFile(os.path.join(BASE, "payload.zip")) as z:
            return z.read("qnx-everywhere-projects/qnx_cluster/.payload_version").decode().strip()
    except (OSError, KeyError, zipfile.BadZipFile):
        return ""


# MAC prefixes (OUI) of Raspberry Pi boards: Raspberry Pi Foundation (b8-27-eb) and Raspberry Pi Trading
PI_OUIS = ("b8-27-eb", "dc-a6-32", "e4-5f-01", "d8-3a-dd", "2c-cf-67", "28-cd-c1")


def local_networks():
    """The /24 networks of this PC's own private IPv4 addresses (not the WSL and Hyper-V ones, 172.16-31.x)."""
    nets = set()
    try:
        addrs = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    except OSError:
        addrs = []
    for *_, sa in addrs:
        ip = sa[0]
        if ip.startswith(("192.168.", "10.")):
            nets.add(ip.rsplit(".", 1)[0])
    return sorted(nets)


def ssh_banner(ip):
    try:
        with socket.create_connection((ip, 22), timeout=2) as s:
            s.settimeout(2)
            return s.recv(256).decode("ascii", "replace").strip()
    except OSError:
        return ""


def find_pis():
    """Finds Raspberry Pi boards on this PC's networks by their MAC address. Sending one UDP datagram to every address
    makes Windows resolve them (ARP), then the ARP table names the MAC of each one that answered."""
    nets = local_networks()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for net in nets:
        for i in range(1, 255):
            try:
                s.sendto(b"", ("%s.%d" % (net, i), 9))     # port 9 (discard): nothing is expected back
            except OSError:
                pass
    s.close()
    time.sleep(2.0)
    try:
        out = subprocess.run(["arp", "-a"], capture_output=True, timeout=10, creationflags=NO_WINDOW).stdout.decode("ascii", "ignore")
    except (OSError, subprocess.TimeoutExpired):
        out = ""
    found = []
    for ip, mac in re.findall(r"(\d+\.\d+\.\d+\.\d+)\s+([0-9a-fA-F]{2}(?:-[0-9a-fA-F]{2}){5})", out):
        mac = mac.lower()
        if mac.startswith(PI_OUIS) and ip.rsplit(".", 1)[0] in nets and ip not in [f["ip"] for f in found]:
            banner = ssh_banner(ip)
            if not banner:
                kind = "SSH 응답 없음 (부팅 중이거나 꺼져 있음)"
            elif re.search(r"Debian|Raspbian|Ubuntu", banner):
                kind = "Raspberry Pi OS(리눅스)로 부팅됨: QNX SD 카드로 바꿔야 합니다"
            else:
                kind = "QNX로 보입니다"
            found.append({"ip": ip, "mac": mac, "kind": kind, "qnx": kind.startswith("QNX")})
    found.sort(key=lambda f: not f["qnx"])
    return {"nets": [n + ".0/24" for n in nets], "found": found}


class Launcher:
    def __init__(self, a):
        self.a = a
        self.cfg = load_config()
        self.url = "http://localhost:%d" % a.port
        self.console = None                   # the cluster_app.py process started here (stopped at the end)
        self.vnc = None
        # what runs now: None, "check" (may be replaced by a new check), "fix" or "start" (must finish first)
        self.mode = None
        self.gen = 0                          # number of the current check; the result of an older one is dropped
        self.win = None
        self.target = self.ip = None
        self.last = None                      # (items, failed) of the last check

    # ---- helpers ----
    def js(self, fn, *args):
        """Calls a function of the page. Right after the window opens the page may not take calls yet: try again."""
        code = "%s(%s)" % (fn, ", ".join(json.dumps(x, ensure_ascii=False) for x in args))
        for _ in range(20):
            try:
                self.win.evaluate_js(code)
                return
            except Exception:                 # pywebview raises different errors while the page is not ready
                time.sleep(0.5)

    def wsl(self, cmd, **kw):
        return subprocess.Popen(["wsl", "-d", self.a.distro, "--", "bash", "-lc", cmd], creationflags=NO_WINDOW,
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)

    def bundled(self, name):
        """A bundled script, run with LF line endings whatever the checkout had."""
        return "sed 's/\\r$//' %s | bash -s --" % shlex.quote(wsl_path(os.path.join(BASE, name)))

    def fill(self, text):
        return text.format(ip=self.ip, arch=TARGETS[self.target]["arch"])

    # ---- called from the page ----
    def choose(self, target, ip):
        if self.mode == "fix":
            return "자동으로 고치는 작업이 끝날 때까지 기다리세요."
        if self.mode == "start":
            return "이미 시작하는 중입니다."
        if target not in TARGETS:
            return "알 수 없는 대상입니다."
        if not valid_ip(ip):
            return "IPv4 주소를 입력하세요 (예: %s)." % TARGETS[target]["ip"]
        self.target, self.ip = target, ip
        self.cfg["target"] = target
        self.cfg["ip"][target] = ip
        save_config(self.cfg)
        self.run_check(auto_start=True)
        return ""

    def recheck(self):
        if self.mode is None and self.target:
            self.run_check(auto_start=False)

    def back(self):
        if self.mode == "check":
            self.gen += 1                     # its result will be dropped
            self.mode = None

    def start_now(self):
        if self.mode in ("fix", "start") or not self.target:
            return
        self.gen += 1                         # a check still running must not draw over the start page
        self.mode = "start"
        threading.Thread(target=self.prepare, daemon=True).start()

    def fix(self, item):
        if self.mode is not None or item not in ITEMS:
            self.rerender()
            return
        kind, cmd = ITEMS[item][1], ITEMS[item][2]
        if kind == "admin":
            # Windows asks for administrator rights; Ubuntu then asks for a Linux user name and password
            ctypes.windll.shell32.ShellExecuteW(None, "runas", "wsl.exe", cmd.split(" ", 1)[1], None, 1)
            self.js("flog", "WSL 설치를 시작했습니다. 끝나면 PC를 다시 시작해야 할 수 있습니다. Ubuntu 창이 열리면 리눅스 사용자 "
                            "이름과 비밀번호(이후 sudo 비밀번호)를 만든 뒤 이 창에서 '다시 점검'을 누르세요.")
            self.rerender()
        elif kind == "win":
            subprocess.Popen(["cmd", "/c", cmd + " & echo. & pause"], creationflags=NEW_CONSOLE)
            self.js("flog", "설치 창이 열렸습니다. 끝나면 '다시 점검'을 누르세요.")
            self.rerender()
        elif kind == "term":
            c = self.fill(cmd)
            subprocess.Popen(["wsl", "-d", self.a.distro, "--", "bash", "-lc",
                              "%s; echo; read -rp '끝났습니다. Enter를 누르면 이 창이 닫힙니다. 그다음 콘솔에서 다시 점검을 누르세요.' _" % c],
                             creationflags=NEW_CONSOLE)
            self.js("flog", "터미널 창에서 진행하세요: " + c)
            self.rerender()
        elif kind == "auto":
            self.mode = "fix"
            threading.Thread(target=self.run_auto, args=(item,), daemon=True).start()
        elif kind == "guide" and item in GUIDES:
            url = guide_url(item, self.target)
            webbrowser.open(url)
            self.js("flog", "설치 가이드를 브라우저로 열었습니다: %s (인터넷이 없으면 Ubuntu의 ~/qnx_cluster 에 같은 "
                            "파일이 있습니다). 끝나면 '다시 점검'을 누르세요." % url)
            self.rerender()

    def rerender(self):
        """Draws the last result again (enables the buttons) without checking: the window just opened is not done yet."""
        if self.last:
            self.js("renderChecks", *self.last)

    # ---- the check ----
    def run_check(self, auto_start):
        self.gen += 1
        self.mode = "check"
        threading.Thread(target=self.check, args=(self.gen, auto_start), daemon=True).start()

    def check(self, gen, auto_start):
        try:
            self.js("checking", "%s 환경 점검 (%s)" % (TARGETS[self.target]["name"], self.ip))
            items, failed = self.collect()
        except Exception as e:                # never leave the launcher stuck in "check"
            if gen == self.gen:
                self.mode = None
                self.js("checking", "점검 중 오류: %r" % (e,))
                self.js("renderChecks", [], 1)
            return
        if gen != self.gen:                   # the user chose again meanwhile, or started
            return
        self.last = (items, failed)
        self.mode = None
        self.js("renderChecks", items, failed)
        if auto_start and failed == 0:
            self.start_now()

    def collect(self):
        t = TARGETS[self.target]
        res = {i: ("WAIT", "WSL이 먼저 필요합니다") for i in t["items"]}
        res["wsl"] = ("PASS", "") if has_distro(self.a.distro) else ("FAIL", "%s 배포판이 없습니다" % self.a.distro)
        if "vnc" in res:
            res["vnc"] = ("PASS", "") if vnc_viewer() else ("FAIL", "vncviewer.exe 가 없습니다 (없어도 시작은 되지만 화면을 볼 수 없습니다)")
        if res["wsl"][0] == "PASS":
            p = self.wsl("%s %s %s %s" % (self.bundled("env_check.sh"), self.target, self.ip, payload_version()))
            try:
                out = p.communicate(timeout=180)[0].decode("utf-8", "replace")
            except subprocess.TimeoutExpired:
                p.kill()
                out = ""
                res = {i: ("FAIL", "점검이 3분 안에 끝나지 않았습니다") if v[0] == "WAIT" else v for i, v in res.items()}
            for ln in out.splitlines():
                parts = ln.strip().split("|", 2)
                if len(parts) == 3 and parts[0] in res:
                    res[parts[0]] = (parts[1], parts[2])
        items, failed = [], 0
        for i in t["items"]:
            title, kind, cmd, needs, optional = ITEMS[i]
            state, detail = res[i]
            if state == "FAIL" and not optional:
                failed += 1
            blocked = any(res.get(n, ("PASS",))[0] in ("FAIL", "WAIT") for n in needs)
            items.append({"id": i, "title": self.fill(title),"state": state, "detail": detail,
                          "label": FIX_LABEL.get(kind, ""), "blocked": blocked,
                          "text": cmd if kind == "guide" else "",
                          "cmd": self.fill(cmd) if kind in ("term", "auto", "win", "admin") and cmd != "INSTALL_PAYLOAD" else ""})
        return items, failed

    def run_auto(self, item):
        try:
            if ITEMS[item][2] == "INSTALL_PAYLOAD":
                z = os.path.join(BASE, "payload.zip")
                if not os.path.exists(z):
                    self.js("flog", "이 exe에 payload.zip이 없습니다. build_exe.ps1로 다시 빌드하세요.")
                    return
                cmd = "%s %s" % (self.bundled("install_payload.sh"), shlex.quote(wsl_path(z)))
            else:
                cmd = self.fill(ITEMS[item][2])
            self.js("flog", "$ " + cmd)
            p = self.wsl(cmd)
            for raw in p.stdout:
                self.js("flog", raw.decode("utf-8", "replace").replace("\x00", "").rstrip())
            self.js("flog", "-> %s" % ("끝났습니다" if p.wait() == 0 else "실패했습니다 (위의 출력을 보세요)"))
        except Exception as e:
            self.js("flog", "오류: %r" % (e,))
        finally:
            self.mode = None
        self.run_check(auto_start=False)

    # ---- the start ----
    def prepare(self):
        try:
            self.js("showLoading", "%s 준비 중 (%s)" % (TARGETS[self.target]["name"], self.ip))
            self.start_target()
        except Exception as e:
            self.js("step", "오류: %r. 이 창을 닫고 다시 실행하세요." % (e,))

    def start_target(self):
        t, a, ip = TARGETS[self.target], self.a, self.ip
        # 1. the target. --no-console: the console must stay attached to this window (WSL ends its background
        #    processes once no wsl.exe is left), so it is started below instead of by the script.
        p = self.wsl("cd %s && bash %s --no-console %s" % (CLUSTER_DIR, t["script"], ip))
        for raw in p.stdout:
            text = raw.decode("utf-8", "replace").replace("\x00", "").rstrip()
            if text.startswith("== "):
                self.js("step", text[3:])
            self.js("line", text)
        if p.wait() != 0:
            self.js("step", "준비하지 못했습니다. 위의 ERROR 줄을 보세요. 고친 뒤 이 창을 닫고 다시 실행하세요.")
            return
        # 2. the screen of the Pi
        if t["vnc"]:
            self.js("step", "VNC로 파이 화면을 여는 중...")
            v = vnc_viewer()
            if v:   # -autoscaling fits the 1920x1080 virtual display into the window
                self.vnc = subprocess.Popen([v, "%s::5900" % ip, "-password", VNC_PASSWORD, "-autoscaling"])
            else:
                self.js("line", "UltraVNC(vncviewer.exe)가 없습니다. VNC 뷰어로 %s:5900 에 직접 접속하세요 (비밀번호 %s)." % (ip, VNC_PASSWORD))
        # 3. the console
        self.js("step", "테스트 콘솔을 시작하는 중...")
        self.console = self.wsl("cd %s && %sexec python3 cluster_app.py %s --port %d --idle-exit 120" % (CLUSTER_DIR, t["env"], ip, a.port))
        console_out = collections.deque(maxlen=40)
        threading.Thread(target=lambda: console_out.extend(self.console.stdout), daemon=True).start()
        end = time.time() + 40
        while time.time() < end and not answering(self.url):
            if self.console.poll() is not None:
                break
            time.sleep(0.4)
        if not answering(self.url):
            time.sleep(0.5)                   # the last lines of a console that stopped
            out = b"".join(console_out)
            self.js("line", out.decode("utf-8", "replace").replace("\x00", "")[-600:])
            self.js("step", "테스트 콘솔이 시작되지 않았습니다. 이 창을 닫고 다시 실행해 환경 점검을 보세요.")
            return
        self.win.load_url(self.url)

    def stop(self):
        if self.vnc is not None and self.vnc.poll() is None:
            self.vnc.terminate()
        if self.console is not None:          # we started the server, so we stop it
            subprocess.run(["wsl", "-d", self.a.distro, "--", "pkill", "-f", "cluster_app.py"], creationflags=NO_WINDOW,
                           stdin=subprocess.DEVNULL, capture_output=True)
            self.console.kill()


class PageApi:
    """What the page may call. pywebview exposes every public attribute of this object to the page, so the launcher
    (with the window and the processes) is kept in a private one."""

    def __init__(self, launcher):
        self._l = launcher

    def choose(self, target, ip):
        return self._l.choose(target, ip)

    def recheck(self):
        self._l.recheck()

    def back(self):
        self._l.back()

    def start_now(self):
        self._l.start_now()

    def fix(self, item):
        self._l.fix(item)

    def find_pi(self):
        return find_pis()


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--pi", action="store_true", help="the Raspberry Pi 4, without asking")
    g.add_argument("--vm", action="store_true", help="the QEMU VM, without asking")
    ap.add_argument("--ip", default=None, help="target address (default: the one used last time for that target)")
    ap.add_argument("--distro", default="Ubuntu-24.04")
    ap.add_argument("--port", type=int, default=8765)
    a = ap.parse_args()

    app = Launcher(a)
    page = PAGE.replace("__CFG__", json.dumps(app.cfg, ensure_ascii=False))
    app.win = webview.create_window("클러스터 테스트 콘솔", html=page, js_api=PageApi(app),
                                    width=1240, height=860, min_size=(760, 560))
    auto = "pi" if a.pi else "vm" if a.vm else None

    def on_start():
        if auto:
            app.win.events.loaded.wait(15)    # the page functions must exist before they are called
            err = app.choose(auto, a.ip or app.cfg["ip"][auto])
            if err:
                alert(err)
                app.win.destroy()

    webview.start(on_start)
    app.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
