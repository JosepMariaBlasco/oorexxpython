# ooRexx/Python — design

How the bridge is built and why. The code comments refer to the sections of
this document by name (for example "docs/design.md, *Values*").
What the bridge does, for those who use it, is in the books of
[`books/`](books): *ooRexx and Python: Reference* and *Cookbook*.

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
3.12, 3.13, 3.14 and 3.15. Any use of an API outside the Stable ABI would be a design
change.

What it costs: no object internals or fast macros (`PyTuple_GET_ITEM`), no
`PyConfig` (Python is configured through `PYTHONHOME` and `PYTHONPATH`
before `Py_InitializeEx`), and no `PyObject_GetBuffer` before 3.11 (numpy
data goes through ordinary calls such as `tobytes()`). A bridge needs none of
those.

Free-threaded CPython (3.13t, 3.14t, 3.15t) is not supported. Its builds
refuse extensions built for `abi3`, as the bridge is. Python 3.15 adds a
Stable ABI for them, `abi3t` (PEP 803), but supporting it would mean a second
build with 3.15 as its minimum, opaque `PyObject`s, the module initialized
through `PyModExport`, and, above all, new locking: the bridge relies on the
GIL (*Threads and locks*), and `abi3t` only makes an extension loadable
without it, not safe. Not planned.

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

- **The Python that Rexx programs use:** recorded as `python = EXE` in
  `oorexxpython.cfg`, in that same folder: `--rexx-python`, else the one an
  earlier install recorded (if it still exists), else the first `--python`
  (its `sys.executable`). The loader reads it when `ORX_PYTHON` is not set
  (*Who starts whom*). Without it, a Python that is not on the `PATH` could
  not be used from Rexx without a variable, and on Windows that is the
  usual case: python.org's installer does not add Python to the `PATH` by
  default, and `python` may then be Windows' alias that opens the Store.

- **Windows installer:** `dist/windows/oorexxpython.nsi` (NSIS), built from
  the Windows zip and the books by `scripts/windows-installer.sh`. It is a
  wrapper around `install.rex`, so the installation is the same on every
  platform: it finds ooRexx (the registry key of ooRexx's own installer, or
  a folder the user picks), the Pythons registered as PEP 514 says (HKCU and
  HKLM, every company: python.org's, uv's), runs `install.rex` with the
  chosen ones, and adds what a Windows user expects and `install.rex` does
  not: the books in ooRexx's `doc` folder, shortcuts in ooRexx's Start menu
  folder, an entry in Settings > Apps, and an uninstaller that runs
  `oorexxpython-uninstall.rex` first.

What was installed is listed in `oorexxpython-installed.txt` in that folder;
`rexx oorexxpython-uninstall` (or `rexx install uninstall`) removes exactly
what it lists, the uninstaller last. The ooRexx is the one
running `install.rex` (`.RexxInfo~executable`, `.RexxInfo~libraryPath`), so a
portable ooRexx is installed into in the same way as a system one.

## ooRexx → Python

```rexx
np  = .py~import("numpy")
a   = np~array((1, 2, 3))
say np~mean(a)                                   -- 2.0
plt = .py~import("matplotlib.pyplot")
plt~plot(a, .py~kw("color", "red"))
plt~savefig("plot.png")

::requires "python.cls"
```

### The .py object

Everything on `.py` is Python's builtin of the same name, unchanged
(`.py~len(x)`, `.py~range(10)`, `.py~list(x)` is `list(x)`), except the
bridge's helpers: `import` (with an optional type, `"string"`: see
*Values*), `box`, `kw`, `main`, `eval`, `exec`, `extend` (*Subclassing*), `raise` (*Exceptions*), `redirectTo` (*Standard streams*), and
`decimal` (`decimal.Decimal`). `.py~str`, `~int`, `~float` are the builtins
too; the only difference from calling them through any other route is that
the result stays a `.PyObject` instead of coming back as a Rexx string
(see *Values*).

To build a list or tuple from items, pass a Rexx Array, which goes to Python
as a list: `.py~list((1, 2))` is `[1, 2]`,
`.py~tuple(("Rexx", 1979))` is `('Rexx', 1979)`.

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
`logging.info` (the lowercase candidate wins), not the constant, and calls
it (see *Messages, items, iteration*), which fails for want of a message;
the constant is `logging~getattr("INFO")`.

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
- `o~makeArrayProxy` is a `.PyArrayProxy`: the sequence `o` as a Rexx
  Array, counting from 1 (BSF4ooRexx's `BSF_Array_Reference`): `[]`, `at`,
  `[]=`, `put` (at `items + 1`, an append), `items`, `size`, `append`,
  `remove`, `hasIndex`, `first`, `last`, `makeArray`, `supplier`. Written in
  Rexx on top of the `.PyObject` (`len`, `o[i - 1]`, `append`, `pop`): it
  copies nothing, so changes go both ways. `o[i]` itself stays Python's, from
  0: Python's indexing is more than positions (keys, tuples, slices, masks,
  negative positions), and one object has one rule.
