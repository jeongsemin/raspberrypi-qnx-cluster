#!/usr/bin/env bash
# Cross-builds the Qt cluster (qt_cluster) for QNX with the Qt from ../../qt_build.
# Run inside the Ubuntu-24.04 terminal.
# Usage: bash build_qt.sh              x86_64 (the VM), result build/qt_cluster
#        bash build_qt.sh aarch64le    Raspberry Pi 4, result build-aarch64le/qt_cluster
set -eu
cd "$(dirname "$0")"

QT_ROOT="${QT_ROOT:-$HOME/qt-build}"
ARCH="${1:-x86_64}"
case "$ARCH" in
    x86_64) QT_QNX="$QT_ROOT/qnx-install"; B=build ;;
    aarch64le) QT_QNX="$QT_ROOT/qnx-install-aarch64le"; B=build-aarch64le ;;
    *) echo "ERROR: arch must be x86_64 or aarch64le" >&2; exit 1 ;;
esac
[ -x "$QT_QNX/bin/qt-cmake" ] || { echo "ERROR: Qt for QNX not found in $QT_QNX. Do steps 12-3 to 12-5 first (QNX_ARCH=$ARCH for 3_qnx_qt.sh)." >&2; exit 1; }

source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null
# WSL adds the Windows PATH; CMake must not search it (see qt_build/qt_env.sh)
PATH="$(echo "$PATH" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd:)"
export PATH

"$QT_QNX/bin/qt-cmake" -S . -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$B"
ls -l "$B/qt_cluster"
