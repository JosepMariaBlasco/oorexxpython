#!/usr/bin/env bash
# setup-oorexxunit.sh: fetch ooRexx's own test framework (ooRexxUnit) into
# $ORXPY_WORK/oorexxunit/ (ORXPY_WORK: the directory that contains the clone),
# at a fixed SVN revision.  It is not part of this repository: it is CPL 1.0
# (ooRexx's licence) and is obtained on demand, as ooRexx.
#
#   OOREXXUNIT_REV  SVN revision (default 13331: test/trunk as the ooRexx bridges pin it)
#   OOREXXUNIT_DIR  target directory (default $ORXPY_WORK/oorexxunit)
#
# Files: OOREXXUNIT.CLS (test/trunk/framework), ooTest.frm, worker.rex and
# testOORexx.rex (test/trunk).  Used by tests/run.sh.  Fetched with curl over
# SourceForge's Subversion HTTP interface (the same bytes as `svn export`), so
# no Subversion client is needed.
set -euo pipefail
WORK=${ORXPY_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
REV=${OOREXXUNIT_REV:-13331}
DIR=${OOREXXUNIT_DIR:-$WORK/oorexxunit}
SVN="https://svn.code.sf.net/p/oorexx/code-0/!svn/bc/$REV/test/trunk"
mkdir -p "$DIR"
get() { curl -sSfL --retry 3 -o "$DIR/$(basename "$1")" "$SVN/$1"; }
get framework/OOREXXUNIT.CLS
for f in ooTest.frm worker.rex testOORexx.rex CPLv1.0.txt; do get "$f"; done
echo "ooRexxUnit r$REV in $DIR"
