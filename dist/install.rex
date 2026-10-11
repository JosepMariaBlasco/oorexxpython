/* install.rex: installs the ooRexx/Python bridge into the ooRexx that runs it,
 * so that Rexx programs find python.cls and Python programs "import oorexx"
 * with no environment variables.
 *
 *   rexx install [install] [--python EXE]... [--user]
 *   rexx install uninstall
 *   rexx install status
 *
 * Where things go (docs/design.md, *Installation*):
 *   Windows:        the bridge, its package and python.cls in the folder of
 *                   rexx.exe and rexx.dll (C:\Program Files\ooRexx).
 *   Linux, macOS:   all three in ooRexx's lib/oorexxpython/; the package is
 *                   linked into lib/ (where ooRexx looks for libraries) and
 *                   python.cls into bin/ (on the PATH, where ::requires looks).
 *   Python:         a one-line oorexxpython.pth in the site-packages of each
 *                   Python given with --python (default: ORX_PYTHON, else
 *                   python3 on the PATH; Windows: python), or in its user
 *                   site-packages with --user.
 * What was installed is listed in oorexxpython-installed.txt, in that same
 * folder; uninstall removes exactly that.  Installing again (a new version,
 * another Python) replaces the files and adds to the list.
 *
 * ooRexx in a system folder needs an administrator: on Windows, a terminal
 * opened "as administrator"; on Linux and macOS, sudo rexx install.
 */
parse arg args
command = "install"; pythons = .array~new; user = .false
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
    when "--user", "-user" then user = .true
    when "-h", "--help", "-?", "help", "/?" then call help
    otherwise call fail "unknown argument:" word "(rexx install --help)"
  end
end

windows = .File~separator == "\"
parse source . . me
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

select case command
  when "status"    then call status
  when "uninstall" then call uninstall
  otherwise             call install
end
exit 0

/* ------------------------------------------------------------------------ */

install:
  say "ooRexx/Python installer"
  say "  ooRexx" .RexxInfo~version "r".RexxInfo~revision "("rexxExe")"
  parse value .RexxInfo~version with major "." minor "."
  if major < 5 | (major = 5 & minor < 2) then
    call fail "ooRexx 5.2.0 or later is needed"
  if major = 5 & minor = 2 then
    say "  note: using the bridge from several threads at once needs ooRexx 5.3.0 r13265 or later"
  do f over loader, bridge, "python.cls"
    if \.File~new(join(from, f))~exists then
      call fail "no" f "next to install.rex: run it from the unzipped oorexxpython folder"
  end
  if windows, \.File~new(join(bin, "rexx.dll"))~exists then
    call fail "rexx.dll is not next to rexx.exe in" bin": this layout of ooRexx is not supported"
  if same(from, home) then call fail "install.rex is already in" home

  if pythons~isEmpty then do
    exe = value("ORX_PYTHON", , "ENVIRONMENT")
    if exe == "" then if windows then exe = "python"; else exe = "python3"
    pythons~append(exe)
  end

  done = readManifest()                 -- what an earlier install left
  -- the bridge
  if \.File~new(home)~isDirectory then
    if \.File~new(home)~makeDirs then call denied home
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
  call writeManifest done

  -- Python
  ok = .true
  do exe over pythons
    info = pythonInfo(exe)
    if info == .nil then do; ok = .false; iterate; end
    parse var info version "0a"x executable "0a"x site
    if \.File~new(site)~isDirectory then .File~new(site)~makeDirs
    pth = join(site, "oorexxpython.pth")
    if \writeFile(pth, home) then call denied pth
    done~put(pth, "pth")
    call writeManifest done
    say "  Python" version "("executable"): oorexxpython.pth in" site
  end

  -- check it, as another program would: from elsewhere, with no variables
  say "Checking:"
  do exe over pythons
    if \check(exe) then ok = .false
  end
  if \ok then do
    say "ooRexx/Python: installed, but a check FAILED (above)"
    exit 1
  end
  say "ooRexx/Python: installed and OK"
  say "  (to remove it: rexx install uninstall)"
  return

uninstall:
  done = readManifest()
  if done~items = 0 then do
    say "ooRexx/Python: not installed in this ooRexx ("rexxExe")"
    return
  end
  failed = .false
  do kind over "pth", "link", "file"
    do path over done~allAt(kind)
      if \.File~new(path)~exists & SysIsFileLink(path) \= 1 then iterate
      if SysFileDelete(path) \= 0 then do
        say "  could not remove" path
        failed = .true
      end
      else say "  removed" path
    end
  end
  if failed then call denied manifest
  call SysFileDelete manifest
  if \windows then call SysRmDir home
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
  do kind over "file", "link", "pth"
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
pythonInfo: procedure expose user windows
  use arg exe
  code = .array~of(-
    "import site, sys, sysconfig",-
    "if sys.version_info < (3, 9): sys.exit('Python 3.9 or later is needed')",-
    "print('%d.%d.%d' % sys.version_info[:3])",-
    "print(sys.executable)",-
    "print(site.getusersitepackages() if" (user~?("True", "False")) "else sysconfig.get_path('purelib'))")
  out = .array~new; err = .array~new
  address system quote(exe) "-" with input using (code) output using (out) error using (err)
  if rc \= 0 | out~items < 3 then do
    say "  Python" exe": cannot be used (" || err~lastItem~string || ")"
    say "    give the Python to use with --python EXE"
    return .nil
  end
  return out[1] || "0a"x || out[2] || "0a"x || out[3]

-- run a Rexx program and a Python program in a temporary folder, with
-- REXX_PATH, PYTHONPATH and the library path cleared and ORX_PYTHON = exe
check: procedure expose rexxExe home windows
  use arg exe
  temp = SysTempFileName(join(.File~temporaryPath~absolutePath, "orxpy????"))
  .File~new(temp)~makeDirs
  rexxProg = .array~of(-
    "sys = .py~import('sys')",-
    "say '    Rexx -> Python: ooRexx' .RexxInfo~version 'r'.RexxInfo~revision',' 'Python' sys~version~word(1)",-
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
  address system quote(exe) "-" with input using (pyProg)
  okPython = rc = 0
  call directory here
  do var over saved~allIndexes
    call value var, saved[var], "ENVIRONMENT"
  end
  call SysFileDelete join(temp, "check.rex")
  call SysRmDir temp
  if \okRexx then say "    Rexx -> Python with" exe": FAILED"
  if \okPython then say "    Python -> Rexx with" exe": FAILED"
  return okRexx & okPython

-- the manifest: lines "kind path" (kind: file, link, pth) -> a Relation
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
  do kind over "file", "link", "pth"
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
  say "Usage: rexx install [install] [--python EXE]... [--user]"
  say "       rexx install uninstall"
  say "       rexx install status"
  say "Installs the ooRexx/Python bridge into the ooRexx that runs this program,"
  say "and makes it importable from the given Pythons (default: ORX_PYTHON, else"
  say "the python3 on the PATH; Windows: python).  --user: into the Pythons' user"
  say "site-packages instead of their site-packages."
  exit 0
