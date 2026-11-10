#!/usr/bin/env bash
# Checks that the tutorial steps you have finished left the machine in the expected state.
# Run it inside the Ubuntu / WSL terminal.
#
# Usage:
#   bash check_env.sh <step>            e.g. bash check_env.sh 2
#   bash check_env.sh <step> <VM IP>    for steps 6 and later, if getip cannot find the VM
#   QNX_ARCH=aarch64le bash check_env.sh <step> [Pi IP]
#                                       the Raspberry Pi 4 instead of the VM (QNX 8.0 quick start image, default
#                                       IP 192.168.3.138): no QEMU checks, login qnxuser, aarch64le builds and Qt
#
# <step> is the number of the last tutorial step you finished (2, 3, 4, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16).
# Every FAIL line says which step to repeat. A FAIL means the result will not match the tutorial.
STEP="${1:-2}"
VMIP="${2:-}"
pass=0
fail=0

# Target: the QEMU VM (default) or the Raspberry Pi 4. The Pi has no QEMU, root cannot log in over SSH there,
# and Qt lives in the home folder of qnxuser (see qnx_cluster/rpi4/start_pi.sh).
QNX_ARCH="${QNX_ARCH:-x86_64}"
case "$QNX_ARCH" in
    x86_64) TGT=VM; TUSER=root; OUTDIR=x86_64; QT_INSTALL=qnx-install; QT_ON_TARGET=/data/qt6 ;;
    aarch64le) TGT=Pi; TUSER="${QNX_USER:-qnxuser}"; OUTDIR=aarch64le; QT_INSTALL=qnx-install-aarch64le
               QT_ON_TARGET=/data/home/qnxuser/qt6; VMIP="${VMIP:-192.168.3.138}" ;;
    *) echo "QNX_ARCH must be x86_64 or aarch64le (is: $QNX_ARCH)" >&2; exit 2 ;;
esac
tssh() { ssh -n -o BatchMode=yes -o ConnectTimeout=8 "$TUSER@$VMIP" "$@"; }

ok()  { printf '  PASS  %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  FAIL  %s\n        -> %s\n' "$1" "$2"; fail=$((fail + 1)); }
chk() { # description  hint  command...
    local d="$1" h="$2"
    shift 2
    if "$@" > /dev/null 2>&1; then ok "$d"; else bad "$d" "$h"; fi
}

sdp_env() { source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null 2>&1; }
has_tools() { ( sdp_env && command -v qcc && command -v mkqnximage ) > /dev/null 2>&1; }
systemd_ok() { case "$(systemctl is-system-running 2> /dev/null)" in running | degraded) return 0 ;; *) return 1 ;; esac; }
license_ok() { [ -s "$HOME/.qnx/license/licenses" ]; }
no_placeholder() { ! grep -rq "__HOME__" "$1"; }
e2e_unit_test() {
    local d r
    d="$(mktemp -d)" || return 1
    ( cd "$HOME/qnx_cluster" && gcc -Wall -Wextra -o "$d/test_e2e" test_e2e.c -lm && "$d/test_e2e" ) > /dev/null 2>&1
    r=$?
    rm -rf "$d"
    return $r
}

if [ "$STEP" -ge 1 ]; then
    echo "[1] WSL2 + Ubuntu 24.04"
    chk "Ubuntu 24.04" "Install Ubuntu-24.04 (step 1)" grep -q 'VERSION_ID="24.04"' /etc/os-release
    chk "systemd is running" "Enable systemd in /etc/wsl.conf (step 1)" systemd_ok
fi

if [ "$STEP" -ge 2 ] && [ "$TGT" = VM ]; then
    echo "[2] QEMU, libvirt, bridge"
    chk "qemu-system-x86_64 installed" "Repeat the apt install of step 2" command -v qemu-system-x86_64
    chk "qemu-system-gui installed (SDL/GTK window)" "sudo apt install qemu-system-gui qemu-system-modules-opengl (step 2)" dpkg -s qemu-system-gui
    chk "virsh installed" "Repeat the apt install of step 2" command -v virsh
    chk "virbr0 bridge exists" "sudo systemctl enable --now libvirtd, then sudo virsh net-start default (step 2)" ip link show virbr0
    chk "/etc/qemu/bridge.conf allows virbr0" "Repeat the bridge.conf commands of step 2" grep -qx "allow virbr0" /etc/qemu/bridge.conf
    chk "/dev/kvm exists" "CPU virtualization is off or unavailable (step 0-1)" test -e /dev/kvm
