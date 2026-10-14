#!/usr/bin/env rexx
/*----------------------------------------------------------------------------*/
/* book-samples.rex DIR [SAMPLES]: puts the nutshell samples into a book.     */
/*                                                                            */
/* Every line of the .xml files in DIR (a book's en-US directory) that is only */
/*   <?sample 20-numpy.rex?>                                                  */
/* is replaced by the sample: its program (an <example>), the Python it       */
/* replaces (for a .rex sample), how to run it, and its output, exactly as    */
/* the sample says them (the output Samples.testGroup checks).  The files are */
/* changed in place: scripts/build-books.sh runs it on its scratch copy of    */
/* the books, never on the repository's, so the book always shows the         */
/* samples as they are.  SAMPLES defaults to the repository's samples/.       */
/*                                                                            */
/* Licensed under the Apache License, Version 2.0.                            */
/*----------------------------------------------------------------------------*/
  parse source . . me
  sep = .file~separator
  root = .file~new(me)~parentFile~parentFile~absolutePath
  parse arg dir samples
  dir = dir~strip; samples = samples~strip
  if dir == "" then do
    say "usage: book-samples.rex DIR [SAMPLES]"
    exit 2
  end
  if samples == "" then samples = root || sep || "samples"

  used = 0
  do file over .file~new(dir)~listFiles
    if file~extension~lower \== "xml" then iterate
    lines = readLines(file~absolutePath)
    out = .array~new
    changed = .false
    do line over lines
      if line~strip~startsWith("<?sample "), line~strip~endsWith("?>") then do
        parse value line~strip with "<?sample " name "?>"
        path = samples || sep || name~strip
        if \.file~new(path)~exists then do
          say "book-samples.rex:" file~name": no sample" name~strip
          exit 1
        end
        out~appendAll(sample(path, name~strip))
        changed = .true
        used += 1
      end
      else out~append(line)
    end
    if changed then call writeLines file~absolutePath, out
  end
  say "book-samples.rex:" used "samples"
  exit 0

/* A sample as DocBook: its program, the Python it replaces, how to run it,   */
/* and its output.                                                            */
::routine sample
  use arg path, file
  src = readLines(path)
  python = .array~new; code = .array~new; output = .array~new
  if file~right(4) == ".rex" then do
    -- header: /* NN-name.rex: summary ... * Python: ... */
    inPython = .false
    do i = 2 while \src[i]~strip~startsWith("*/")
      l = src[i]
      parse var l " *" rest
      if rest~strip == "Python:" then do; inPython = .true; iterate; end
      if inPython then python~append(rest~substr(4))
    end
    do i = i + 1 while \src[i]~startsWith("/* Output:")
      code~append(src[i])
    end
    do i = i + 1 while src[i] \== "*/"
      output~append(src[i])
    end
    lang = "rexx"; run = "rexx" file
  end
  else do
    -- """NN-name.py: summary ... """ then code, then # Output: lines
    do i = 2 while src[i]~strip \== '"""'
    end
    do i = i + 1 while src[i] \== "# Output:"
      code~append(src[i])
    end
    do i = i + 1 to src~items
      output~append(src[i]~substr(3))
    end
    lang = "python"; run = "python3" file
  end
  call trim code

  x = .array~new
  id = "ex." || file~left(file~lastPos(".") - 1)
  x~append('<example id="'id'">')
  x~append('<title><filename>'file'</filename></title>')
  x~append(listing(lang, code))
  x~append('</example>')
  if python~items > 0 then do
    x~append('<para>The same in Python:</para>')
    x~append(listing("python", python))
  end
  x~append('<para>Run with <command>'run'</command>, it prints:</para>')
  x~append('<screen>' || escape(output~makeString("L", "0a"x)) || '</screen>')
  return x

::routine listing
  use arg lang, lines
  return '<programlisting language="'lang'">' || escape(lines~makeString("L", "0a"x)) || '</programlisting>'

::routine escape
  use arg text
  return text~changeStr("&", "&amp;")~changeStr("<", "&lt;")~changeStr(">", "&gt;")

::routine trim                             -- no blank lines at either end
  use arg lines
  do while lines~items > 0, lines[1]~strip == ""; lines~delete(1); end
  do while lines~items > 0, lines~lastItem~strip == ""; lines~delete(lines~last); end

::routine readLines
  use arg path
  s = .stream~new(path)
  lines = s~arrayIn
  s~close
  -- a file with CRLF line ends (a Windows checkout)
  do i = 1 to lines~items
    if lines[i]~right(1) == "0d"x then lines[i] = lines[i]~left(lines[i]~length - 1)
  end
  return lines

::routine writeLines
  use arg path, lines
  call SysFileDelete path
  s = .stream~new(path)
  s~open("write replace")
  do line over lines
    s~lineOut(line)
  end
  s~close
