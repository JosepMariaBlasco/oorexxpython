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
 *               ~extend(.Class, base)    the Rexx class (a subclass of
 *                                        PyObject) extends the Python class
 *                                        base (py.extend, below)
 *               ~raise(exc [, args])     raise a Python exception
 *             Every other name is Python's builtin, unchanged: .py~list(x) is
 *             list(x); to build a list or tuple from items, pass a Rexx Array:
 *             .py~list((1, 2)), .py~tuple(("a", 1)).
 * Routines, as BSF4ooRexx's bsf.import: py.import(name), py.box(type, x),
 * py.kw(name, value, ...), py.redirectTo(...), py.extend(class, base).
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
 *               o~makeArrayProxy         a sequence as a Rexx Array, from 1
 *               do x over o              iter(o)
 *               say o                    str(o)
 *
 * Values: Python str, int, float, bytes and None (.nil) come back as Rexx
 * strings, bool as .Boolean~true / .Boolean~false (boolean.cls); everything
 * else as a .PyObject.  Rexx strings that are numbers go to Python as int or
 * float (but 1/0 as True/False to a parameter annotated bool), a .Boolean as
 * a bool; Rexx Arrays as lists (copies).
 * The design, and the reasons: docs/design.md.
 * A Python exception is SYNTAX 98.900 "Python error: Type: message", with
 * the exception in condition("O")~additional[2] and the traceback in
 * additional[3], as BSF4ooRexx; if it goes back to Python, it is raised
 * again as itself.
 */

.local~py = .Python~new

::requires "rexxpython" LIBRARY
::requires "boolean.cls"           -- .Boolean: Python's bool in Rexx

::class PyObject public
-- Every method but init is UNGUARDED: a proxy has no Rexx state that changes
-- (CSELF is set once), Python's state is protected by the GIL, and a guarded
-- method would hold the object while Python runs, so another thread using the
-- same proxy (e.g. a Python thread calling back Rexx code that uses .py)
-- would wait for it: a deadlock when the first thread waits for that one.
::method init
  expose cself pypair pyuninit pystrings
  if self~class \== .PyObject then do
    pyclass = .PyObject~"PY.EXTENSIONOF"(self~class)
    if pyclass \== .nil then do        -- .MyClass~new(args): a pair (py.extend, below)
      -- the class's UNINIT runs when the pair is over ("PY.FINAL"), not when
      -- Rexx no longer holds the object (an object's own UNINIT cannot be
      -- removed: it becomes a NOP)
      pyuninit = self~instanceMethod("UNINIT")
      if pyuninit == .PyObject~method("UNINIT") then pyuninit = .nil
      self~setMethod("UNINIT", "nop")
      signal on syntax
      made = self~"PY.PAIRNEW"(pyclass, arg(1, "A"))  -- the Python object (__new__)
      cself = made[1]
      pypair = made[2]
      self~"PY.ARM"
      self~"PY.PAIRINIT"(arg(1, "A"))                  -- the base's __init__
      return
    end
    if \self~isA(.Python) then
      raise syntax 93.900 array ("ooRexx/Python:" self~class~id "is a subclass of PyObject that no Python class extends: call .py~extend(self, base) in its ACTIVATE class method")
  end
  -- a .Pointer the bridge made; pystrings: reached through a module imported
  -- as "string" (.py~import(name, "string")), whose calls get strings as str
  use strict arg cself, pystrings = .false
  return
syntax: raise propagate
::method "PY.PAIRNEW" private unguarded external "LIBRARY rexxpython PyPair_New"
::method "PY.PAIRINIT" private unguarded external "LIBRARY rexxpython PyPair_Init"
-- a new sentinel for a pair's Rexx object, while Rexx holds it: its UNINIT
-- tells the bridge when Rexx no longer does (docs/design.md, *Subclassing*)
::method "PY.ARM" unguarded
  expose pypair pysentinel
  pysentinel = .PyPeerSentinel~new(self, pypair, PyPair_Arm(pypair))
-- the pair is over (the bridge): the class's UNINIT, once
::method "PY.FINAL" unguarded
  expose cself pysentinel pyuninit
  drop pysentinel
  cself = .nil                         -- the Python object is gone
  if var("PYUNINIT") then if pyuninit \== .nil then do
    u = pyuninit
    drop pyuninit
    signal on syntax name final
    -- UNINIT sent with the class that defines it as the scope: it runs in
    -- its own scope (its variables), also when a superclass defines it, and
    -- skips the object's NOP (not setMethod(..., "OBJECT"), whose scope is
    -- the object's class; not self~run(u), whose scope is none of them)
    .Message~new(self, .array~of("UNINIT", u~scope))~send
  end
