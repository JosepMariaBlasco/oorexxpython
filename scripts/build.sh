#!/usr/bin/env bash
# Build the bridge into $BUILD (default $ORXPY_WORK/build; ORXPY_WORK is
# the directory that contains the clone):
#   oorexx.abi3.so    the bridge (Python module + ooRexx package), Stable ABI 3.9
#   librexxpython.so  the ooRexx package loader (librexxpython.dylib on macOS)
#   python.cls        the Rexx side
# Python headers: those of the minimum version (3.9), so nothing newer slips in.
# ooRexx: the installation of the `rexx` on the PATH (OOREXX_HOME to choose
# another), which must have include/oorexxapi.h and lib/librexx.*; an
# installation outside /usr and /usr/local is also recorded as the libraries'
# run path.
set -euo pipefail
WORK=${ORXPY_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
SRC=$(cd "$(dirname "$0")/../src" && pwd)
. "$(dirname "$0")/platform.sh"
BUILD=${BUILD:-$WORK/build}
export UV_PYTHON_INSTALL_DIR=${UV_PYTHON_INSTALL_DIR:-$WORK/pythons}
PY39=$(uv python find 3.9)
PYINC=$("$PY39" -c 'import sysconfig; print(sysconfig.get_paths()["include"])')

if [ -z "${OOREXX_HOME:-}" ]; then
  REXX=$(command -v rexx) || { echo "build.sh: no rexx on the PATH (or set OOREXX_HOME)" >&2; exit 2; }
  OOREXX_HOME=$(cd "$(dirname "$(resolve_path "$REXX")")/.." && pwd)
fi
[ -f "$OOREXX_HOME/include/oorexxapi.h" ] || { echo "build.sh: no $OOREXX_HOME/include/oorexxapi.h" >&2; exit 2; }
ORX="-I$OOREXX_HOME/include -L$OOREXX_HOME/lib"
case $OOREXX_HOME in /usr|/usr/local) ;; *) ORX="$ORX -Wl,-rpath,$OOREXX_HOME/lib" ;; esac

mkdir -p "$BUILD"
CXXFLAGS="-O2 -g -fPIC -Wall -Wno-unused-parameter -std=c++17 -fvisibility=hidden"
if [ "$OS" = Darwin ]; then
  # the bridge leaves the Py* symbols to whichever Python loads it (Linux
  # does that by default); the loader's install name is its bare file name
  BRIDGE="-dynamiclib -undefined dynamic_lookup"
  LOADER="-dynamiclib -Wl,-install_name,librexxpython.dylib"
else
  BRIDGE="-shared"
  LOADER="-shared -ldl"
fi
"$CXX" $CXXFLAGS $BRIDGE -DPy_LIMITED_API=0x03090000 -I"$PYINC" $ORX \
    "$SRC/bridge.cpp" -o "$BUILD/oorexx.abi3.so" -lrexx -lrexxapi
"$CXX" $CXXFLAGS -I"$OOREXX_HOME/include" "$SRC/loader.cpp" $LOADER -o "$BUILD/librexxpython.$SOEXT"
cp "$SRC/python.cls" "$BUILD/"
echo "built in $BUILD (ooRexx: $OOREXX_HOME)"
