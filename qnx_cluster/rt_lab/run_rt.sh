#!/usr/bin/env bash
# Runs the step 17 real-time experiments on the target and keeps every output on the PC.
#
# Usage: bash run_rt.sh <IP> <experiment>
#   idle       17-1  timer jitter with nothing else running (at 10 and 30), and the gaps of the CPU itself
#   preempt    17-2  timer jitter while a load at priority 20 shares the CPU: timer at 10, then at 30
#   msg        17-3  message round trip with the same load: inheriting channel, then fixed priority channel
#   inversion  17-4  mutex priority inversion: protocol none, inherit, protect
#   trace      17-5  kernel trace of one inversion round (none and inherit) as a time line (rt_trace.py)
#   cluster    17-6  the cluster under a load at 15 on every CPU, three rounds: A everything at the usual 10,
#                    B qt_cluster, cluster_gateway and cluster_diag at 20, C screen at 20 too. Shows the DTCs of
#                    each round. Needs those running (start_env.sh) and no other sender on the PC. It clears the
#                    DTC memory and puts every priority back. The VM only: on the Pi qnxuser cannot change screen.
#   all        idle to trace, one after another (about 1 minute); cluster is left out because it clears DTCs
#
# Target: the QEMU VM by default. QNX_ARCH=aarch64le selects the Raspberry Pi 4 (login qnxuser).
# Results: rt_lab/results/<arch>-<date>-<time>/<experiment>.txt, plus summary.txt (the RESULT lines).
# Build first: bash build_rt.sh [aarch64le]
set -eu
cd "$(dirname "$0")"

IP="${1:?usage: bash run_rt.sh <IP> idle|preempt|msg|inversion|trace|all}"
EXP="${2:?usage: bash run_rt.sh <IP> idle|preempt|msg|inversion|trace|all}"
QNX_ARCH="${QNX_ARCH:-x86_64}"
case "$QNX_ARCH" in
    x86_64) T="root@$IP" ;;
    aarch64le) T="${QNX_USER:-qnxuser}@$IP" ;;
    *) echo "ERROR: QNX_ARCH must be x86_64 or aarch64le (is: $QNX_ARCH)" >&2; exit 1 ;;
esac
# CPU 1, not 0: on both targets CPU 0 takes most interrupts, which would add noise that is not ours
CPU="${RT_CPU:-1}"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"
DIR=/tmp/rt_lab
OUT="results/$QNX_ARCH-$(date +%Y%m%d-%H%M%S)"
PROGS="rt_timer rt_gap rt_load rt_msg rt_inversion"

# shellcheck disable=SC2086
on_target() { ssh -n $SSH_OPTS "$T" "$@"; }

for p in $PROGS; do
    [ -x "$QNX_ARCH/$p" ] || { echo "ERROR: $QNX_ARCH/$p not found. Run: bash build_rt.sh $QNX_ARCH" >&2; exit 1; }
done
on_target true 2> /dev/null || { echo "ERROR: cannot reach $T over SSH" >&2; exit 1; }
on_target "mkdir -p $DIR"
# shellcheck disable=SC2086
scp -q $SSH_OPTS $(for p in $PROGS; do echo "$QNX_ARCH/$p"; done) "$T:$DIR/"
mkdir -p "$OUT"
echo "results in rt_lab/$OUT"

# runs one shell line on the target, shows it and keeps it in <experiment>.txt
run() {
    local name="$1"; shift
    # "cd ...;" and not "cd ... &&": with "&&" a line like "A & B" would put the cd into A's background job only
    { echo "### $*"; on_target "cd $DIR || exit 1; $*" 2>&1; echo; } | tee -a "$OUT/$name.txt"
}

# A load on CPU $CPU, busy 5 ms of every 10 ms at priority 20, started one second before the measurement and
# running a little longer than it. "sleep 1" because the QNX shell sleeps in whole seconds.
LOAD="./rt_load -p 20 -c $CPU -b 5 -P 10"