fi

if [ "$STEP" -ge 3 ]; then
    echo "[3] QNX SDP 8.0 (Linux, inside Ubuntu)"
    chk "~/qnx800/qnxsdp-env.sh exists" "Install SDP 8.0 into /home/<user>/qnx800 with the Ubuntu QNX Software Center (step 3-6)" test -f "$HOME/qnx800/qnxsdp-env.sh"
    chk "qcc and mkqnximage are found after sourcing the SDP" "The SDP install is incomplete or is the Windows one (step 3)" has_tools
    chk "licence files present in ~/.qnx/license" "Log in to the Ubuntu QNX Software Center and check your account has an SDP 8.0 licence (step 3-5)" license_ok
fi

if [ "$STEP" -ge 4 ] && [ "$TGT" = VM ]; then
    echo "[4] QSTI image"
    Q="$HOME/qnx800/images/qemu/qemu/output"
    chk "image unpacked: ifs.bin" "Install com.qnx.qnx800.quickstart.qemu and run unpack_qemu_image.sh (step 4)" test -f "$Q/ifs.bin"
    chk "image unpacked: disk-qemu.vmdk" "Run unpack_qemu_image.sh again (step 4)" test -f "$Q/disk-qemu.vmdk"
fi

if [ "$STEP" -ge 6 ] && [ "$TGT" = Pi ]; then
    echo "[5-6] Raspberry Pi 4 and SSH ($VMIP)"
    OUT="$(tssh uname -a 2> /dev/null)"
    case "$OUT" in
        QNX*aarch64*) ok "SSH key login as $TUSER works, QNX on aarch64" ;;
        *) bad "SSH key login as $TUSER failed or the target is not QNX aarch64" "Boot the Pi from the QNX card, then ssh-copy-id $TUSER@$VMIP"; VMIP="" ;;
    esac
    if [ -n "$VMIP" ]; then
        chk "without a monitor the virtual display is used (post_startup.sh)" "Run qnx_cluster/rpi4/use_virtual_display.sh on the Pi with sudo, then reboot" tssh grep -q graphics-rpi5-virtual.conf /usr/etc/startup/post_startup.sh
        chk "the VNC server listens ($VMIP:5900)" "Reboot the Pi; vncserv is started by post_startup.sh" timeout 5 bash -c "exec 3<> /dev/tcp/$VMIP/5900"
    fi
fi

if [ "$STEP" -ge 6 ] && [ "$TGT" = VM ]; then
    echo "[5-6] Virtual machine and SSH"
    if [ -z "$VMIP" ]; then
        VMIP="$( ( sdp_env; cd "$HOME/qnx800/images/qemu/qemu" 2> /dev/null && timeout 30 mkqnximage --getip 2> /dev/null | tail -1 ) )"
    fi
    case "$VMIP" in
        192.168.*) ok "VM is running, IP $VMIP" ;;
        *) bad "VM is not running (no IP found)" "Start it as in step 5, wait 1-2 minutes, or pass the IP: bash check_env.sh $STEP <IP>"; VMIP="" ;;
    esac
    if [ -n "$VMIP" ]; then
        OUT="$(ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new root@"$VMIP" uname -s 2> /dev/null)"
        if [ "$OUT" = "QNX" ]; then ok "SSH key login as root works (uname -s: $OUT)"; else bad "SSH key login as root failed" "Repeat 6-3 and 6-4 (key registration, PermitRootLogin)"; fi
    fi
fi

if [ "$STEP" -ge 7 ]; then
    echo "[7] Projects and VS Code files"
    for p in qnx_demo qnx_cluster; do
        chk "$p installed in ~/$p" "Run bash setup.sh from the qnx-everywhere-projects folder (step 7-4)" test -f "$HOME/$p/.vscode/tasks.json"
        chk "$p: __HOME__ placeholders filled in" "Run setup.sh instead of copying by hand (step 7-4)" no_placeholder "$HOME/$p"
    done
    chk "qnx_demo builds" "Check step 3-7 (qcc and licence)" bash "$HOME/qnx_demo/build.sh"
fi

