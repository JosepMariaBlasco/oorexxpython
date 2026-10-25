/* install.rex: installs the ooRexx/Python bridge into the ooRexx that runs it,
 * so that Rexx programs find python.cls and Python programs "import oorexx"
 * with no environment variables.
 *
 *   rexx install [install] [--python EXE]... [--rexx-python EXE] [--user]
 *   rexx install uninstall
 *   rexx install status
 *   rexx oorexxpython-uninstall [status]    (from anywhere, once installed)
 *
 * Where things go (docs/design.md, *Installation*):
 *   Windows:        the bridge, its package and python.cls in the folder of
 *                   rexx.exe and rexx.dll (C:\Program Files\ooRexx).
 *   Linux, macOS:   all three in ooRexx's lib/oorexxpython/; the package is
 *                   linked into lib/ (where ooRexx looks for libraries),
 *                   python.cls into bin/ (on the PATH, where ::requires
 *                   looks).  Python's bool is ooRexx's own .LogicalWrapper
 *                   (logicalwrapper.cls, in ooRexx's bin/ since r13278).
 *   boolean.cls:    development builds after 0.1.0-preview.13 installed one
 *                   (.Boolean, shared by the ooRexx bridges); installing or
 *                   uninstalling removes it, or, if another bridge (net.cls,
 *                   lua.cls, js.cls) is left in bin/, turns a link to ours
 *                   into a plain copy.
 *   Python:         a one-line oorexxpython.pth in the site-packages of each
 *                   Python given with --python (default: ORX_PYTHON, else
 *                   python3 on the PATH; Windows: python), or in its user
 *                   site-packages with --user.
 *   ooRexx's Python: the Python that Rexx programs use (when ORX_PYTHON is
 *                   not set), recorded as "python = EXE" in oorexxpython.cfg
 *                   next to the bridge: --rexx-python EXE, else the one
 *                   recorded by an earlier install (if it still exists),
 *                   else the first --python.
 *   This program:   copied as oorexxpython-uninstall.rex next to rexx, which
 *                   uninstalls by default, so the unzipped folder can go.
 *   The samples:    in ooRexx's samples folder, under python/ (Windows:
 *                   samples\ beside rexx.exe; the portable builds: samples/
 *                   beside bin/; Linux packages: share/ooRexx/samples; else
 *                   the bridge's folder's samples/python).
 * What was installed is listed in oorexxpython-installed.txt, in that same
 * folder; uninstall removes exactly that.  Installing again (a new version,
 * another Python) replaces the files and adds to the list.
 *
 * ooRexx in a system folder needs an administrator: on Windows, a terminal
 * opened "as administrator"; on Linux and macOS, sudo rexx install.
 */
parse arg args
parse source . . me
uninstaller = "oorexxpython-uninstall.rex"
-- the copy next to rexx uninstalls by default
if .File~new(me)~name~caselessEquals(uninstaller) then command = "uninstall"
else command = "install"
pythons = .array~new; user = .false; rexxPython = ""
do while args \= ""
  parse var args word args
  select case word~lower
    when "install", "uninstall", "status" then command = word~lower
    when "--python", "-python" then do
      parse var args exe args
      if exe~left(1) == '"' then parse value exe args with '"' exe '"' args
      if exe == "" then call fail "--python needs the path of a Python executable"
      pythons~append(exe)
    end
    when "--rexx-python", "-rexx-python" then do
      parse var args exe args
      if exe~left(1) == '"' then parse value exe args with '"' exe '"' args
      if exe == "" then call fail "--rexx-python needs the path of a Python executable"
      rexxPython = exe
    end
    when "--user", "-user" then user = .true
    when "-h", "--help", "-?", "help", "/?" then call help
    otherwise call fail "unknown argument:" word "(rexx install --help)"
  end
end

windows = .File~separator == "\"
from = .File~new(me)~parent
mac = .File~new(from || .File~separator || "librexxpython.dylib")~exists
select
  when windows then do; loader = "rexxpython.dll";     bridge = "oorexx.pyd";     end
  when mac     then do; loader = "librexxpython.dylib"; bridge = "oorexx.abi3.so"; end
  otherwise           ; loader = "librexxpython.so";    bridge = "oorexx.abi3.so"
