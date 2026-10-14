# The books

Two books document the bridge, in the format and with the tools of ooRexx's
own books (DocBook 4.5, built with `docs/trunk/tools/bldoc_orx` of ooRexx's
SVN, whose highlighting of Rexx listings is the Rexx Parser's):

- `pythonref/` — **ooRexx and Python: Reference**, in the style of ooRexx's
  `rexxref`: the classes, methods and routines of `python.cls`, and the
  Python module `oorexx`, each with its syntax diagram, and small examples for
  the concepts.
- `pythoncookbook/` — **ooRexx and Python: Cookbook**: the main concepts, one
  recipe per nutshell sample: the program, the Python it replaces, how to run
  it, what it prints, and what happens.

## Kept true

- Every `<example>` of the books that holds a program and a `<screen>` is run
  by `tests/python/t60_book_examples.py` (part of `tests/run.sh`), and its
  output compared with the screen.
- The cookbook's recipes take their programs and outputs from `samples/`
  when the book is built: a line `<?sample 20-numpy.rex?>` is replaced by
  that sample (`scripts/book-samples.rex`). `tests/rexx/Samples.testGroup`
  checks the samples.
- The syntax diagrams are made from `pythonref/en-US/images/pythonref.ebnf`
  (the Railroad Diagram Generator's EBNF, as ooRexx's books): after changing
  it, build with `DIAGRAMS=1` and commit the SVGs.

## Building

```bash
svn co https://svn.code.sf.net/p/oorexx/code-0/docs/trunk docSVN   # next to the clone
# the Rexx Parser, from https://rexx.epbcn.com/rexx-parser/, unzipped as rexx-parser
sudo apt install xsltproc docbook-xml docbook-xsl fop fonts-liberation
oorexxpython/scripts/build-books.sh          # -> books/pythonref.pdf, books/pythoncookbook.pdf
```

`scripts/build-books.sh` explains its options (`BOOKS=`, `DIAGRAMS=1`,
`DOCS_SVN=`, `REXX_PARSER=`, `OUT=`).

Licensed under the Apache License, Version 2.0.
