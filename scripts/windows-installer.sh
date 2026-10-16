#!/usr/bin/env bash
# windows-installer.sh ZIP DOCDIR [DEST]: the Windows installer of the bridge,
# DEST/oorexxpython-VERSION-windows-x64.exe, from the Windows zip that
# package.sh makes (oorexxpython-VERSION-windows-x64.zip: VERSION is taken
# from its name) and the books (DOCDIR/pythonref.pdf, pythoncookbook.pdf:
# build-books.sh).  Needs NSIS 3 (makensis: Windows, or apt install nsis);
# the script is dist/windows/oorexxpython.nsi.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
ZIP=${1:?usage: windows-installer.sh ZIP DOCDIR [DEST]}
DOCDIR=$(cd "${2:?usage: windows-installer.sh ZIP DOCDIR [DEST]}" && pwd)
DEST=${3:-.}
name=$(basename "$ZIP" .zip)
case $name in
  oorexxpython-*-windows-x64) VERSION=${name#oorexxpython-}; VERSION=${VERSION%-windows-x64} ;;
  *) echo "windows-installer.sh: $ZIP is not an oorexxpython-VERSION-windows-x64.zip" >&2; exit 2 ;;
esac
for f in pythonref.pdf pythoncookbook.pdf; do
  [ -f "$DOCDIR/$f" ] || { echo "windows-installer.sh: no $DOCDIR/$f" >&2; exit 2; }
done
MAKENSIS=$(command -v makensis || true)
[ -n "$MAKENSIS" ] || [ ! -x "/c/Program Files (x86)/NSIS/makensis.exe" ] || MAKENSIS="/c/Program Files (x86)/NSIS/makensis.exe"
[ -n "$MAKENSIS" ] || { echo "windows-installer.sh: NSIS 3 (makensis) is needed" >&2; exit 2; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
unzip -q "$ZIP" -d "$TMP"
[ -f "$TMP/oorexxpython/install.rex" ] || { echo "windows-installer.sh: no oorexxpython/install.rex in $ZIP" >&2; exit 2; }
mkdir -p "$DEST"
OUT=$(cd "$DEST" && pwd)/oorexxpython-$VERSION-windows-x64.exe
native() { if command -v cygpath >/dev/null; then cygpath -w "$1"; else echo "$1"; fi; }
# "-" options, not "/": Git Bash would take /DVERSION=... for a path
"$MAKENSIS" -V2 -DVERSION="$VERSION" -DPKGDIR="$(native "$TMP/oorexxpython")" \
  -DDOCDIR="$(native "$DOCDIR")" -DOUTFILE="$(native "$OUT")" \
  "$(native "$ROOT/dist/windows/oorexxpython.nsi")"
echo "$OUT"
