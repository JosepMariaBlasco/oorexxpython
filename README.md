# ooRexx/Python

[![build](https://github.com/JosepMariaBlasco/oorexxpython/actions/workflows/build.yml/badge.svg)](https://github.com/JosepMariaBlasco/oorexxpython/actions/workflows/build.yml)

A native, bidirectional bridge between [ooRexx](https://sourceforge.net/projects/oorexx/)
and Python 3, modelled after BSF4ooRexx.

- **ooRexx → Python**: a Rexx program imports Python modules (numpy,
  matplotlib, the standard library...) and uses Python objects as Rexx
  objects (`.py`, `.PyObject`), with keyword arguments, Python's operators,
  iteration and exceptions.
- **Python → ooRexx**: a Python program creates ooRexx interpreters, runs
  Rexx code and uses Rexx objects as Python objects (`oorexx.Interpreter`,
  `oorexx.RexxObject`); Python functions can be `ADDRESS` environments, and
  Rexx's `SAY` goes through Python's `sys.stdout`.
- **Both ways at once, on any thread**: Rexx calls Python, which calls back a
  Rexx object, which calls Python again.
- **One build for every Python 3 from 3.9**: the bridge uses Python's Stable
  ABI only, so the same binary works with 3.9, 3.10, 3.11, 3.12, 3.13 and
  later, with no virtual environments and no per-version builds.

```rexx
np = .py~import("numpy")
m  = np~array(.array~of(.array~of(1, 2), .array~of(3, 4)))
say m~sum m~shape                                -- 10 (2, 2)
say .py~round(np~linalg~det(m), 2)               -- -2.0

::requires "python.cls"
```

```python
import oorexx

with oorexx.Interpreter() as rexx:
    print(rexx.eval("6 * 7"))                    # 42
    times = rexx.routine("use arg a, b; return a * b")
    print(times(6, 7))                           # 42
```

**Status: prototype.** Both directions work and are covered by automated
tests (ooRexxUnit test groups and Python checks, every nutshell example
included), on Linux x86_64 and macOS (arm64), with CPython 3.9–3.13,
against ooRexx 5.3.0 (trunk). Every push runs them all with every Python on
both (GitHub Actions: `.github/workflows/build.yml`). Windows is next. The design is
still being discussed and parts of the API may change.

## Getting started

Linux x86_64 or macOS, with a C++ compiler (`g++`; Xcode's command line
tools on macOS), `svn` and [uv](https://docs.astral.sh/uv/) (or
`pip`, which `setup-pythons.sh` uses to install uv), and an ooRexx 5.3.0
with its API headers: an installed ooRexx (`/usr/local`), or one of ooRexx's
portable builds from
[SourceForge](https://sourceforge.net/projects/oorexx/files/oorexx/5.3.0beta/portable/)
with its `bin/` on the `PATH`.

```bash
git clone https://github.com/JosepMariaBlasco/oorexxpython
oorexxpython/scripts/setup-pythons.sh        # CPython 3.9 ... 3.13, with uv
oorexxpython/scripts/build.sh                # the bridge, seconds
oorexxpython/examples/run-one.sh 01-hello.rex
```

Everything goes into the directory that contains your clone
(`$ORXPY_WORK`, overridable), next to it:

```
build/        oorexx.abi3.so, librexxpython.so (.dylib on macOS), python.cls
pythons/      the CPython versions to test against
oorexxunit/   ooRexx's test framework (for the tests)
```

To use the bridge from your own programs:

- **From Rexx**: put the build directory on `REXX_PATH` and
  `LD_LIBRARY_PATH` (`DYLD_LIBRARY_PATH` on macOS), and say which Python to
  use with `ORX_PYTHON` (default: the `python3` on the `PATH`; its installed
  packages are the ones Rexx sees):
  `ORX_PYTHON=$(which python3) LD_LIBRARY_PATH=build REXX_PATH=build rexx prog.rex`
- **From Python**: put the build directory on `PYTHONPATH` and
  `import oorexx`: `PYTHONPATH=build python3 prog.py`.

## Tests

```bash
oorexxpython/scripts/setup-oorexxunit.sh     # ooRexxUnit, from ooRexx's SVN
# numpy and matplotlib in each Python, for Numpy.testGroup:
for v in 3.9 3.10 3.11 3.12 3.13; do
  uv pip install --python "$(uv python find $v)" --break-system-packages numpy matplotlib
done
oorexxpython/tests/run.sh                    # every test with every Python, ~1 min
```

`run.sh` runs each test file in its own process under a timeout (a deadlock
is a failure). `PYTHONS="3.9 3.13"` and `TESTS=Threads.testGroup` choose;
`ORXPY_THREADS="16 16 2000"` makes the threads stress test heavy.
`UV_PYTHON_INSTALL_DIR` defaults to `$ORXPY_WORK/pythons`, where
`setup-pythons.sh` installs them.

## Layout

- `src/` — the bridge.
  - `bridge.cpp` — `oorexx.abi3.so`: both directions, the proxies, the
    locks, `ADDRESS` (Stable ABI only, ooRexx's public native API only).
  - `loader.cpp` — `librexxpython.so`, the ooRexx package that finds and
    loads a Python when Rexx starts, then hands over to the bridge.
  - `python.cls` — the Rexx side: `.py`, `.PyObject`, `py.import` & co.
- `examples/` — short "nutshell" programs, one feature each, both ways;
  each shows the Python it replaces and its exact output (see its README).
- `tests/` — `run.sh`; `rexx/` (ooRexxUnit test groups: basics, callbacks,
  numpy, lifetime, threads, and all the examples), `python/` (the Python
  side).
- `scripts/` — `build.sh`, `setup-pythons.sh`, `setup-oorexxunit.sh`, and
  `setup-oorexx.sh` (builds ooRexx from SVN, when no build with headers is
  at hand).
- `docs/design.md` — how the bridge works and why: names, values, keyword
  arguments, threads and locks, lifetime.
- `notes/oorexx-bugs.md` — the ooRexx bugs the bridge's tests found, and
  what the code does about each.

## Credits

The bridge was suggested by Rony G. Flatscher, author of
[BSF4ooRexx](https://sourceforge.net/projects/bsf4oorexx/), the ooRexx–Java
bridge it is modelled after; the `.JSObject` bridge of
[ooRexx/WASM](https://github.com/JosepMariaBlasco/oorexxwasm) is its sister.
The tests use ooRexxUnit, ooRexx's own test framework (CPL 1.0), fetched from
ooRexx's SVN, not included here.

Written by Josep Maria Blasco, with Claude (Anthropic) as coding assistant.

## License

Apache License 2.0 (`LICENSE`).
