#!/usr/bin/env bash
# Installs or updates the projects from the payload.zip inside ClusterConsole.exe (runs in WSL, no sudo).
#
# Usage: bash install_payload.sh <payload.zip as a WSL path>
#
# Fresh PC: unpacks ~/qnx-everywhere-projects and copies qnx_demo and qnx_cluster into the home folder (what
# setup.sh does). Existing folders are first copied to <name>.bak-<date-time>,
# then the new files are written over them: recordings, reports and the build folders stay, edited project files are
# replaced (the backup keeps them). The programs built from the old sources are deleted, so they get built again.
# Each folder gets a list of the files that came from the payload (.payload_files). The next update deletes the ones
# of that list that the new payload no longer has, so a file that was removed or renamed does not stay behind.
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
    [ -e "$1" ] || return 0
    local b="$1.bak-$STAMP" n=1
    while [ -e "$b" ]; do b="$1.bak-$STAMP-$n"; n=$((n + 1)); done   # two updates within one second
    cp -a "$1" "$b"
    echo "backup  $b"
}

# install <payload folder> <destination>: copy, after deleting what the last payload had and this one has not
install_dir() {
    local src="$1" dst="$2" f
    mkdir -p "$dst"
    if [ -f "$dst/.payload_files" ]; then
        while IFS= read -r f; do
            case "$f" in ""|/*|*..*) continue ;; esac            # only plain relative paths of the list
            [ -e "$src/$f" ] || { rm -f "$dst/$f" && echo "removed $dst/$f (not in this version)"; }
        done < "$dst/.payload_files"
    fi
    cp -r "$src/." "$dst/"
    (cd "$src" && find . -type f | sed 's|^\./||' | sort) > "$dst/.payload_files"
}

backup "$HOME/qnx-everywhere-projects"
install_dir "$NEW" "$HOME/qnx-everywhere-projects"
echo "OK      $HOME/qnx-everywhere-projects"

for p in qnx_demo qnx_cluster; do
    d="$HOME/$p"
    backup "$d"
    install_dir "$NEW/$p" "$d"
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
