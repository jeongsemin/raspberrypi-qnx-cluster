#!/usr/bin/env bash
# Builds qnx_demo for the x86_64 QNX target. Run inside the Ubuntu / WSL terminal.
set -eu
source "$HOME/qnx800/qnxsdp-env.sh" > /dev/null
cd "$(dirname "$0")"
mkdir -p x86_64
qcc -Vgcc_ntox86_64 -g -O0 -Wall -o x86_64/qnx_demo qnx_demo.c
ls -l x86_64
