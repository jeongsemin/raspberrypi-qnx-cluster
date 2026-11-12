#!/usr/bin/env bash
# Copies qt_cluster to the target and starts it there.
#
# Usage: bash run_qt.sh <IP>          start (stops a running qt_cluster or cluster_display first)
#        bash run_qt.sh <IP> stop     stop it
#
# Target: the QEMU VM by default. QNX_ARCH=aarch64le selects the Raspberry Pi 4 (QNX 8.0 quick start image):
#        QNX_ARCH=aarch64le bash run_qt.sh 192.168.3.138
set -eu
cd "$(dirname "$0")"

IP="${1:?usage: [QNX_ARCH=aarch64le] bash run_qt.sh <IP> [stop]}"
QNX_ARCH="${QNX_ARCH:-x86_64}"
case "$QNX_ARCH" in
    x86_64)     # VM: root login, Qt in /data/qt6 (4_deploy_qt.sh)
        T="root@$IP"; BIN=build/qt_cluster; Q=/data/qt6 ;;
    aarch64le)  # Pi: root cannot log in over SSH, so qnxuser and its home folder
                # (QNX_USER=qnxuser QT_QNX_PREFIX=/data/home/qnxuser/qt6 for 4_deploy_qt.sh).
        T="${QNX_USER:-qnxuser}@$IP"; BIN=build-aarch64le/qt_cluster; Q="${QT_QNX_PREFIX:-/data/home/qnxuser/qt6}" ;;
    *) echo "ERROR: QNX_ARCH must be x86_64 or aarch64le (is: $QNX_ARCH)" >&2; exit 1 ;;
esac
# The clocks of both targets run in UTC; TZ makes only the cluster show Korean time.
EXTRA_ENV="export TZ=KST-9"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"
# shellcheck disable=SC2086
ssh -n $SSH_OPTS "$T" 'slay -f qt_cluster; slay -f cluster_display; true' > /dev/null 2>&1 || true
[ "${2:-}" = "stop" ] && { echo "stopped"; exit 0; }

[ -f "$BIN" ] || { echo "ERROR: $BIN not found. Run build_qt.sh first (bash build_qt.sh $QNX_ARCH)." >&2; exit 1; }
sleep 1   # the file is busy until the old process is gone
# shellcheck disable=SC2086
scp -q $SSH_OPTS "$BIN" "$T:$Q/qt_cluster"

# QT_QPA_PLATFORM=qnx:rootwindow: without "rootwindow" OpenGL windows are drawn but stay black
#   (on the VM, and on the Pi's virtual display seen through VNC)
# QT_QPA_FONTDIR: Qt was built without fontconfig, so it reads the font files from this folder
# shellcheck disable=SC2086
ssh -n $SSH_OPTS "$T" "
export LD_LIBRARY_PATH=$Q/lib QT_PLUGIN_PATH=$Q/plugins QML_IMPORT_PATH=$Q/qml
export QT_QPA_PLATFORM=qnx:rootwindow QT_QPA_FONTDIR=/usr/share/fonts/liberation
$EXTRA_ENV
chmod +x $Q/qt_cluster
cd $Q
(nohup $Q/qt_cluster > /tmp/qt_cluster.log 2>&1 &)
sleep 3
pidin ar | grep -c '[q]t_cluster'" | grep -q '^[1-9]' && echo "qt_cluster is running on $IP (log: slog2info | grep qt_cluster)" \
    || { echo "ERROR: qt_cluster did not start. Check: ssh $T slog2info | grep qt_cluster" >&2; exit 1; }
