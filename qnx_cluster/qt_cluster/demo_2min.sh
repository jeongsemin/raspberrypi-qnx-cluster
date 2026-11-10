#!/usr/bin/env bash
# Two-minute drive for the Qt cluster: ONE sender runs for 120 s and the speed keeps changing.
# qt_cluster and the gateway are started first if they are not running. Run it in the Ubuntu
# terminal, then watch the QEMU window. The gear and the rpm follow the speed.
#
#    0 -  28 s  ECO      0 -> 30 km/h in 4 s, about 30 for 4 s (+-3), slowly up to 50, about 50 for 6 s
#   28 -  49 s  COMFORT  slowly up to 90, about 90 for 6 s
#   49 -  69 s  SPORT    up to 140, about 140 for 5 s
#   69 -  99 s           back down to 60, about 60 for 4 s, down to 30, about 30 for 4 s
#   99 - 120 s           slowing down to a stop, standing for 9 s
#
# Because the sender is never restarted, the frame counter keeps counting: no DATA ERROR appears.
# Optional second argument: a fault from send_signal.py (crc, burst, repeat, lost, wrongseq, silent);
# it starts at the 6th second and shows DATA ERROR / DATA INVALID / NO SIGNAL on top of the drive.
#
# Usage: bash demo_2min.sh <VM IP> [fault]
set -eu
cd "$(dirname "$0")"

IP="${1:?usage: bash demo_2min.sh <VM IP> [fault]}"
FAULT="${2:-}"
SSH="ssh -n -o BatchMode=yes -o ConnectTimeout=8 root@$IP"

# "[q]t_cluster" keeps grep from matching its own command line
running() {
    local n
    n="$($SSH "pidin ar | grep -c '[${1:0:1}]${1:1}'" 2> /dev/null || true)"
    [ "${n:-0}" -gt 0 ]
}

if ! running qt_cluster; then
    echo "starting qt_cluster"
    bash run_qt.sh "$IP"
fi
if ! running cluster_gateway; then
    echo "starting cluster_gateway"
    $SSH 'test -x /tmp/cluster_gateway' 2> /dev/null || scp -q -o BatchMode=yes "$HOME/qnx_cluster/x86_64/cluster_gateway" "root@$IP:/tmp/"
    $SSH '(nohup /tmp/cluster_gateway > /tmp/g.log 2>&1 &)'
    sleep 1
fi

PROFILE="set:mode=eco:warn=left ramp:30:4 hold:30:4 ramp:50:14 hold:50:6"
PROFILE="$PROFILE set:mode=comfort:warn=right ramp:90:15 hold:90:6"
PROFILE="$PROFILE set:mode=sport:warn=check,oil ramp:140:15 hold:140:5"
PROFILE="$PROFILE ramp:60:12 hold:60:4 set:mode=comfort:warn=left ramp:30:10 hold:30:4 ramp:0:12 hold:0:9"

ARGS=(--can --profile "$PROFILE")
[ -n "$FAULT" ] && ARGS+=(--fault "$FAULT")
echo "[$(date +%T)] driving for 120 s (the numbers below are what is being sent)"
python3 -u "$HOME/qnx_cluster/send_signal.py" "$IP" "${ARGS[@]}" | grep --line-buffered -E '^t= *[0-9]*[05]s|done'
echo "[$(date +%T)] done"
