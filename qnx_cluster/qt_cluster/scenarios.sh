#!/usr/bin/env bash
# Driving scenarios for the Qt cluster: each one shows a different situation on the screen.
# qt_cluster and the gateway are started first if they are not running. Run it in the Ubuntu
# terminal, then watch the QEMU window. The gear and the rpm follow the speed.
#
#   city       stop and go with turn signals, seat belt lamp at the start      about  70 s
#   highway    long cruise, overtaking at 160, ECO / COMFORT / SPORT           about  93 s
#   overheat   coolant climbs from 92 to 122 C, check engine and battery lamps about  65 s
#   lowfuel    fuel drops from 22 % to 2 % while driving                       about  65 s
#   coldstart  coolant warms up from 15 to 90 C, slow start                    about  55 s
#   warnings   every warning lamp one after the other, then all of them        about  50 s
#   modes      ECO, COMFORT and SPORT with the matching speed ranges           about  55 s
#   faults     crc, burst, repeat, lost, wrongseq and silent, 15 s each        about 105 s
#   all        all of the above, one after the other                           about 560 s
#
# Every scenario is its own sender run. The frame counter starts again at each run, so the SRC counter
# on the screen grows by about 5 at every start: that is not an error of the scenario.
#
# Usage: bash scenarios.sh <VM IP> <scenario>
#        DRY=1 bash scenarios.sh <VM IP> <scenario>    only prints the profile and its length
set -eu
cd "$(dirname "$0")"

IP="${1:?usage: bash scenarios.sh <VM IP> <city|highway|overheat|lowfuel|coldstart|warnings|modes|faults|all>}"
NAME="${2:?usage: bash scenarios.sh <VM IP> <city|highway|overheat|lowfuel|coldstart|warnings|modes|faults|all>}"
SSH="ssh -n -o BatchMode=yes -o ConnectTimeout=8 root@$IP"
SENDER="$HOME/qnx_cluster/send_signal.py"

profile_of() {
    case "$1" in
    city)
        echo "set:mode=comfort:warn=seatbelt:fuel=70:coolant=60 hold:0:3 set:warn=none:coolant=70" \
             "ramp:30:5 hold:30:3 set:warn=left ramp:0:4 hold:0:4 set:warn=none ramp:45:5 hold:45:4" \
             "set:warn=right ramp:15:3 ramp:35:3 set:warn=none:coolant=85 hold:35:3 ramp:0:5 hold:0:6" \
             "ramp:50:6 hold:50:4 ramp:20:4 set:warn=left ramp:0:3 hold:0:4 set:warn=none" ;;
    highway)
        echo "set:mode=comfort:fuel=55:coolant=88 ramp:60:6 ramp:100:8 hold:100:6 set:warn=left ramp:125:5" \
             "hold:125:4 set:warn=none ramp:120:2 hold:120:10 set:mode=sport:warn=right ramp:160:6 hold:160:5" \
             "set:warn=none ramp:120:6 set:mode=eco hold:120:8 ramp:80:8 hold:80:4 ramp:0:12 hold:0:3" ;;
    overheat)
        echo "set:mode=comfort:coolant=92 ramp:70:6 hold:70:5 set:coolant=98 hold:70:4 set:coolant=104 hold:70:4" \
             "set:coolant=110:warn=check hold:70:4 set:coolant=116 ramp:40:6 set:coolant=122:warn=check,battery" \
             "hold:40:5 ramp:0:8 hold:0:5 set:coolant=118 hold:0:6 set:coolant=108 hold:0:6 set:coolant=98:warn=none hold:0:5" ;;
    lowfuel)
        echo "set:mode=eco:fuel=22:coolant=90 ramp:60:6 hold:60:5 set:fuel=18 hold:60:5 set:fuel=14 ramp:80:5 hold:80:5" \
             "set:fuel=10 hold:80:4 set:fuel=7:warn=check hold:80:4 set:fuel=5 ramp:50:6 hold:50:5 set:fuel=3" \
             "hold:50:5 set:fuel=2 ramp:0:8 hold:0:5" ;;
    coldstart)
        echo "set:mode=eco:fuel=65:coolant=15:warn=seatbelt hold:0:4 set:warn=none:coolant=20 ramp:20:5 hold:20:4" \
             "set:coolant=35 ramp:35:5 hold:35:4 set:coolant=50 ramp:50:5 hold:50:4 set:coolant=65 ramp:60:5 hold:60:4" \
             "set:coolant=80 hold:60:4 set:coolant=90 ramp:0:8 hold:0:4" ;;
    warnings)
        echo "set:mode=comfort:warn=check hold:0:4 set:warn=battery hold:0:4 set:warn=oil hold:0:4 set:warn=seatbelt hold:0:4" \
             "set:warn=left ramp:25:5 set:warn=right hold:25:5 set:warn=left,right hold:25:4" \
             "set:warn=check,oil,battery,seatbelt,left,right ramp:60:5 hold:60:5 set:warn=none ramp:0:6 hold:0:3" ;;
    modes)
        echo "set:mode=eco ramp:40:5 hold:40:4 set:mode=comfort ramp:80:5 hold:80:4 set:mode=sport ramp:130:6 hold:130:4" \
             "set:mode=comfort ramp:70:6 hold:70:3 set:mode=eco ramp:30:5 hold:30:3 ramp:0:5 hold:0:3" ;;
    *) return 1 ;;
    esac
}

length_of() {
    python3 -I - "$SENDER" "$1" << 'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("send_signal", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
print(round(mod.Profile(mod.parse_profile(sys.argv[2])).duration))
PY
}

# "[q]t_cluster" keeps grep from matching its own command line
running() {
    local n
    n="$($SSH "pidin ar | grep -c '[${1:0:1}]${1:1}'" 2> /dev/null || true)"
    [ "${n:-0}" -gt 0 ]
}

ensure_running() {
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
}

drive() {   # drive <label> <profile> [fault]
    local args=(--can --profile "$2")
    [ -n "${3:-}" ] && args+=(--fault "$3")
    echo "[$(date +%T)] $1: $(length_of "$2") s (the numbers below are what is being sent)"
    python3 -u "$SENDER" "$IP" "${args[@]}" | grep --line-buffered -E '^t= *[0-9]*[05]s|done'
}

run_one() {
    local p
    if [ "$1" = faults ]; then
        for f in crc burst repeat lost wrongseq silent; do
            if [ -n "${DRY:-}" ]; then echo "faults/$f: hold 60 km/h for 15 s, fault from the 6th to the 12th second"; continue; fi
            drive "faults/$f" "set:mode=comfort hold:60:15" "$f"
            sleep 3
        done
        return
    fi
    p="$(profile_of "$1")" || { echo "unknown scenario '$1'" >&2; exit 2; }
    if [ -n "${DRY:-}" ]; then
        echo "$1: $(length_of "$p") s"
        echo "  $p"
        return
    fi
    drive "$1" "$p"
}

[ -n "${DRY:-}" ] || ensure_running
if [ "$NAME" = all ]; then
    for s in city highway overheat lowfuel coldstart warnings modes faults; do
        run_one "$s"
        [ -n "${DRY:-}" ] || sleep 3
    done
else
    run_one "$NAME"
fi
[ -n "${DRY:-}" ] || echo "[$(date +%T)] done"
