#!/usr/bin/env bash
# Starts the whole test environment in WSL: the QNX VM, then qt_cluster, cluster_gateway and cluster_diag on it,
# then the test console (cluster_app.py). Every step is skipped when it is already done, so it is safe to run again.
#
# Usage: bash start_env.sh [VM IP]        start everything (default IP: the one saved by the console, else 192.168.122.146)
#        bash start_env.sh stop           stop the console and the VM
#        bash start_env.sh --no-console [VM IP]
#                                         everything but the console; the Windows launcher (app_launcher) starts
#                                         the console itself and keeps it attached to its window
#
# The VM runs detached (setsid): it keeps running when this terminal is closed.
# The VM has 4 CPUs (-smp in the qemu options). LP_NUM_THREADS=N limits the CPU threads that draw its screen (default 3).
# One step is yours: /dev/kvm must be writable (sudo chmod 666 /dev/kvm, after every WSL restart).
set -u
cd "$(dirname "$0")"

CONSOLE=1
IP=""
for a in "$@"; do
    case "$a" in
        stop)
            pkill -f cluster_app.py && echo "console stopped"
            pkill -f qemu-system-x86_64 && echo "VM stopped"
            exit 0 ;;
        --no-console) CONSOLE=0 ;;
        *) IP="$a" ;;
    esac
done
if [ -z "$IP" ]; then
    # the IP the console used last, but only a VM address: the console may have been aimed at the Raspberry Pi
    IP="$(python3 -c "import json,os;print(json.load(open(os.path.expanduser('~/.cluster_console.json')))['ip'])" 2> /dev/null)"
    case "$IP" in 192.168.122.*) ;; *) IP=192.168.122.146 ;; esac
