#!/usr/bin/env bash
# Builds the step 17 programs (rt_timer, rt_gap, rt_load, rt_msg, rt_inversion) for QNX and runs the PC unit test.
# Usage: bash build_rt.sh              x86_64 (the VM), output in rt_lab/x86_64/
#        bash build_rt.sh aarch64le    Raspberry Pi 4, output in rt_lab/aarch64le/
# -O2: these programs measure time, so they are built like a product, not like a debug build (-O0).
set -eu
source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null
cd "$(dirname "$0")"
ARCH="${1:-x86_64}"
case "$ARCH" in
    x86_64) V=gcc_ntox86_64 ;;
    aarch64le) V=gcc_ntoaarch64le ;;
    *) echo "ERROR: arch must be x86_64 or aarch64le" >&2; exit 1 ;;
esac
gcc -Wall -Wextra -O2 -o /tmp/test_rt_stats test_rt_stats.c
/tmp/test_rt_stats
mkdir -p "$ARCH"
for p in rt_timer rt_gap rt_load rt_msg rt_inversion; do
    qcc -V$V -g -O2 -Wall -Wextra -o "$ARCH/$p" "$p.c"
done
ls -l "$ARCH"