exp_idle() {
    # at 10 the timer shares the CPU with every ordinary program (qt_cluster, the gateway, sshd: all at 10);
    # at 30 only drivers and the kernel come first, so what is left is the jitter of the system itself
    run idle "./rt_timer -t 1000 -n 5000 -p 10 -c $CPU -H"
    run idle "./rt_timer -t 1000 -n 5000 -p 30 -c $CPU -H"
    # the floor: gaps a thread at 60 cannot prevent (interrupts; on the VM also the PC not running the virtual CPU)
    run idle "./rt_gap -s 5 -p 60 -c $CPU"
}

exp_preempt() {
    run preempt "$LOAD -s 6 > load.txt & sleep 1; ./rt_timer -t 1000 -n 3000 -p 10 -c $CPU -H; wait; cat load.txt"
    run preempt "$LOAD -s 6 > load.txt & sleep 1; ./rt_timer -t 1000 -n 3000 -p 30 -c $CPU -H; wait; cat load.txt"
}

exp_msg() {
    run msg "./rt_msg -n 2000 -c $CPU -H"
    run msg "$LOAD -s 5 > load.txt & sleep 1; ./rt_msg -n 2000 -c $CPU -H; wait; cat load.txt"
    run msg "$LOAD -s 5 > load.txt & sleep 1; ./rt_msg -n 2000 -c $CPU -f -H; wait; cat load.txt"
}

exp_inversion() {
    local m
    for m in none inherit protect; do
        run inversion "./rt_inversion -m $m -n 20 -c $CPU"
    done
}

exp_trace() {
    local m tl
    command -v python3 > /dev/null || { echo "ERROR: python3 is needed for the time line" >&2; return 1; }
    tl="$HOME/qnx800/target/qnx/$QNX_ARCH/usr/sbin/tracelogger"
    if ! on_target "command -v tracelogger" > /dev/null 2>&1; then
        # the image has no tracelogger: copy the one from the SDP next to the programs
        scp -q $SSH_OPTS "$tl" "$T:$DIR/"
        TRACELOGGER="$DIR/tracelogger"
    else
        TRACELOGGER=tracelogger
    fi
    for m in none inherit; do
        # wide mode (-w) keeps the thread priorities in the events; the log runs 2 s, one round takes about 30 ms
        run trace "rm -f trace-$m.kev; $TRACELOGGER -w -s 2 -f trace-$m.kev > /dev/null 2>&1 & sleep 1; ./rt_inversion -m $m -n 1 -c $CPU; wait; ls -l trace-$m.kev"
        scp -q $SSH_OPTS "$T:$DIR/trace-$m.kev" "$OUT/"
        traceprinter -f "$OUT/trace-$m.kev" > "$OUT/trace-$m.txt" 2> /dev/null || true
        python3 rt_trace.py "$OUT/trace-$m.txt" "$CPU" | tee -a "$OUT/trace.txt"
    done
}