end

-- ooRexx: the one running this program
rexxExe = .RexxInfo~executable~absolutePath   -- (both are File objects)
bin = .File~new(rexxExe)~parent
lib = .RexxInfo~libraryPath~absolutePath
if windows then home = bin; else home = join(lib, "oorexxpython")
manifest = join(home, "oorexxpython-installed.txt")
config = join(home, "oorexxpython.cfg")

select case command
  when "status"    then call status
  when "uninstall" then call uninstall
  otherwise             call install
end
exit 0

/* ------------------------------------------------------------------------ */

install:
  if .File~new(me)~name~caselessEquals(uninstaller) then
    call fail "this is the uninstaller; to install, run install.rex from the unzipped oorexxpython folder"
  say "ooRexx/Python installer"
  say "  ooRexx" .RexxInfo~version "r".RexxInfo~revision "("rexxExe")"
  parse value .RexxInfo~version with major "." minor "."
  if major < 5 | (major = 5 & minor < 3) | (major = 5 & minor = 3 & .RexxInfo~revision < 13297) then
    call fail "ooRexx 5.3.0 r13297 or later is needed (logicalwrapper.cls, the fix for #2122, .CommandHandler, the ooRexx bridges' revision)"
  do f over loader, bridge, "python.cls"
    if \.File~new(join(from, f))~exists then
      call fail "no" f "next to install.rex: run it from the unzipped oorexxpython folder"
  end
  if windows, \.File~new(join(bin, "rexx.dll"))~exists then
    call fail "rexx.dll is not next to rexx.exe in" bin": this layout of ooRexx is not supported"
  if same(from, home) then call fail "install.rex is already in" home

  if pythons~isEmpty then do
    if rexxPython \== "" then exe = rexxPython
    else exe = value("ORX_PYTHON", , "ENVIRONMENT")
    if exe == "" then if windows then exe = "python"; else exe = "python3"
    pythons~append(exe)
  end
  if rexxPython \== "", \pythons~hasItem(rexxPython) then pythons~append(rexxPython)

  done = readManifest()                 -- what an earlier install left
  -- the bridge
  if \.File~new(home)~isDirectory then
    if \.File~new(home)~makeDirs then call denied home
  if \dropBoolean() then exit 1         -- an earlier version's boolean.cls
  do f over loader, bridge, "python.cls"
    target = join(home, f)
    if SysFileCopy(join(from, f), target) \= 0 then call denied target
    done~put(target, "file")
  end
  say "  installed" loader"," bridge "and python.cls in" home
  if \windows then do
    call link join(home, loader), join(lib, loader)
    call link join(home, "python.cls"), join(bin, "python.cls")
    say "  linked" loader "into" lib "and python.cls into" bin
  end
  -- the uninstaller, next to rexx
  target = join(bin, uninstaller)
  if SysFileCopy(me, target) \= 0 then call denied target
  done~put(target, "file")
  say "  copied this program to" target "(rexx oorexxpython-uninstall)"
  -- the samples
  if .File~new(join(from, "samples"))~isDirectory then do
    samples = join(samplesFolder(), "python")
    call copyTree join(from, "samples"), samples
    say "  copied the samples to" samples
  end
  call writeManifest done

  -- Python
  ok = .true
  executables = .Directory~new           -- exe as given -> sys.executable
  withLib = .Directory~new               -- those with their shared library
  noLib = .Array~new                     -- those without (installed, Rexx cannot load them)
  do exe over pythons
    info = pythonInfo(exe)
    if info == .nil then do; ok = .false; iterate; end
    parse var info version "0a"x executable "0a"x site "0a"x hasLib
    executables[exe] = executable
    if hasLib then withLib[exe] = executable
    if \.File~new(site)~isDirectory then .File~new(site)~makeDirs
    pth = join(site, "oorexxpython.pth")
    if \writeFile(pth, home) then call denied pth
    done~put(pth, "pth")
    call writeManifest done
    say "  Python" version "("executable"): oorexxpython.pth in" site
  end

  -- ooRexx's Python: --rexx-python, else the earlier choice, else the first
  chosen = ""
  if rexxPython \== "" then chosen = executables[rexxPython]
  else do
    chosen = configuredPython()
    if chosen \== "", \.File~new(chosen)~exists then chosen = ""
    -- the first that Rexx can load, else the first
    if chosen == "" then do exe over pythons
      if withLib~hasIndex(exe) then do; chosen = withLib[exe]; leave; end
    end
    if chosen == "" then do exe over pythons
      if executables~hasIndex(exe) then do; chosen = executables[exe]; leave; end
    end
  end
  if chosen == .nil | chosen == "" then do
    say "  ooRexx's Python: none could be run"
    ok = .false
  end
  else do
    text = "# The Python that ooRexx uses for the bridge (written by install.rex;" || "0a"x ||,
           "# the environment variable ORX_PYTHON, when set, takes precedence)" || "0a"x ||,
           "python =" chosen
    if \writeFile(config, text) then call denied config
    done~put(config, "file")
    call writeManifest done
    say "  ooRexx's Python:" chosen "(in" config")"
  end

  -- check it, as another program would: from elsewhere, with no variables
  say "Checking:"
  do exe over pythons
    if executables~hasIndex(exe), \check(exe) then ok = .false
  end
  if chosen \== .nil, chosen \== "", \check("") then ok = .false
  if \ok then do
    say "ooRexx/Python: installed, but a check FAILED (above)"
    do exe over noLib
      say "  Python" exe "has no shared library installed: see above what to install"
    end
    -- ::requires finds python.cls through the PATH (design.md, *Installation*)
    if \onPath(bin) then do
      say "  ooRexx's folder," bin", is not on the PATH, so Rexx programs cannot find"
      say "  python.cls: add it to the PATH (ooRexx's own installer does; a terminal"
      say "  or program started before it ran still has the old PATH)"
    end
    exit 1
  end
  say "ooRexx/Python: installed and OK"
  say "  (to remove it: rexx oorexxpython-uninstall, from anywhere)"
  return

