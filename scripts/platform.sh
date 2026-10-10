# shellcheck shell=bash
# What differs between Linux, macOS and Windows (Git Bash, with MSVC's cl on
# the PATH), for the build and test scripts (sourced, not run:
# `. "$(dirname "$0")/../scripts/platform.sh"`). Sets:
#
#   OS        Linux | Darwin | Windows
#   LOADER    the ooRexx package's file: librexxpython.so | librexxpython.dylib
#             | rexxpython.dll (ooRexx loads lib<name>.so/.dylib, <name>.dll)
#   BRIDGE    the bridge's file: oorexx.abi3.so | oorexx.pyd
#   LIBVAR    LD_LIBRARY_PATH | DYLD_LIBRARY_PATH | PATH: where ooRexx finds
#             external libraries by name (macOS drops DYLD_* variables when
#             it starts a protected program such as /bin/sh, so a command run
#             through a shell must set it itself)
#   CXX       g++ | c++ | cl
#
# and the functions resolve_path PATH (symbolic links followed), native_path
# PATH and native_path_list PATH... (as the platform's programs want them:
# C:\x and ";" on Windows) and timeout (coreutils' timeout, else Homebrew's
# gtimeout, else none).

OS=$(uname -s)
case $OS in MINGW*|MSYS*|CYGWIN*) OS=Windows ;; esac

resolve_path() {
  local p=$1 t
  while [ -L "$p" ]; do
    t=$(readlink "$p")
    case $t in /*) p=$t ;; *) p=$(dirname "$p")/$t ;; esac
  done
  echo "$(cd "$(dirname "$p")" && pwd)/$(basename "$p")"
}

if [ "$OS" = Windows ]; then
  native_path() { cygpath -w "$1"; }
  native_path_list() { local IFS=:; cygpath -w -p "$*"; }
else
  native_path() { echo "$1"; }
  native_path_list() { local IFS=:; echo "$*"; }
fi

case $OS in
  Darwin)  LOADER=librexxpython.dylib BRIDGE=oorexx.abi3.so LIBVAR=DYLD_LIBRARY_PATH CXX=${CXX:-c++} ;;
  Windows) LOADER=rexxpython.dll      BRIDGE=oorexx.pyd     LIBVAR=PATH              CXX=${CXX:-cl} ;;
  *)       LOADER=librexxpython.so    BRIDGE=oorexx.abi3.so LIBVAR=LD_LIBRARY_PATH   CXX=${CXX:-g++} ;;
esac

if ! command -v timeout >/dev/null 2>&1; then
  if command -v gtimeout >/dev/null 2>&1; then timeout() { gtimeout "$@"; }
  else timeout() { while [ "${1#-}" != "$1" ]; do shift 2; done; shift; "$@"; }
  fi
fi