if [ "$STEP" -ge 9 ]; then
    echo "[9] Cluster example"
    chk "qnx_cluster builds for $QNX_ARCH (all four programs)" "Check step 3-7 (qcc and licence)" bash "$HOME/qnx_cluster/build.sh" "$QNX_ARCH"
    D="$HOME/qnx_cluster/$OUTDIR"
    chk "cluster_display, speed_source, cluster_supervisor and cluster_gateway exist in $OUTDIR/" "Run ~/qnx_cluster/build.sh $QNX_ARCH" test -f "$D/cluster_display" -a -f "$D/speed_source" -a -f "$D/cluster_supervisor" -a -f "$D/cluster_gateway"
fi

dem_unit_test() {
    local d r
    d="$(mktemp -d)" || return 1
    ( cd "$HOME/qnx_cluster" && gcc -Wall -Wextra -o "$d/test_dem" test_dem.c && "$d/test_dem" ) > /dev/null 2>&1
    r=$?
    rm -rf "$d"
    return $r
}

isotp_unit_test() {
    local d r
    d="$(mktemp -d)" || return 1
    ( cd "$HOME/qnx_cluster" && gcc -Wall -Wextra -o "$d/test_isotp" test_isotp.c && "$d/test_isotp" ) > /dev/null 2>&1
    r=$?
    rm -rf "$d"
    return $r
}

uds_unit_test() {
    local d r
    d="$(mktemp -d)" || return 1
    ( cd "$HOME/qnx_cluster" && gcc -Wall -Wextra -o "$d/test_uds" test_uds.c && "$d/test_uds" ) > /dev/null 2>&1
    r=$?
    rm -rf "$d"
    return $r
}

if [ "$STEP" -ge 10 ]; then
    echo "[10] Safety patterns (unit test of the message checks, runs on the PC)"
    chk "gcc is installed (needed for the unit test)" "sudo apt install -y gcc (step 10)" command -v gcc
    chk "E2E unit test passes" "Run setup.sh again into an empty folder; cluster_e2e.h and test_e2e.c must be unchanged (step 10)" e2e_unit_test
fi

if [ "$STEP" -ge 11 ]; then
    echo "[11] Signal input over UDP"
    chk "python3 is installed" "sudo apt install -y python3 (step 11)" command -v python3
    chk "send_signal.py is installed in ~/qnx_cluster" "Run setup.sh again into an empty folder (step 11)" test -f "$HOME/qnx_cluster/send_signal.py"
    chk "send_signal.py has no syntax errors" "Run setup.sh again into an empty folder (step 11)" python3 -m py_compile "$HOME/qnx_cluster/send_signal.py"
fi

if [ "$STEP" -ge 12 ]; then
    echo "[12] Qt 6.8.5 on QNX"
    QT_ROOT="$HOME/qt-build"
    chk "build packages installed (cmake, ninja, GL, fontconfig, SSL headers)" "sudo apt install -y build-essential cmake ninja-build pkg-config libgl1-mesa-dev libfontconfig1-dev libssl-dev (step 12-2)" dpkg -s cmake ninja-build libgl1-mesa-dev libfontconfig1-dev libssl-dev
    chk "Qt sources unpacked in ~/qt-build/src" "Run qt_build/1_download.sh (step 12-3)" test -d "$QT_ROOT/src/qt-everywhere-src-6.8.5"
    chk "Qt for the PC installed (host/bin/qmake)" "Run qt_build/2_host_qt.sh (step 12-4)" test -x "$QT_ROOT/host/bin/qmake"
    chk "Qt for QNX $QNX_ARCH installed (libqqnx.so)" "Run QNX_ARCH=$QNX_ARCH qt_build/3_qnx_qt.sh (step 12-5)" test -f "$QT_ROOT/$QT_INSTALL/plugins/platforms/libqqnx.so"
    chk "qt_cluster sources are in ~/qnx_cluster/qt_cluster" "Run setup.sh again into an empty folder (step 12-6)" test -f "$HOME/qnx_cluster/qt_cluster/Main.qml"
    chk "qt_cluster builds for $QNX_ARCH" "Check steps 12-4 and 12-5 (Qt for the PC and for QNX)" bash "$HOME/qnx_cluster/qt_cluster/build_qt.sh" "$QNX_ARCH"
    if [ -n "$VMIP" ]; then
        if [ "$TGT" = Pi ]; then hint="Run QNX_ARCH=aarch64le QNX_USER=$TUSER QT_QNX_PREFIX=$QT_ON_TARGET qt_build/4_deploy_qt.sh $VMIP"; else hint="Run qt_build/4_deploy_qt.sh $VMIP (step 12-6)"; fi
        chk "Qt is on the $TGT ($QT_ON_TARGET/plugins/platforms/libqqnx.so)" "$hint" tssh test -f "$QT_ON_TARGET/plugins/platforms/libqqnx.so"
    fi
