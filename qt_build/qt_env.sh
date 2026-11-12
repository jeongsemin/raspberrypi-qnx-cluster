#!/usr/bin/env bash
# Common settings for the Qt build scripts. Source it, do not run it.
#
# WSL appends the Windows PATH (/mnt/c/...) to the Linux PATH. CMake then searches the Windows
# drives for libraries: configure took 24 minutes and linked Windows FreeType/zlib. Every script
# therefore strips /mnt entries, also after sourcing the SDP (qnxsdp-env.sh prepends to PATH).

QT_VER=6.8.5
QT_ROOT="${QT_ROOT:-$HOME/qt-build}"
QT_SRC="$QT_ROOT/src/qt-everywhere-src-$QT_VER"
QT_HOST="$QT_ROOT/host"             # Qt for the PC (needed as -qt-host-path by the cross build)
# QNX_ARCH picks the target: x86_64 (the QEMU VM, default) or aarch64le (Raspberry Pi 4).
# Each one gets its own build folder and install folder, so both can exist side by side.
QNX_ARCH="${QNX_ARCH:-x86_64}"
case "$QNX_ARCH" in
    x86_64)
        QNX_VARIANT=gcc_ntox86_64
        QNX_TOOL_PREFIX=x86_64-pc-nto-qnx8.0.0-
        QT_QNX="$QT_ROOT/qnx-install"       # Qt for QNX, installed on the PC; copied to the target by 4_deploy_qt.sh
        QT_QNX_BUILD="$QT_ROOT/build-qnx" ;;
    aarch64le)
        QNX_VARIANT=gcc_ntoaarch64le
        QNX_TOOL_PREFIX=aarch64-unknown-nto-qnx8.0.0-
        QT_QNX="$QT_ROOT/qnx-install-aarch64le"
        QT_QNX_BUILD="$QT_ROOT/build-qnx-aarch64le" ;;
    *)
        echo "ERROR: QNX_ARCH must be x86_64 or aarch64le (is: $QNX_ARCH)" >&2
        return 1 2> /dev/null || exit 1 ;;
esac
# Where Qt lives on the target, and the SSH user. The Pi's quick start image does not let root log in over SSH and
# qnxuser cannot write /data, so there it is qnxuser's home folder (as in run_qt.sh, check_env.sh and the launcher).
if [ "$QNX_ARCH" = aarch64le ]; then
    QT_QNX_PREFIX="${QT_QNX_PREFIX:-/data/home/qnxuser/qt6}"
    QNX_USER="${QNX_USER:-qnxuser}"
else
    QT_QNX_PREFIX="${QT_QNX_PREFIX:-/data/qt6}"
    QNX_USER="${QNX_USER:-root}"
fi
JOBS="${JOBS:-6}"                   # parallel compile jobs; 6 worked with 11 GB for WSL, use JOBS=3 if memory runs out

qt_clean_path() {
    PATH="$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd:)"
    export PATH
}

qt_require_ubuntu_2404() {
    if ! grep -q 'VERSION_ID="24.04"' /etc/os-release; then
        echo "ERROR: this is not the Ubuntu 24.04 distro. Open the Ubuntu-24.04 terminal (wsl -d Ubuntu-24.04)." >&2
        return 1
    fi
}

qt_source_sdp() {
    # shellcheck disable=SC1090
    source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null 2>&1
    qt_clean_path
}
