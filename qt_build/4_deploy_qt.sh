#!/usr/bin/env bash
# Copies the QNX build of Qt (libraries, plugins, QML modules; about 120 MB) to /data/qt6 on the target.
# For the Raspberry Pi: QNX_ARCH=aarch64le bash 4_deploy_qt.sh <Pi IP>  (QNX_USER, QT_QNX_PREFIX change user and folder)
# Headers, CMake files and static libraries stay on the PC.
#
# Usage: bash 4_deploy_qt.sh <target IP>
set -eu
cd "$(dirname "$0")"
# shellcheck disable=SC1091
source ./qt_env.sh

IP="${1:?usage: bash 4_deploy_qt.sh <target IP>}"
T="$QNX_USER@$IP"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=8"

[ -f "$QT_QNX/plugins/platforms/libqqnx.so" ] || { echo "ERROR: $QT_QNX has no Qt for QNX. Run 3_qnx_qt.sh first." >&2; exit 1; }
# shellcheck disable=SC2086
ssh $SSH_OPTS "$T" "mkdir -p $QT_QNX_PREFIX"
cd "$QT_QNX"
tar cf - --exclude='./include' --exclude='lib/cmake' --exclude='lib/pkgconfig' \
    --exclude='*.a' --exclude='*.prl' --exclude='*.debug' lib plugins qml \
    | ssh $SSH_OPTS "$T" "cd $QT_QNX_PREFIX && tar xf -"
ssh $SSH_OPTS "$T" "du -sk $QT_QNX_PREFIX; ls $QT_QNX_PREFIX/plugins/platforms"
echo "Qt copied to $T:$QT_QNX_PREFIX"
