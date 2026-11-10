#!/usr/bin/env bash
# Builds cluster_display, speed_source, cluster_supervisor, cluster_gateway and cluster_diag for QNX.
# Run inside the Ubuntu / WSL terminal.
# Usage: bash build.sh              x86_64 (the VM), output in x86_64/
#        bash build.sh aarch64le    Raspberry Pi 4, output in aarch64le/
set -eu
source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null
cd "$(dirname "$0")"
ARCH="${1:-x86_64}"
case "$ARCH" in
    x86_64) V=gcc_ntox86_64 ;;
    aarch64le) V=gcc_ntoaarch64le ;;
    *) echo "ERROR: arch must be x86_64 or aarch64le" >&2; exit 1 ;;
esac
mkdir -p "$ARCH"
qcc -V$V -g -O0 -Wall -o "$ARCH/cluster_display" cluster_display.c -lscreen -lm
qcc -V$V -g -O0 -Wall -o "$ARCH/speed_source" speed_source.c -lm
qcc -V$V -g -O0 -Wall -o "$ARCH/cluster_supervisor" cluster_supervisor.c
qcc -V$V -g -O0 -Wall -o "$ARCH/cluster_gateway" cluster_gateway.c -lsocket -lm
qcc -V$V -g -O0 -Wall -o "$ARCH/cluster_diag" cluster_diag.c -lsocket
ls -l "$ARCH"
