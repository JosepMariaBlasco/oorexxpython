# ooRexx/Python — design

How the bridge is built and why. The code comments refer to the sections of
this document by name (for example "docs/design.md, *Values*").

The model is [BSF4ooRexx](https://sourceforge.net/projects/bsf4oorexx/),
Rony G. Flatscher's bridge between ooRexx and Java, and its sister in the
[ooRexx/WASM](https://github.com/JosepMariaBlasco/oorexxwasm) project, the
`.JSObject` bridge to JavaScript: proxies by reference, case-insensitive names
with an exact form, values converted only when they are simple. Where this
bridge departs from them, the reason is given.

## Shape

Two directions, which nest freely on any thread (Rexx calls Python, which
calls back a Rexx object, which calls Python again):

- **ooRexx → Python**: an ooRexx extension package, `python.cls`, gives Rexx
  `.py` (Python's builtins plus a few helpers) and `.PyObject` proxies.
- **Python → ooRexx**: a Python extension module, `oorexx`, creates ooRexx
  interpreter instances (`oorexx.Interpreter`), runs Rexx code, and gives
  Python `oorexx.RexxObject` proxies. Python callables can be ooRexx
  `ADDRESS` environments.

Only public APIs are used on both sides: Python's Stable ABI and ooRexx's
native API (`oorexxapi.h`). The interpreter needs no change: the bridge is an
extension package like rxsock or BSF4ooRexx.

### Two libraries

- `oorexx.abi3.so` is the whole bridge: both directions, the proxies, the
  locks, `ADDRESS`. It exports `PyInit_oorexx` and is not linked against
  libpython, as every extension module. One copy of the code and of the
  shared state, which nesting needs.
- `librexxpython.so` is the ooRexx native package that `python.cls`
  requires (`::requires "rexxpython" LIBRARY`), a few dozen lines. If Python
  is already in the process (a Python program running Rexx code that
  requires `python.cls`), it uses it. Otherwise it finds a Python (see *Who
  starts whom*), loads its libpython with `RTLD_GLOBAL`, then
  `oorexx.abi3.so`, and hands ooRexx the bridge's package.

One file cannot do both: Python wants `oorexx.abi3.so` with `PyInit_oorexx`,
ooRexx looks for `lib<name>.so`; and libpython must be loaded globally before
the bridge's `Py*` references are bound (otherwise it fails at once, or works
until a distribution links with `-z now`).

## Python versions

The bridge is compiled once, with `Py_LIMITED_API=0x03090000` against the
3.9 headers, and uses only the **Stable ABI** (PEP 384, PEP 652). The same
binary loads into every CPython from 3.9 up, with no virtual environments and
no per-version builds; the tests run that one build with 3.9, 3.10, 3.11,
3.12, 3.13 and 3.14. Any use of an API outside the Stable ABI would be a design
change.

What it costs: no object internals or fast macros (`PyTuple_GET_ITEM`), no
`PyConfig` (Python is configured through `PYTHONHOME` and `PYTHONPATH`
before `Py_InitializeEx`), and no `PyObject_GetBuffer` before 3.11 (numpy
data goes through ordinary calls such as `tobytes()`). A bridge needs none of
those.

Free-threaded CPython (3.13t, 3.14t) is not supported: the Stable ABI does not cover
it before 3.15.

## Platforms

Linux x86_64, macOS (arm64 and x86_64) and Windows x64, all built and tested by CI
with the same scripts (on Windows, in Git Bash with MSVC). The
platform-dependent parts (finding and loading Python's library, file names,
paths) are kept apart, in the loader and `scripts/platform.sh`.

- **macOS:** the ooRexx package is `librexxpython.dylib`, the library path
  variable `DYLD_LIBRARY_PATH`, which macOS drops when it starts a protected
  program such as `/bin/sh`: a command run through a shell must set it
  itself. The bridge leaves its Python symbols to be bound at load time.
- **Windows:** the bridge is `oorexx.pyd` (linked against `python3.dll`, the
  Stable ABI's), the package `rexxpython.dll`, found on the `PATH`. When Rexx
  starts, the loader loads `python3X.dll` and `python3.dll` from the chosen
  Python's home before the bridge. Python (3.8 and later) loads an extension
  module's DLLs without searching the `PATH`, so the bridge delay-loads
  `rexx.dll` and finds it, when it is imported, in its own folder (where
  `install.rex` puts it), in `OOREXX_HOME\bin` or on the `PATH`. `rexx_path` is separated by `;`.

## Installation

The release zip works where it is unzipped, with the library path,
`REXX_PATH` and `PYTHONPATH` pointing at it, or installed by `install.rex`
(`rexx install` in that folder) into the ooRexx that runs it, with no
variables at all. Installing means putting the three files where each side
already looks, keeping them together: the loader finds the bridge next to
itself, and the bridge loads `python.cls` from its own folder.

- **Windows:** all three in the folder of `rexx.exe` and `rexx.dll`. That
  folder is on the `PATH` (`::requires` finds `python.cls`), Windows looks
  for a program's DLLs there (ooRexx finds `rexxpython.dll`), and the bridge
  finds `rexx.dll` next to itself.
- **Linux and macOS:** ooRexx keeps its `.cls` files in `bin` and its
  libraries in `lib` (it looks for a package's library in `bin/../lib`), so
  the three files go into `lib/oorexxpython/`, with symbolic links
  `lib/librexxpython.so` (`.dylib`) and `bin/python.cls`. The loader and
  the bridge resolve their own folder through the link (`realpath`). The
  bridge carries a run path `$ORIGIN/..` (`@loader_path/..`), so a Python
  that imports it loads the ooRexx it is installed in, not another one the
  system may have.
- **Python:** a file `oorexxpython.pth` in the Python's site-packages (or
  user site-packages) with the folder's path, so `import oorexx` finds the
  bridge. Nothing is copied into Python, any version from 3.9 takes it, and
  each Python that should see the bridge gets its own `.pth`.

- **The uninstaller and the samples:** `install.rex` copies itself next to
  `rexx` as `oorexxpython-uninstall.rex`, which uninstalls by default (it
  knows itself by its name), and the samples into ooRexx's samples folder,
  under `python` (beside `rexx.exe` on Windows, beside `bin/` in the
  portable builds, `share/ooRexx/samples` in the Linux packages). So the
  unzipped folder can be deleted once installed.

What was installed is listed in `oorexxpython-installed.txt` in that folder;
`rexx oorexxpython-uninstall` (or `rexx install uninstall`) removes exactly
what it lists, the uninstaller last. The ooRexx is the one
running `install.rex` (`.RexxInfo~executable`, `.RexxInfo~libraryPath`), so a
portable ooRexx is installed into in the same way as a system one.

## ooRexx → Python

```rexx
np  = .py~import("numpy")
a   = np~array(.array~of(1, 2, 3))
say np~mean(a)                                   -- 2.0
plt = .py~import("matplotlib.pyplot")
plt~plot(a, .py~kw("color", "red"))
plt~savefig("plot.png")

::requires "python.cls"
```

### The .py object

Everything on `.py` is Python's builtin of the same name, unchanged
(`.py~len(x)`, `.py~range(10)`, `.py~list(x)` is `list(x)`), except the
bridge's helpers: `import`, `box`, `kw`, `main`, `eval`, `exec`, `extend`
(*Subclassing*), `raise` (*Exceptions*), `redirectTo` (*Standard streams*), and
`decimal` (`decimal.Decimal`). `.py~str`, `~int`, `~float` are the builtins
too; the only difference from calling them through any other route is that
the result stays a `.PyObject` instead of coming back as a Rexx string
(see *Values*).

To build a list or tuple from items, pass a Rexx Array, which goes to Python
as a list: `.py~list(.array~of(1, 2))` is `[1, 2]`,
`.py~tuple(.array~of("Rexx", 1979))` is `('Rexx', 1979)`.

The routines `py.import`, `py.box` and `py.kw` echo BSF4ooRexx's
`bsf.import`; `.py~redirectTo` and `py.redirectTo` its `bsf.redirectTo`
(see *Standard streams*).

### Names

ooRexx uppercases every message name, quoted ones too, so the bridge
resolves names case-insensitively against the object's attributes (cached
per type), as `.JSObject` does: one match is it; with two or more, the one
starting in lowercase wins if it is unique (numpy 1.x's `nan` / `NaN` /
`NAN` → `nan`); otherwise the error names the candidates.

Exact forms, for ambiguous names and names that do not exist yet:
`o~getattr("name")`, `o~setattr("name", v)`, `o~invoke("name", args...)`.
`.JSObject` uses `o["name"]` for the exact property, but in Python `[]` must
be `__getitem__` (dicts, lists, numpy indexing), so the exact forms take
Python's own names, which no ordinary Python object hides.

One consequence no rule can avoid: `logging~INFO` finds the function
`logging.info` (the lowercase candidate wins), not the constant; the
constant is `logging~getattr("INFO")`.

### Messages, items, iteration

- `o~name(args...)` gets the attribute and calls it; `o~name` without
  arguments calls it only if it is a function or method (classes and modules
  come back as objects); `o~name = v` sets an existing attribute;
  `o~new(args...)` is `o(args...)`.
- `o[k]` is `o.__getitem__(k)`, `o[k] = v` is `__setitem__`; `o[i, j]` is
  `o[(i, j)]`. Indexes are Python's (0-based): it is Python's object.
  During `o[k] = v` the bridge holds a reference to `o` of its own, as a
  Python caller's frame does: pandas looks at `sys.getrefcount(self)` to
  detect chained assignment, and with only the proxy's reference took `df`
  for a temporary (pandas 2 warned; pandas 3 failed looking for the caller's
  frame).
- Slices are Python's `slice` objects: `o[.py~slice(a, b)]` is `o[a:b]`,
  `.py~slice(a, b, c)` is `a:b:c`, and `.nil` leaves a part out
  (`.py~slice(-3, .nil)` is `-3:`). No syntax of its own: `o[i, j]` must stay
  the tuple numpy needs (`m[.py~slice(0, 2), 1]` is `m[0:2, 1]`).
- `do x over o` iterates `iter(o)`; `say o` shows `str(o)`.
- `copy`, `send`, `start` and `run`, which ooRexx's Object already
  understands, go to the Python object when it has an attribute of that name,
  and to Object's method otherwise.

### Operators

Rexx operators on a `.PyObject` (`a + b`, `-a`, `a < b`...) are Python's.
`==` (strict equality) is Python's `is`; `=` is Python's `==`. `hashCode`
is consistent with `==`, so `.PyObject`s work as keys of Rexx collections.

### Values

From Python to Rexx: `str`, `int`, `float`, `bool` (1 / 0), `bytes` and
`None` (`.nil`) come back as Rexx strings; everything else as a `.PyObject`,
by reference. numpy scalars (`np.float64`, `np.int64`...) are converted with
`item()`, subclasses of `int` give their int value, floats use the plain
`repr`.

From Rexx to Python, a Rexx string becomes:

- a canonical integer (`0`, `42`, `-7`: no leading zeros, blanks or `+`) →
  `int` (exact: Python ints are arbitrary precision);
- any other valid Rexx number with a decimal point or an exponent (`3.14`,
  `-0.5`, `1E3`) → `float`;
- anything else (`"007"`, `" 5"`, `"+5"`, text) → `str`.

Rexx Arrays go as `list`s (copies), each item under the same rule.

One exception, by reflection: a `1` or `0` passed to a parameter annotated
`bool` (also `bool | None`, `Optional[bool]`, or a name the function's
module binds to `bool`) goes as `True` or `False`. `.true` and `.false` are
those strings, and libraries that validate their flags (pandas' `inplace=`)
reject an `int`. Only functions, methods and classes written in Python are
looked at, through `inspect.signature`, cached per function; C functions
have no annotations, and any other annotation, or none, leaves the rule
alone. This is BSF4ooRexx's use of Java signatures, as far as Python
allows: annotations are optional and never enforced (numpy and the
standard library have none), so only this lossless case is taken from
them; `py.box` covers the rest.

This departs from `.JSObject`, which passes strings always: JavaScript
coerces strings to numbers, Python never does (`np.array(["1", "2"]).mean()`
fails, `"2" * 3` is `"222"`), so strings-always would need a wrapper on every
numpy or matplotlib argument. The cost: a number-looking string may arrive as
a number (`d["1"]` is not `d[1]`). `.py~str(x)` forces a string;
`.py~int(x)`, `.py~float(x)`, `.py~decimal(x)` force a type (`decimal` keeps
Rexx's precision); for any type, see *Boxing*.

The same rule applies to Rexx results going to Python (see *Python →
ooRexx*).

### Keyword arguments

ooRexx has no named arguments. A marker object as the last argument carries
them: `plt~plot(x, y, .py~kw("color", "red", "linewidth", 2))`, or
`.py~kw(directory)`. Only the marker is taken as keywords: a trailing
Directory or StringTable stays a positional argument, so there is no silent
ambiguity. Keys go exactly as written; values follow *Values*.

### Boxing

`.py~box(type, value)` (and `py.box`) builds a Python object of a given type
from the Rexx value's text, never through the *Values* rule. Types: `str`,
`int`, `float`, `bool`, `bytes`, `complex`, `decimal`, `fraction`, or a
Python class or function. The argument order is that of BSF4ooRexx's `box`,
so that the related APIs of both bridges take their arguments the same way.

### Exceptions

A Python exception becomes SYNTAX 98.900 "Python error: Type: message", with
the traceback in `condition("O")~additional[2]` and the exception itself, a
`.PyObject`, in `additional[3]`.

**A Python exception that goes back to Python is raised again as itself**,
not as an `oorexx.RexxError`: Python calls Rexx code (a callback, a method of
a class made by `.py~extend`), which calls Python, which raises; if the Rexx
code does not trap the SYNTAX condition, the caller in Python gets the
original exception, traceback included. Python's protocols depend on it: a
sequence ends with `IndexError`, an iterator with `StopIteration`, a mapping
says "missing" with `KeyError`. Any other Rexx condition is still a
`RexxError`.

`.py~raise(exc)` raises a Python exception from Rexx code: `exc` is an
exception, or an exception class called with the rest of the arguments
(`.py~raise(.py~IndexError, i)`). It is the same SYNTAX 98.900, so Rexx
code can trap it, and Python gets the exception as itself.

### Subclassing

`.py~extend(base, rexx [, name])` (or `py.extend`) makes a real Python class
deriving from `base` whose methods are a Rexx class's, as BSF4ooRexx's
`bsf.createProxyClass` does for Java:

```rexx
Worker = .py~extend("threading.Thread", .Worker)
w = Worker~new(.py~kw("name", "worker"))
w~start; w~join

::class Worker
::method run
  use arg slot
  say "hello from" slot~self~name
```

- **base**: a Python class, a dotted name (`"http.server.BaseHTTPRequestHandler"`,
  a builtin such as `"dict"`), or an Array of them. Python's
  `types.new_class` makes the class at run time, with any base, written in
  Python or in C, mixins and abstract base classes included: no code
  generation.
- **rexx**: a Rexx class, and each Python object gets its own new instance
  of it; or a Rexx object, shared by all of them. The result is the Python
  class, for Rexx (`cls~new(args)`) and for Python code that instantiates it
  itself (`json.dumps(x, cls=...)`, `socketserver`'s handlers).
- **The methods**: every public method of the Rexx class and of its Rexx
  superclasses but `Object`, except `INIT`, `UNINIT` and names with a dot
  (Rexx's own: a dot cannot be part of a Python name). Its Python name: the
  one given by `::ANNOTATE method do_get python "do_GET"`; else, for a
  `__dunder__` name, the lowercase one; else the base classes' attribute
  that matches without regard to case (`RUN` is `run`, `DOIT` is `doIt`);
  else the lowercase one. Two attributes that match (`foo` and `FOO`) are
  an error that asks for the annotation.
- **The calls**: a method gets the arguments Python passes and, last, a
  StringTable: `SELF`, the Python object; `SUPER`, `super(cls, self)`, so
  `slot~super~run` is the base class's `run`; `KWARGS`, a StringTable of
  the keyword arguments (as BSF4ooRexx appends a Directory of context
  information to a message from Java). Its result follows *Values*; the
  results of special methods are converted as Python requires (`__bool__`,
  `__contains__` to `bool`, `__len__`, `__hash__`, `__index__` to `int`,
  `__str__`, `__repr__`, `__format__` to `str`...).
- **Construction**: the Rexx object is made first, with no arguments (its
  `INIT` is Rexx's own), then Python's `__init__` runs with the constructor's
  arguments: the Rexx class's `__init__` method, if it has one, which calls
  the base's (`slot~super~__init__(...)`), else the base's. The Rexx object
  exists before the base's `__init__` runs, because some bases call
  overridden methods there (a `socketserver` handler does all its work in
  its constructor).
- **Lifetime**: the Python object holds its Rexx object (in an attribute,
  `_oorexx_peer`, through the usual reference of *Lifetime*); the Rexx
  object does not hold the Python object, which it gets with each call. So
  there is no cycle across the two collectors: when Python drops the object,
  the Rexx object goes too. (Rexx code that keeps `SELF` keeps the Python
  object, as it would any other.)
- **Threads**: Python calls the methods on its own threads, which attach to
  the interpreter instance for the call, as any callback; several at once
  need ooRexx 5.3.0.

## Python → ooRexx

```python
import oorexx

with oorexx.Interpreter(rexx_path=["/my/classes"]) as rexx:
    print(rexx.run("hello.rex", "world"))        # CALL a program: its result
    print(rexx.eval("6 * 7"))                    # 42 (an int)
    times = rexx.routine("use arg a, b; return a * b")
    print(times(6, 7))                           # 42
    d = rexx.eval(".directory~new")              # a Rexx object
    d["NAME"] = "Ann"
    print(len(d), d["NAME"])
```

### Interpreter options

`oorexx.Interpreter(...)` exposes `rexx_path` (EXTERNAL_CALL_PATH),
`extensions` (EXTERNAL_CALL_EXTENSIONS), `initial_address`
(INITIAL_ADDRESS_ENVIRONMENT), `load_libraries` (LOAD_REQUIRED_LIBRARY),
`command_environments={"NAME": callable}` (see *ADDRESS in Python*;
`add_command_environment` adds one later), `stdio` and `prefixes` (see
*Standard streams*).

### Standard streams

**Rexx's standard streams go through Python's by default**, as BSF4ooRexx
does with Java's (`bsf.redirectTo`, `BSF.InputStream`, `BSF.OutputStream`):
when Python creates an interpreter instance, the five `.local` monitors get
new destinations, `.input` and `.debugInput` a `.PyInputStream` reading
`sys.stdin`, `.output` a `.PyOutputStream` writing `sys.stdout`, `.error` and
`.traceOutput` one writing `sys.stderr`. So everything Rexx writes reaches
Python: `SAY`, trace output, error messages, and also `.output~lineout`,
`.error~lineout`, `.traceOutput~lineout`; console pulls and `LINEIN()` read
Python's input. Rexx is not mute in Jupyter or wherever Python redirects its
output (`contextlib.redirect_stdout`): the stream objects look up
`sys.stdout`, `sys.stderr`, `sys.stdin` at every call. `stdio=False` leaves
Rexx's own `.stdin`, `.stdout`, `.stderr`.

This replaces an I/O exit (RXSIO), which only sees `SAY`, trace output and
console pulls: what a program writes to the monitors directly bypassed it
and went to the process's file descriptors, invisible in a notebook.

**Each line carries a prefix**, BSF4ooRexx's, so that Rexx's lines stand out
in a log or a notebook: `REXXout>`, `REXXerr>`, `REXXtrc>`; for input,
`REXXin?>` and `REXXdbgIn?>` are written to `.output` as a prompt before
reading. The prefix goes at the start of every line, also after each line
feed inside a string. `prefixes=False` turns them off; a dict changes them
(`prefixes={"output": "rexx> ", "error": None}`, names as the monitors',
case-insensitive; `None` is no prefix). From Rexx, each stream has a
`prefix` attribute: `.output~current~prefix = .nil`.

A program started from Rexx keeps Rexx's streams; it can send them to
Python's with `call py.redirectTo` (or `.py~redirectTo`), BSF4ooRexx's
`bsf.redirectTo` with the same arguments: `py.redirectTo("P", input, output,
error, debugInput, traceOutput, prefixes)`, any argument omitted taking
Python's standard stream (or, for `debugInput` and `traceOutput`, the
stream of `.input` and `.error`), and `"R"` going back to Rexx's own.

Each line is a single `write` call, so lines from several threads do not
mix, and the stream methods are unguarded, as `.PyObject`'s (*Threads and
locks*, rule 5).

`python.cls` is loaded into every instance, so Python objects passed to Rexx
are `.PyObject`s and Rexx code can use `.py`.

### Rexx objects in Python

A Rexx object is an `oorexx.RexxObject`: `o.name(args)` sends the message
(names uppercased), `o[k]` and `o[k] = v` are Rexx's `[]` and `[]=`, `len()`
and iteration work, `str()` is its string value. Rexx results follow the
*Values* rule: `rexx.eval("6 * 7")` is the `int` 42 and a Rexx Array comes
back as a `list` (a copy); other objects stay proxies.

Messages can also be sent by name, with the methods of BSF4ooRexx's
`RexxProxy` (Java), under the same names: `o.sendMessage(name[, args])`
(`args` a list or tuple), `o.sendMessage0(name)` … `o.sendMessage5(name, a1,
…, a5)`, and `o.sendMessageScope(name, scope[, args])`, where the method
search starts at `scope`, a Rexx class, as in Rexx's `o~name:scope`. The name
is any string, uppercased as `o.name` is: one held in a variable, or one that
is no Python name (`"[]="`, `"||"`, `"<>"`). These eight names are Python
methods of `RexxObject`, so `o.sendMessage` never sends a Rexx message of
that name; `o.sendMessage("sendMessage")` does.

A Python thread that uses an interpreter attaches to it (`AttachThread`) for
the duration of the call.

## ADDRESS in Python

```python
def calc(command, ctx):
    try:
        ctx.set_var("RESULT", eval(command, {"__builtins__": {}, "max": max}))
        return 0                                 # RC
    except Exception as e:
        ctx.set_var("RESULT", str(e))
        return 1

with oorexx.Interpreter(command_environments={"CALC": calc}) as rexx:
    rexx.routine("""
        address calc
        '2 ** 10';      say rc result            -- 0 1024
        '1 / 0';        say rc result            -- 1 division by zero
    """)()
```

A Python callable registered as a command environment receives the command
and an `oorexx.CommandContext`: `line_in()` and `lines()` (the redirected
input), `output(line)` and `error(line)`, `is_redirected(...)`, the caller's
variables (`get_var`, `set_var`, `drop_var`), and `raise_error(rc)` /
`raise_failure(rc)`. The return value is RC. The conventions are those of
the command handlers written in Rexx in ooRexx/WASM: a numeric RC, ERROR for
a positive one, FAILURE for a negative one.

## Who starts whom

- **Python starts** (`python3 prog.py` with `import oorexx`): the module
  needs `librexx` and `librexxapi` on the library path, as any program that
  embeds ooRexx.
- **Rexx starts** (`rexx prog.rex` using numpy): the loader must find and
  load a libpython at run time. It takes the Python in `ORX_PYTHON`, else the
  `python3` on the `PATH`; asks that executable for its library, home and
  `sys.path`; loads that libpython with `RTLD_GLOBAL` (otherwise C extension
  modules such as numpy fail with undefined `Py*` symbols); initializes
  Python with `Py_InitializeEx(0)` and `PYTHONHOME` set to the executable's
  home for the initialization only (children started with `ADDRESS SYSTEM`
  do not inherit it), leaving the process's signals to ooRexx; and sets
  `sys.path`, `sys.executable` and `sys.argv` from what the executable said
  (so a virtual environment's packages are found too). Asking the executable
  is what makes the bridge load *that* Python, the one numpy and friends were
  installed for. Python is never finalized: it lives until the process ends.

## Threads and locks

Two global locks meet: CPython's **GIL** (a thread must hold it to use the C
API) and ooRexx's **kernel lock** (taken by every API call; ooRexx releases
it while native code runs). The rules:

1. **Python → Rexx: never hold the GIL while calling an ooRexx API that may
   run Rexx code.** The GIL is released (`PyEval_SaveThread`) around every
   such call and restored after. The deadlock this prevents: a Python thread
   runs Rexx holding the GIL, and that Rexx code waits for another Rexx thread
   (a guarded method, a queue, `REPLY`) which waits for the GIL. This
   includes converting Rexx objects to strings, which sends `string` and may
   run Rexx code. Making a Rexx string from bytes runs no Rexx code and may be
   done holding the GIL.
2. **Rexx → Python: every entry takes the GIL** with `PyGILState_Ensure` /
   `Release`, which works on threads Python has never seen (`START`,
   `REPLY`).
3. **A Python thread entering Rexx attaches** to the instance and detaches
   after.
4. **ooRexx runs native code with the kernel lock released**
   (`NativeActivation::run` releases it before calling a native method and
   takes it back after; the same for routines and exits), so a long Python
   call does not block other Rexx threads.
5. **A proxy method never holds a Rexx guard while in Python.** Every method
   of `.PyObject` and `.Python` but `init` is `UNGUARDED`: a proxy has no
   Rexx state that changes, and Python's state is protected by the GIL. A
   guarded method would hold the object's guard while Python runs, so
   another thread using the same proxy would wait for it: a deadlock when the
   first thread is itself waiting for that one (a Python thread pool calling
   a Rexx routine that uses `.py`, while the Rexx thread is inside
   `.py~sum(pool~map(...))`).

Each native method of the Rexx side therefore works in three phases: Rexx
arguments to plain values (ooRexx API only, no GIL); with the GIL, plain
values to Python objects, the Python work, and the result back to plain
values (Python API only); then the Rexx result, or the Python error raised as
a Rexx condition (ooRexx API only, no GIL).

`tests/rexx/Threads.testGroup` is the stress test of these rules: N Rexx
threads calling Python and M Python threads calling Rexx at once, with
callbacks both ways, under a timeout.

## Lifetime

**Python objects held by Rexx.** A `.PyObject` holds a strong reference.
Its `uninit` is not native: it is two Rexx clauses that send only built-in
messages, so it never gives up the kernel lock, and it only queues the
pointer on `.PyObject~graveyard`. The bridge drains that queue (no GIL) and
drops the Python references (with the GIL) at the start of every native
method and of every call from Python into Rexx. The queue is one for the
whole process, made by the bridge and kept by a global reference: a package
loaded while no other instance is alive is loaded afresh, with a new
`.PyObject` class, and a queue per class would be lost with the last
instance that used it. This avoids ooRexx bug #2101 (an uninit that releases the kernel lock
can crash `runUninits` when other threads create objects meanwhile), present
in every released ooRexx 5.x. The cost: a Python object is freed at the next
bridge call rather than at once, and a few may still be queued when the
program ends.

**Rexx objects held by Python.** The bridge keeps them in a per-instance
Directory (`InstanceData::keep` in `bridge.cpp`) rather than with one global
reference per object, because `ReleaseGlobalReference` does not release in
released ooRexx 5.x (bug #2100). Measured: 0.8–1.1 µs per object for the
Directory, 0.3–0.4 µs for global references, both invisible next to a bridge
call (2–8 µs). Global references are counted, so switching to them once a
fixed ooRexx is the minimum is a small change.

**Interpreters.** A Python object returned by Rexx to Python is given its
own reference before the thread detaches from the instance, so a garbage
collection on another thread cannot free it before the GIL comes back
(`tests/python/t20_returned_objects.py`). The thread that created an
interpreter stays attached until `close()`: detaching it makes `Terminate`
wait forever (ooRexx bug #2107).

**ooRexx versions.** The bridge uses nothing newer than ooRexx 5.2.0's API
(5.3.0's `oorexxapi.h` adds only the version constant), so the minimum is
5.2.0, the current release, and CI runs the tests with it too. One thing
needs 5.3.0 (r13265 or later): using an interpreter from several threads at
once. On 5.2.0 that crashes inside ooRexx, in its tables of activities
(`AttachThread`, `addWaitingActivity`), and the three concurrent tests
(`Threads.testGroup`, `t15_concurrent_calls.py`,
`t20_returned_objects.py`) fail, as can the sample `09-threads.rex` (Python's
thread pool calling a Rexx routine), which `Samples.testGroup` skips there;
everything else passes. 5.2.0 also has
bug #2102, which slows down programs that make many proxies.

**Objects shared between interpreters.** All interpreter instances in a
process share one object memory and one garbage collector, and `.environment`
is common to all of them. So a Rexx object made in one instance can be used
in another, whether Python passes it or Rexx code leaves it in
`.environment`, and it lives as long as any instance reaches it. The bridge
accepts this rather than forbid it (a ban could not see what crosses through
`.environment`), and nothing it keeps belongs to one instance in a way that
would break it:

- `close()` empties the instance's keep Directory before `Terminate`: the
  Rexx objects only Python held become collectable, and those another
  instance reaches survive.
- The Python objects that the instance's `.PyObject`s held are dropped as
  their uninits run, during `Terminate` or in a later collection in any
  instance, through the process-wide graveyard. A `.PyObject` another
  instance still reaches keeps its Python object.
- A Python proxy of a closed instance's object (`oorexx.RexxObject`) cannot
  be used, nor passed to another instance: its object may be gone. ooRexx
  reuses the address of a terminated instance for a later one, so proxies
  identify their instance by a handle of the bridge's, never reused, not by
  the `RexxInstance` pointer.

`tests/python/t30_instances.py` checks all of this.

The ooRexx bugs the bridge has found, and what in the code depends on each,
are listed in [`../notes/oorexx-bugs.md`](../notes/oorexx-bugs.md).

## Names

The Rexx package is `python.cls`, with the entry object `.py` and the proxy
class `.PyObject`; its native library is `librexxpython.so`. The Python
module is `oorexx`, with `oorexx.Interpreter`, `oorexx.RexxObject`,
`oorexx.CommandContext` and `oorexx.RexxError`.

## Licence

Apache License 2.0, as BSF4ooRexx: a patent grant, compatible with Python's
licence and the PyPI world, and no obstacle to distribution with ooRexx.
