# shellcheck shell=bash
# What differs between Linux and macOS, for the build and test scripts
# (sourced, not run: `. "$(dirname "$0")/../scripts/platform.sh"`). Sets:
#
#   OS        Linux | Darwin
#   SOEXT     so | dylib: an ooRexx external library is lib<name>.$SOEXT
#   LIBVAR    LD_LIBRARY_PATH | DYLD_LIBRARY_PATH: where ooRexx finds
#             external libraries by name (macOS drops DYLD_* variables when
#             it starts a protected program such as /bin/sh, so a command run
#             through a shell must set it itself)
#   CXX       g++ | c++
#
# and the functions resolve_path PATH (symbolic links followed) and timeout
# (coreutils' timeout, else Homebrew's gtimeout, else none).

OS=$(uname -s)

resolve_path() {
  local p=$1 t
  while [ -L "$p" ]; do
    t=$(readlink "$p")
    case $t in /*) p=$t ;; *) p=$(dirname "$p")/$t ;; esac
  done
  echo "$(cd "$(dirname "$p")" && pwd)/$(basename "$p")"
}

if [ "$OS" = Darwin ]; then
  SOEXT=dylib
  LIBVAR=DYLD_LIBRARY_PATH
  CXX=${CXX:-c++}
else
  SOEXT=so
  LIBVAR=LD_LIBRARY_PATH
  CXX=${CXX:-g++}
fi

if ! command -v timeout >/dev/null 2>&1; then
  if command -v gtimeout >/dev/null 2>&1; then timeout() { gtimeout "$@"; }
  else timeout() { while [ "${1#-}" != "$1" ]; do shift 2; done; shift; "$@"; }
  fi
fi
