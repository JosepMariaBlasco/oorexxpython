#!/usr/bin/env bash
# run-one.sh EXAMPLE [PYTHON_VERSION]: run one example the way Examples.testGroup does.
set -u
WORK=${ORXPY_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${BUILD:-$WORK/build}
export UV_PYTHON_INSTALL_DIR=${UV_PYTHON_INSTALL_DIR:-$WORK/pythons}
export ORX_PYTHON=${ORX_PYTHON:-$(uv python find "${2:-3.13}" 2>/dev/null)}
export LD_LIBRARY_PATH=$BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} REXX_PATH=$BUILD
cd "${TMPDIR:-/tmp}"
case $1 in
  *.rex) rexx "$HERE/$(basename "$1")" ;;
  *.py)  PYTHONPATH=$BUILD "$ORX_PYTHON" "$HERE/$(basename "$1")" ;;
esac
