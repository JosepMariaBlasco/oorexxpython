#!/usr/bin/env bash
# samples-html.sh: builds samples/slides/samples.html, the samples' slide
# deck as one HTML file (the release zips carry it, not its sources), with
# md2slides (the Rexx Parser's, on the PATH) and Pandoc.  Run it after
# scripts/samples-deck.rex, and commit both.
#
# Needs the Rexx Parser of 12/10/2026 or later: older md2slides wrote the
# deck's absolute path into the page.
#
# Licensed under the Apache License, Version 2.0.
set -euo pipefail
cd "$(dirname "$0")/../samples/slides"
# the Rexx Parser's bin on the PATH: md2slides.rex there
found=; IFS=: read -ra dirs <<< "$PATH"
for d in "${dirs[@]}"; do [ -f "$d/md2slides.rex" ] && found=1; done
[ -n "$found" ] || { echo "samples-html.sh: put the Rexx Parser's bin on the PATH (md2slides.rex)" >&2; exit 1; }
rexx md2slides samples.md
if grep -q 'data-footer-file-path=' samples.html; then
  echo "samples-html.sh: this md2slides writes the deck's path into the page: use a newer Rexx Parser" >&2
  exit 1
fi
echo "samples-html.sh: wrote samples/slides/samples.html"
