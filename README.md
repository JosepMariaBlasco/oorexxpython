# ooRexx/Python

[![build](https://github.com/JosepMariaBlasco/oorexxpython/actions/workflows/build.yml/badge.svg)](https://github.com/JosepMariaBlasco/oorexxpython/actions/workflows/build.yml)

A native, bidirectional bridge between [ooRexx](https://sourceforge.net/projects/oorexx/)
and Python 3, modelled after BSF4ooRexx. Binaries for Linux, macOS and
Windows: [releases](https://github.com/JosepMariaBlasco/oorexxpython/releases)
(see *Installing*).

- **ooRexx → Python**: a Rexx program imports Python modules (numpy,
  matplotlib, the standard library...) and uses Python objects as Rexx
  objects (`.py`, `.PyObject`), with keyword arguments, Python's operators,
  iteration and exceptions; and Rexx classes extend Python classes, whose
  methods they override (`::class Worker subclass PyObject`, `.py~extend`,
  as ooRexx/.NET's `.net~extend`).
- **Python → ooRexx**: a Python program creates ooRexx interpreters, runs
  Rexx code and uses Rexx objects as Python objects (`oorexx.Interpreter`,
  `oorexx.RexxObject`); Python functions can be `ADDRESS` environments, and
  Rexx's standard streams go through Python's (`sys.stdout`...), each line
  marked as Rexx's (`REXXout>`, as in BSF4ooRexx).
- **Both ways at once, on any thread**: Rexx calls Python, which calls back a
  Rexx object, which calls Python again.
- **One build for every Python 3 from 3.9**: the bridge uses Python's Stable
  ABI only, so the same binary works with 3.9, 3.10, 3.11, 3.12, 3.13,
  3.14 and later, with no virtual environments and no per-version builds.

```rexx
np = .py~import("numpy")
m  = np~array(((1, 2), (3, 4)))
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
tests (ooRexxUnit test groups and Python checks, every nutshell sample
included), on Linux x86_64, macOS (arm64 and x86_64) and Windows x64, with CPython
3.9–3.14, against ooRexx 5.3.0 (trunk). Every push runs them all with every
Python on all four (GitHub Actions: `.github/workflows/build.yml`), and on
Linux also with ooRexx 5.2.0, the minimum: all but the three tests that use an
interpreter from several threads at once, which needs 5.3.0
([`docs/design.md`](docs/design.md), *ooRexx versions*). The design is
still being discussed ([the specification](docs/ooRexx-and-Python.md)) and
parts of the API may change.

## Installing

You need ooRexx and Python on your computer, and the bridge's zip for your
system. The installer puts the bridge into your ooRexx and tells your Python
where it is: no environment variables to set.

1. **ooRexx 5.2.0 or later.** Get it from
   [SourceForge](https://sourceforge.net/projects/oorexx/files/oorexx/) and
   check that `rexx -v` runs in a terminal. Using the bridge from several
   threads at once (a Python thread pool calling Rexx, say) needs ooRexx
   5.3.0 (beta, r13265 or later): 5.2.0 crashes there.
2. **Python 3.9 or later**, any version: the bridge is one binary for all of
   them. If you want the samples that use numpy, matplotlib and pandas,
   install them too: `pip install numpy matplotlib pandas`.
3. **The bridge.** From
   [the releases page](https://github.com/JosepMariaBlasco/oorexxpython/releases),
   download the zip for your system under the newest version
   (`windows-x64`, `linux-x64`, `macos-arm64` for Apple silicon,
   `macos-x64` for Intel Macs) and unzip it anywhere.
4. **Install.** Open a terminal in the unzipped folder `oorexxpython` and
   run the installer, as shown below for each system. It installs, then
   checks both directions, and ends with `ooRexx/Python: installed and OK`.
5. **Try it.** The installer copies the samples into ooRexx's samples
   folder, under `python`. Go there and run one: `rexx 01-hello.rex`, or
   `python 30-run-rexx.py` (`python3` on Linux and macOS). Their
   [README](samples/README.md) lists them all. The unzipped folder is no
   longer needed: you can delete it.

### Windows

- Python: the installer from [python.org](https://www.python.org/downloads/),
  with **"Add python.exe to PATH"** ticked, so that `python` runs in a
  terminal.
- ooRexx in `C:\Program Files\ooRexx` (the default) needs an administrator:
  open the terminal **as administrator** (right-click on Command Prompt or
  Terminal, "Run as administrator"), then

  ```bat
  cd C:\path\to\oorexxpython
  rexx install
  ```

- The samples end up in `C:\Program Files\ooRexx\samples\python`.

### Linux

- Python: your distribution's `python3`, with its shared library, which some
  distributions package apart. On Debian and Ubuntu, that is
  `libpython3.X`, X being your Python's (`python3 --version`): for
  example, `sudo apt install libpython3.12`.
- ooRexx installed from a package (in `/usr` or `/usr/local`) needs `sudo`,
  and `sudo` may find another `python3` than yours, so name yours:

  ```bash
  cd /path/to/oorexxpython
  sudo rexx install --python $(which python3)
  ```

- The samples end up in `/usr/local/share/ooRexx/samples/python` (or
  `/usr/share/...`, where ooRexx is).

### macOS

- Python: the installer from [python.org](https://www.python.org/downloads/),
  or Homebrew's (`brew install python`).
- An ooRexx in a system folder such as `/usr/local` needs `sudo`, naming
  your Python as on Linux:

  ```bash
  cd /path/to/oorexxpython
  sudo rexx install --python $(which python3)
  ```

### A portable ooRexx

An ooRexx unzipped from one of the portable builds, with its `bin` on the
`PATH`, is installed into in the same way, without administrator rights:
`rexx install` (or `rexx install --python /path/to/python`).

### More

- `rexx oorexxpython-uninstall status`, from any folder, shows what was
  installed and where; `rexx oorexxpython-uninstall` removes exactly that.
- **Updating**: run the new version's `rexx install`; it replaces the old one.
- **Another Python**: `rexx install --python /path/to/python`, as many times
  as you like (each Python gets its own link to the bridge); `--user` puts
  the link in your user folder, which needs no administrator.
- **Which Python Rexx uses**: the one you installed for, if it is the
  `python3` on the `PATH` (`python` on Windows); otherwise set `ORX_PYTHON`
  to its full path. Its installed packages are the ones Rexx programs see.
- **Without installing**: the zip's own `README.md` says which environment
  variables to set instead.
- **The newest build**: every push to this repository builds the same zips,
  available for 90 days as artifacts `oorexxpython-<platform>` of its
  [Actions](https://github.com/JosepMariaBlasco/oorexxpython/actions) run
  (you must be signed in to GitHub to download them).

## Building from source

To build the bridge yourself, or to work on it, you need Linux x86_64,
macOS or Windows, with:

- a C++ compiler: `g++`; Xcode's command line tools on macOS; MSVC on
  Windows, with the scripts run from Git Bash in a developer command
  prompt's environment;
- `curl`, and [uv](https://docs.astral.sh/uv/) (or `pip`, which
  `setup-pythons.sh` uses to install uv);
- an ooRexx 5.2.0 or later (5.3.0 for several threads at once) with its API
  headers: an installed ooRexx (`/usr/local`), or one of ooRexx's portable
  builds from
  [SourceForge](https://sourceforge.net/projects/oorexx/files/oorexx/5.3.0beta/portable/)
  with its `bin/` on the `PATH`.

```bash
git clone https://github.com/JosepMariaBlasco/oorexxpython
oorexxpython/scripts/setup-pythons.sh        # CPython 3.9 ... 3.14, with uv
oorexxpython/scripts/build.sh                # the bridge, seconds
oorexxpython/samples/run-one.sh 01-hello.rex
```

Everything goes into the directory that contains your clone
(`$ORXPY_WORK`, overridable), next to it:

```
build/        oorexx.abi3.so, librexxpython.so, python.cls
              (macOS: librexxpython.dylib; Windows: oorexx.pyd, rexxpython.dll)
pythons/      the CPython versions to test against
oorexxunit/   ooRexx's test framework (for the tests)
```

To use the bridge from your own programs:

- **From Rexx**: put the build directory on `REXX_PATH` and
  `LD_LIBRARY_PATH` (`DYLD_LIBRARY_PATH` on macOS), and say which Python to
  use with `ORX_PYTHON` (default: the `python3` on the `PATH`; its installed
  packages are the ones Rexx sees):
  `ORX_PYTHON=$(which python3) LD_LIBRARY_PATH=build REXX_PATH=build rexx prog.rex`.
  On Windows the build directory goes on the `PATH` instead, and
  `ORX_PYTHON` defaults to the `python` on the `PATH`.
- **From Python**: put the build directory on `PYTHONPATH` and
  `import oorexx`: `PYTHONPATH=build python3 prog.py`. On Windows, ooRexx's
  `bin` directory must be on the `PATH` (or `OOREXX_HOME` set): the module
  finds `rexx.dll` there itself.

## Tests

```bash
oorexxpython/scripts/setup-oorexxunit.sh     # ooRexxUnit, from ooRexx's SVN
# numpy, matplotlib and pandas in each Python, for Numpy.testGroup:
for v in 3.9 3.10 3.11 3.12 3.13 3.14; do
  uv pip install --python "$(uv python find $v)" --break-system-packages numpy matplotlib pandas
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
- `samples/` — short "nutshell" programs, one feature each, both ways;
  each shows the Python it replaces and its exact output (see its README).
- `tests/` — `run.sh`; `rexx/` (ooRexxUnit test groups: basics, callbacks,
  numpy, lifetime, threads, subclassing, and all the samples), `python/` (the Python
  side).
- `scripts/` — `build.sh`, `setup-pythons.sh`, `setup-oorexxunit.sh`, and
  `setup-oorexx.sh` (builds ooRexx from SVN, when no build with headers is
  at hand).
- `docs/ooRexx-and-Python.md` — the specification: what the bridge does,
  for Rexx and Python programmers (a living document, for discussion).
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
