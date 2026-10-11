/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * python.cls: Python objects as Rexx objects (the ooRexx/Python bridge).
 *
 *   np = .py~import("numpy")
 *   a  = np~array((1, 2, 3))
 *   say np~mean(a)                       -- 2.0
 *   ::requires "python.cls"
 *
 * .py         Python's builtins (.py~len(x), .py~range(10), .py~sorted(...))
 *             plus the bridge's helpers:
 *               ~import(name)            a module ("matplotlib.pyplot" too)
 *               ~kw(name, value, ...)    keyword arguments, as the last
 *               ~kw(directory)           argument of a call
 *               ~box(type, x)            x as a Python object of that type,
 *                                        built from its text ("str" default;
 *                                        int float bool bytes complex decimal
 *                                        fraction, or a Python class/function)
 *               ~str(x) ~int(x) ~float(x) ~decimal(x)
 *                                        Python's str(x)... (decimal.Decimal),
 *                                        kept as a Python object instead of
 *                                        coming back as a Rexx string
 *               ~eval(expr) ~exec(code)  in the namespace of ~main
 *               ~main                    the __main__ module
 *               ~redirectTo(...)         Rexx's standard streams to Python's
 *                                        (py.redirectTo, below)
 *             Every other name is Python's builtin, unchanged: .py~list(x) is
 *             list(x); to build a list or tuple from items, pass a Rexx Array:
 *             .py~list((1, 2)), .py~tuple(("a", 1)).
 * Routines, as BSF4ooRexx's bsf.import: py.import(name), py.box(type, x),
 * py.kw(name, value, ...), py.redirectTo(...).
 * .PyObject   a Python object, by reference:
 *               o~name(args...)          attribute, called (names in any case)
 *               o~name                   attribute; called if a function/method
 *               o~name = v               set an existing attribute
 *               o[k] / o[k] = v          items (o[i, j] -> o[(i, j)])
 *               o[.py~slice(a, b, c)]    o[a:b:c] (.nil for an empty part)
 *               o~new(args...)           o(args...) (classes)
 *               o~getattr("exactName"), o~setattr("exactName", v),
 *               o~invoke("exactName" | .nil, args...)
 *                                        exact names, when two differ only in
 *                                        case: logging~getattr("INFO") is the
 *                                        constant, logging~info the function
 *               do x over o              iter(o)
 *               say o                    str(o)
 *
 * Values: Python str, int, float, bool (1/0), bytes and None (.nil) come back
 * as Rexx strings; everything else as a .PyObject.  Rexx strings that are
 * numbers go to Python as int or float (but 1/0 as True/False to a
 * parameter annotated bool); Rexx Arrays as lists (copies).
 * The design, and the reasons: docs/design.md.
 * A Python exception is SYNTAX 98.900 "Python error: Type: message", with
 * the traceback in condition("O")~additional[2].
 */

.local~py = .Python~new

::requires "rexxpython" LIBRARY

::class PyObject public
-- Every method but init is UNGUARDED: a proxy has no Rexx state that changes
-- (CSELF is set once), Python's state is protected by the GIL, and a guarded
-- method would hold the object while Python runs, so another thread using the
-- same proxy (e.g. a Python thread calling back Rexx code that uses .py)
-- would wait for it: a deadlock when the first thread waits for that one.
::method init
  expose cself
  use strict arg cself                 -- a .Pointer the bridge made
-- uninit is Rexx, not native: it only queues the pointer; the bridge drops
-- the Python reference later (see drainGraveyard in bridge.cpp: a native
-- uninit releases the kernel lock inside ooRexx's runUninits loop, which
-- crashes when other threads create objects meanwhile: ooRexx bug #2101,
-- fixed in r13256)
::method uninit unguarded
  expose cself
  if cself~isA(.Pointer) then .PyObject~graveyard~queue(cself)
::attribute graveyard class get unguarded
::method activate class
  expose graveyard
  graveyard = self~"PY.GRAVEYARD"        -- one Queue for the whole process
