# ooRexx/Python in a nutshell

Short programs, each showing one thing. Every file starts with the Python it
replaces, as a comment, and ends with an `Output:` block: what it prints,
exactly. `tests/rexx/Samples.testGroup` runs them all with every Python
(3.9–3.15) and checks that output, so they stay true.

Licensed under the Apache License, Version 2.0.

## Running one

```
export ORX_PYTHON=/path/to/python3          # which Python (default: python3)
rexx 01-hello.rex                           # with python.cls and the bridge on REXX_PATH
PYTHONPATH=/path/to/bridge python3 30-run-rexx.py
```

(`./run-one.sh 01-hello.rex [3.11]` does that with the build of `scripts/build.sh`.)

## As slides

`slides/samples.html` is every sample as one slide deck (open it in a
browser): each sample's code, the Python it replaces and its output. It is
made with [md2slides](https://rexx.epbcn.com/rexx-parser/doc/utilities/md2slides/),
the Rexx Parser's slide tool, in the RexxLA skin. Its sources are in the
repository: `slides/samples.md`, generated from the samples and this README
by `scripts/samples-deck.rex` (edit those, not the deck), and the skin
(`skin-default.css`, `master-default.css`, the logos in `img/`, credited in
`img/README.md`); `scripts/samples-html.sh` builds the HTML.

## ooRexx using Python

| File | Shows |
|---|---|
| `01-hello.rex` | `.py~import`, calling a module's functions; big ints |
| `02-values.rex` | what comes back as a Rexx string, what stays a `.PyObject`; `.py~str` |
| `03-names.rex` | Python names from upper-case Rexx symbols; `getattr` for the exact name |
| `04-keywords.rex` | keyword arguments: `.py~kw(...)`, `py.kw(...)`, from a Directory |
| `05-items.rex` | `o[k]`, `o[k] = v`, `DO ... OVER` a Python object |
| `06-errors.rex` | Python exceptions as SYNTAX 98.900: the exception, its traceback |
| `07-callbacks.rex` | Rexx routines and objects called from Python |
| `08-box.rex` | `py.box(type, value)`: choosing the Python type (float, bytes, fraction, decimal) |
| `09-threads.rex` | Rexx threads and a Python thread pool, together |
| `27-string-module.rex` | number-looking text: `.py~str(x)`, or a module imported as `"string"` |
| `28-address.rex` | an ADDRESS environment written in Python, registered and removed from Rexx: `.py~addCommandEnvironment`, `.py~removeCommandEnvironment` |

## The Python standard library, from ooRexx

| File | Module |
|---|---|
| `10-json.rex` | `json`: dumps, loads |
| `11-csv.rex` | `csv`: DictReader, writer |
| `12-statistics.rex` | `statistics`: mean, median, mode, pstdev |
| `13-sqlite3.rex` | `sqlite3`: an in-memory SQL database |
| `14-datetime.rex` | `datetime`: dates, timedelta, date arithmetic |
| `15-random.rex` | `random` (seeded) |
| `16-uuid.rex` | `uuid`: uuid5, uuid4 |
| `17-urllib.rex` | `urllib.parse`: urlparse, urlencode, quote |
| `18-logging.rex` | `logging`: logger, handler, formatter |
| `19-html-parser.rex` | `html.parser`, extended by a Rexx class (`.py~extend`) |

## Data science

| File | Shows |
|---|---|
| `20-numpy.rex` | numpy arrays with Rexx operators, boolean masks, matrices |
| `21-matplotlib.rex` | a chart saved as PNG |
| `22-slices.rex` | slices: `o[.py~slice(start, stop, step)]` for Python's `o[start:stop:step]`, numpy's `m[0:2, 1]` |
| `23-pandas.rex` | a pandas DataFrame from CSV text: a new column, `inplace=True`, a boolean mask, groupby |

## Python classes in Rexx

| File | Shows |
|---|---|
| `24-subclass.rex` | `.py~extend`: Rexx classes that extend `threading.Thread` and `collections.abc.Sequence`; `.py~raise` |
| `25-immutable.rex` | `.py~extend` on immutable bases: a `str` whose value INIT sets, an `int` with a method |
| `26-array-proxy.rex` | `~makeArrayProxy`: a Python list as a Rexx Array, counting from 1 |

## Python using ooRexx

| File | Shows |
|---|---|
| `30-run-rexx.py` | `oorexx.Interpreter`: eval, routine, run a program |
| `31-rexx-objects.py` | Rexx objects in Python: messages, `[]`, `len`, iteration |
| `32-address.py` | an ADDRESS environment written in Python |
| `33-stdio.py` | Rexx's streams through Python's, with prefixes (`REXXout>`) |
| `34-send-message.py` | messages by name: `sendMessage`, `sendMessage0`…`5`, `sendMessageScope` (as BSF4ooRexx) |

## Things worth knowing (they show up in the samples)

- Rexx symbols are upper case: `o~name` finds `name`, `Name` or `NAME`, the
  lowercase one first. `logging~INFO` therefore finds the function
  `logging.info`; the constant needs `logging~getattr("INFO")` (18).
- An attribute that is a function or method is called when used without
  arguments; a class is not: `io~StringIO~new` makes an instance (11, 18).
- `.py~list(x)`, `.py~tuple(x)`, `.py~dict(...)`... are Python's builtins.
  A Rexx Array goes to Python as a list (nested Arrays, and the dimensions of
  a multi-dimensional Array, as nested lists), so
  `.py~list((1, 2))` is `[1, 2]` (05, 13). `.py~str`, `~int`,
  `~float` are Python's too, but keep the result as a Python object instead
  of a Rexx string (02).
- `.true` and `.false` are the Rexx strings `1` and `0`, so they go to
  Python as ints, except to a parameter annotated `bool`, where they go as
  `True` and `False` (pandas' `inplace=`, 23). For a real `bool`, use
  `.LogicalWrapper~true` and `~false` (ooRexx's `logicalwrapper.cls`, which
  a program that names them requires), which are `1` and `0` for Rexx;
  Python's `True` and `False` come back as them (10).
- A Rexx class that extends a Python class is a subclass of `PyObject` that
  calls `.py~extend(self, base)` in its `ACTIVATE` class method. Its INIT
  passes the arguments to the base with `self~init:super(...)`, and
  `self~name:super(...)` is the base's own member (19, 24, 25).
