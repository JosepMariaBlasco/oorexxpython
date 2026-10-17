#!/usr/bin/env bash
# Build the bridge into $BUILD (default $ORXPY_WORK/build; ORXPY_WORK is
# the directory that contains the clone):
#   oorexx.abi3.so    the bridge (Python module + ooRexx package), Stable ABI 3.9
#                     (oorexx.pyd on Windows)
#   librexxpython.so  the ooRexx package loader (librexxpython.dylib on macOS,
#                     rexxpython.dll on Windows)
#   python.cls        the Rexx side
#   boolean.cls       .Boolean (the ooRexx bridges' shared copy; python.cls requires it)
# Python headers: those of the minimum version (3.9), so nothing newer slips in.
# ooRexx: the installation of the `rexx` on the PATH (OOREXX_HOME to choose
# another), which must have include/oorexxapi.h and lib/librexx.* (rexx.lib
# on Windows); an installation outside /usr and /usr/local is also recorded as
# the libraries' run path.  Windows: run from Git Bash with MSVC's cl on the
# PATH (a developer command prompt's environment).
set -euo pipefail
WORK=${ORXPY_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
SRC=$(cd "$(dirname "$0")/../src" && pwd)
. "$(dirname "$0")/platform.sh"
BUILD=${BUILD:-$WORK/build}
export UV_PYTHON_INSTALL_DIR=${UV_PYTHON_INSTALL_DIR:-$(native_path "$WORK/pythons")}
PY39=$(uv python find 3.9)
PYINC=$("$PY39" -c 'import sysconfig; print(sysconfig.get_paths()["include"])')

if [ -z "${OOREXX_HOME:-}" ]; then
  REXX=$(command -v rexx) || { echo "build.sh: no rexx on the PATH (or set OOREXX_HOME)" >&2; exit 2; }
  OOREXX_HOME=$(cd "$(dirname "$(resolve_path "$REXX")")/.." && pwd)
fi
[ -f "$OOREXX_HOME/include/oorexxapi.h" ] || { echo "build.sh: no $OOREXX_HOME/include/oorexxapi.h" >&2; exit 2; }
mkdir -p "$BUILD"
if [ "$OS" = Windows ]; then
  # MSVC; no argument conversion by MSYS: every path is given the Windows way
  export MSYS2_ARG_CONV_EXCL='*'
  PYLIBS=$("$PY39" -c 'import sys, os; print(os.path.join(sys.base_prefix, "libs"))')
  W() { cygpath -w "$1"; }
  OBJ=$(mktemp -d); trap 'rm -rf "$OBJ"' EXIT
  CLFLAGS="-nologo -LD -EHsc -O2 -MD -std:c++17 -W3 -Fo$(W "$OBJ")\\"
  # python3.lib (the Stable ABI's python3.dll) is linked by pyconfig.h's
  # pragma; rexx.dll is delay-loaded (PyInit_oorexx finds it on the PATH)
  "$CXX" $CLFLAGS -DPy_LIMITED_API=0x03090000 -I"$PYINC" -I"$(W "$OOREXX_HOME/include")" \
      "$(W "$SRC/bridge.cpp")" -Fe"$(W "$BUILD/$BRIDGE")" \
      -link -LIBPATH:"$PYLIBS" -LIBPATH:"$(W "$OOREXX_HOME/lib")" rexx.lib -DELAYLOAD:rexx.dll delayimp.lib
  "$CXX" $CLFLAGS -I"$(W "$OOREXX_HOME/include")" "$(W "$SRC/loader.cpp")" -Fe"$(W "$BUILD/$LOADER")"
  rm -f "$BUILD"/*.exp "$BUILD"/*.lib
else
  ORX="-I$OOREXX_HOME/include -L$OOREXX_HOME/lib"
  case $OOREXX_HOME in /usr|/usr/local) ;; *) ORX="$ORX -Wl,-rpath,$OOREXX_HOME/lib" ;; esac
  # installed (install.rex), the bridge lives in ooRexx's lib/oorexxpython/:
  # Python, importing it, must load the ooRexx it is installed in
  BRIDGERPATH='-Wl,-rpath,$ORIGIN/..'
  [ "$OS" = Darwin ] && BRIDGERPATH='-Wl,-rpath,@loader_path/..'
  CXXFLAGS="-O2 -g -fPIC -Wall -Wno-unused-parameter -std=c++17 -fvisibility=hidden"
  if [ "$OS" = Darwin ]; then
    # the bridge leaves the Py* symbols to whichever Python loads it (Linux
    # does that by default); the loader's install name is its bare file name
    BRIDGEFLAGS="-dynamiclib -undefined dynamic_lookup"
    LOADERFLAGS="-dynamiclib -Wl,-install_name,$LOADER"
  else
    BRIDGEFLAGS="-shared"
    LOADERFLAGS="-shared -ldl"
  fi
  "$CXX" $CXXFLAGS $BRIDGEFLAGS $BRIDGERPATH -DPy_LIMITED_API=0x03090000 -I"$PYINC" $ORX \
      "$SRC/bridge.cpp" -o "$BUILD/$BRIDGE" -lrexx -lrexxapi
  "$CXX" $CXXFLAGS -I"$OOREXX_HOME/include" "$SRC/loader.cpp" $LOADERFLAGS -o "$BUILD/$LOADER"
fi
cp "$SRC/python.cls" "$SRC/boolean.cls" "$BUILD/"
echo "built in $BUILD (ooRexx: $OOREXX_HOME)"
