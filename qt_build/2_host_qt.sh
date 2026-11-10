#!/usr/bin/env bash
# Builds Qt 6.8.5 for the PC (Linux). The QNX build in step 3 needs it for tools such as moc, rcc
# and the QML compiler. Only the modules the cluster uses are built.
# Needs about 15 GB of disk and takes from tens of minutes to over an hour (not timed exactly).
# An interrupted build is resumed when you run the script again.
set -eu
cd "$(dirname "$0")"
# shellcheck disable=SC1091
source ./qt_env.sh
qt_require_ubuntu_2404
qt_clean_path

[ -d "$QT_SRC" ] || { echo "ERROR: $QT_SRC not found. Run 1_download.sh first." >&2; exit 1; }

B="$QT_ROOT/build-host"
mkdir -p "$B"
cd "$B"
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"

if [ ! -f CMakeCache.txt ]; then
    "$QT_SRC/configure" -prefix "$QT_HOST" -release -opensource -confirm-license \
        -nomake examples -nomake tests \
        -submodules qtbase,qtshadertools,qtsvg,qtdeclarative -no-feature-xcb
fi

# a cache that points at the Windows drives means the PATH was not clean: stop before the long build
if grep -q -E 'D:/|/mnt/' CMakeCache.txt; then
    echo "ERROR: CMakeCache.txt contains Windows paths. Delete $B and run this script again." >&2
    exit 2
fi

cmake --build . --parallel "$JOBS"
cmake --install .
ls "$QT_HOST/bin" | head -n 5
echo "host Qt installed in $QT_HOST"