- `copy`, `send`, `start` and `run`, which ooRexx's Object already
  understands, go to the Python object when it has an attribute of that name,
  and to Object's method otherwise; so does `request` with no arguments
  (Object's needs one): a `socketserver` or `http.server` handler's
  `self~request`.

### Operators

Rexx operators on a `.PyObject` (`a + b`, `-a`, `a < b`...) are Python's.
`==` (strict equality) is Python's `is`; `=` is Python's `==`. `hashCode`
is consistent with `==`, so `.PyObject`s work as keys of Rexx collections.

### Values

From Python to Rexx: `str`, `int`, `float`, `bytes` and `None` (`.nil`)
come back as Rexx strings, `bool` as `.Boolean~true` / `.Boolean~false` (see
*Booleans*); everything else as a `.PyObject`,
by reference. numpy scalars (`np.float64`, `np.int64`...) are converted with
`item()`, subclasses of `int` give their int value. A float is written as
a Rexx number, as the ooRexx bridges for .NET, Lua and JavaScript write
theirs: its shortest digits (those of `repr`) placed as ooRexx writes `x +
0` under `NUMERIC DIGITS 17`, that is, plainly unless the integer part needs
more than 17 places or the decimals more than 34 (`10000000000000000.0`,
`0.0000001`, `0.30000000000000004`), otherwise in scientific form with an
uppercase `E` and a signed exponent (`1E+17`, `1E-35`, `5E-324`); a whole
number written plainly keeps a `.0` (`1.0`, `100.0`), so that it goes back
to Python as a float, since the number rule tells a float by its point (the
bridges whose host does not tell integers from floats by the text write
`1`); `-0.0` is kept; NaN and the infinities are ooRexx's own spellings,
`nan`, `+infinity`, `-infinity` (what ooRexx writes for a double, and reads;
not Rexx numbers). Every such text reads back as the same float through
`.py~float(x)` or `.py~box("float", x)` (which also read Python's `NaN`,
`Infinity`, `inf`); by the number rule, the three non-numbers go as `str`.
Until 0.1.0-preview.13 a float was its `repr` (`1e+16`, `1e-07`, `inf`).
The Python objects of Rexx classes that extend Python classes are
the exception: they come as their Rexx objects, whatever their base (see
*Subclassing*).

From Rexx to Python, a Rexx string becomes:

- a canonical integer (`0`, `42`, `-7`: no leading zeros, blanks or `+`) →
  `int` (exact: Python ints are arbitrary precision);
- any other valid Rexx number with a decimal point or an exponent (`3.14`,
  `-0.5`, `1E3`) → `float`, rounded to the nearest double as any `float` is
  (`3.14159265358979323846264` is `3.141592653589793`; `.py~decimal(x)`
  keeps every digit). `-0.0` is the float `-0.0` (and `-0`, a canonical
  integer, the int `0`). The exception is a number that the float would
  turn into a value of another kind: when it overflows (`1E999`, `-1E999`:
  `inf`) or underflows to zero while it is not zero (`1E-999`), it goes as
  `decimal.Decimal`, exact, and comes back to Rexx as a `.PyObject`, as
  `.py~decimal` does (until 0.1.0-preview.13: `inf` and `0.0`, silently);
- anything else (`"007"`, `" 5"`, `"+5"`, text) → `str`.

Rexx Arrays go as `list`s (copies), each item under the same rule; an empty
slot as `None`, so the list keeps the Array's size. A multi-dimensional Array
goes as lists nested by dimension, the first outermost (`m[i, j]` is
`m[i-1][j-1]`; numpy makes a 2-D array of it), not flattened: its shape is
information (until 0.1.0-preview.13 it was flattened silently). The ooRexx
bridges for JavaScript and Lua do the same, and .NET's makes an array of the
same rank. The copy has the structure of the original: while one value is
converted, an Array met again is the list already made for it, so an Array
that contains itself is a list that contains itself (`a~append(a)`: `x[2] is
x`), Arrays that contain each other keep the cycle, and an Array that appears
twice (`.array~of(x, x)`) is one list; each conversion starts afresh (`f(x,
x)` gets two lists). Arrays nested more than 1000 deep are an error
(`ValueError`, SYNTAX 98.900 in Rexx), since the copy recurses (until
0.1.0-preview.13 a cycle crashed the process). The way back is not symmetric: a list of lists comes to Rexx as a
`.PyObject`, as any list. A
`.Boolean` goes as a `bool` (see *Booleans*).

### Booleans

In Rexx, `.true` and `.false` are the strings `"1"` and `"0"`: nothing tells
`.false` from `0`, so a Rexx program cannot hand a real boolean to Python as
a value. The ooRexx bridges (for Python, .NET, Lua and JavaScript) share one
class for that, `.Boolean`, from `boolean.cls`: two instances,
`.Boolean~true` and `.Boolean~false`, that are `"1"` and `"0"` in every Rexx
context (`IF`, operators, `DATATYPE`, string methods, stem and collection
indexes) and answer `isA(.Boolean)`. The class is proposed for the ooRexx
core (RFE #880, the name still open: `.Boolean` or `.Logical`); until it is
there, each bridge carries the same verbatim copy (`src/boolean.cls`, the
family's **version 2**, its `Version:` line; its tests
`tests/rexx/boolean-test.rex`), CPL 1.0 as ooRexx. Like a core class, it is
visible everywhere: its prolog puts it in `.environment` as `BOOLEAN`, unless
an entry is there already (a core `.Boolean`, or the copy another package
loaded first). A program that requires two bridges gets one `.Boolean`, and
so do several interpreter instances in one process: ooRexx shares a package
loaded from the same file (measured; only a copy loaded explicitly from
another path is another class).

- Python's `True` and `False` come to Rexx as `.Boolean~true` and
  `.Boolean~false` (until 0.1.0-preview.13: the strings `1` and `0`). Rexx
  programs see no difference, and a bool that goes through Rexx and back is
  a bool.
- Any Rexx object with `isA(.Boolean)` (a subclass's too) goes to Python as
  a `bool`, also to a module imported as `"string"`. `.true` and `.false`
  are still the strings `1` and `0`: the number rule, and the `bool`
  annotation below.
- `python.cls` requires `boolean.cls`; the Rexx code that Python runs
  (`oorexx.Interpreter`), which has no `::requires`, finds `.Boolean` in
  `.environment`.
- Installing (`install.rex`): `boolean.cls` goes next to `python.cls`, and
  every ooRexx bridge that installs into ooRexx's `bin/` uses the one there.
  A `bin/boolean.cls` already there is kept unless ours has a higher
  `Version:` line (version 1 has none: 1); uninstalling removes it only when
  no other bridge's package (`net.cls`, `lua.cls`, `js.cls`) is left in
  `bin/`, and otherwise leaves a plain copy where our link was.

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

Two alternatives were measured on every sample, and rejected. Strings
always, with a type declared per module at import as the way to get numbers:
even with numpy, pandas and matplotlib declared, 19 of the 31 samples broke
(`math~sqrt(2)`, `datetime~date(2026, 10, 7)`, `random~randint(1, 6)`,
every `o[i]` on a list or tuple), one silently (`self * 9` on an `int`
subclass is the string "9" repeated). Python's type hints (typeshed's stubs,
the packages' own, inline annotations), read per call: they decide where they
ask for one type (`math.sqrt`, `np.linspace`, and `str` for `os.getenv`,
`os.path.join`, `re.compile`), but most of the rest are type variables,
`Any` or unions with `str` (`np.array`, `dict(...)`, `slice`,
`random.seed`'s `int | float | str | ...`), so the same value would go as a
number or as a string depending on how a stub was written, and differently
with another version of the library; reading them took a type checker's
work (aliases, generics, protocols, overloads chosen by the whole call) and
0.1 to 0.5 s per program, on a short program's 0.2 s. A rule the programmer
can apply by looking at the value was preferred to both.

**Modules imported as "string".** `.py~import(name, "string")` marks the
module's `.PyObject` (`pystrings`, an argument of `PyObject`'s `init`), and
every `.PyObject` made from a call, attribute or item of a marked one is
marked too (`RexxSide::strings`). While a marked object converts its
arguments, and the values it stores (`o[k] = v`, `o~x = v`), every Rexx
string goes as a `str` (`argStrings`, set per native method by
`StringsScope`). Keys and positions in `o[k]` keep the rule, as do Rexx
results returned to Python; the `bool` exception stays. `"number"` is the
default, the rule. Scoping by reach rather than by the callee's
`__module__` makes `os~path~join` count as `os` (whose `path` is
`posixpath`).

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
the exception itself, a `.PyObject`, in `condition("O")~additional[2]` and
the traceback, as text, in `additional[3]`. This is BSF4ooRexx's layout
(`[1]` the message, `[2]` the host's exception, `[3]` its traceback), which
the ooRexx bridges for .NET, Lua and JavaScript share. (Until
0.1.0-preview.13 the two were the other way round.) The condition stays
98.900, "execution error", where BSF4ooRexx raises 40.900: a Python call is
not a call to a Rexx routine.

**A Python exception that goes back to Python is raised again as itself**,
not as an `oorexx.RexxError`: Python calls Rexx code (a callback, an
override of a Rexx class that extends a Python class), which calls Python,
which raises; if the Rexx
code does not trap the SYNTAX condition, the caller in Python gets the
original exception, traceback included. Python's protocols depend on it: a
sequence ends with `IndexError`, an iterator with `StopIteration`, a mapping
says "missing" with `KeyError`. Any other Rexx condition is still a
`RexxError`.

A `RexxError` carries the condition's parts as attributes (`condition`,
`code`, `rc`, `message`, `errortext`, `position`, `program`, `description`,
`traceback`),
as Tom Dyer's bridge in Alchemy does: Python code can tell errors apart, or
log where they happened, without parsing the text. They are read from the
condition (`DecodeConditionInfo`) under the kernel lock, when it is cleared,
and set on the exception with the GIL (`takeCondition`, `raiseRexxError` in
`bridge.cpp`); `None` when the error is not a Rexx condition. `traceback` is
not in `DecodeConditionInfo`'s result: it is the condition object's
`TRACEBACK` List, its lines joined by line ends, as Rexx shows them.

`.py~raise(exc)` raises a Python exception from Rexx code: `exc` is an
exception, or an exception class called with the rest of the arguments
(`.py~raise(.py~IndexError, i)`). It is the same SYNTAX 98.900, so Rexx
code can trap it, and Python gets the exception as itself.

### Subclassing

A Rexx class extends a Python class, so that Python calls the Rexx methods
where it would call the class's own, as `.js~extend` does in ooRexx/WASM and
`.net~extend` in ooRexx/.NET: the Rexx class is a subclass of `PyObject`,
and `.py~extend(class, base)` (or `py.extend`), once per class (its
`ACTIVATE` class method is the natural place), makes the Python class:

```rexx
w = .Worker~new(.py~kw("name", "worker"))
w~start; w~join

::class Worker subclass PyObject
::method activate class
  .py~extend(self, "threading.Thread")
::method run
  say "hello from" self~name
```

- **base**: a Python class, a dotted name (`"http.server.BaseHTTPRequestHandler"`,
  a builtin such as `"dict"`), or an Array of them. Python's
  `types.new_class` makes the class at run time, with any base, written in
  Python or in C, mixins and abstract base classes included: no code
  generation. It is named as the Rexx class and has one more base,
  `oorexx._Pair` (no state, so any base takes it), by which the bridge
  recognizes its instances. `.py~extend` answers it, and
  `.Worker~pyClass` is it later, for Python code that makes the objects
  itself (`json.dumps(x, cls=.MyEncoder~pyClass)`, `socketserver`'s
  handlers). It reaches the Rexx class only through a `WeakReference`: the
  Rexx class holds the Python class, not the other way round (a cycle across
  the two collectors would never be collected). The interpreter instance
  keeps every extended Rexx class (`.local["PY.EXTENDED"]`), as long as it
  lives: Python code may hold the Python class after Rexx dropped every
  reference to the Rexx class and its package (a routine made with
  `Interpreter.routine`, called once), and without it the Rexx collector
  took the class while Python still made objects of it ("the Rexx class of
  this Python class is gone"; seen about once in 400 runs of t50's stress
  test). Classes live that long in Python too.
- **The overrides**: every public method of the Rexx class and of its
  superclasses below `PyObject`, except `INIT`, `UNINIT`, `UNKNOWN` and
  names with a dot (Rexx's own: a dot cannot be part of a Python name),
  looked at when `extend` runs. Its Python name: the one given by
  `::ANNOTATE method do_get python "do_GET"`; else, for a `__dunder__`
  name, the lowercase one; else the base classes' attribute that matches
  without regard to case (`RUN` is `run`, `DOIT` is `doIt`); else the
  lowercase one. Two attributes that match (`foo` and `FOO`) are an error
  that asks for the annotation. `__new__`, `__init__`, `__del__` and the
  like cannot be overridden: `INIT` and `UNINIT` do that work.
- **The calls**: Python calls an override with its positional arguments,
  and a `.PyKwArgs` last if it passes keyword arguments
  (`kw["by"]`). `self` is the Rexx object. The result follows *Values*;
  the results of special methods are converted as Python requires
  (`__bool__`, `__contains__` to `bool`, `__len__`, `__hash__`, `__index__`
  to `int`, `__str__`, `__repr__`, `__format__` to `str`...).
- **The base's members**: `self~name:super(args)` is the base's own member,
  not the override (`PyObject`'s `UNKNOWN` resolves a name the class
  overrides through Python's `super()`). A member that is not overridden is
  reached as with any `.PyObject`: `w~start`.
- **Construction**: `.Worker~new(args)` makes the Rexx object, whose `INIT`
  calls `self~init:super(args)` (or has no `INIT`, and `PyObject`'s gets the
  arguments): that runs the base's `__new__` with the arguments (immutable
  bases, `str`, `int`, `float`, `tuple`, `bytes`, take their value there, so
  `INIT` can change it: `self~init:super("<" || name || ">")`), and then the
  base's `__init__` (`object`'s gets none: with `object` as the only base,
  arguments are an error, as in Python). The Rexx object has its Python
  object before the base's `__init__` runs, because some bases call
  overrides there (a `socketserver` handler does all its work in its
  constructor). When Python makes one itself (`Worker(...)`,
  `cls.__new__(cls, ...)`), the class's `__new__` makes the Rexx object
  first, with the same arguments (keyword arguments as a `.PyKwArgs`), and
  its `INIT` makes the Python object; a Python subclass of the class gets an
  object of its own class. An `INIT` that does not call `init:super` leaves
  the Rexx object without a Python object: using it is an error that says
  so. A base that calls an override from its `__new__` is not supported.
- **Identity**: the Python object comes back to Rexx as the Rexx object
  itself, a `Worker`, even for `str` or `int` bases (not as a Rexx string):
  `.py~list((w,))[0] == w`.
- **Lifetime**: the Rexx object and its Python object are a pair, which
  lives while Rexx holds the one or Python holds the other, and the
  collectors release it: nothing needs a hand (no `detach`). Each collector
  sees only its own side, so the bridge lets the link that keeps the other
  half alive point one way at a time. While Rexx holds the Rexx object, it
  holds its Python object (a reference), and the Python object reaches it
  through the bridge without holding it. When Rexx no longer does, the
  bridge holds the Rexx object, and the Rexx object no longer holds the
  Python object, so Python's collector decides: if Python no longer holds it
  either, the pair is over; if Python hands it to Rexx again (passes it, or
  calls an override), Rexx holds it again. The bridge learns that Rexx no
  longer holds it from an `UNINIT`: not the object's own, which runs only
  once, but a small sentinel's (`.PyPeerSentinel`), held only by the Rexx
  object and holding it, a new one each round, whose `UNINIT` queues it on
  the graveyard (see *Lifetime*: no native code in an `UNINIT`). Crossings
  are counted, so that a sentinel that missed one only arms a new one. The
  Python object's `__del__` says when it is gone (weak references would not
  do: instances of subclasses of `int`, `tuple` and `bytes` cannot have
  them). The class's `UNINIT`, if it has one, runs once, when the pair is
  over: the Python object is gone by then, so it cannot use it. The details
  are in `bridge.cpp`, *Pairs*; `Subclass.testGroup` and
  `t50_subclass.py` force both collectors.
- **Threads**: Python calls the overrides on its own threads, which attach
  to the interpreter instance for the call, as any callback; several at once
  need ooRexx 5.3.0. The overrides are Rexx methods, guarded unless declared
  `UNGUARDED`: a call on one thread waits while another thread is in a
  guarded method of the same object.

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

Two proxies of the same Rexx object are `==`, with the same hash (the
object's address: ooRexx does not move live objects, and the proxy keeps
its object alive), so either serves as a dict key; they are not `is`.
Comparing does not call Rexx's `=`, which would enter the interpreter for
every comparison and hash.

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
  one an installation recorded in `oorexxpython.cfg` next to the loader
  (*Installation*), else the `python3` on the `PATH`; asks that executable for its library, home and
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
