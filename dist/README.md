# ooRexx/Python — binaries

A bridge between ooRexx and Python 3, both ways: ooRexx programs use Python
modules as objects, and Python programs run Rexx code and use Rexx objects.
Source, documentation and design: https://github.com/JosepMariaBlasco/oorexxpython

**Status: prototype.** Parts of the API may still change.

## What you need

- **ooRexx 5.3.0, r13344 or later** (beta, from
  [SourceForge](https://sourceforge.net/projects/oorexx/files/oorexx/5.3.0beta/)),
  installed so that `rexx` runs. The bridge needs its `logicalwrapper.cls`
  (Python's `bool` in Rexx), new in r13278, and the fix for ooRexx bug
  #2122 (threads), in r13286, ooRexx's `.CommandHandler` (RFE #882), and
  the fix for #2123 (conditions other than SYNTAX reach the bridge), in
  r13320, and `RaiseConditionObject` (RFE #883, a condition raised again as
  it is), in r13329, and the fixes for #2130 and #2132 (a raised
  condition keeps the call's value; no crash under threads), in r13344;
  r13344 is the revision the ooRexx bridges are built and tested on. 5.2.0, the current release, has none of them.
- **Python 3.9 or later**, any version: the bridge is one binary for all of
  them (Python's Stable ABI). It must have its shared library, as the
  installers from python.org (Windows, macOS), Homebrew, uv and most Linux
  distributions provide (Debian and Ubuntu: the package `libpython3.X` for
  your `python3`).
- The Linux binaries are built on Ubuntu 24.04 (x86_64); an older
  distribution may need to build from source.

## Install

Unzip anywhere, open a terminal in the folder `oorexxpython`, and run

```
rexx install
```

It installs the bridge into the ooRexx that runs it and makes it importable
from the Python given (`--python`; default: `ORX_PYTHON`, else the `python3`
on the `PATH`; Windows: `python`), records that Python as the one Rexx
programs use (in `oorexxpython.cfg`, next to the bridge), then checks both
directions from another folder with no environment variables. Nothing else
to set: Rexx programs find `python.cls`, Python programs `import oorexx`.

- **Windows**: the bridge goes into ooRexx's folder (next to `rexx.exe`). In
  `C:\Program Files\ooRexx`, run it from a terminal opened *as
  administrator*. Or use the release's installer,
  `oorexxpython-VERSION-windows-x64.exe`, instead of this zip: it does the
  same, with a wizard.
- **Linux and macOS**: the bridge goes into ooRexx's `lib/oorexxpython/`,
  with links in its `lib` and `bin`. An ooRexx in `/usr` or `/usr/local`
  needs `sudo rexx install`; `sudo` may find another `python3` than yours, so
  name it: `sudo rexx install --python $(which python3)`.
- **Other Pythons**: `rexx install --python /path/to/python` (as many
  `--python` as you like; again later adds them). `--user` puts the link in
  the Pythons' user site-packages instead (no administrator needed for that
  part).
- **The Python Rexx programs use**: the first `--python`, or the one named
  with `--rexx-python /path/to/python`; installing again keeps the earlier
  choice unless `--rexx-python` names another. `ORX_PYTHON`, when set,
  takes precedence.
- It also copies itself next to `rexx`, as `oorexxpython-uninstall.rex`, and
  the samples into ooRexx's samples folder, under `python` (Windows:
  `samples\python` beside `rexx.exe`; Linux packages:
  `share/ooRexx/samples/python`). After that, **the unzipped folder can be
  deleted**.
- `rexx oorexxpython-uninstall status` (from any folder) shows what is
  installed and where; `rexx oorexxpython-uninstall` removes exactly that,
  itself included. To update, run the new version's `rexx install`.

### Without installing

The folder `oorexxpython` holds everything; tell ooRexx and Python where it
is. With the folder at `/path/to/oorexxpython`:

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
executable), else the one an installation recorded (`oorexxpython.cfg` next
to the bridge), else the `python3` on the `PATH` (Windows: `python`). Its
installed packages are the ones Rexx programs see.

## Check

```
rexx check.rex       ooRexx -> Python
python check.py      Python -> ooRexx (python3 on Linux and macOS)
```

(Not needed after `rexx install`, which runs its own checks.) Both end
with `ooRexx/Python: OK`. A check that ends with `NOT ON PYTHONPATH`
or `NOT ON REXX_PATH` found the bridge only because it sits next to it, and
other programs (the samples, yours) would not: it shows the `set` (or
`export`) that is missing.

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

The `samples` folder has short samples of every feature, each with its
expected output (`samples/README.md`), and `samples/slides/samples.html`
has them all as one slide deck (made with
[md2slides](https://rexx.epbcn.com/rexx-parser/doc/utilities/md2slides/);
open it in a browser). Once installed, they are in ooRexx's samples folder,
under `python`. `20-numpy.rex`, `21-matplotlib.rex` and `23-pandas.rex` need
`pip install numpy matplotlib pandas` in the Python used.

## Licence

Apache License 2.0 (`LICENSE`).
