#!/usr/bin/env bash
# Run the tests against every Python (PYTHONS="3.9 3.13" to choose).  The same
# build is used for all of them: that is the Stable ABI promise.
#   Rexx starts:   tests/rexx/*.testGroup  (ooRexxUnit; scripts/setup-oorexxunit.sh)
#   Python starts: tests/python/t*.py
# Each test file runs in its own process, under a timeout (a deadlock is a
# failure).  TESTS="Threads.testGroup t10_interpreter.py" to choose; TIMEOUT=
# seconds per file (default 120); logs in OUT (default /tmp/orxpy-tests).
set -uo pipefail
WORK=${ORXPY_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../scripts/platform.sh"
BUILD=${BUILD:-$WORK/build}
UNIT=${OOREXXUNIT_DIR:-$WORK/oorexxunit}
export UV_PYTHON_INSTALL_DIR=${UV_PYTHON_INSTALL_DIR:-$WORK/pythons}
PYTHONS=${PYTHONS:-3.9 3.10 3.11 3.12 3.13}
TESTS=${TESTS:-$(cd "$HERE/rexx" && ls *.testGroup; cd "$HERE/python" && ls t*.py)}
[ -f "$UNIT/testOORexx.rex" ] || { echo "ooRexxUnit missing: run scripts/setup-oorexxunit.sh"; exit 2; }
export ORXPY_BUILD=$BUILD
export $LIBVAR="$BUILD${!LIBVAR:+:${!LIBVAR}}"
export REXX_PATH=$BUILD:$UNIT:$HERE/rexx
OUT=${OUT:-/tmp/orxpy-tests}; mkdir -p "$OUT"
status=0
for v in $PYTHONS; do
  export ORX_PYTHON=$(uv python find "$v" 2>/dev/null)
  for t in $TESTS; do
    log="$OUT/${t%.*}-$v.log"
    case $t in
      *.testGroup)
        (cd "$OUT" && timeout -s KILL ${TIMEOUT:-120} rexx "$UNIT/testOORexx.rex" -f "$HERE/rexx/$t" -O -U) > "$log" 2>&1
        rc=$?
        n() { grep -E "^$1:" "$log" | tail -1 | awk '{print $NF}'; }
        summary="$(n 'Tests ran') tests, $(n Assertions) assertions, $(n Failures) failures, $(n Errors) errors"
        # testOORexx's exit code is non-zero on failures; also require that tests ran
        [ $rc -eq 0 ] && [ "$(n 'Tests ran')" != "" ] && [ "$(n 'Tests ran')" != 0 ] || rc=${rc/#0/1}
        ;;
      *.py)
        (cd "$OUT" && PYTHONPATH=$BUILD timeout -s KILL ${TIMEOUT:-120} "$ORX_PYTHON" -u "$HERE/python/$t") > "$log" 2>&1
        rc=$?
        summary=$(grep -E '^[0-9]+ checks' "$log" | tail -1)
        ;;
    esac
    if [ $rc -eq 0 ]; then echo "PASS $v $t: $summary"
    else
      echo "FAIL $v $t (rc $rc): ${summary:-see $log}"
      grep -E -A8 '^\[(failure|error)\]|^not ok' "$log" | head -40
      tail -3 "$log"; status=1
    fi
  done
done
exit $status
