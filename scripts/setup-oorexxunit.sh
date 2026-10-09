#!/usr/bin/env bash
# setup-oorexxunit.sh: fetch ooRexx's own test framework (ooRexxUnit) into
# $ORXPY_WORK/oorexxunit/ (ORXPY_WORK: the directory that contains the clone),
# at a fixed SVN revision.  It is not part of this repository: it is CPL 1.0
# (ooRexx's licence) and is obtained on demand, as ooRexx.
#
#   OOREXXUNIT_REV  SVN revision (default 13254, the ooRexx we build against)
#   OOREXXUNIT_DIR  target directory (default $ORXPY_WORK/oorexxunit)
#
# Files: OOREXXUNIT.CLS (test/trunk/framework), ooTest.frm, worker.rex and
# testOORexx.rex (test/trunk).  Used by tests/run.sh.
set -euo pipefail
WORK=${ORXPY_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
REV=${OOREXXUNIT_REV:-13254}
DIR=${OOREXXUNIT_DIR:-$WORK/oorexxunit}
SVN=https://svn.code.sf.net/p/oorexx/code-0/test/trunk
if ! command -v svn >/dev/null; then
  apt-get install -y -q subversion >/dev/null 2>&1 || { apt-get update -qq && apt-get install -y -q subversion >/dev/null; }
fi
mkdir -p "$DIR"
svn export -q --force -r "$REV" "$SVN/framework/OOREXXUNIT.CLS" "$DIR/OOREXXUNIT.CLS"
for f in ooTest.frm worker.rex testOORexx.rex; do
  svn export -q --force -r "$REV" "$SVN/$f" "$DIR/$f"
done
svn export -q --force -r "$REV" "$SVN/CPLv1.0.txt" "$DIR/CPLv1.0.txt"
echo "ooRexxUnit r$REV in $DIR"