# 17-6: one round of the cluster test. CAN frames go out at 20 Hz the whole round; at about t=7 s a load at
# priority 15 takes every CPU for 3 s. io-sock (21) still receives the frames, but whatever runs below 15 waits.
# The DTCs are read 2 s after the load while the frames still come, so a DTC that is "testFailed" then is still
# failing because of the load, not because the sender stopped.
CLUSTER_APPS="qt_cluster cluster_gateway cluster_diag"
cluster_round() {
    local label="$1" prio="$2" screen_prio="${3:-}" sp
    echo "## $label" | tee -a "$OUT/cluster.txt"
    # frames first, so the cluster already has a signal when cluster_diag starts (or it would log U0100 at once)
    python3 ../send_signal.py "$IP" --can speed=80 rpm=2500 gear=4 mode=comfort --seconds 30 > /dev/null 2>&1 &
    sp=$!
    sleep 2
    # a fresh DTC memory, so only this round's DTCs show. The restart starts cluster_diag at 10, so the
    # priorities are set after it.
    bash ../run_diag.sh "$IP" clear > /dev/null
    bash ../run_diag.sh "$IP" > /dev/null
    set_prio "$prio"
    [ -n "$screen_prio" ] && { on_target "slay -P $screen_prio screen" > /dev/null 2>&1 || true; }
    on_target "pidin -f anp" 2> /dev/null | grep -E "qt_cluster|cluster_gateway|cluster_diag|screen" | sort | uniq -c \
        | tee -a "$OUT/cluster.txt"
    sleep 3
    echo "DTCs before the load:" | tee -a "$OUT/cluster.txt"
    python3 ../uds_tester.py "$IP" read-dtc 2>&1 | tee -a "$OUT/cluster.txt"
    run cluster "./rt_load -p 15 -c all -s 3"
    sleep 2
    echo "DTCs after the load (frames still coming):" | tee -a "$OUT/cluster.txt"
    python3 ../uds_tester.py "$IP" read-dtc 2>&1 | tee -a "$OUT/cluster.txt"
    kill "$sp" 2> /dev/null || true
    wait "$sp" 2> /dev/null || true
    echo | tee -a "$OUT/cluster.txt"
}
set_prio() {
    local a
    # slay returns 1 even when it worked (as in run_diag.sh); pidin in cluster_round shows the result
    for a in $CLUSTER_APPS; do on_target "slay -P $1 $a" > /dev/null 2>&1 || true; done
}
# screen (the graphics server) has threads at 10 and at 15. These two keep and restore them one by one.
SCREEN_PRIOS=""
save_screen() {
    SCREEN_PRIOS="$(on_target "pidin -P screen -f bp" 2> /dev/null | awk 'NR > 1 { sub(/[a-z]$/, "", $2); print $1 ":" $2 }')"
}
restore_screen() {
    local tp
    for tp in $SCREEN_PRIOS; do on_target "slay -P ${tp#*:} -T ${tp%%:*} screen" > /dev/null 2>&1 || true; done
}
exp_cluster() {
    local a
    for a in $CLUSTER_APPS; do
        on_target "pidin ar" 2> /dev/null | grep -q "$a" || { echo "ERROR: $a is not running (bash start_env.sh)" >&2; return 1; }
    done
    pgrep -f "send_signal.py|cluster_app.py|uds_tester.py" > /dev/null && { echo "ERROR: another sender runs on the PC; stop it first" >&2; return 1; }
    save_screen
    cluster_round "A: everything at priority 10 (as started), load at 15" 10
    cluster_round "B: qt_cluster, cluster_gateway, cluster_diag at 20, load at 15" 20
    # qt_cluster draws through screen: its frames, and so its heartbeat, wait for screen's own threads
    cluster_round "C: as B, and the graphics server screen at 20 too" 20 20
    restore_screen
    set_prio 10
    on_target "pidin -P screen -f bp" 2> /dev/null | awk 'NR > 1 { print $2 }' | sort | uniq -c \
        | sed 's/^/screen threads back at: /' | tee -a "$OUT/cluster.txt"
    bash ../run_diag.sh "$IP" clear > /dev/null
    bash ../run_diag.sh "$IP" > /dev/null
    echo "priorities back at 10, DTC memory cleared" | tee -a "$OUT/cluster.txt"
}

case "$EXP" in
    idle|preempt|msg|inversion|cluster) "exp_$EXP" ;;
    trace)
        source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null
        exp_trace ;;
    all)
        source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null
        exp_idle; exp_preempt; exp_msg; exp_inversion; exp_trace ;;
    *) echo "ERROR: unknown experiment $EXP" >&2; exit 1 ;;
esac

grep -h "^TARGET\|^CONFIG\|^RESULT" "$OUT"/*.txt > "$OUT/summary.txt" 2> /dev/null || true
echo
echo "== summary (rt_lab/$OUT/summary.txt)"
grep "^RESULT" "$OUT/summary.txt" || true
