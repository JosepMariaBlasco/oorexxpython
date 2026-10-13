# ooRexx and Python

Working draft

*This is the living version of the specification: it follows the implementation and changes with it. How the bridge is built, and why each choice was made, is in [`design.md`](design.md); the code refers to that document's sections.*

## What this is

This document specifies a native bridge between ooRexx and Python 3, both ways. An ooRexx program uses Python modules (numpy, pandas, matplotlib, the standard library) as Rexx objects; a Python program creates ooRexx interpreters, runs Rexx code and uses Rexx objects as Python objects. Both directions nest freely, on any thread: Rexx calls Python, which calls back a Rexx object, which calls Python again. All of it is implemented, tested on Linux, macOS and Windows, and released as [binaries](https://github.com/JosepMariaBlasco/oorexxpython/releases). It is offered for discussion.

From Rexx, any Python object is a `.PyObject`, by reference, used with ordinary messages. The way in is `.py`, which `::requires "python.cls"` defines: it holds Python's builtins (`len`, `list`, `range`...) and a few helpers, `import` first among them.

```rexx
np = .py~import("numpy")
m  = np~array(((1, 2), (3, 4)))
say m~sum m~shape                               -- 10 (2, 2)
say .py~round(np~linalg~det(m), 2)              -- -2.0
plt = .py~import("matplotlib.pyplot")
plt~plot((1, 4, 9), .py~kw("color", "red"))
plt~savefig("plot.png")

::requires "python.cls"
```

From Python, the module `oorexx` gives interpreters and Rexx objects:

```python
import oorexx

with oorexx.Interpreter() as rexx:
    print(rexx.eval("6 * 7"))                   # 42
    times = rexx.routine("use arg a, b; return a * b")
    print(times(6, 7))                          # 42
    d = rexx.eval(".directory~new")             # a Rexx object
    d["NAME"] = "Ann"
    print(len(d), d["NAME"])                    # 1 Ann
```

**One build for every Python.** The bridge uses only Python's Stable ABI, so the same binary works with CPython 3.9 and every later version, with no virtual environments and no per-version builds. It uses only ooRexx's public native API, so the interpreter needs no change: the bridge is an extension package, like rxsock or BSF4ooRexx. The minimum ooRexx is 5.2.0; using one interpreter from several threads at once needs 5.3.0.

**Which Python.** A program started by Python uses that Python. A program started by `rexx` uses the Python named by the environment variable `ORX_PYTHON`, or else the `python3` on the `PATH`, with that Python's own packages (a virtual environment's too).

## .py

Everything on `.py` is Python's builtin of the same name, unchanged: `.py~len(x)` is `len(x)`, `.py~isinstance(x, c)`, `.py~sorted(x)`, `.py~IndexError`. Besides them:

