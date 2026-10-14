#!/usr/bin/env bash
# build-books.sh: builds the documentation books in docs/books (DocBook 4.5,
# the format of the ooRexx books) as PDFs, with ooRexx's own documentation
# tools: bldoc_orx's hldoc2pdf, which highlights the Rexx listings with the
# Rexx Parser.
#
#   scripts/build-books.sh                  # every book -> $OUT/<book>.pdf
#   BOOKS=pythonref scripts/build-books.sh  # one of them
#   DIAGRAMS=1 scripts/build-books.sh       # also regenerate the syntax
#                                           # diagrams (images/*.svg) from
#                                           # their .ebnf, in the clone
#
# Needs:
#   DOCS_SVN    a checkout of ooRexx's docs/trunk (default
#               $ORXPY_WORK/docSVN): its tools/bldoc_orx, its oorexx/en-US
#               (the books' Common_Content) and, for DIAGRAMS=1,
#               tools/RailRoadDiagrams (rr.war);
#                 svn co https://svn.code.sf.net/p/oorexx/code-0/docs/trunk docSVN
#   REXX_PARSER the Rexx Parser (default $ORXPY_WORK/rexx-parser), from
#               https://rexx.epbcn.com/rexx-parser/
#   rexx, xsltproc, the DocBook 4.5 DTD and the DocBook XSL stylesheets
#   (Debian/Ubuntu: docbook-xml docbook-xsl), Apache FOP (fop) and Java;
#   fonts-liberation for the fonts the stylesheets ask for.
#
# The books are built in a scratch copy ($WORK/books-build), where
# scripts/book-samples.rex puts the samples into them: the docs tree
# that bldoc_orx expects (oorexx/ and the books side by side) and a copy of
# bldoc_orx, which writes its work files into its own directory.
#
# Licensed under the Apache License, Version 2.0.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${ORXPY_WORK:-$(cd "$REPO/.." && pwd)}
DOCS_SVN=${DOCS_SVN:-$WORK/docSVN}
REXX_PARSER=${REXX_PARSER:-$WORK/rexx-parser}
OUT=${OUT:-$WORK/books}
STAGE=${STAGE:-$WORK/books-build}
BOOKS=${BOOKS:-"pythonref pythoncookbook"}

for need in "$DOCS_SVN/tools/bldoc_orx/hldocprep.rex" "$DOCS_SVN/oorexx/en-US/Legal_Notice.xml" \
            "$REXX_PARSER/bin/Rexx.Parser.cls"; do
  [ -f "$need" ] || { echo "build-books.sh: $need not found (see the comments at the top)" >&2; exit 2; }
done
for tool in rexx xsltproc fop java; do
  command -v $tool >/dev/null || { echo "build-books.sh: $tool not on the PATH" >&2; exit 2; }
done
export PATH="$REXX_PARSER/bin:$PATH"

if [ "${DIAGRAMS:-0}" = 1 ]; then
  RR=$DOCS_SVN/tools/RailRoadDiagrams
  for ebnf in "$REPO"/docs/books/*/en-US/images/*.ebnf; do
    [ -f "$ebnf" ] || continue
    rm -f "$(dirname "$ebnf")"/*.svg
    REXX_PATH="$RR${REXX_PATH:+:$REXX_PATH}" rexx "$REXX_PARSER/bin/ebnf2svg.rex" \
      --rr "$RR/rr-1.67-java8/rr.war" "${ebnf%.ebnf}" >/dev/null
    echo "build-books.sh: diagrams of $(basename "$ebnf") regenerated"
  done
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/docs" "$OUT"
cp -r "$DOCS_SVN/oorexx" "$STAGE/docs/oorexx"
cp -r "$DOCS_SVN/tools/bldoc_orx" "$STAGE/bldoc"
( cd "$STAGE/bldoc"
  rm -rf .svn bldoc.env work_folder/* fo_files/* log_files/* pdf_files/* html_folders/* hl-styles \
         pdf-hl.xsl html-hl.xsl rexx-highlights.xsl rexx-highlights-html.xsl )

# the edition: today, and the commit the books come from; the version: the
# latest release tag, when the clone has the tags (else the .ent's)
commit=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
edition="$(date +%Y.%m.%d) (commit $commit)"
version=$(git -C "$REPO" describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || true)

for book in $BOOKS; do
  [ -d "$REPO/docs/books/$book/en-US" ] || { echo "build-books.sh: no book $book in docs/books" >&2; exit 2; }
  cp -r "$REPO/docs/books/$book" "$STAGE/docs/$book"
  sed -i.bak "s|<!ENTITY EDITION \"[^\"]*\">|<!ENTITY EDITION \"$edition\">|" "$STAGE/docs/$book/en-US/$book.ent"
  [ -z "$version" ] || sed -i.bak "s|<!ENTITY VERSION \"[^\"]*\">|<!ENTITY VERSION \"$version\">|" "$STAGE/docs/$book/en-US/$book.ent"
  rm -f "$STAGE/docs/$book/en-US/$book.ent.bak"
  # the samples, where a book asks for them (<?sample 20-numpy.rex?>)
  rexx "$REPO/scripts/book-samples.rex" "$STAGE/docs/$book/en-US" >/dev/null
done

cd "$STAGE/bldoc"
rexx docpath.rex "$STAGE/docs" >/dev/null
# our books are not in ooRexx's SVN: docprep must not ask svn for their revision
cat > "$STAGE/noSvn.rex" <<'EOF'
props = .properties~load("bldoc.env")
do book over arg(1)~space~makeArray(" ")
  props~setLogical(book~lower, .false)
end
props~save("bldoc.env")
EOF
rexx "$STAGE/noSvn.rex" "$BOOKS"
for book in $BOOKS; do
  log="$STAGE/$book.log"
  if ! rexx hldoc2pdf.rex "$book" >"$log" 2>&1 || [ ! -f "pdf_files/$book.pdf" ]; then
    tail -30 "$log" >&2
    echo "build-books.sh: $book failed (log: $log)" >&2
    exit 1
  fi
  foplog="log_files/$book.log"           # FOP's own log
  if grep -qE 'SEVERE|ERROR' "$log" "$foplog"; then
    grep -hE 'SEVERE|ERROR' "$log" "$foplog" | head -20 >&2
    echo "build-books.sh: $book has errors (logs: $log, $STAGE/bldoc/$foplog)" >&2
    exit 1
  fi
  cp "pdf_files/$book.pdf" "$OUT/$book.pdf"
  echo "build-books.sh: $OUT/$book.pdf ($(grep -c '\[WARN\]' "$foplog" || true) FOP warnings, as ooRexx's books have; log: $STAGE/bldoc/$foplog)"
done
