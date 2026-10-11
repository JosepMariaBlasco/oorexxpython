#!/usr/bin/env bash
# package.sh [DEST]: the bridge for this platform as a zip of binaries, for
# people who want to use it without building it.  Run after build.sh.
#
#   oorexxpython-VERSION-PLATFORM.zip, one folder oorexxpython/: the bridge
#   (oorexx.abi3.so / oorexx.pyd), the ooRexx package (librexxpython.so /
#   .dylib, rexxpython.dll), both stripped, python.cls, README.md (dist/README.md: how to
#   install), check.rex and check.py, the examples (with their md2slides deck,
#   examples/slides/) and the licence.
#
# VERSION: ORXPY_VERSION, else YYYYMMDD-COMMIT.  PLATFORM: linux-x64,
# macos-arm64, windows-x64...  The folder is checked before it is zipped:
# check.rex and check.py run from outside it, finding everything through the
# variables the README tells users to set.  ORX_PYTHON: the Python for that
# (default: uv's 3.13).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
. "$HERE/platform.sh"
WORK=${ORXPY_WORK:-$(cd "$ROOT/.." && pwd)}
BUILD=${BUILD:-$WORK/build}
DEST=${1:-.}
export UV_PYTHON_INSTALL_DIR=${UV_PYTHON_INSTALL_DIR:-$(native_path "$WORK/pythons")}
PY=${ORX_PYTHON:-$(uv python find 3.13)}

case $(uname -m) in arm64|aarch64) ARCH=arm64 ;; *) ARCH=x64 ;; esac
case $OS in Darwin) PLAT=macos-$ARCH ;; Windows) PLAT=windows-$ARCH ;; *) PLAT=linux-$ARCH ;; esac
VERSION=${ORXPY_VERSION:-$(date +%Y%m%d)-$(git -C "$ROOT" rev-parse --short HEAD)}
NAME=oorexxpython-$VERSION-$PLAT

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
STAGE=$TMP/oorexxpython
mkdir -p "$STAGE/examples/slides" "$TMP/elsewhere"
for f in "$BRIDGE" "$LOADER" python.cls; do
  [ -f "$BUILD/$f" ] || { echo "package.sh: no $BUILD/$f (run build.sh)" >&2; exit 2; }
  cp "$BUILD/$f" "$STAGE/"
done
# build.sh keeps debug information (-g); users get the binaries without it
# (and without local symbols): a tenth of the size.  On Windows there is
# nothing to strip (build.sh asks cl for no debug information).
case $OS in
  Linux)  strip --strip-unneeded "$STAGE/$BRIDGE" "$STAGE/$LOADER" ;;
  Darwin) strip -x "$STAGE/$BRIDGE" "$STAGE/$LOADER" ;;
esac
cp "$ROOT/dist/README.md" "$ROOT/dist/check.rex" "$ROOT/dist/check.py" "$ROOT/LICENSE" "$STAGE/"
cp "$ROOT"/examples/*.rex "$ROOT"/examples/*.py "$ROOT/examples/README.md" "$STAGE/examples/"
cp "$ROOT"/examples/slides/examples.md "$ROOT"/examples/slides/skin-default.css "$STAGE/examples/slides/"

# the check, as a user would run it: from another directory
DIR=$(native_path "$STAGE")
(cd "$TMP/elsewhere" &&
  env "$LIBVAR=$STAGE${!LIBVAR:+:${!LIBVAR}}" REXX_PATH="$DIR" ORX_PYTHON="$PY" rexx "$DIR/check.rex" &&
  env PYTHONPATH="$DIR" "$PY" "$DIR/check.py")

mkdir -p "$DEST"
ZIP=$(cd "$DEST" && pwd)/$NAME.zip
rm -f "$ZIP"
(cd "$TMP" && "$PY" -m zipfile -c "$(native_path "$ZIP")" oorexxpython)
echo "$ZIP"
