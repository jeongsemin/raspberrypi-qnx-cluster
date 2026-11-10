#!/usr/bin/env bash
# The Raspberry Pi 4 version of start_env.sh: starts qt_cluster, cluster_gateway and cluster_diag on the Pi
# (QNX 8.0 quick start image), then the test console in WSL aimed at the Pi. Safe to run again.
#
# Usage: bash rpi4/start_pi.sh [Pi IP]        start everything (default IP: 192.168.3.138)
#        bash rpi4/start_pi.sh stop [Pi IP]   stop the console and the cluster programs on the Pi
#        bash rpi4/start_pi.sh --no-console [Pi IP]
#                                             only the Pi part; the Windows launcher (app_launcher) starts the
#                                             console itself and keeps it attached to its window
#
# Needs: Qt for QNX aarch64le on the Pi (qt_build: QNX_ARCH=aarch64le QNX_USER=qnxuser
# QT_QNX_PREFIX=/data/home/qnxuser/qt6 bash 4_deploy_qt.sh <IP>), qt_cluster built with
# "bash qt_cluster/build_qt.sh aarch64le", and an SSH key for qnxuser (ssh-copy-id qnxuser@<IP>).
# The screen is seen through VNC (<IP>:5900) when no monitor is attached; see rpi4/use_virtual_display.sh.
set -u
cd "$(dirname "$0")/.."
export QNX_ARCH=aarch64le                    # run_qt.sh, run_diag.sh and the console (app_jobs.py) read it

STOP=0
CONSOLE=1
IP=192.168.3.138
for a in "$@"; do
    case "$a" in
        stop) STOP=1 ;;
        --no-console) CONSOLE=0 ;;
        *) IP="$a" ;;
    esac
done
PORT=8765
T="${QNX_USER:-qnxuser}@$IP"
SSH="ssh -n -o BatchMode=yes -o ConnectTimeout=6 $T"
step() { echo; echo "== $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
# runs a command, shows only its last N lines, and keeps its exit status (a pipe into tail would lose it)
last() { local n="$1" out rc; shift; out="$("$@" 2>&1)"; rc=$?; printf '%s\n' "$out" | tail -n "$n"; return "$rc"; }
# pidin shows qnxuser only its own processes, which is all of the cluster here
on_pi() { $SSH "pidin ar" 2> /dev/null | grep -q "$1"; }

if [ "$STOP" = 1 ]; then
    pkill -f cluster_app.py && echo "console stopped"
    $SSH 'slay -f qt_cluster; slay -f cluster_gateway; slay -f cluster_diag; true' > /dev/null 2>&1
    echo "cluster programs on $IP stopped"
    exit 0
fi

step "1. the Pi ($IP)"
$SSH true 2> /dev/null || die "no SSH to $T. Is the Pi on, and was the key copied (ssh-copy-id $T)?"
# uname -m gives the board name (RaspberryPi4B), so look for the CPU in the whole line
$SSH 'uname -a' 2> /dev/null | grep -q aarch64 || die "$IP is not an aarch64 QNX target"
echo "up"
[ -x aarch64le/cluster_gateway ] && [ -x aarch64le/cluster_diag ] || bash build.sh aarch64le > /dev/null || die "build.sh aarch64le failed"

step "2. qt_cluster"
if on_pi qt_cluster; then echo "already running"; else last 3 bash qt_cluster/run_qt.sh "$IP" || die "qt_cluster did not start (see above)"; fi

step "3. cluster_gateway"
if on_pi cluster_gateway; then
    echo "already running"
else
    scp -q -o BatchMode=yes aarch64le/cluster_gateway "$T:/tmp/" || die "cannot copy aarch64le/cluster_gateway"
    $SSH '(nohup /tmp/cluster_gateway > /tmp/g.log 2>&1 &)'
    sleep 1
    on_pi cluster_gateway && echo "started" || die "cluster_gateway did not start (ssh $T cat /tmp/g.log)"
fi

step "4. cluster_diag"
if on_pi cluster_diag; then echo "already running"; else last 2 bash run_diag.sh "$IP" || die "cluster_diag did not start (see above)"; fi

step "5. test console"
# Only one sender may run (a second one makes "counter jumped" and false U0401), and a console started for the VM
# would log in as root, so any running console is replaced by one aimed at the Pi.
if pkill -f cluster_app.py; then echo "stopped the console that was running"; sleep 1; fi
if [ "$CONSOLE" = 0 ]; then
    echo "left to the launcher"
    echo
    echo "ready: the Pi part runs ($IP, screen: VNC $IP:5900)"
    exit 0
fi
setsid nohup python3 cluster_app.py "$IP" --port "$PORT" --idle-exit 1800 > /tmp/app.log 2>&1 < /dev/null &
for _ in $(seq 1 10); do curl -fs "http://localhost:$PORT/api/meta" > /dev/null 2>&1 && break; sleep 1; done
curl -fs "http://localhost:$PORT/api/meta" > /dev/null 2>&1 || die "the console did not start; look at /tmp/app.log"
echo "started"

echo
echo "ready: open http://localhost:$PORT  (Pi $IP, screen: VNC $IP:5900)"