final: return
-- the Python class that this class (or its nearest superclass) extends
-- (.py~extend), or .nil: for Python code that makes the objects itself
-- (json.dumps(x, cls=.MyEncoder~pyClass))
::method pyClass class unguarded
  return .PyObject~"PY.EXTENSIONOF"(self)
-- the Python class that a class (or its nearest superclass) extends, or .nil
::method "PY.EXTENSIONOF" class unguarded
  use strict arg cls
  do while cls \== .PyObject, cls~isSubclassOf(.PyObject)
    ext = cls~"PY.CLASS"
    if ext \== .nil then return ext
    cls = cls~superClass
  end
  return .nil
::method "PY.CLASS" class unguarded
  expose pyclass
  if var("PYCLASS") then return pyclass
  return .nil
::method "PY.CLASS=" class unguarded
  expose pyclass
  use strict arg pyclass
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
-- Object's REQUEST needs an argument: without one, it is Python's (the
-- request of a socketserver or http.server handler)
::method request unguarded
  if arg() = 0, self~"PY.HAS"("REQUEST") then forward message "UNKNOWN" arguments (.array~of("REQUEST", .array~new))
  forward class (super)
::method "PY.HAS" unguarded     external "LIBRARY rexxpython PyObj_Has"
-- a Python sequence (a list, a tuple, a 1-D numpy array...) as a Rexx Array,
-- counting from 1 (BSF4ooRexx's BSF_Array_Reference): .PyArrayProxy, below
::method makeArrayProxy unguarded
  return .PyArrayProxy~new(self)

-- held only by a pair's Rexx object, and holding it: its UNINIT runs when Rexx
-- no longer holds that object.  Like .PyObject's, it only queues (no native
-- code in an UNINIT: ooRexx bug #2101); the bridge takes it from there.  A new
-- one each round, as an object's UNINIT runs only once.
::class PyPeerSentinel
::method init
  expose peer pair n
  use strict arg peer, pair, n
::method uninit
  expose peer pair n
  .PyObject~graveyard~queue(.array~of(peer, pair, n))
::routine PyPair_Arm external "LIBRARY rexxpython PyPair_Arm"

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
-- ADDRESS environments written in Python, registered from Rexx: handler is
-- a Python callable, handler(command, context), as for the Python side's
-- Interpreter.add_command_environment; removeCommandEnvironment makes the
-- name unknown again (FAILURE, RC 30); a name not registered is an error
::method addCommandEnvironment unguarded    external "LIBRARY rexxpython Py_AddEnvironment"
::method removeCommandEnvironment unguarded external "LIBRARY rexxpython Py_RemoveEnvironment"
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
::method extend unguarded                -- py.extend, below
  return .context~package~findRoutine("PY.EXTEND")~callWith(arg(1, "A"))
-- raise a Python exception (an instance, or a class to call with the rest of
-- the arguments): SYNTAX 98.900 as when Python raises it, and Python gets it
-- as itself if the condition goes back there (an override of a class that
-- extends a Python class: IndexError ends a sequence, StopIteration an iterator)
::method raise unguarded
  use strict arg exc, ...
  if self~invoke("isinstance", exc, self~type)~string == 1 then
    exc = exc~sendWith("NEW", arg(1, "A")~section(2))
  if self~invoke("isinstance", exc, self~getattr("BaseException"))~string \== 1 then
    raise syntax 93.900 array (".py~raise: not a Python exception:" exc)
  text = self~str(exc)~string
  name = self~type(exc)~getattr("__name__")
  if text == "" then message = "Python error:" name
  else message = "Python error:" name":" text
  raise syntax 98.900 array (message, exc, "")

-- BSF4ooRexx-style routines (bsf.import ...), for those who prefer them
::routine py.import public
  return .py~sendWith("IMPORT", arg(1, "A"))
::routine py.box public
  return .py~sendWith("BOX", arg(1, "A"))
::routine py.kw public
  return .py~sendWith("KW", arg(1, "A"))

/*
 * Rexx classes that extend Python classes (docs/design.md, *Subclassing*),
 * as .js~extend does in ooRexx/WASM and .net~extend in ooRexx/.NET:
 *
 *   t = .Worker~new(.py~kw("name", "worker"))         -- a threading.Thread
 *   t~start; t~join
 *
 *   ::class Worker subclass PyObject
 *   ::method activate class
 *     .py~extend(self, "threading.Thread")            -- or py.extend(...)
 *   ::method run                          -- overrides Thread.run
 *     say "running in" self~name          -- self: the object itself
 *
 * py.extend(class, base): class is a subclass of PyObject; base is a Python
 * class, a dotted name ("http.server.BaseHTTPRequestHandler"), or an Array
 * of them.  It makes a Python class that extends base, named as the Rexx
 * class, and answers it; once per class (its ACTIVATE class method).  Every
 * public method of the class (and of its superclasses below PyObject)
 * overrides the base's, except INIT, UNINIT, UNKNOWN and names with a dot;
 * its Python name is: the ::ANNOTATE'd one (::annotate method do_get python
 * "do_GET"), else the lowercase one for __dunder__ names, else the base
 * classes' attribute that matches without regard to case, else the
 * lowercase one.  Python calls an override with its positional arguments,
 * and a .PyKwArgs last if it passes keyword arguments.  .Worker~new(args)
 * makes the Python object: the class's INIT calls self~init:super(args),
 * which runs the base's __new__ and __init__ with them (immutable bases,
 * str, int, tuple, take their value there).  Python code that makes one
 * itself (Worker(...)) gets the same: the Rexx object first, its INIT, then
 * the Python object.  self~name:super(args) is the base's member, not the
 * override.  The object comes back to Rexx as itself.  The pair lives while
 * either side holds it, and the class's UNINIT runs once, when it is over.
 * The results of special methods are converted as Python requires
 * (__bool__ -> bool, __len__ -> int, __str__ -> str...).  The Rexx class
 * lives as long as its interpreter instance (.local["PY.EXTENDED"]): the
 * Python class may outlive every Rexx reference to it (its package's
 * included), as classes do in Python.
 */
::routine py.extend public
  use strict arg class, base
  ok = class~isA(.Class)
  if ok then ok = class \== .PyObject & class~isSubclassOf(.PyObject)
  if \ok then
    raise syntax 93.900 array ("py.extend(class, base): class must be a subclass of PyObject (::class X subclass PyObject), not" class)
  -- the bases, as Python classes
  if base~isA(.Array) then bases = base~copy
  else bases = .array~of(base)
  do i = 1 to bases~items
    if bases[i]~isA(.String) then bases[i] = py.dottedName(bases[i])
  end
  -- the methods: "REXXNAME exactName" (exactName from ::ANNOTATE, or "")
  specs = .array~new
  seen = .Set~new
  c = class
  do while c \== .PyObject
    s = c~methods(c)
    do while s~available
      m = s~index
      method = s~item
      s~next
      if seen~hasIndex(m) then iterate
      seen~put(m)
      if m == "INIT" | m == "UNINIT" | m == "UNKNOWN" | m~pos(".") > 0 | method~isPrivate then iterate
      exact = method~annotation("python")
      if exact == .nil then exact = ""
      specs~append(m exact)
    end
    c = c~superClass
  end
  helper = py.extendHelper()
  -- the Python class reaches its Rexx class only through a WeakReference:
  -- the Rexx class holds the Python class, not the other way round (a cycle
  -- across the two collectors would never be collected); the instance keeps
  -- the Rexx class, below, so the reference never goes stale while Python
  -- can still make objects of it
  pyclass = helper~make(.py~tuple(bases), .py~str(class~id), specs, .WeakReference~new(class), -
                        .context~package~findRoutine("PY.NEWFOR"))
  class~"PY.CLASS="(pyclass)
  extended = .local["PY.EXTENDED"]
  if extended == .nil then do
    extended = .IdentityTable~new
    .local["PY.EXTENDED"] = extended
  end
  extended[class] = pyclass
  return pyclass

-- "a.b.C": the attribute C of the module a.b
::routine py.dottedName private
  use strict arg name
  if name~lastPos(".") = 0 then return .py~getattr(name)    -- a builtin: object, dict
  module = name~left(name~lastPos(".") - 1)
  return .py~import(module)~getattr(name~substr(name~lastPos(".") + 1))

-- the helper module, made once per interpreter instance
::routine py.extendHelper private
  helper = .local["PY.EXTENDHELPER"]
  if helper \== .nil then return helper
  types = .py~import("types")
  helper = types~ModuleType~new("oorexx_extend")
  code = .context~package~resources["EXTENDPY"]~makeString("L", "0a"x)
  .py~invoke("exec", .py~str(code), helper~getattr("__dict__"))
  .local["PY.EXTENDHELPER"] = helper
  return helper

-- Python makes an object of an extending class itself (Worker(...)): the
-- Rexx object first, whose INIT makes the Python object
::routine py.newFor private
  use arg weak, pyArgs, kwargs
  class = weak~value
  if class == .nil then
    raise syntax 93.900 array ("ooRexx/Python: the Rexx class of this Python class is gone")
  args = pyArgs~makeArray
  if kwargs \== .nil then args~append(.PyKwArgs~new(.array~of(kwargs)))
  return class~sendWith("NEW", args)

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

/*
 * A Python sequence seen as a Rexx Array, counting from 1, as BSF4ooRexx's
 * BSF_Array_Reference does for a Java array (docs/design.md, *Messages*):
 *
 *   a = .py~eval("[10, 20, 30]")~makeArrayProxy
 *   say a[1] a~items                     -- 10 3
 *   a[2] = 21; a~append(40)              -- the Python list changes
 *   do x over a; say x; end
 *
 * It holds the Python object (~pyObject) and reads and writes through it:
 * nothing is copied.  Positions go from 1 to ~items; at/[] outside them give
 * .nil, as a Rexx Array's do; put/[]= at ~items + 1 appends.
 */
::class PyArrayProxy public
::attribute pyObject get unguarded
::method init
  expose pyObject
  use strict arg pyObject
::method items unguarded
  expose pyObject
  return .py~len(pyObject)
::method size unguarded
  return self~items
::method dimension unguarded
  use strict arg n = 0
  if arg(1, "E") then do
    if n = 1 then return self~items
    return 0
  end
  return 1
::method isEmpty unguarded
  return self~items = 0
::method position private unguarded     -- a position (1..items), or .nil
  use strict arg i, extra = 0
  if \datatype(i, "W") then
    raise syntax 93.900 array ("PyArrayProxy: the position must be a whole number, not" i)
  if i < 1 | i > self~items + extra then return .nil
  return i
::method "[]" unguarded
  expose pyObject
  use strict arg i
  if self~position(i) == .nil then return .nil
  return pyObject[i - 1]
::method at unguarded
  forward message "[]"
::method "[]=" unguarded
  expose pyObject
  use strict arg value, i
  if self~position(i, 1) == .nil then
    raise syntax 93.900 array ("PyArrayProxy: position" i "is outside 1 to" self~items + 1)
  if i = self~items + 1 then pyObject~append(value)
  else pyObject[i - 1] = value
::method put unguarded
  forward message "[]="
::method hasIndex unguarded
  use strict arg i
  return datatype(i, "W") & i >= 1 & i <= self~items
::method append unguarded
  expose pyObject
  use strict arg value
  pyObject~append(value)
  return self~items
::method remove unguarded               -- the item, or .nil
  expose pyObject
  use strict arg i
  if \self~hasIndex(i) then return .nil
  return pyObject~pop(i - 1)
::method delete unguarded
  forward message "REMOVE"
::method first unguarded
  if self~items = 0 then return .nil
  return 1
::method last unguarded
  n = self~items
  if n = 0 then return .nil
  return n
::method firstItem unguarded
  return self[1]
::method lastItem unguarded
  return self[self~items]
::method makeArray unguarded            -- a Rexx Array of the items (a copy)
  expose pyObject
  return pyObject~makeArray
::method allItems unguarded
  return self~makeArray
::method allIndexes unguarded
  a = .array~new
  do i = 1 to self~items; a~append(i); end
  return a
::method supplier unguarded
  return .supplier~new(self~makeArray, self~allIndexes)
::method string unguarded
  expose pyObject
  return "a PyArrayProxy" pyObject

::class PyKwArgs public                -- what .py~kw makes (and what Python's keyword arguments are for an override)
::attribute names get
::attribute values get
::method "[]"                           -- the value of a name (exact, else in any case), or .nil
  expose names values
  use strict arg name
  do i = 1 to names~items
    if names[i] == name then return values[i]
  end
  do i = 1 to names~items
    if names[i]~caselessEquals(name) then return values[i]
  end
  return .nil
::method items
  expose names
  return names~items
::method init
  expose names values
  use strict arg args
  names = .array~new
  values = .array~new
  if args~items = 1, args[1]~isA(.PyObject) then do   -- a Python dict (keyword arguments from Python)
    do name over args[1]
      names~append(name)
      values~append(args[1][name])
    end
  end
  else if args~items = 1, args[1]~isA(.MapCollection) then do
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

-- the Python half of py.extend: the class, its construction, its overrides
::resource extendpy
import builtins

# results of special methods, as Python requires them
_CONVERT = {
    '__bool__': bool, '__contains__': bool,
    '__len__': int, '__length_hint__': int, '__hash__': int,
    '__index__': int, '__int__': int,
    '__float__': float, '__complex__': complex,
    '__str__': str, '__repr__': str, '__format__': str,
    '__bytes__': bytes,
}

def _python_name(rexx_name, exact, bases):
    if exact:
        return exact
    low = rexx_name.lower()
    if low.startswith('__') and low.endswith('__'):
        return low
    found = set()
    for b in bases:
        for n in dir(b):
            if n.lower() == low:
                found.add(n)
    if len(found) > 1:
        raise TypeError('py.extend: %s matches %s in the base classes: give the name with '
                        '::annotate method %s python "..."' % (rexx_name, ', '.join(sorted(found)), low))
    return found.pop() if found else low

import threading
import oorexx

_tls = threading.local()

def _pending():
    # the classes Python is making objects of on this thread (Worker(...)),
    # whose Python objects the Rexx INITs make (_new)
    try:
        return _tls.pending
    except AttributeError:
        _tls.pending = []
        return _tls.pending

_RESERVED = ('__new__', '__init__', '__del__', '__init_subclass__', '__class_getitem__',
             '__getattribute__', '__setattr__', '__delattr__', '__dict__', '__class__')

def make(bases, name, specs, rexx_class, new_for):
    ns = {}
    overrides = {}                        # REXXNAME -> Python name
    cls = None

    def method(py_name, rexx_name):
        convert = _CONVERT.get(py_name)
        def f(self, *args, **kwargs):
            r = oorexx._call_peer(self, rexx_name, args, kwargs)
            if convert is not None and r is not NotImplemented:
                r = convert(r)
            return r
        f.__name__ = py_name
        f.__qualname__ = name + '.' + py_name
        return f

    for spec in specs:
        rexx_name, _, exact = spec.partition(' ')
        py_name = _python_name(rexx_name, exact.strip(), bases)
        if py_name in _RESERVED:
            raise TypeError('py.extend: %s (method %s) cannot be overridden from Rexx: '
                            'the INIT and UNINIT of the Rexx class do that work' % (py_name, rexx_name))
        ns[py_name] = method(py_name, rexx_name)
        overrides[rexx_name] = py_name

    # Construction.  The Rexx object comes first, and its INIT makes the
    # Python object (self~init:super(args): _new, then _init).  When Python
    # makes one itself (Worker(...)), __new__ asks Rexx for the Rexx object.
    def __new__(cls_, *args, **kwargs):
        stack = _pending()
        stack.append(cls_)
        depth = len(stack)
        try:
            obj = new_for(rexx_class, list(args), kwargs or None)
        finally:
            if len(stack) == depth and stack[-1] is cls_:
                stack.pop()               # INIT never called init:super
        if not isinstance(obj, cls_):
            raise TypeError('%s: its INIT must call self~init:super(...), which makes the '
                            'Python object' % name)
        return obj
    ns['__new__'] = staticmethod(__new__)

    def __init__(self, *args, **kwargs):
        pass                              # done by _init, from the Rexx INIT
    ns['__init__'] = __init__

    def _new(pair, args, kwargs):
        kwargs = kwargs or {}
        klass = cls
        stack = _pending()
        if stack and isinstance(stack[-1], type) and issubclass(stack[-1], cls):
            klass = stack.pop()           # Python is making it (Worker(...))
        base = super(cls, klass)
        if base.__new__ is object.__new__:
            if (args or kwargs) and base.__init__ is object.__init__:
                raise TypeError('%s() takes no arguments' % name)
            obj = object.__new__(klass)
        else:
            obj = base.__new__(klass, *args, **kwargs)
        object.__setattr__(obj, '_oorexx_pair', pair)
        return obj
    ns['_oorexx_new'] = staticmethod(_new)

    def _init(obj, args, kwargs):
        kwargs = kwargs or {}
        if super(cls, type(obj)).__init__ is object.__init__:
            return                        # object's gets none: they were __new__'s
        super(cls, obj).__init__(*args, **kwargs)
    ns['_oorexx_init'] = staticmethod(_init)

    def __del__(self):
        try:
            oorexx._pair_dead(self)
        except Exception:
            pass                          # Python is shutting down
        d = getattr(super(cls, self), '__del__', None)
        if d is not None:
            d()
    ns['__del__'] = __del__

    ns['_oorexx_overrides'] = overrides
    if not any(issubclass(b, oorexx._Pair) for b in bases):
        # (object goes after _Pair, which is an object too)
        bases = tuple(b for b in bases if b is not object) + (oorexx._Pair,)
    import types
    cls = types.new_class(name, tuple(bases), {}, lambda d: d.update(ns))
    cls._oorexx_class = cls
    return cls
::END
