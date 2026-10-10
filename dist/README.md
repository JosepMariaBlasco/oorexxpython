# ooRexx/Python — binaries

A bridge between ooRexx and Python 3, both ways: ooRexx programs use Python
modules as objects, and Python programs run Rexx code and use Rexx objects.
Source, documentation and design: https://github.com/JosepMariaBlasco/oorexxpython

**Status: prototype.** Parts of the API may still change.

## What you need

- **ooRexx 5.3.0** (beta, from
  [SourceForge](https://sourceforge.net/projects/oorexx/files/oorexx/5.3.0beta/)),
  installed so that `rexx` runs. Revision r13259 or later is recommended for
  serious work: earlier ones have bugs the bridge can work around but not
  avoid (see `notes/oorexx-bugs.md` in the repository).
- **Python 3.9 or later**, any version: the bridge is one binary for all of
  them (Python's Stable ABI). It must have its shared library, as the
  installers from python.org (Windows, macOS), Homebrew, uv and most Linux
  distributions provide (Debian and Ubuntu: the package `libpython3.X` for
  your `python3`).
- The Linux binaries are built on Ubuntu 24.04 (x86_64); an older
  distribution may need to build from source.

## Install

Unzip anywhere. The folder `oorexxpython` holds everything; tell ooRexx and
Python where it is. With the folder at `/path/to/oorexxpython`:

**Linux**

```bash
export LD_LIBRARY_PATH=/path/to/oorexxpython:$LD_LIBRARY_PATH
export REXX_PATH=/path/to/oorexxpython
export PYTHONPATH=/path/to/oorexxpython
```

**macOS**: the same, with `DYLD_LIBRARY_PATH` instead of `LD_LIBRARY_PATH`.
macOS drops `DYLD_*` variables when it starts a system program such as
`/bin/sh`, so a command run through a shell (ooRexx's `ADDRESS SYSTEM`,
`make`...) must set it again itself.

**Windows** (Command Prompt; ooRexx's `bin` directory must be on the `PATH`
too, or `OOREXX_HOME` set to ooRexx's folder):

```bat
set PATH=C:\path\to\oorexxpython;%PATH%
set REXX_PATH=C:\path\to\oorexxpython
set PYTHONPATH=C:\path\to\oorexxpython
```

**Which Python ooRexx uses**: the one in `ORX_PYTHON` (the full path of its
executable), else the `python3` on the `PATH` (Windows: `python`). Its
installed packages are the ones Rexx programs see.

## Check

```
rexx check.rex       ooRexx -> Python
python check.py      Python -> ooRexx (python3 on Linux and macOS)
```

Both end with `ooRexx/Python: OK`.

## Use

From Rexx, `::requires "python.cls"` and `.py`:

```rexx
math = .py~import("math")
say math~sqrt(2)                    -- 1.4142135623730951

::requires "python.cls"
```

From Python, `import oorexx`:

```python
import oorexx
with oorexx.Interpreter() as rexx:
    print(rexx.eval("'Hello'~reverse"))    # olleH
```

The `examples` folder has short examples of every feature, each with its
expected output (`examples/README.md`). `20-numpy.rex` and
`21-matplotlib.rex` need `pip install numpy matplotlib` in the Python used.

## Licence

Apache License 2.0 (`LICENSE`).
