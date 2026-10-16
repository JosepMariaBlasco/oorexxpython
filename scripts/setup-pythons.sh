#!/usr/bin/env bash
# Standalone CPython builds to test the bridge against (Stable ABI: one binary,
# every version from the minimum up).  Seconds.
#   setup-pythons.sh           (PYTHONS="3.9 3.10 ...", UV_PYTHON_INSTALL_DIR)
set -euo pipefail
. "$(dirname "$0")/platform.sh"
WORK=${ORXPY_WORK:-$(cd "$(dirname "$0")/../.." && pwd)}   # the directory that contains the clone
export UV_PYTHON_INSTALL_DIR=${UV_PYTHON_INSTALL_DIR:-$(native_path "$WORK/pythons")}
PYTHONS=${PYTHONS:-3.9 3.10 3.11 3.12 3.13 3.14 3.15}
command -v uv >/dev/null || pip install -q --break-system-packages uv
uv python install --no-bin $PYTHONS 2>&1 | grep -v UV_NATIVE_TLS || true
for v in $PYTHONS; do
  p=$(uv python find "$v" 2>/dev/null)
  printf '%-5s %s  ' "$v" "$p"
  "$p" -c "import sys, sysconfig; print(sys.version.split()[0], 'shared' if sysconfig.get_config_var('Py_ENABLE_SHARED') else 'static', sysconfig.get_config_var('LIBDIR'))"
done
