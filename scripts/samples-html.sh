#!/usr/bin/env bash
# samples-html.sh: builds samples/slides/samples.html, the samples' slide
# deck as one HTML file (the release zips carry it, not its sources), with
# md2slides (the Rexx Parser's, on the PATH) and Pandoc.  Run it after
# scripts/samples-deck.rex, and commit both.
#
# md2slides writes the deck's absolute path into the page (for footer
# fields this skin does not use); it is replaced by the path in the
# repository, so the page says nothing about the machine that built it.
#
# Licensed under the Apache License, Version 2.0.
set -euo pipefail
cd "$(dirname "$0")/../samples/slides"
# the Rexx Parser's bin on the PATH: md2slides.rex there
found=; IFS=: read -ra dirs <<< "$PATH"
for d in "${dirs[@]}"; do [ -f "$d/md2slides.rex" ] && found=1; done
[ -n "$found" ] || { echo "samples-html.sh: put the Rexx Parser's bin on the PATH (md2slides.rex)" >&2; exit 1; }
rexx md2slides samples.md
sed -i.bak -E 's#data-footer-file-path="[^"]*"#data-footer-file-path="samples/slides"#; s#data-footer-file-name-full="[^"]*"#data-footer-file-name-full="samples/slides/samples.md"#' samples.html
rm -f samples.html.bak
echo "samples-html.sh: wrote samples/slides/samples.html"