uninstall:
  done = readManifest()
  if done~items = 0 then do
    say "ooRexx/Python: not installed in this ooRexx ("rexxExe")"
    return
  end
  self = join(bin, uninstaller)
  failed = \dropBoolean()               -- an earlier version's boolean.cls
  do kind over "pth", "link", "file"
    do path over done~allAt(kind)
      if same(path, self) then iterate   -- last: it may be the one running
      if \.File~new(path)~exists & SysIsFileLink(path) \= 1 then iterate
      if kind == "link", \linksInto(path, home) then iterate   -- replaced since: not ours
      if SysFileDelete(path) \= 0 then do
        say "  could not remove" path
        failed = .true
      end
      else say "  removed" path
    end
  end
  -- folders, deepest first (only if empty: nothing of the user's is lost)
  dirs = done~allAt("dir")~sortWith(.DescendingLength~new)
  do path over dirs
    if .File~new(path)~isDirectory then call SysRmDir path
  end
  if failed then call denied manifest
  call SysFileDelete manifest
  if \windows then call SysRmDir home
  if done~hasItem(self, "file") | .File~new(self)~exists then do
    if SysFileDelete(self) \= 0 then say "  could not remove" self "(delete it by hand)"
    else say "  removed" self
  end
  say "ooRexx/Python: uninstalled"
  return

status:
  say "ooRexx" .RexxInfo~version "r".RexxInfo~revision "("rexxExe")"
  done = readManifest()
  if done~items = 0 then do
    say "ooRexx/Python: not installed in this ooRexx"
    return
  end
  say "ooRexx/Python: installed in" home
  py = configuredPython()
  if py \== "" then say "  ooRexx's Python:" py
  do kind over "file", "dir", "link", "pth"
    do path over done~allAt(kind)
      if .File~new(path)~exists then state = ""; else state = "  (MISSING)"
      say "  "kind~left(5) path || state
    end
  end
  return

/* ------------------------------------------------------------------------ */

-- a symbolic link at "name" to "target", replacing whatever is there
link: procedure expose done
  use arg target, name
  if .File~new(name)~exists | SysIsFileLink(name) = 1 then call SysFileDelete name
  address system "ln -s '"target"' '"name"'"
  if rc \= 0 then call denied name
  done~put(name, "link")
  return

-- the Python's version, executable and site-packages (or user
-- site-packages), one per line; .nil (and a message) if it cannot be run
pythonInfo: procedure expose user windows noLib
  use arg exe
  code = .array~of(-
    "import site, sys, sysconfig",-
    "if sys.version_info < (3, 9): sys.exit('Python 3.9 or later is needed')",-
    "print('%d.%d.%d' % sys.version_info[:3])",-
    "print(sys.executable)",-
    "print(site.getusersitepackages() if" (user~?("True", "False")) "else sysconfig.get_path('purelib'))",-
    "import os; v = lambda n: sysconfig.get_config_var(n) or ''",-
    "c = [os.path.join(v('LIBDIR'), v('INSTSONAME'))] if v('INSTSONAME') else []",-
    "c += [os.path.join(v('LIBDIR'), v('LDLIBRARY'))] if v('LDLIBRARY').endswith(('.so', '.dylib')) else []",-
    "c += [os.path.join(v('PYTHONFRAMEWORKPREFIX'), v('LDLIBRARY'))] if v('PYTHONFRAMEWORKPREFIX') and '.framework/' in v('LDLIBRARY') else []",-
    "print('shared' if os.name == 'nt' or any(os.path.exists(x) for x in c) else 'missing ' + c[0] if c else 'static')")
  out = .array~new; err = .array~new
  address system quote(exe) "-" with input using (code) output using (out) error using (err)
  if rc \= 0 | out~items < 3 then do
    say "  Python" exe": cannot be used (" || err~lastItem~string || ")"
    say "    give the Python to use with --python EXE"
    return .nil
  end
  shared = out[4]~string
  if shared~word(1) == "missing" then do
    call noLibpython out[1], shared~subword(2)
    noLib~append(out[2])
  end
  else if shared == "static" then do
    say "  Python" out[1] "("out[2]") has no shared library (it was built statically):"
    say "    Rexx programs cannot load it; Python programs can still import oorexx."
    say "    For Rexx, use another Python (python.org's, uv's, Homebrew's, your distribution's)."
  end
  return out[1] || "0a"x || out[2] || "0a"x || out[3] || "0a"x || (shared == "shared")

