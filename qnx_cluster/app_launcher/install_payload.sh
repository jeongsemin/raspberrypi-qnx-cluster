#!/usr/bin/env bash
# Installs or updates the projects from the payload.zip inside ClusterConsole.exe (runs in WSL, no sudo).
#
# Usage: bash install_payload.sh <payload.zip as a WSL path>
#
# Fresh PC: unpacks ~/qnx-everywhere-projects and copies qnx_demo and qnx_cluster into the home folder (what
# setup.sh does, step 0-3 and 7-4 of the tutorial). Existing folders are first copied to <name>.bak-<date-time>,
# then the new files are written over them: recordings, reports and the build folders stay, edited project files are
# replaced (the backup keeps them). The programs built from the old sources are deleted, so they get built again.
set -eu
ZIP="${1:?usage: bash install_payload.sh <payload.zip>}"
STAMP="$(date +%Y%m%d-%H%M%S)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

command -v python3 > /dev/null || { echo "ERROR: python3 is missing (sudo apt install -y python3)" >&2; exit 1; }
python3 -m zipfile -e "$ZIP" "$TMP"
NEW="$TMP/qnx-everywhere-projects"
[ -f "$NEW/setup.sh" ] || { echo "ERROR: the payload has no qnx-everywhere-projects/setup.sh" >&2; exit 1; }

backup() {
    if [ -e "$1" ]; then
        cp -a "$1" "$1.bak-$STAMP"
        echo "backup  $1.bak-$STAMP"
    fi
}

backup "$HOME/qnx-everywhere-projects"
mkdir -p "$HOME/qnx-everywhere-projects"
cp -r "$NEW/." "$HOME/qnx-everywhere-projects/"
echo "OK      $HOME/qnx-everywhere-projects"

for p in qnx_demo qnx_cluster; do
    d="$HOME/$p"
    backup "$d"
    mkdir -p "$d"
    cp -r "$NEW/$p/." "$d/"
    # the same as setup.sh: fill in the home folder, make the build script runnable
    # (the placeholder is put together here, so this script does not match and rewrite itself)
    ph="__HO""ME__"
    grep -rl "$ph" "$d" 2> /dev/null | while read -r f; do sed -i "s|$ph|$HOME|g" "$f"; done
    chmod +x "$d/build.sh"
    # the programs built from the old sources must not stay: the check then offers to build them again
    if [ "$p" = qnx_cluster ]; then
        rm -f "$d"/x86_64/cluster_* "$d"/x86_64/speed_source "$d"/aarch64le/cluster_* "$d"/aarch64le/speed_source \
              "$d"/qt_cluster/build/qt_cluster "$d"/qt_cluster/build-aarch64le/qt_cluster
    fi
    echo "OK      $d"
done
echo "done"