::method "PY.GRAVEYARD" class private unguarded external "LIBRARY rexxpython Py_Graveyard"
::method "PY.POINTER" unguarded external "LIBRARY rexxpython PyObj_Pointer"
::method unknown unguarded      external "LIBRARY rexxpython PyObj_Unknown"
::method getattr unguarded      external "LIBRARY rexxpython PyObj_GetAttr"
::method setattr unguarded      external "LIBRARY rexxpython PyObj_SetAttr"
::method invoke unguarded       external "LIBRARY rexxpython PyObj_Invoke"
::method "[]" unguarded         external "LIBRARY rexxpython PyObj_GetItem"
::method "[]=" unguarded        external "LIBRARY rexxpython PyObj_SetItem"
::method string unguarded       external "LIBRARY rexxpython PyObj_String"
::method makeArray unguarded    external "LIBRARY rexxpython PyObj_MakeArray"
-- Rexx operators: Python's (a + b, -a, a < b...); == is identity (Python's is)
::method "+" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method "-" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method "*" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method "/" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method "%" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method "//" unguarded  external "LIBRARY rexxpython PyObj_Operator"
::method "**" unguarded  external "LIBRARY rexxpython PyObj_Operator"
::method "<" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method ">" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method "<=" unguarded  external "LIBRARY rexxpython PyObj_Operator"
::method ">=" unguarded  external "LIBRARY rexxpython PyObj_Operator"
::method "=" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method "\=" unguarded  external "LIBRARY rexxpython PyObj_Operator"
::method "<>" unguarded  external "LIBRARY rexxpython PyObj_Operator"
::method "><" unguarded  external "LIBRARY rexxpython PyObj_Operator"
::method "==" unguarded  external "LIBRARY rexxpython PyObj_Operator"
::method "\==" unguarded external "LIBRARY rexxpython PyObj_Operator"
::method "&" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method "|" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method "&&" unguarded  external "LIBRARY rexxpython PyObj_Operator"
::method "\" unguarded   external "LIBRARY rexxpython PyObj_Operator"
::method hashCode unguarded external "LIBRARY rexxpython PyObj_HashCode"
-- Object's methods that Python objects commonly have too (dict.copy,
-- generator.send, Thread.start, subprocess.run): Python's when the object
-- has the attribute, Object's otherwise
::method copy unguarded
  if self~"PY.HAS"("COPY") then forward message "UNKNOWN" arguments (.array~of("COPY", arg(1, "A")))
  forward class (super)
::method send unguarded
  if self~"PY.HAS"("SEND") then forward message "UNKNOWN" arguments (.array~of("SEND", arg(1, "A")))
  forward class (super)
::method start unguarded
  if self~"PY.HAS"("START") then forward message "UNKNOWN" arguments (.array~of("START", arg(1, "A")))
  forward class (super)
::method run unguarded
  if self~"PY.HAS"("RUN") then forward message "UNKNOWN" arguments (.array~of("RUN", arg(1, "A")))
  forward class (super)
::method "PY.HAS" unguarded     external "LIBRARY rexxpython PyObj_Has"

::class Python public subclass PyObject
::method init
  forward class (super) arguments (.array~of(self~builtinsPointer))
::method builtinsPointer private unguarded external "LIBRARY rexxpython Py_Builtins"
::method import unguarded  external "LIBRARY rexxpython Py_Import"
::method str unguarded     external "LIBRARY rexxpython Py_Make"
::method int unguarded     external "LIBRARY rexxpython Py_Make"
::method float unguarded   external "LIBRARY rexxpython Py_Make"
::method decimal unguarded external "LIBRARY rexxpython Py_Make"
::method box unguarded     external "LIBRARY rexxpython Py_Box"
::method kw unguarded
  return .PyKwArgs~new(arg(1, "A"))
::method main unguarded
  return self~import("__main__")
::method eval unguarded
  use strict arg code
  return self~invoke("eval", self~str(code), self~main~getattr("__dict__"))
::method exec unguarded
  use strict arg code
  return self~invoke("exec", self~str(code), self~main~getattr("__dict__"))
::method redirectTo unguarded            -- py.redirectTo, below
  .context~package~findRoutine("PY.REDIRECTTO")~callWith(arg(1, "A"))

-- BSF4ooRexx-style routines (bsf.import ...), for those who prefer them
::routine py.import public
  use strict arg name
  return .py~import(name)
::routine py.box public
  return .py~sendWith("BOX", arg(1, "A"))
::routine py.kw public
  return .py~sendWith("KW", arg(1, "A"))

/*
 * Rexx's standard streams through Python's (docs/design.md, *Standard
 * streams*), as BSF4ooRexx's bsf.redirectTo, BSF.InputStream and
 * BSF.OutputStream do for Java's.
 *
 *   call py.redirectTo                   .input .output .error .debugInput
 *                                        .traceOutput -> sys.stdin/stdout/stderr
 *   call py.redirectTo "R"               back to Rexx's .stdin .stdout .stderr
 *   call py.redirectTo "P", in, out, err, dbgIn, trc [, prefixes]
 *                                        Python file objects (or .PyInputStream /
 *                                        .PyOutputStream, kept as they are)
 *   .py~redirectTo(...)                  the same, as a message
 *
 * Each stream has a prefix that marks Rexx's lines among Python's: REXXin?>,
 * REXXout>, REXXerr>, REXXdbgIn?> and REXXtrc> (BSF4ooRexx's).  prefixes is
 * .true (the default), .false (none), or a Directory/StringTable from
 * INPUT, OUTPUT, ERROR, DEBUGINPUT, TRACEOUTPUT to a prefix (.nil for none);
 * later, .output~current~prefix = .nil (or another string).
 * An interpreter instance that Python creates does this by itself
 * (oorexx.Interpreter(stdio=True, prefixes=True)).
 */