-- a Python whose shared library (libpython3.X.so) is not installed: what to
-- install, for the usual Linux distributions (/etc/os-release)
noLibpython: procedure
  use arg version, path
  parse var version major "." minor "."
  say "  Python" version": its shared library," path", is not installed:"
  say "    Rexx programs cannot load this Python until it is (Python programs can"
  say "    still import oorexx)."
  ids = ""
  if .File~new("/etc/os-release")~exists then do
    s = .Stream~new("/etc/os-release")
    do line over s~arrayIn
      parse var line key "=" value
      if key == "ID" | key == "ID_LIKE" then ids = ids value~strip(, '"')
    end
    s~close
  end
  ids = ids~lower
  select
    when ids~wordPos("debian") > 0 | ids~wordPos("ubuntu") > 0 then
      say "    Install it: sudo apt install libpython"major"."minor
    when ids~wordPos("fedora") > 0 | ids~wordPos("rhel") > 0 | ids~wordPos("centos") > 0 then
      say "    Install it: sudo dnf install python"major"-libs"
    when ids~wordPos("opensuse") > 0 | ids~wordPos("suse") > 0 then
      say "    Install it: sudo zypper install libpython"major"_"minor"-1_0"
    otherwise
      say "    Install your system's package with libpython"major"."minor".so, or use another Python."
  end
  return

