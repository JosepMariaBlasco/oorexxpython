#!/usr/bin/env bash
# run-one.sh SAMPLE [PYTHON_VERSION]: run one sample the way Samples.testGroup does.
set -u
WORK=${ORXPY_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../scripts/platform.sh"
BUILD=${BUILD:-$WORK/build}
export UV_PYTHON_INSTALL_DIR=${UV_PYTHON_INSTALL_DIR:-$(native_path "$WORK/pythons")}
export ORX_PYTHON=${ORX_PYTHON:-$(uv python find "${2:-3.13}" 2>/dev/null)}
export $LIBVAR="$BUILD${!LIBVAR:+:${!LIBVAR}}" REXX_PATH=$(native_path "$BUILD")
cd "${TMPDIR:-/tmp}"
case $1 in
  *.rex) rexx "$HERE/$(basename "$1")" ;;
  *.py)  PYTHONPATH=$(native_path "$BUILD") "$ORX_PYTHON" "$HERE/$(basename "$1")" ;;
esac
