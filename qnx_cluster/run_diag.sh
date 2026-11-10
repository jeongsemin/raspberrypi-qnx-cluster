#!/usr/bin/env bash
# Starts and inspects cluster_diag, the DTC server, on the VM (step 13).
#
# Usage: bash run_diag.sh <VM IP>          copy and start it (stops a running one first)
#        bash run_diag.sh <VM IP> log      show its log (every DTC change)
#        bash run_diag.sh <VM IP> dump     show the DTC memory file (what a restart would load)
#        bash run_diag.sh <VM IP> stop     stop it (the DTC memory stays)
#        bash run_diag.sh <VM IP> clear    stop it and delete the DTC memory file
#        bash run_diag.sh <VM IP> freeze [S]   freeze qt_cluster for S seconds (default 4): no heartbeat, U1A04
#
# Target: the QEMU VM by default. QNX_ARCH=aarch64le selects the Raspberry Pi 4: login qnxuser (root cannot log in
# over SSH there) and the DTC memory in its home folder (qnxuser cannot write /data).
set -eu
cd "$(dirname "$0")"

IP="${1:?usage: bash run_diag.sh <VM IP> [log|dump|stop|clear|freeze [S]]}"
ACTION="${2:-start}"
QNX_ARCH="${QNX_ARCH:-x86_64}"
case "$QNX_ARCH" in
    x86_64) T="root@$IP"; NVM=/data/cluster_diag.nvm ;;
    aarch64le) T="${QNX_USER:-qnxuser}@$IP"; NVM=/data/home/qnxuser/cluster_diag.nvm ;;
    *) echo "ERROR: QNX_ARCH must be x86_64 or aarch64le (is: $QNX_ARCH)" >&2; exit 1 ;;
esac
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"
BIN=/tmp/cluster_diag

# shellcheck disable=SC2086
ssh_vm() { ssh -n $SSH_OPTS "$T" "$@"; }

case "$ACTION" in
log)
    ssh_vm 'cat /tmp/diag.log'
    ;;
dump)
    ssh_vm "test -x $BIN" 2> /dev/null || { echo "ERROR: $BIN is not on the VM. Start it once with: bash run_diag.sh $IP" >&2; exit 1; }
    ssh_vm "$BIN --dump $NVM"
    ;;
stop)
    ssh_vm 'slay -f cluster_diag; true' > /dev/null 2>&1 || true
    echo "stopped (the DTC memory $NVM stays)"
    ;;
freeze)
    secs="${3:-4}"
    # slay -h holds a process (SIGSTOP), slay -u lets it go on. slay returns 1 even when it worked, so look at the threads.
    n="$(ssh_vm 'slay -h qt_cluster > /dev/null 2>&1; pidin -p qt_cluster 2> /dev/null | grep -c STOPPED' || true)"
    [ "${n:-0}" -gt 0 ] || { echo "ERROR: could not hold qt_cluster (is it running?)" >&2; exit 1; }
    echo "qt_cluster frozen for $secs s"
    sleep "$secs"
    ssh_vm 'slay -u qt_cluster' > /dev/null 2>&1 || true
    echo "qt_cluster running again"
    ;;
clear)
    ssh_vm 'slay -f cluster_diag; true' > /dev/null 2>&1 || true
    sleep 1
    ssh_vm "rm -f $NVM $NVM.tmp"
    echo "stopped, DTC memory deleted"
    ;;
start)
    [ -f "$QNX_ARCH/cluster_diag" ] || { echo "ERROR: $QNX_ARCH/cluster_diag not found. Run build.sh $QNX_ARCH first." >&2; exit 1; }
    ssh_vm 'slay -f cluster_diag; true' > /dev/null 2>&1 || true
    sleep 1   # the file is busy until the old process is gone
    # shellcheck disable=SC2086
    scp -q $SSH_OPTS "$QNX_ARCH/cluster_diag" "$T:$BIN"
    ssh_vm "chmod +x $BIN; ($BIN $NVM > /tmp/diag.log 2>&1 &); sleep 1; pidin ar | grep -c '[c]luster_diag'" | grep -q '^[1-9]' \
        && echo "cluster_diag is running on $IP (log: bash run_diag.sh $IP log)" \
        || { echo "ERROR: cluster_diag did not start. Check: ssh $T cat /tmp/diag.log" >&2; exit 1; }
    ;;
*)
    echo "unknown action '$ACTION'" >&2
    exit 2
    ;;
esac