| Method | Does |
| --- | --- |
| `~import(name)` | a module: `.py~import("os.path")` |
| `~kw(name, value, ...)`, `~kw(directory)` | keyword arguments (see [Messages](#messages)) |
| `~str(x)`, `~int(x)`, `~float(x)`, `~decimal(x)` | Python's `str`, `int`, `float`, `decimal.Decimal`, kept as a `.PyObject` (see [Values](#values)) |
| `~box(type, x)` | a Python object of the given type, made from `x`'s text (see [Values](#values)) |
| `~eval(text)`, `~exec(text)`, `~main` | Python's `eval` and `exec` in the module `__main__`, and that module |
| `~extend(class, base)` | the Rexx class extends the Python class (see [Classes](#classes)) |
| `~raise(exception, args...)` | raises a Python exception (see [Errors](#errors)) |
| `~redirectTo(...)` | sends Rexx's standard streams to Python's (see [Streams](#streams)) |

The routines `py.import`, `py.box`, `py.kw`, `py.extend` and `py.redirectTo` are the same, named as BSF4ooRexx's `bsf.import`, `box` and `bsf.redirectTo`.

## Messages

`o~name(a, b)` calls the attribute `name` with the arguments. `o~name` without arguments gives the attribute, or calls it when it is a function or a method (classes and modules are returned, so that `np~linalg~det` works). `o~name = v` sets an attribute that exists. `o~new(args...)` calls the object itself: `.py~import("collections")~Counter~new("abba")` is `collections.Counter("abba")`.

ooRexx uppercases message names, so they are resolved case-insensitively against the object's attributes: a single match is the one; of several, the one that starts in lowercase, if it is the only one (numpy's `nan`, not `NaN`); otherwise the error names the candidates. The exact forms are `o~getattr("name")`, `o~setattr("name", v)` and `o~invoke("name", args...)`. They take Python's own names (`getattr`, `setattr`), because `o["name"]` must stay Python's indexing.

`o[k]` and `o[k] = v` are Python's indexing: `d["key"]`, `m[0, 1]` (the tuple `(0, 1)`, as numpy wants it). Positions count from 0, as in Python: it is Python's object. `.py~slice(a, b [, step])` is `a:b:step`, with `.nil` leaving a part out: `x[.py~slice(-3, .nil)]` is `x[-3:]`.

`o~makeArrayProxy` gives a sequence (a list, a tuple, a 1-D numpy array) as an object that behaves as a Rexx Array, counting from 1, as BSF4ooRexx's `BSF_Array_Reference`: `a[1]` is the first item, `a~items` the length, `a[a~items + 1] = v` and `a~append(v)` append, `a~remove(i)` takes one out, and `do x over a`, `a~makeArray` and `a~supplier` work. It reads and writes through to the Python object (`a~pyObject`): nothing is copied. Outside 1 to `~items`, `a[i]` is `.nil`, as in a Rexx Array.

Keyword arguments go in a marker as the last argument: `plt~plot(x, y, .py~kw("color", "red", "linewidth", 2))`, or `.py~kw(directory)`. Only the marker is taken as keywords: a Directory or StringTable at the end stays a positional argument. Keys go as written, exactly.

| Rexx | Python |
| --- | --- |
| `do x over o` | `for x in o` |
| `say o`, `o~string` | `str(o)` |
| `a + b`, `-a`, `a < b`, `a // b`... | Python's operators |
| `a = b`, `a \= b` | `a == b`, `a != b` |
| `a == b`, `a \== b` | `a is b`, `a is not b` |
| `o~hashCode` | consistent with `=`, so a `.PyObject` can be the index of a Rexx collection |
| `o~copy`, `o~send`, `o~start`, `o~run`, `o~request` | Python's, when the object has that attribute (`request` with no arguments); Rexx's Object methods otherwise |

## Values

Python's `str`, `int`, `float`, `bool` (as 1 or 0) and `bytes` come back as Rexx strings, and `None` as `.nil`; numpy's scalars (`np.float64`, `np.int64`...) as their values. Everything else comes back as a `.PyObject`, by reference: lists, dicts, tuples, arrays, functions, classes, modules.

A Rexx string goes as:

- an `int` when it is a whole number written canonically (`0`, `42`, `-7`: no leading zeros, blanks or `+`);
- a `float` when it is any other Rexx number with a decimal point or an exponent (`3.14`, `1E3`);
- a `str` otherwise (`"007"`, `" 5"`, `"+5"`, any text).

So `np~array((1, 2))~mean` works, as Python never turns a string into a number by itself. One exception, taken from the function's own annotations: a `1` or `0` passed to a parameter annotated `bool` goes as `True` or `False`, so `.true` works for pandas' `inplace=`. A Rexx Array goes as a `list`, item by item under the same rule, as a copy. A `.PyObject` goes as its Python object. Any other Rexx object goes by reference, as an `oorexx.RexxObject` (see [Python using Rexx](#python-using-rexx)), and comes back to Rexx as itself.

When the rule picks the wrong type, a type can be forced. `.py~str(x)`, `~int(x)`, `~float(x)` and `~decimal(x)` (the latter keeping all of Rexx's digits) make the Python object and keep it as a `.PyObject`, so it is not converted back. `.py~box(type, x)` makes an object of any type from `x`'s text, never through the rule: `str`, `int`, `float`, `bool`, `bytes`, `complex`, `decimal`, `fraction`, or a Python class or function. Its argument order is BSF4ooRexx's.

## Errors

A Python exception is SYNTAX 98.900, "Python error: TypeError: …", with the traceback in `condition("O")~additional[2]` and the exception itself, a `.PyObject`, in `additional[3]`.

A Rexx condition that reaches Python is an `oorexx.RexxError` (a `RuntimeError`), whose text is Rexx's message: "Rexx error 42.3: Arithmetic overflow; divisor must not be zero."

A Python exception that goes through Rexx code back to Python is raised again as itself: Python calls Rexx (a callback, an override of a class that extends a Python class), the Rexx code calls Python, which raises, and if the Rexx code does not trap the condition, the Python caller gets the original exception, with its traceback. Python's protocols depend on it: a sequence ends with `IndexError`, an iterator with `StopIteration`, a mapping says "missing" with `KeyError`. `.py~raise(exception)` raises one from Rexx code: an exception object, or a class called with the remaining arguments, `.py~raise(.py~IndexError, i)`. Rexx code can trap it as any SYNTAX condition.

## Classes

A Rexx class can extend a Python class, so that Python calls the Rexx methods where it would call the class's own, as `.js~extend` does in ooRexx/WASM and `.net~extend` in ooRexx/.NET:

```rexx
c = .MyCar~new("Seat", 2020)
say c~describe                    -- Car's describe() calls self.age(): "Seat is 7"

::class MyCar subclass PyObject
::method activate class
  .py~extend(self, .py~import("cars")~Car)    -- a Python class, or its dotted name
::method init
  use arg name, year
  self~init:super(name, year)     -- Car(name, year)
::method age
  return self~age:super + 1       -- Car's own age()

::requires "python.cls"
```

`.py~extend(class, base)`, once per class (its `ACTIVATE` class method is the natural place), makes a Python class that extends `base`, named as the Rexx class, and answers it; `.MyCar~pyClass` is it later. `base` is a Python class, a dotted name (`"http.server.BaseHTTPRequestHandler"`), or an Array of them. The class must be a subclass of `PyObject`.

- **What is overridden.** Every method of the Rexx class, and of its superclasses below `PyObject`, but `INIT`, `UNINIT`, `UNKNOWN`, private ones and names with a dot, looked at when `extend` runs. Its Python name is the one an annotation gives, `::annotate method do_get python "do_GET"`; else, for a `__dunder__` name, the lowercase one; else the base's attribute that matches without regard to case (`RUN` is `run`, `DOIT` is `doIt`); else the lowercase one. When two of the base's attributes match (`foo` and `FOO`), it is an error asking for the annotation.
- **The call.** Python calls an override with its arguments, and a `.PyKwArgs` last if it passes keyword arguments (`kw["by"]`). Its result follows [Values](#values). Special methods are converted as Python requires: a Rexx `0` from `__bool__` is `False`, a `"7"` from `__len__` is `7`, a `42` from `__repr__` is `'42'`.
- **Making one.** `.MyCar~new(args...)` makes the Rexx object; its `INIT` calls `self~init:super(args...)`, which runs the base's `__new__` and `__init__` with them. Immutable bases (`str`, `int`, `tuple`) take their value there, so `INIT` can change it: `self~init:super("<" || name || ">")`. A base constructor that calls an override reaches the Rexx method. Python code that makes one itself (`MyCar("Seat", 2020)`, `json.dumps(x, cls=.MyEncoder~pyClass)`, `socketserver`'s handlers) gets the same: the Rexx object, its `INIT`, then the Python object.
- **The base's members.** `self~name:super(args)` is the base's own member, not the override. A member that is not overridden is reached as with any `.PyObject`: `c~name`.
- **Identity.** The Python object comes back to Rexx as the Rexx object itself, even when its base is `str` or `int`: `.py~list((c,))[0] == c`, and it is a `MyCar`.
- **Lifetime.** The Rexx object and its Python object are a pair, which lives while Rexx holds the one or Python holds the other, and the collectors release it, as they release everything else: nothing needs a hand. Each collector sees only its own side, so the bridge lets the link that keeps the other half alive point one way at a time. While Rexx holds the Rexx object, it holds its Python object. When Rexx no longer does, the bridge holds the Rexx object, and the Python object is held by Python alone, so that Python's collector decides: if Python no longer holds it either, the pair is over; if Python hands it to Rexx again, Rexx holds it again. The class's `UNINIT`, if it has one, runs once, when the pair is over, not when only Rexx lets it go; the Python object is gone by then. A Rexx class that extends a Python class, and the Python class it makes, are released as any class is.
- **Threads.** Python may call the overrides on any of its threads. Several at once need ooRexx 5.3.0.

## Python using Rexx

`oorexx.Interpreter(...)` creates an ooRexx interpreter instance, and `close()` (or the end of a `with` block) terminates it. Its options are ooRexx's instance options: `rexx_path` (where `CALL` and `::requires` look), `extensions`, `initial_address`, `load_libraries`, `command_environments` (see [ADDRESS](#address)), and `stdio` and `prefixes` (see [Streams](#streams)). A process may have several interpreters at once.

| Python | Does |
| --- | --- |
| `rexx.run(file, args...)` | `CALL`s a program, and gives its result |
| `rexx.eval(expression)` | the value of a Rexx expression |
| `rexx.routine(code)` | a Rexx routine from source, as a Python callable |
| `rexx.add_command_environment(name, f)` | a new `ADDRESS` environment |

Rexx's results follow [Values](#values) the other way: a Rexx string that is a number comes as an `int` or `float`, any other as a `str`, `.nil` as `None`, an Array as a `list` (a copy), a `.PyObject` as its Python object, and any other Rexx object as an `oorexx.RexxObject`, by reference:

| Python | Rexx |
| --- | --- |
| `o.name(a, b)` | `o~name(a, b)`: any attribute is the message, its name uppercased |
| `o[k]`, `o[k] = v` | `o[k]`, `o[k] = v` |
| `len(o)`, `for x in o`, `str(o)` | `o~items`, `do x over o`, `o~string` |
| `o.sendMessage(name, [args])`, `o.sendMessage0(name)` … `o.sendMessage5(name, a1, …, a5)` | the message, its name a string (`"[]="`, `"||"`, or a name held in a variable) |
| `o.sendMessageScope(name, scope, [args])` | `o~name:scope(args)`: the method search starts at the class `scope` |

The `sendMessage` methods are those of BSF4ooRexx's `RexxProxy`, with the same names. Two `oorexx.RexxObject`s of the same Rexx object are `==` (not `is`), with the same hash, and a Rexx object given back to Rexx is the object itself. A Python thread that uses an interpreter attaches to it for the call.

## ADDRESS

A Python callable can be a Rexx command environment:

```python
def calc(command, ctx):
    try:
        ctx.set_var("RESULT", eval(command, {"__builtins__": {}}))
        return 0                                # RC
    except Exception as e:
        ctx.set_var("RESULT", str(e))
        return 1

with oorexx.Interpreter(command_environments={"CALC": calc}) as rexx:
    rexx.routine("""
        address calc
        '2 ** 10';  say rc result               -- 0 1024
        '1 / 0';    say rc result               -- 1 division by zero
    """)()
```

It gets the command and an `oorexx.CommandContext`: the redirected input (`line_in()`, `lines()`), output (`output(line)`, `error(line)`, `is_redirected(...)`), the caller's variables (`get_var`, `set_var`, `drop_var`), and `raise_error(rc)`, `raise_failure(rc)`. What it returns is RC: a positive one raises ERROR, a negative one FAILURE.

## Streams

When Python creates an interpreter, Rexx's standard streams go through Python's, as BSF4ooRexx sends them through Java's: `SAY`, trace output, error messages, `.output~lineout` and everything else written to `.output`, `.error` and `.traceOutput` reaches `sys.stdout` and `sys.stderr`, and `PULL` and `LINEIN()` read `sys.stdin`. They are looked up at every call, so Rexx's output is seen in Jupyter, or wherever Python sends its own (`contextlib.redirect_stdout`). `stdio=False` keeps Rexx's own streams.

Each line carries BSF4ooRexx's prefix, so that Rexx's lines stand out in a log or a notebook: `REXXout>`, `REXXerr>`, `REXXtrc>`, and, as the prompt before reading, `REXXin?>` and `REXXdbgIn?>`. `prefixes=False` leaves them out; a dict changes them (`prefixes={"output": "rexx> ", "error": None}`). From Rexx, `.output~current~prefix = .nil`.

A program started by `rexx` keeps Rexx's streams, and can send them to Python's with `call py.redirectTo`, which takes the arguments of BSF4ooRexx's `bsf.redirectTo` (`"R"` goes back to Rexx's own).

## Threads

Any Rexx thread may use any `.PyObject`, and any Python thread any interpreter and `oorexx.RexxObject`. Two global locks meet, Python's GIL and ooRexx's kernel lock, and the bridge never holds one while waiting for the other: while Python works, other Rexx threads run, and while Rexx works, other Python threads run. So Rexx threads (`START`, `REPLY`) and Python threads (`threading`, a thread pool) can call each other at once, in both directions, with callbacks. Using one interpreter from several threads at once needs ooRexx 5.3.0: ooRexx 5.2.0 crashes there, inside the interpreter.

## Pitfalls

- **`o~name` calls a method.** Without arguments, a method is called, not returned: `server~handle_request` runs it. To pass a method as a value (a callback, a thread's target), take it with `o~getattr("handle_request")`.
- **Case-insensitive names can pick the wrong attribute.** Of `logging.info` and `logging.INFO`, the lowercase one wins, so `logging~INFO` calls the function (and fails, as it wants a message). The constant is `logging~getattr("INFO")`.
- **Number-looking strings become numbers.** `"2026"` goes as the int `2026`, which `os.path.join("dir", "2026")` refuses, and `d["1"]` is `d[1]`. Use `.py~str("2026")`.
- **Positions count from 0.** `o[0]` is the first item of a Python list or tuple, and `server_address[1]` the port.
- **Rexx methods win.** `class`, `isA`, `string`, `hasMethod` and the other methods of Rexx's Object are Rexx's: `l~class` is `.PyObject`, Python's is `l~getattr("__class__")`. `copy`, `send`, `start`, `run` and `request` are the exception (see [Messages](#messages)).
- **Bytes are strings.** Python's `bytes` come to Rexx as Rexx strings, and a Rexx string goes as `str` (or a number): to write to a binary stream, `.py~box("bytes", text)`.

## Guarantees

- `a == b` on two `.PyObject`s is Python's `is`: the same Python object, however it came over.
- In Python, `a == b` on two `oorexx.RexxObject`s is true when they are the same Rexx object, and their hashes agree, so either can be a dict key.
- A Rexx object that goes to Python and back is the object itself, and so is a Python object that goes to Rexx and back.
- Names are looked up when the message arrives, so an attribute added later is found.
- What Python raised is kept in `condition("O")~additional[3]`, and is raised again as itself if it goes back to Python.
- A `.PyObject` keeps its Python object alive while Rexx holds it, and an `oorexx.RexxObject` its Rexx object while Python holds it. When Rexx collects a `.PyObject`, its Python object is let go at the bridge's next call.
- A Python call releases ooRexx's kernel lock, and a Rexx call releases Python's GIL.
- Each line that Rexx writes to Python's streams is a single `write`, so lines from several threads do not mix.
- The same bridge binary works with every CPython from 3.9 on.

## Relation to BSF4ooRexx and .JSObject

The design follows Rony Flatscher's BSF4ooRexx, moved from Java to Python: `.PyObject` is to Python objects what `BSF.CLS` is to Java objects, and `oorexx.RexxObject` what `RexxProxy` is to Rexx objects in Java, with the same `sendMessage` methods. `box` takes BSF4ooRexx's argument order, and the streams take its `bsf.redirectTo` and prefixes. Its sister in the ooRexx/WASM project, `.JSObject` ([ooRexx/WASM and JavaScript](https://github.com/JosepMariaBlasco/oorexxwasm/blob/main/docs/ooRexx-WASM-and-JavaScript.md)), shares the shape: proxies by reference, case-insensitive names with an exact form, and Rexx classes that extend classes of the other side (`.py~extend`, `.js~extend`, as ooRexx/.NET's `.net~extend`). Python departs from both where Python itself differs: it never coerces a string to a number (so the bridge converts numbers, see [Values](#values)), and `o[k]` is its own indexing, from 0.

## Questions decided

1. **Values.** Decided: a Rexx string goes to Python as a string, even when it looks like a number, unless Python's type hints for the parameter ([typeshed](https://github.com/python/typeshed)'s stubs, and the ones packages such as numpy carry) ask for a number; and a program can say, when it imports a module, what type such strings take in calls to it (`.py~import("numpy", "float")`), for the parameters whose hints do not decide. Not done yet: until then, the rule in [Values](#values) holds.
2. **Positions.** Decided: `o[i]` stays Python's, from 0, as all of Python's indexing (dict keys, numpy's tuples, slices and masks, negative positions), and `o~makeArrayProxy` gives a sequence as a Rexx Array, counting from 1. Done: see [Messages](#messages).
3. **Subclassing.** Decided: the bridges converge on ooRexx/.NET's and `.JSObject`'s model, in which the Rexx class itself is the subclass. Done: see [Classes](#classes).

