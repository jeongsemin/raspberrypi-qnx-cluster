#!/usr/bin/env bash
# The WSL part of the environment check of ClusterConsole.exe. One line per item: ID|PASS or FAIL or SKIP|detail.
# The exe shows the titles and the fixes; this script only looks. It changes nothing.
#
# Usage: bash env_check.sh pi <Pi IP> [payload version]
#        bash env_check.sh vm <VM IP> [payload version]
# The payload version is the one in the exe (make_payload.py). When it is given, the installed projects must have it.
TARGET="${1:-pi}"
IP="${2:-}"
WANT="${3:-}"
P="$HOME/qnx-everywhere-projects"
C="$HOME/qnx_cluster"
if [ "$TARGET" = pi ]; then ARCH=aarch64le; QT_INSTALL=qnx-install-aarch64le; QB=build-aarch64le
else ARCH=x86_64; QT_INSTALL=qnx-install; QB=build; fi

say() { echo "$1|$2|$3"; }
item() { # id detail-when-failing command...
    local id="$1" d="$2"
    shift 2
    if "$@" > /dev/null 2>&1; then say "$id" PASS ""; else say "$id" FAIL "$d"; fi
}

item apt "python3, gcc, curl, ssh 중 없는 것이 있습니다" \
    bash -c 'command -v python3 && command -v gcc && command -v curl && command -v ssh && command -v scp'

# the projects: present, and new enough for the launcher (Raspberry Pi support, QNX_ARCH in the scripts)
if [ ! -f "$C/cluster_app.py" ] || [ ! -f "$P/check_env.sh" ]; then
    say projects FAIL "~/qnx-everywhere-projects 또는 ~/qnx_cluster 가 없습니다"
elif [ ! -f "$C/rpi4/start_pi.sh" ] || ! grep -q QNX_ARCH "$P/qt_build/qt_env.sh" 2> /dev/null \
     || ! grep -q QNX_ARCH "$C/run_diag.sh" 2> /dev/null || ! grep -q TARGET_NAME "$C/app_jobs.py" 2> /dev/null \
     || ! grep -q -- --no-console "$C/start_env.sh" 2> /dev/null; then
    say projects FAIL "설치된 프로젝트가 예전 버전입니다 (파이 지원이 없음)"
elif [ -n "$WANT" ] && [ "$(cat "$C/.payload_version" 2> /dev/null)" != "$WANT" ]; then
    say projects FAIL "설치된 프로젝트가 이 exe에 든 것과 다릅니다 (설치된 것을 백업하고 바꿉니다)"
else
    say projects PASS ""
fi

if [ -f "$HOME/qnx800/qnxsdp-env.sh" ] && [ -s "$HOME/.qnx/license/licenses" ]; then say sdp PASS ""
elif [ -f "$HOME/qnx800/qnxsdp-env.sh" ]; then say sdp FAIL "SDP는 있지만 라이선스 파일(~/.qnx/license/licenses)이 없습니다"
else say sdp FAIL "~/qnx800 에 SDP 8.0이 없습니다"; fi

item qt "~/qt-build/$QT_INSTALL 에 QNX용 Qt($ARCH)가 없습니다" test -f "$HOME/qt-build/$QT_INSTALL/plugins/platforms/libqqnx.so"

item build "$ARCH 용 실행 파일(cluster_gateway, cluster_diag, qt_cluster)이 없습니다" \
    test -x "$C/$ARCH/cluster_gateway" -a -x "$C/$ARCH/cluster_diag" -a -f "$C/qt_cluster/$QB/qt_cluster"

item udsvenv "~/uds-venv 에 udsoncan, can-isotp가 없습니다 (교차 확인에만 필요)" \
    "$HOME/uds-venv/bin/python" -c "import udsoncan, isotp"

SSHO="-n -o BatchMode=yes -o ConnectTimeout=6"
if [ "$TARGET" = pi ]; then
    if timeout 5 bash -c "exec 3<> /dev/tcp/$IP/22" 2> /dev/null; then
        say pi_net PASS ""
        # shellcheck disable=SC2086
        OUT="$(ssh $SSHO "qnxuser@$IP" uname -a 2>&1)"
        case "$OUT" in
            QNX*aarch64*) say pi_ssh PASS "" ;;
            *"IDENTIFICATION HAS CHANGED"*) say pi_ssh FAIL "호스트 키가 바뀌었습니다 (SD 카드를 새로 구웠다면 정상)" ;;
            *) say pi_ssh FAIL "qnxuser로 키 로그인이 되지 않습니다" ;;
        esac
        if [ "${OUT#QNX}" != "$OUT" ]; then
            # shellcheck disable=SC2086
            item pi_vdisp "모니터 없이 쓰는 가상 디스플레이 설정이 없습니다" ssh $SSHO "qnxuser@$IP" grep -q graphics-rpi5-virtual.conf /usr/etc/startup/post_startup.sh
            # shellcheck disable=SC2086
            item pi_qt "파이에 Qt(/data/home/qnxuser/qt6)가 없습니다" ssh $SSHO "qnxuser@$IP" test -f /data/home/qnxuser/qt6/plugins/platforms/libqqnx.so
        else
            say pi_vdisp SKIP "SSH가 되어야 확인할 수 있습니다"
            say pi_qt SKIP "SSH가 되어야 확인할 수 있습니다"
        fi
    else
        say pi_net FAIL "$IP 의 SSH(22번 포트)에 닿지 않습니다"
        for i in pi_ssh pi_vdisp pi_qt; do say $i SKIP "파이에 닿아야 확인할 수 있습니다"; done
    fi
else
    item qemu "QEMU, libvirt 또는 브리지(virbr0) 설정이 없습니다" \
        bash -c 'command -v qemu-system-x86_64 && ip link show virbr0 && grep -qx "allow virbr0" /etc/qemu/bridge.conf'
    item kvm "/dev/kvm 에 쓸 수 없습니다 (사용자를 kvm 그룹에 넣으면 WSL을 다시 켜도 유지됩니다)" test -w /dev/kvm
    item vm_image "QNX QEMU 이미지(~/qnx800/images/qemu)가 없습니다" test -f "$HOME/qnx800/images/qemu/qemu/output/ifs.bin"
    if ping -c1 -W2 "$IP" > /dev/null 2>&1; then
        # shellcheck disable=SC2086
        item vm_ssh "root로 키 로그인이 되지 않습니다" bash -c "[ \"\$(ssh $SSHO root@$IP uname -s)\" = QNX ]"
        # shellcheck disable=SC2086
        item vm_qt "VM에 Qt(/data/qt6)가 없습니다" ssh $SSHO "root@$IP" test -f /data/qt6/plugins/platforms/libqqnx.so
    else
        say vm_ssh SKIP "VM이 꺼져 있습니다 (시작할 때 부팅합니다)"
        say vm_qt SKIP "VM이 꺼져 있습니다"
    fi
fi