fi

if [ "$STEP" -ge 13 ]; then
    echo "[13] Diagnostic trouble codes (cluster_diag)"
    chk "DTC logic unit test passes (test_dem.c)" "Run setup.sh again into an empty folder; cluster_dem.h and test_dem.c must be unchanged (step 13)" dem_unit_test
    chk "cluster_diag is built for $QNX_ARCH" "Run ~/qnx_cluster/build.sh $QNX_ARCH (step 13-2)" test -f "$HOME/qnx_cluster/$OUTDIR/cluster_diag"
    chk "run_diag.sh is installed" "Run setup.sh again into an empty folder (step 13-3)" test -f "$HOME/qnx_cluster/run_diag.sh"
    chk "qt_cluster reports to cluster_diag (receiver.cpp)" "Run setup.sh again into an empty folder (step 13-1)" grep -q DIAG_PULSE_E2E "$HOME/qnx_cluster/qt_cluster/receiver.cpp"
fi

if [ "$STEP" -ge 14 ]; then
    echo "[14] UDS server (cluster_diag)"
    chk "ISO-TP unit test passes (test_isotp.c)" "Run setup.sh again into an empty folder; cluster_isotp.h and test_isotp.c must be unchanged (step 14)" isotp_unit_test
    chk "UDS unit test passes (test_uds.c)" "Run setup.sh again into an empty folder; cluster_isotp.h, cluster_uds.h and test_uds.c must be unchanged (step 14)" uds_unit_test
    chk "uds_tester.py is installed and its self test passes" "Run setup.sh again into an empty folder (step 14-3)" python3 "$HOME/qnx_cluster/uds_tester.py" --selftest
    chk "uds_crosscheck.py is installed" "Run setup.sh again into an empty folder (step 14-5)" test -f "$HOME/qnx_cluster/uds_crosscheck.py"
    chk "cluster_diag has the UDS server (built with it)" "Run setup.sh again into an empty folder, then ~/qnx_cluster/build.sh (step 14-2)" grep -q uds_thread "$HOME/qnx_cluster/cluster_diag.c"
fi

if [ "$STEP" -ge 15 ]; then
    echo "[15] Heartbeat and service lamp (qt_cluster and cluster_diag)"
    chk "qt_cluster sends the heartbeat (main.cpp)" "Run setup.sh again into an empty folder (step 15)" grep -q DIAG_PULSE_HEARTBEAT "$HOME/qnx_cluster/qt_cluster/main.cpp"
    chk "qt_cluster shows the service lamp (Main.qml)" "Run setup.sh again into an empty folder (step 15)" grep -q serviceLamp "$HOME/qnx_cluster/qt_cluster/Main.qml"
    chk "cluster_diag watches the heartbeat (cluster_dem.h)" "Run setup.sh again into an empty folder (step 15)" grep -q dem_heartbeat_check "$HOME/qnx_cluster/cluster_dem.h"
fi

if [ "$STEP" -ge 16 ]; then
    echo "[16] Security access, ECU reset, snapshot, response pending (cluster_diag)"
    chk "the UDS server has security access and ECU reset (cluster_uds.h)" "Run setup.sh again into an empty folder (step 16)" grep -q UDS_SID_ECU_RESET "$HOME/qnx_cluster/cluster_uds.h"
    chk "qt_cluster sends the vehicle data for the snapshot (receiver.cpp)" "Run setup.sh again into an empty folder (step 16)" grep -q DIAG_PULSE_VEHICLE_A "$HOME/qnx_cluster/qt_cluster/receiver.cpp"
    chk "cluster_diag answers response pending (cluster_diag.c)" "Run setup.sh again into an empty folder (step 16)" grep -q NRC_RESPONSE_PENDING "$HOME/qnx_cluster/cluster_diag.c"
fi

echo
echo "Result: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