-- run a Rexx program and a Python program in a temporary folder, with
-- REXX_PATH, PYTHONPATH and the library path cleared and ORX_PYTHON = exe;
-- exe = "": only the Rexx program, with no ORX_PYTHON (ooRexx's Python)
check: procedure expose rexxExe home windows
  use arg exe
  temp = SysTempFileName(join(.File~temporaryPath~absolutePath, "orxpy????"))
  .File~new(temp)~makeDirs
  rexxProg = .array~of(-
    "sys = .py~import('sys')",-
    "say '    Rexx -> Python: ooRexx' .RexxInfo~version 'r'.RexxInfo~revision',' 'Python' sys~version~word(1) '('sys~executable')'",-
    "if .py~list((1, 2))~__str__ \== '[1, 2]' then exit 1",-
    "exit 0",-
    "::requires 'python.cls'")
  call writeFile join(temp, "check.rex"), rexxProg~makeString("L", "0a"x)
  pyProg = .array~of(-
    "import os, sys, oorexx",-
    "with oorexx.Interpreter() as r:",-
    "    v = r.eval(""'r'.RexxInfo~revision"")",-
    "    ok = r.routine('use arg a; return a~reverse')('Python') == 'nohtyP'",-
    "print('    Python -> Rexx: oorexx from', os.path.dirname(oorexx.__file__) + ',', 'ooRexx', v)",-
    "sys.exit(0 if ok else 1)")
  saved = .Directory~new
  do var over "REXX_PATH", "PYTHONPATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "ORX_PYTHON"
    saved[var] = value(var, , "ENVIRONMENT")
    call value var, "", "ENVIRONMENT"
  end
  call value "ORX_PYTHON", exe, "ENVIRONMENT"
  here = directory()
  call directory temp
  address system quote(rexxExe) "check.rex"
  okRexx = rc = 0
  if exe == "" then okPython = .true
  else do
    address system quote(exe) "-" with input using (pyProg)
    okPython = rc = 0
  end
  call directory here
  do var over saved~allIndexes
    call value var, saved[var], "ENVIRONMENT"
  end
  call SysFileDelete join(temp, "check.rex")
  call SysRmDir temp
  if \okRexx then do
    if exe == "" then say "    Rexx -> Python with ooRexx's Python (no ORX_PYTHON): FAILED"
    else say "    Rexx -> Python with" exe": FAILED"
  end
  if \okPython then say "    Python -> Rexx with" exe": FAILED"
  return okRexx & okPython

-- is the folder on the PATH?
onPath: procedure expose windows
  use arg dir
  if windows then sep = ";"; else sep = ":"
  dir = .File~new(dir)~absolutePath
  path = value("PATH", , "ENVIRONMENT")
  do while path \== ""
    parse var path entry (sep) path
    if entry == "" then iterate
    entry = .File~new(entry~strip(, '"'))~absolutePath
    if .File~isCaseSensitive then do; if entry == dir then return .true; end
    else if entry~caselessEquals(dir) then return .true
  end
  return .false

-- the Python recorded in oorexxpython.cfg, or ""
configuredPython: procedure expose config
  if \.File~new(config)~exists then return ""
  s = .Stream~new(config)
  found = ""
  do line over s~arrayIn
    if line~strip~left(1) == "#" then iterate
    parse var line key "=" value
    if key~strip~caselessEquals("python") then found = value~strip
  end
  s~close
  return found

-- ooRexx's samples folder: beside rexx.exe (Windows), beside bin/ (the
-- portable builds), share/ooRexx/samples (Linux packages), else one of ours
samplesFolder: procedure expose bin home windows
  if windows then candidates = .array~of(join(bin, "samples"), join(.File~new(bin)~parent, "samples"))
  else candidates = .array~of(join(.File~new(bin)~parent, "samples"),-
                              join(join(join(.File~new(bin)~parent, "share"), "ooRexx"), "samples"))
  do c over candidates
    if .File~new(c)~isDirectory then return c
  end
  if windows then return join(bin, "samples")
  return join(home, "samples")

-- copy a folder's files and subfolders, adding them to the manifest
copyTree: procedure expose done windows
  use arg source, target
  if \.File~new(target)~isDirectory then do
    if \.File~new(target)~makeDirs then call denied target
  end
  done~put(target, "dir")
  do f over .File~new(source)~listFiles
    name = join(target, f~name)
    if f~isDirectory then call copyTree f~absolutePath, name
    else do
      if SysFileCopy(f~absolutePath, name) \= 0 then call denied name
      done~put(name, "file")
    end
  end
  return

