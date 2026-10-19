#!/usr/bin/env bash
# Install ooRexx from SourceForge's own Ubuntu 24.04 package, at the revision
# the CI uses (OOREXX_REV in .github/workflows/build.yml), so that a local
# build and the CI never test against different ooRexx revisions.  Installs
# into /usr/local (bin/rexx, lib/librexx.so, include/oorexxapi.h).  Needs
# root (dpkg).  Idempotent: does nothing if that revision is installed.
#   setup-oorexx-deb.sh            (OOREXX_REV to choose another revision)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
REV=${OOREXX_REV:-$(sed -n 's/^ *OOREXX_REV: *"\([0-9]*\)".*/\1/p' "$HERE/.github/workflows/build.yml" | head -1)}
[ -n "$REV" ] || { echo "setup-oorexx-deb.sh: OOREXX_REV not found in the workflow" >&2; exit 2; }
if command -v rexx >/dev/null && rexx -v 2>/dev/null | grep -q "r$REV\$"; then
    echo "ooRexx r$REV already installed: $(command -v rexx)"
    exit 0
fi
URL="https://downloads.sourceforge.net/project/oorexx/oorexx/5.3.0beta/oorexx-5.3.0-$REV.ubuntu2404.x86_64.deb"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "== ooRexx r$REV from $URL"
curl -sSfL -o "$TMP/oorexx.deb" "$URL"
dpkg -i "$TMP/oorexx.deb" >/dev/null 2>&1 || { apt-get update -qq && apt-get install -f -y -qq >/dev/null; }
ldconfig                                  # the package does not: Python must find librexx.so.4
rexx -v | head -1
