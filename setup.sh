#!/usr/bin/env bash
# Copies the projects (qnx_demo, qnx_cluster) into your WSL home directory and fills in the
# __HOME__ placeholders with your real home path.
#
# Usage (run inside the Ubuntu / WSL terminal, from this folder):
#   bash setup.sh            # installs into $HOME
#   bash setup.sh /some/dir  # installs into another directory
#
# Existing project folders are never overwritten.
set -eu

SRC="$(cd "$(dirname "$0")" && pwd)"
DEST="${1:-$HOME}"
PROJECTS="qnx_demo qnx_cluster"

for p in $PROJECTS; do
    if [ ! -d "$SRC/$p" ]; then
        echo "ERROR: $SRC/$p not found. Run this script from the folder that contains $p." >&2
        exit 1
    fi
done

mkdir -p "$DEST"
for p in $PROJECTS; do
    if [ -e "$DEST/$p" ]; then
        echo "SKIP  $DEST/$p already exists (not overwritten)"
        continue
    fi
    cp -r "$SRC/$p" "$DEST/$p"
    # Windows checkouts may have added CRLF line endings; remove them.
    find "$DEST/$p" -type f \( -name '*.sh' -o -name '*.c' -o -name '*.h' -o -name '*.json' \
        -o -name '*.code-workspace' \) -exec sed -i 's/\r$//' {} +
    # Replace the placeholder with the real home directory.
    grep -rl "__HOME__" "$DEST/$p" | while read -r f; do
        sed -i "s|__HOME__|$HOME|g" "$f"
    done
    chmod +x "$DEST/$p/build.sh"
    echo "OK    $DEST/$p"
done

echo
echo "Next: open the project in VS Code (WSL window):"
echo "  File > Open Folder > $DEST/qnx_demo"