-- the boolean.cls an earlier version installed (in home; on Linux and macOS
-- linked into bin/; on Windows home is bin/): the other ooRexx bridges may
-- use it there.  If one of them (net.cls, lua.cls, js.cls) is left in bin/,
-- bin/boolean.cls stays (a link to ours becomes a plain copy); otherwise ours
-- is removed.  A bin/boolean.cls that is not ours is left alone.  Updates
-- the manifest; .false if something could not be done
dropBoolean: procedure expose done bin home windows manifest
  mine = join(home, "boolean.cls")
  shared = join(bin, "boolean.cls")
  others = .false
  do f over "net.cls", "lua.cls", "js.cls"
    if .File~new(join(bin, f))~exists | SysIsFileLink(join(bin, f)) = 1 then others = .true
  end
  ok = .true
  if \windows, SysIsFileLink(shared) = 1, linksInto(shared, home) then do
    call SysFileDelete shared
    if others then do
      if SysFileCopy(mine, shared) \= 0 then do
        say "  could not copy boolean.cls to" shared
        ok = .false
      end
      else say "  left a copy of boolean.cls in" bin "(another ooRexx bridge uses it)"
    end
    else say "  removed" shared "(an earlier version's)"
  end
  if done~hasItem(mine, "file"), .File~new(mine)~exists, \(windows & others) then do
    if SysFileDelete(mine) \= 0 then do
      say "  could not remove" mine
      ok = .false
    end
    else say "  removed" mine "(an earlier version's)"
  end
  done~removeItem(shared, "link")
  done~removeItem(mine, "file")
  if done~items > 0 then call writeManifest done
  return ok

-- whether the link path still points into folder (another installer may have
-- replaced it since)
linksInto: procedure
  use arg path, folder
  address system "readlink '"path"'" with output stem out.
  if rc \= 0 | out.0 = 0 then return .false
  return out.1~startsWith(folder || .File~separator)

-- the manifest: lines "kind path" (kind: file, dir, link, pth) -> a Relation
readManifest: procedure expose manifest
  done = .Relation~new
  if \.File~new(manifest)~exists then return done
  s = .Stream~new(manifest)
  do line over s~arrayIn
    parse var line kind path
    if path \== "" then done~put(path, kind)
  end
  s~close
  return done

writeManifest: procedure expose manifest
  use arg done
  lines = .array~new
  do kind over "file", "dir", "link", "pth"
    seen = .Set~new
    do path over done~allAt(kind)
      if seen~hasIndex(path) then iterate
      seen~put(path)
      lines~append(kind path)
    end
  end
  if \writeFile(manifest, lines~makeString("L", "0a"x)) then call denied manifest
  return

writeFile: procedure                    -- replaces the file; .false if it cannot
  use arg path, text
  call SysFileDelete path
  s = .Stream~new(path)
  if s~open("write replace") \= "READY:" then return .false
  s~lineOut(text)
  s~close
  return .true

join: procedure                         -- dir + separator + name
  use arg dir, name
  if dir~right(1) == .File~separator then return dir || name
  return dir || .File~separator || name

same: procedure                         -- the same folder?
  use arg a, b
  a = .File~new(a)~absolutePath; b = .File~new(b)~absolutePath
  if .File~isCaseSensitive then return a == b
  return a~caselessEquals(b)

quote: procedure                        -- a program path for a command line
  use arg exe
  if exe~pos(" ") = 0 then return exe
  return '"'exe'"'

denied:
  use arg path
  say "ooRexx/Python: cannot write" path
  if windows then say "  ooRexx is in a protected folder: run this from a terminal opened as administrator"
  else say "  ooRexx is in a system folder: run it as root (sudo rexx install ...)"
  exit 1

fail:
  use arg why
  say "ooRexx/Python:" why
  exit 1

help:
  say "Usage: rexx install [install] [--python EXE]... [--rexx-python EXE] [--user]"
  say "       rexx install uninstall"
  say "       rexx install status"
  say "       rexx oorexxpython-uninstall [status]   (once installed, from anywhere)"
  say "Installs the ooRexx/Python bridge into the ooRexx that runs this program,"
  say "and makes it importable from the given Pythons (default: ORX_PYTHON, else"
  say "the python3 on the PATH; Windows: python).  --user: into the Pythons' user"
  say "site-packages instead of their site-packages.  --rexx-python: the Python"
  say "that Rexx programs use (recorded in oorexxpython.cfg; ORX_PYTHON overrides"
  say "it); default: the one an earlier install recorded, else the first --python."
  exit 0

::class DescendingLength                -- longer paths (deeper folders) first
::method compare
  use arg a, b
  return b~length - a~length
