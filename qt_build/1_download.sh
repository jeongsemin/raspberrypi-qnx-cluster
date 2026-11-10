#!/usr/bin/env bash
# Downloads the Qt 6.8.5 sources (about 1 GB) and checks the md5 sum, then unpacks them.
# Run inside the Ubuntu-24.04 terminal.
set -eu
cd "$(dirname "$0")"
# shellcheck disable=SC1091
source ./qt_env.sh
qt_require_ubuntu_2404

BASE="https://download.qt.io/archive/qt/6.8/$QT_VER/single"
F="qt-everywhere-opensource-src-$QT_VER.tar.xz"
D="$QT_ROOT/src"
mkdir -p "$D"
cd "$D"
df -h "$D" | tail -n 1

curl -fL --retry 3 --retry-delay 5 -sS -o md5sums.txt "$BASE/md5sums.txt"
curl -fL --retry 3 --retry-delay 5 -sS -C - -o "$F" "$BASE/$F"
ls -l "$F"

# md5sums.txt lists several files; check only ours
grep " $F\$" md5sums.txt | md5sum -c -

if [ -d "$QT_SRC" ]; then
    echo "already unpacked: $QT_SRC"
else
    tar xf "$F"
    echo "unpacked: $QT_SRC"
fi
