# ooRexx/Python in a nutshell

Short programs, each showing one thing. Every file starts with the Python it
replaces, as a comment, and ends with an `Output:` block: what it prints,
exactly. `tests/rexx/Examples.testGroup` runs them all with every Python
(3.9–3.13) and checks that output, so they stay true.

Licensed under the Apache License, Version 2.0.

## Running one

```
export ORX_PYTHON=/path/to/python3          # which Python (default: python3)
rexx 01-hello.rex                           # with python.cls and the bridge on REXX_PATH
PYTHONPATH=/path/to/bridge python3 30-run-rexx.py
```

(`./run-one.sh 01-hello.rex [3.11]` does that with the build of `scripts/build.sh`.)

## ooRexx using Python

| File | Shows |
|---|---|
| `01-hello.rex` | `.py~import`, calling a module's functions; big ints |
| `02-values.rex` | what comes back as a Rexx string, what stays a `.PyObject`; `.py~str` |
| `03-names.rex` | Python names from upper-case Rexx symbols; `getattr` for the exact name |
| `04-keywords.rex` | keyword arguments: `.py~kw(...)`, `py.kw(...)`, from a Directory |
| `05-items.rex` | `o[k]`, `o[k] = v`, `DO ... OVER` a Python object |
| `06-errors.rex` | Python exceptions as SYNTAX 98.900, the traceback |
| `07-callbacks.rex` | Rexx routines and objects called from Python |
| `08-box.rex` | `py.box(type, value)`: choosing the Python type (float, bytes, fraction, decimal) |
| `09-threads.rex` | Rexx threads and a Python thread pool, together |

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
| `19-html-parser.rex` | `html.parser`, with the handler in Rexx |

## Data science

| File | Shows |
|---|---|
| `20-numpy.rex` | numpy arrays with Rexx operators, boolean masks, matrices |
| `21-matplotlib.rex` | a chart saved as PNG |
| `22-slices.rex` | slices: `o[.py~slice(start, stop, step)]` for Python's `o[start:stop:step]`, numpy's `m[0:2, 1]` |

## Python using ooRexx

| File | Shows |
|---|---|
| `30-run-rexx.py` | `oorexx.Interpreter`: eval, routine, run a program |
| `31-rexx-objects.py` | Rexx objects in Python: messages, `[]`, `len`, iteration |
| `32-address.py` | an ADDRESS environment written in Python |
| `33-stdio.py` | SAY/PULL through Python's `sys.stdout`/`sys.stdin` |

## Things worth knowing (they show up in the examples)

- Rexx symbols are upper case: `o~name` finds `name`, `Name` or `NAME`, the
  lowercase one first. `logging~INFO` therefore finds the function
  `logging.info`; the constant needs `logging~getattr("INFO")` (18).
- An attribute that is a function or method is called when used without
  arguments; a class is not: `io~StringIO~new` makes an instance (11, 18).
- `.py~list(x)`, `.py~tuple(x)`, `.py~dict(...)`... are Python's builtins.
  A Rexx Array goes to Python as a list (nested Arrays as nested lists), so
  `.py~list(.array~of(1, 2))` is `[1, 2]` (05, 13). `.py~str`, `~int`,
  `~float` are Python's too, but keep the result as a Python object instead
  of a Rexx string (02).
