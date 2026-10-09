/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * python.cls: Python objects as Rexx objects (the ooRexx/Python bridge).
 *
 *   np = .py~import("numpy")
 *   a  = np~array(.array~of(1, 2, 3))
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
 *             Every other name is Python's builtin, unchanged: .py~list(x) is
 *             list(x); to build a list or tuple from items, pass a Rexx Array:
 *             .py~list(.array~of(1, 2)), .py~tuple(.array~of("a", 1)).
 * Routines, as BSF4ooRexx's bsf.import: py.import(name), py.box(type, x),
 * py.kw(name, value, ...).
 * .PyObject   a Python object, by reference:
 *               o~name(args...)          attribute, called (names in any case)
 *               o~name                   attribute; called if a function/method
 *               o~name = v               set an existing attribute
 *               o[k] / o[k] = v          items (o[i, j] -> o[(i, j)])
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
 * numbers go to Python as int or float; Rexx Arrays as lists (copies).
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
  graveyard = .Queue~new
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

-- BSF4ooRexx-style routines (bsf.import ...), for those who prefer them
::routine py.import public
  use strict arg name
  return .py~import(name)
::routine py.box public
  return .py~sendWith("BOX", arg(1, "A"))
::routine py.kw public
  return .py~sendWith("KW", arg(1, "A"))

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
