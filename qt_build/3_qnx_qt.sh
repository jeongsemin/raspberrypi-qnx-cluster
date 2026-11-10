#!/usr/bin/env bash
# Cross-builds Qt 6.8.5 for QNX SDP 8.0. Needs the PC Qt from 2_host_qt.sh.
# Target: QNX_ARCH=x86_64 (VM, default) or QNX_ARCH=aarch64le (Raspberry Pi 4), see qt_env.sh.
# Takes about 70 minutes with 6 jobs (measured on 8 cores, 11 GB RAM). Re-running resumes an interrupted build.
# Only the modules the cluster needs: qtbase, qtshadertools, qtsvg, qtdeclarative.
set -eu
cd "$(dirname "$0")"
# shellcheck disable=SC1091
source ./qt_env.sh
qt_require_ubuntu_2404

[ -d "$QT_SRC" ] || { echo "ERROR: $QT_SRC not found. Run 1_download.sh first." >&2; exit 1; }
[ -x "$QT_HOST/bin/qmake" ] || { echo "ERROR: $QT_HOST not found. Run 2_host_qt.sh first." >&2; exit 1; }
qt_source_sdp
command -v qcc > /dev/null || { echo "ERROR: qcc not found. Check the SDP (step 3-7)." >&2; exit 1; }

# CMake toolchain file for the QNX target (follows Qt's "Building Qt for QNX" page)
TC="$QT_ROOT/qnx-$QNX_ARCH.cmake"
cat > "$TC" << EOF
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_PROCESSOR $QNX_ARCH)
set(arch $QNX_VARIANT)

set(CMAKE_C_COMPILER qcc)
set(CMAKE_C_COMPILER_TARGET \${arch})
set(CMAKE_CXX_COMPILER q++)
set(CMAKE_CXX_COMPILER_TARGET \${arch})

set(CMAKE_FIND_ROOT_PATH \$ENV{QNX_TARGET};\$ENV{QNX_TARGET}/\${CMAKE_SYSTEM_PROCESSOR})
set(CMAKE_SYSROOT \$ENV{QNX_TARGET})

# CMake does not pass these correctly on QNX, so name them explicitly
set(CMAKE_AR      \$ENV{QNX_HOST}/usr/bin/${QNX_TOOL_PREFIX}ar      CACHE FILEPATH "")
set(CMAKE_RANLIB  \$ENV{QNX_HOST}/usr/bin/${QNX_TOOL_PREFIX}ranlib  CACHE FILEPATH "")
set(CMAKE_NM      \$ENV{QNX_HOST}/usr/bin/${QNX_TOOL_PREFIX}nm      CACHE FILEPATH "")
set(CMAKE_STRIP   \$ENV{QNX_HOST}/usr/bin/${QNX_TOOL_PREFIX}strip   CACHE FILEPATH "")
set(CMAKE_OBJCOPY \$ENV{QNX_HOST}/usr/bin/${QNX_TOOL_PREFIX}objcopy CACHE FILEPATH "")
EOF

B="$QT_QNX_BUILD"
mkdir -p "$B"
cd "$B"
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"

if [ ! -f CMakeCache.txt ]; then
    # fontconfig is off (QNX has none): fonts come from QT_QPA_FONTDIR at run time
    "$QT_SRC/configure" -release -opensource -confirm-license -nomake examples -nomake tests \
        -qt-host-path "$QT_HOST" -extprefix "$QT_QNX" -prefix "$QT_QNX_PREFIX" \
        -submodules qtbase,qtshadertools,qtsvg,qtdeclarative \
        -qt-freetype -qt-harfbuzz -qt-libpng -no-feature-fontconfig \
        -- -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_SYSTEM_VERSION=800
fi
if grep -q -E 'D:/|/mnt/' CMakeCache.txt; then
    echo "ERROR: CMakeCache.txt contains Windows paths. Delete $B and run this script again." >&2
    exit 2
fi

cmake --build . --parallel "$JOBS"
cmake --install .

echo "--- platform plugins (libqqnx.so is the one that draws on QNX Screen):"
ls "$QT_QNX/plugins/platforms"
file "$QT_QNX/lib/libQt6Core.so.6.8.5" | cut -c1-120
echo "Qt for QNX ($QNX_ARCH) installed in $QT_QNX"
