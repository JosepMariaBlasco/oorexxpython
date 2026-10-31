# ooRexx/Python

[![build](https://github.com/JosepMariaBlasco/oorexxpython/actions/workflows/build.yml/badge.svg)](https://github.com/JosepMariaBlasco/oorexxpython/actions/workflows/build.yml)

A native, bidirectional bridge between [ooRexx](https://sourceforge.net/projects/oorexx/)
and Python 3, modelled after BSF4ooRexx. Binaries for Linux, macOS and
Windows: [releases](https://github.com/JosepMariaBlasco/oorexxpython/releases)
(see *Installing*).

- **ooRexx → Python**: a Rexx program imports Python modules (numpy,
  matplotlib, the standard library...) and uses Python objects as Rexx
  objects (`.py`, `.PyObject`), with keyword arguments, Python's operators,
  iteration and exceptions; a Python list can be used as a Rexx Array,
  counting from 1 (`~makeArrayProxy`); and Rexx classes extend Python classes, whose
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
  3.14, 3.15 and later, with no virtual environments and no per-version builds.

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
3.9–3.15, against ooRexx 5.3.0 (trunk, r13344, also the minimum:
[`docs/design.md`](docs/design.md), *ooRexx versions*). Every push runs them
all with every Python on all four (GitHub Actions:
`.github/workflows/build.yml`). The design is still being discussed, and
parts of the API may change.

## Documentation

Two books, in the format of ooRexx's own and built with its documentation
tools, as PDF and HTML: **ooRexx and Python: Reference** (`pythonref`, in the
style of ooRexx's `rexxref`: every class, method and routine, with syntax
diagrams and small examples) and **ooRexx and Python: Cookbook**
(`pythoncookbook`: the main concepts, one recipe per sample). Each release
carries them; every push builds them (artifact `oorexxpython-docs` of the
[Actions](https://github.com/JosepMariaBlasco/oorexxpython/actions) run).
Their sources are in [`docs/books`](docs/books).

## Installing

You need ooRexx and Python on your computer, and then the bridge: on
Windows, its installer program; on Linux and macOS, its zip, whose
`install.rex` does the same. Either one puts the bridge into your ooRexx,
tells your Pythons where it is and records which Python Rexx programs use:
no environment variables to set.

1. **ooRexx 5.3.0, r13344 or later** (beta). Get it from
   [SourceForge](https://sourceforge.net/projects/oorexx/files/oorexx/5.3.0beta/)
   and check that `rexx -v` runs in a terminal. The bridge needs its
   `logicalwrapper.cls` (Python's `bool` in Rexx), new in r13278, and the fix
   for ooRexx bug #2122 (threads), in r13286, and ooRexx's `.CommandHandler`
   (RFE #882), and the fix for #2123 (conditions other than SYNTAX reach the
   bridge), in r13320, and `RaiseConditionObject` (RFE #883, a condition raised again as it is), in r13329, and the fixes for #2130 and #2132 (a raised condition keeps the call's value; no crash under threads), in r13344; r13344 is the revision the ooRexx bridges are built and tested on. 5.2.0, the current release, has none of them.
2. **Python 3.9 or later**, any version: the bridge is one binary for all of
   them. If you want the samples that use numpy, matplotlib and pandas,
   install them too: `pip install numpy matplotlib pandas`.
3. **The bridge**, from
   [the releases page](https://github.com/JosepMariaBlasco/oorexxpython/releases),
   under the newest version, as below for each system. Both ways check both
   directions at the end (`ooRexx/Python: installed and OK`).
4. **Try it.** The samples are in ooRexx's samples folder, under `python`.
   Go there and run one: `rexx 01-hello.rex`, or `python 30-run-rexx.py`
   (`python3` on Linux and macOS). Their [README](samples/README.md) lists
   them all.

### Windows

- Python: from [python.org](https://www.python.org/downloads/) or from the
  Microsoft Store (Python install manager). It need not be on the `PATH`.
- Download `oorexxpython-VERSION-windows-x64.exe` and run it. Windows may
  warn that the program is not signed ("Windows protected your PC": More
  info, Run anyway), as it does with ooRexx's own installer.
- It finds ooRexx (or asks for its folder) and the Pythons installed on the
  computer, and asks which of them will `import oorexx` and which one Rexx
  programs use; another Python can be given by its `python.exe`.
- It puts the samples in `C:\Program Files\ooRexx\samples\python`, the books
  in ooRexx's `doc` folder, and, in ooRexx's Start menu folder, the books
  (ooRexx Documentation), the samples' slide deck (ooRexx Samples) and
  "Uninstall ooRexx-Python". It can also be uninstalled from Settings >
  Apps. To update, run the new version's installer.
- Unattended: `oorexxpython-VERSION-windows-x64.exe /S
  /PYTHON=C:\path\to\python.exe /D=C:\Program Files\ooRexx` (`/D` last;
  several Pythons separated by `;`; `/REXXPYTHON=` for the one Rexx uses;
  without `/PYTHON`, every Python it finds). What it did is in
  `%TEMP%\oorexxpython-install.log`.
- The zip works on Windows too, for a portable ooRexx or when you prefer
  the command line (below).

### The zip: Linux, macOS (and Windows, if you prefer)

Download the zip for your system (`linux-x64`, `macos-arm64` for Apple
silicon, `macos-x64` for Intel Macs, `windows-x64`), unzip it anywhere, open a terminal in
the unzipped folder `oorexxpython` and run `rexx install`, as below. The
samples are then in ooRexx's samples folder, and the unzipped folder is no
longer needed: you can delete it.

#### Linux

- Python: your distribution's `python3`, with its shared library, which some
  distributions package apart. On Debian and Ubuntu, that is
  `libpython3.X`, X being your Python's (`python3 --version`): for
  example, `sudo apt install libpython3.12`. `rexx install` notices when it
  is missing, and says what to install.
- ooRexx installed from a package (in `/usr` or `/usr/local`) needs `sudo`,
  and `sudo` may find another `python3` than yours, so name yours:

  ```bash
  cd /path/to/oorexxpython
  sudo rexx install --python $(which python3)
  ```

- The samples end up in `/usr/local/share/ooRexx/samples/python` (or
  `/usr/share/...`, where ooRexx is).

#### macOS

- Python: the installer from [python.org](https://www.python.org/downloads/),
  or Homebrew's (`brew install python`).
- An ooRexx in a system folder such as `/usr/local` needs `sudo`, naming
  your Python as on Linux:

  ```bash
  cd /path/to/oorexxpython
  sudo rexx install --python $(which python3)
  ```

#### Windows, with the zip

- ooRexx in `C:\Program Files\ooRexx` (the default) needs an administrator:
  open the terminal **as administrator** (right-click on Command Prompt or
  Terminal, "Run as administrator"), then

  ```bat
  cd C:\path\to\oorexxpython
  rexx install --python C:\path\to\python.exe
  ```

  (without `--python`, the `python` on the `PATH`).
- The samples end up in `C:\Program Files\ooRexx\samples\python`.

### A portable ooRexx

An ooRexx unzipped from one of the portable builds, with its `bin` on the
`PATH`, is installed into in the same way, without administrator rights:
`rexx install` (or `rexx install --python /path/to/python`).

### More

- `rexx oorexxpython-uninstall status`, from any folder, shows what was
  installed and where; `rexx oorexxpython-uninstall` removes exactly that.
- **Updating**: run the new version's `rexx install` (or installer); it
  replaces the old one.
- **Another Python**: `rexx install --python /path/to/python`, as many times
  as you like (each Python gets its own link to the bridge); `--user` puts
  the link in your user folder, which needs no administrator.
- **Which Python Rexx uses**: the one the installer recorded in
  `oorexxpython.cfg`, next to the bridge: the first `--python` (or the one
  named with `--rexx-python`); installing again keeps that choice unless
  `--rexx-python` names another. The environment variable `ORX_PYTHON`,
  when set, takes precedence. Its installed packages are the ones Rexx
  programs see.
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
- an ooRexx 5.3.0, r13344 or later, with its API headers: an installed ooRexx (`/usr/local`), or one of ooRexx's portable
  builds from
  [SourceForge](https://sourceforge.net/projects/oorexx/files/oorexx/5.3.0beta/portable/)
  with its `bin/` on the `PATH`.

```bash
git clone https://github.com/JosepMariaBlasco/oorexxpython
oorexxpython/scripts/setup-pythons.sh        # CPython 3.9 ... 3.15, with uv
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
for v in 3.9 3.10 3.11 3.12 3.13 3.14 3.15; do
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
- `tests/` — `run.sh`; `rexx/` (ooRexxUnit test groups: basics, booleans,
  callbacks, ADDRESS, numpy, lifetime, threads, subclassing, and all the samples),
  `python/` (the Python
  side), `probe/` (the conversion probe and its expected output, run by
  `python/t80_conversion_probe.py`).
- `scripts/` — `build.sh`, `setup-pythons.sh`, `setup-oorexxunit.sh`,
  `setup-oorexx-deb.sh` (installs SourceForge's ooRexx package for Ubuntu
  24.04 at the CI's revision, `OOREXX_REV` in the workflow), and
  `setup-oorexx.sh` (builds ooRexx from SVN, for a revision with no
  package).
- `docs/design.md` — how the bridge works and why: names, values, keyword
  arguments, threads and locks, lifetime.
- `docs/books/` — two books in the format of ooRexx's own, built with its
  documentation tools: *Reference* (`pythonref`, in the style of `rexxref`)
  and *Cookbook* (`pythoncookbook`, one recipe per sample); see its README.
- `notes/oorexx-bugs.md` — the ooRexx bugs the bridge's tests found, and
  what the code does about each.

## Credits

The bridge was suggested by Rony G. Flatscher, author of
[BSF4ooRexx](https://sourceforge.net/projects/bsf4oorexx/), the ooRexx–Java
bridge it is modelled after; the `.JSObject` bridge of
[ooRexx/WASM](https://github.com/JosepMariaBlasco/oorexxwasm) is its sister.
The tests use ooRexxUnit, ooRexx's own test framework (CPL 1.0), fetched from
ooRexx's SVN, not included here. `tests/python/t70_identity_lifetime.py` asks of this bridge
what Tom Dyer's qualification probes for his ooRexx/Python proof of concept in
[Alchemy](https://github.com/animatedads/alchemy) ask of his, contributed by
him under the Apache License 2.0; `oorexx.RexxError`'s attributes follow his
bridge's.

Written by Josep Maria Blasco, with Claude (Anthropic) as coding assistant.

## License

Apache License 2.0 (`LICENSE`).