::routine py.redirectTo public
  use strict arg redirectTo="P", input=.nil, output=.nil, error=.nil, debugInput=.nil, -
                 traceOutput=.nil, prefixes=.true
  toPython = redirectTo~strip~left(1)~caselessEquals("P")
  if \toPython then do                  -- back to Rexx's streams (or the objects given)
    call py.redirect .input, input, .stdin
    call py.redirect .output, output, .stdout
    call py.redirect .error, error, .stderr
    call py.redirect .debugInput, debugInput, .input
    call py.redirect .traceOutput, traceOutput, .error
    return
  end
  prefix = py.prefixes(prefixes)
  call py.redirect .input, py.stream(.PyInputStream, input, "stdin", prefix["INPUT"])
  call py.redirect .output, py.stream(.PyOutputStream, output, "stdout", prefix["OUTPUT"])
  call py.redirect .error, py.stream(.PyOutputStream, error, "stderr", prefix["ERROR"])
  -- separate stream objects, for their own prefixes (as BSF4ooRexx)
  call py.redirect .debugInput, py.stream(.PyInputStream, debugInput, .input~current, prefix["DEBUGINPUT"])
  call py.redirect .traceOutput, py.stream(.PyOutputStream, traceOutput, .error~current, prefix["TRACEOUTPUT"])

-- the monitor's new destination: the object given, or the default
::routine py.redirect private
  use strict arg monitor, object, default=.nil
  if object == .nil then object = default
  monitor~destination(object)           -- pushed: ~destination pops it

-- a .PyInputStream/.PyOutputStream: as given, or wrapping a Python stream
::routine py.stream private
  use strict arg class, object, default, prefix
  if object~isA(.PyInputStream) | object~isA(.PyOutputStream) then return object
  if object == .nil then object = default
  -- another stream object of ours: the same Python stream, with its own prefix
  if object~isA(.PyInputStream) | object~isA(.PyOutputStream) then object = object~stream
  return class~new(object, prefix)

-- an object's string value, as Rexx's own streams take it (makeString first)
::routine py.text private
  use strict arg object
  if object~isA(.String) then return object
  text = object~request("STRING")
  if text == .nil then text = object~string
  return text

::routine py.describe private
  use strict arg prefix
  if prefix == .nil then return "no prefix"
  return "prefix" prefix

::routine py.prefixes private
  use strict arg prefixes
  default = .Directory~new
  default["INPUT"] = "REXXin?>"
  default["OUTPUT"] = "REXXout>"
  default["ERROR"] = "REXXerr>"
  default["DEBUGINPUT"] = "REXXdbgIn?>"
  default["TRACEOUTPUT"] = "REXXtrc>"
  table = .Directory~new
  names = .array~of("INPUT", "OUTPUT", "ERROR", "DEBUGINPUT", "TRACEOUTPUT")
  if prefixes~isA(.MapCollection) then
    do n over prefixes~allIndexes
      if \names~hasItem(n~upper) then
        raise syntax 93.900 array ('py.redirectTo: no stream "'n'"; prefixes are for' names~makeString(, ", "))
    end
  do name over names
    select
      when prefixes~isA(.MapCollection) then do
        value = .nil
        do with index n item v over prefixes
          if n~caselessEquals(name) then value = v
        end
        table[name] = value
      end
      when prefixes == .nil then table[name] = .nil
      when prefixes~datatype("O") then
        table[name] = (prefixes = 1)~?(default[name], .nil)
      otherwise
        raise syntax 93.900 array ("py.redirectTo: prefixes is .true, .false, or a Directory of prefixes")
    end
  end
  return table

/*
 * A Python text stream as a Rexx stream (BSF.OutputStream's methods).  The
 * stream is a name in sys ("stdout", "stderr"), looked up at every write, so
 * that Jupyter and contextlib.redirect_stdout are followed; or a Python file
 * object.  Each line starts with the prefix (.nil for none), also after
 * every line feed inside it.  One write call per line, so that lines from
 * several threads do not mix.  Unguarded, as .PyObject (Python may call back
 * into Rexx while it writes).
 */