fi
IMG="$HOME/qnx800/images/qemu/qemu"
PORT=8765
SSH="ssh -n -o BatchMode=yes -o ConnectTimeout=6 root@$IP"
step() { echo; echo "== $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }
# runs a command, shows only its last N lines, and keeps its exit status (a pipe into tail would lose it)
last() { local n="$1" out rc; shift; out="$("$@" 2>&1)"; rc=$?; printf '%s\n' "$out" | tail -n "$n"; return "$rc"; }
vm_up() { ping -c1 -W2 "$IP" > /dev/null 2>&1; }
on_vm() { $SSH "pidin ar" 2> /dev/null | grep -q "$1"; }

# The descriptor output/disk-qemu.vmdk (a 512 byte file) of the image has a parentCID line but no CID line. QEMU writes
# a new CID when it opens the disk for writing: it looks for the text "CID", finds it inside "parentCID" and so adds one
# more parentCID line on every boot. After about 20 boots the line that names the disk file ("RW <sectors> FLAT ...")
# is cut off and the guest cannot mount its disk. The repaired file has a CID line, so QEMU updates that line instead.
fix_vmdk() {
    local d="$IMG/output/disk-qemu.vmdk" disk="$IMG/output/disk-qemu"
    [ -f "$disk" ] || die "no disk image at $disk (see step 4 of the tutorial)"
    if grep -aq '^RW [0-9]* FLAT' "$d" 2> /dev/null && grep -aq '^CID=' "$d" && [ "$(grep -ac '^parentCID' "$d")" -le 1 ]; then
        return
    fi
    cp --update=none "$d" "$d.bak-$(date +%Y%m%d)"
    printf '# Disk DescriptorFile\nversion=1\nencoding="UTF-8"\nCID=fffffffe\nparentCID=ffffffff\nisNativeSnapshot="no"\ncreateType="monolithicFlat"\n\n# Extent description\nRW %d FLAT "disk-qemu" 0\n' \
        "$(($(stat -c %s "$disk") / 512))" > "$d"
    echo "repaired $d (the old one is kept as $d.bak-*)"
}

step "1. the VM ($IP)"
if vm_up; then
    echo "already running"
else
    [ -w /dev/kvm ] || die "/dev/kvm is not writable. Run once:  sudo chmod 666 /dev/kvm   (or, from PowerShell: wsl -d Ubuntu-24.04 -u root chmod 666 /dev/kvm)"
    pgrep -f qemu-system-x86_64 > /dev/null && die "a VM process runs but does not answer; stop it with: bash start_env.sh stop"
    fix_vmdk
    # shellcheck disable=SC1091
    source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null 2>&1
    # The screen of the VM is drawn by the PC through OpenGL (virtio-vga-gl). Default: on the CPU (Mesa llvmpipe); LP_NUM_THREADS keeps it
    # from taking every core, which the VM's own CPUs need too. CLUSTER_GL=d3d12 would use the GPU through WSL, but on this setup
    # QEMU crashes at start with it (segmentation fault, tried 2026-11-04), so it is only an option.
    if [ "${CLUSTER_GL:-llvmpipe}" = d3d12 ]; then
        [ -e /dev/dxg ] || die "/dev/dxg is missing: this WSL has no GPU"
        GLENV="GALLIUM_DRIVER=d3d12"
    else
        GLENV="LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS=${LP_NUM_THREADS:-3}"
    fi
    # shellcheck disable=SC2086
    (cd "$IMG" && env DISPLAY="${DISPLAY:-:0}" $GLENV setsid nohup mkqnximage --run > /tmp/vm.log 2>&1 < /dev/null &)
    echo "booting (log: /tmp/vm.log) ..."
    for _ in $(seq 1 40); do vm_up && break; sleep 5; done
    vm_up || die "the VM does not answer ping after 200 s; look at /tmp/vm.log"
    for _ in $(seq 1 20); do $SSH true 2> /dev/null && break; sleep 3; done
    $SSH true 2> /dev/null || die "the VM answers ping but not ssh"
    echo "up"
fi

step "2. qt_cluster"
if on_vm qt_cluster; then echo "already running"; else last 3 bash qt_cluster/run_qt.sh "$IP" || die "qt_cluster did not start (see above)"; fi

step "3. cluster_gateway"
if on_vm cluster_gateway; then
    echo "already running"
else
    $SSH 'test -x /tmp/cluster_gateway' 2> /dev/null || scp -q -o BatchMode=yes x86_64/cluster_gateway "root@$IP:/tmp/" || die "cannot copy x86_64/cluster_gateway (build it first)"
    $SSH '(nohup /tmp/cluster_gateway > /tmp/g.log 2>&1 &)'
    sleep 1
    on_vm cluster_gateway && echo "started" || die "cluster_gateway did not start"
fi

step "4. cluster_diag"
if on_vm cluster_diag; then echo "already running"; else last 2 bash run_diag.sh "$IP" || die "cluster_diag did not start (see above)"; fi

step "5. test console"
if [ "$CONSOLE" = 0 ]; then
    # only one sender may run, and a console aimed at the Raspberry Pi must not stay: the launcher starts its own
    if pkill -f cluster_app.py; then echo "stopped the console that was running"; fi
    echo "left to the launcher"
    echo
    echo "ready: the VM part runs ($IP)"
    exit 0
fi
if curl -fs "http://localhost:$PORT/api/meta" > /dev/null 2>&1; then
    echo "already running"
else
    # 30 minutes without an open page, then it stops by itself (so a forgotten one does not stay for days)
    setsid nohup python3 cluster_app.py "$IP" --port "$PORT" --idle-exit 1800 > /tmp/app.log 2>&1 < /dev/null &
    for _ in $(seq 1 10); do curl -fs "http://localhost:$PORT/api/meta" > /dev/null 2>&1 && break; sleep 1; done
    curl -fs "http://localhost:$PORT/api/meta" > /dev/null 2>&1 || die "the console did not start; look at /tmp/app.log"
    echo "started"
fi

echo
echo "ready: open http://localhost:$PORT  (VM $IP)"