::class PyOutputStream public subclass OutputStream
::attribute stream get unguarded
::attribute prefix unguarded             -- .nil: none
::method init
  expose stream prefix atLineStart
  use strict arg stream="stdout", prefix=.nil
  atLineStart = .true
::method file unguarded                  -- the Python stream (.nil: None)
  expose stream
  if stream~isA(.String) then return .py~import("sys")~getattr(stream)
  return stream
::method write private unguarded
  use strict arg text
  target = self~file
  if target == .nil then return          -- sys.stdout is None (pythonw)
  target~write(.py~str(text))
::method charOut unguarded
  expose prefix atLineStart
  use strict arg chars, start=1, skipPrefix=.false
  chars = py.text(chars)
  if start > 1 then chars = chars~substr(start)
  if chars == "" then return 0
  if prefix \== .nil then do
    text = .MutableBuffer~new
    if atLineStart, \skipPrefix then text~append(prefix)
    -- the prefix after every line feed but a last one (it goes before the
    -- next line's first character)
    last = chars~right(1) == "0a"x
    if last then chars = chars~left(chars~length - 1)
    text~append(chars~changeStr("0a"x, "0a"x || prefix))
    if last then text~append("0a"x)
    chars = text~string
  end
  if \skipPrefix then atLineStart = chars~right(1) == "0a"x
  self~write(chars)
  return 0
::method lineOut unguarded
  expose prefix atLineStart
  use strict arg line=""
  line = py.text(line)                   -- trace output is a TraceObject (5.1)
  if prefix \== .nil then do
    line = line~changeStr("0a"x, "0a"x || prefix)
    if atLineStart then line = prefix || line
  end
  atLineStart = .true
  self~write(line || "0a"x)
  return 0
::method say unguarded
  forward message "LINEOUT"
::method arrayOut unguarded
  use strict arg lines
  do line over lines
    self~lineOut(line)
  end
  return 0
::method close unguarded
  return 0
::method string unguarded
  expose stream prefix
  return "a PyOutputStream ("stream~string", "py.describe(prefix)")"

/*
 * A Python text stream as a Rexx input stream (BSF.InputStream's methods):
 * "stdin" (looked up at every read) or a Python file object.  linein first
 * writes the prefix to .output, as a prompt (without .output's own prefix).
 */
::class PyInputStream public subclass InputStream
::attribute stream get unguarded
::attribute prefix unguarded
::method init
  expose stream prefix
  use strict arg stream="stdin", prefix=.nil
::method file unguarded                  -- the Python stream (.nil: None)
  expose stream
  if stream~isA(.String) then return .py~import("sys")~getattr(stream)
  return stream
::method lineIn unguarded
  expose prefix
  if prefix \== .nil then do
    out = .output~current
    if out~isA(.PyOutputStream) then out~charOut(prefix, 1, .true)
    else .output~charOut(prefix)
    -- a prompt must be seen before the read waits
    if out~isA(.PyOutputStream) then do
      target = out~file
      if target \== .nil then target~flush
    end
  end
  target = self~file
  if target == .nil then return ""
  line = target~readline
  if line == .nil then return ""
  if line~right(1) == "0a"x then line = line~left(line~length - 1)
  if line~right(1) == "0d"x then line = line~left(line~length - 1)
  return line
::method charIn unguarded
  use strict arg start=1, length=1
  target = self~file
  if target == .nil then return ""
  chars = target~read(length)
  if chars == .nil then return ""
  return chars
::method arrayIn unguarded
  lines = .array~new
  target = self~file
  if target == .nil then return lines
  do line over target~readlines
    if line~right(1) == "0a"x then line = line~left(line~length - 1)
    lines~append(line)
  end
  return lines
::method chars unguarded                 -- a Python stream cannot tell: as a terminal
  return 1
::method lines unguarded
  return 1
::method close unguarded
  return 0
::method string unguarded
  expose stream prefix
  return "a PyInputStream ("stream~string", "py.describe(prefix)")"

::class PyKwArgs public                -- what .py~kw makes
::attribute names get
::attribute values get
::method init
  expose names values
  use strict arg args
  names = .array~new
  values = .array~new
  if args~items = 1, args[1]~isA(.MapCollection) then do
    do with index name item value over args[1]
      names~append(name)
      values~append(value)
    end
  end
  else do
    if args~size // 2 \= 0 then
      raise syntax 93.900 array (".py~kw: names and values go in pairs")
    do i = 1 to args~size by 2
      names~append(args[i])
      values~append(args[i + 1])
    end
  end
