; SPDX-License-Identifier: Apache-2.0
;
; oorexxpython.nsi: the Windows installer of the ooRexx/Python bridge, an
; NSIS 3 script.  A thin wrapper around install.rex, which does the real work
; on every platform: this program finds ooRexx and the Pythons, runs
; install.rex with them, and adds what a Windows user expects (an entry in
; Settings > Apps, the books and the samples' deck in the Start menu, an
; uninstaller).  Modelled on ooRexx's own installer and ooSQLite's.
;
;   makensis /DVERSION=0.1.0 /DPKGDIR=<the unzipped oorexxpython folder>
;            /DDOCDIR=<folder with pythonref.pdf and pythoncookbook.pdf>
;            /DOUTFILE=<the .exe to write> oorexxpython.nsi
;
; Run it as:
;   oorexxpython-VERSION-windows-x64.exe                   (the wizard)
;   ... /S [/PYTHON=C:\path\python.exe[;...]] [/REXXPYTHON=C:\path\python.exe]
;          [/D=C:\ooRexx folder]                          (silent: /D last)
; Without /PYTHON, a silent install uses every Python 3.9+ it finds.  The
; Python that Rexx programs use (oorexxpython.cfg, see install.rex): the one
; chosen in the wizard, or /REXXPYTHON=; else install.rex keeps the one an
; earlier install chose, or takes the first.

Unicode true
!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "StrFunc.nsh"
${StrLoc}
${StrRep}

!ifndef VERSION
  !error "makensis /DVERSION=... /DPKGDIR=... /DDOCDIR=... /DOUTFILE=..."
!endif

!define NAME        "ooRexx/Python"
!define LONGNAME    "ooRexx/Python bridge ${VERSION}"
!define REGKEY      "Software\Microsoft\Windows\CurrentVersion\Uninstall\ooRexxPython"
!define REXXREGKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\ooRexx"
!define UNINSTALLER "uninstall-oorexxpython.exe"
!define SMFOLDER    "$SMPROGRAMS\Open Object Rexx"       ; ooRexx's own
!define SMDOCS      "${SMFOLDER}\ooRexx Documentation"
!define SMSAMPLES   "${SMFOLDER}\ooRexx Samples"
!define PYINI       "$PLUGINSDIR\pythons.ini"
!define LOG         "$TEMP\oorexxpython-install.log"   ; kept: what happened

Name "${NAME}"
Caption "${LONGNAME}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show
InstallDir "$PROGRAMFILES64\ooRexx"
BrandingText "${LONGNAME}"

VIProductVersion "0.0.0.0"
VIAddVersionKey "ProductName" "${NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${LONGNAME} installer"
VIAddVersionKey "LegalCopyright" "Apache License 2.0"

Var Dialog
Var PythonsChosen     ; the --python arguments for install.rex
Var OtherPython       ; a python.exe the user typed or browsed for
Var OtherField
Var RexxPyList        ; the drop list "Rexx programs use"
Var RexxPython        ; --rexx-python for install.rex, or ""
Var Problem

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This wizard installs the ${LONGNAME} into an \
  installed ooRexx (5.3.0 r13297 or later), and makes it importable from the Pythons \
  (3.9 or later) you choose.$\r$\n$\r$\nRexx programs then use Python with \
  ::requires $\"python.cls$\", and Python programs use Rexx with \
  $\"import oorexx$\".$\r$\n$\r$\nClose any program that uses the bridge \
  before you continue."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PKGDIR}\LICENSE"
!define MUI_DIRECTORYPAGE_TEXT_TOP "The bridge is installed into ooRexx's folder, \
  the one with rexx.exe and rexx.dll."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "ooRexx folder"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE RexxFolderLeave
!insertmacro MUI_PAGE_DIRECTORY
Page custom PythonsPage PythonsLeave
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\samples\python\slides\samples.html"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show the samples (a slide deck, in the browser)"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_LINK "github.com/JosepMariaBlasco/oorexxpython"
!define MUI_FINISHPAGE_LINK_LOCATION "https://github.com/JosepMariaBlasco/oorexxpython"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

;-------------------------------------------------------------------------------
; Installer

Function .onInit
  SetShellVarContext all
  InitPluginsDir
  ; ooRexx's installer (32-bit) writes its key in the 32-bit view: look
  ; there first, then in the 64-bit one
  ReadRegStr $0 HKLM "${REXXREGKEY}" "UnInstallLocation"
  ${If} $0 == ""
    SetRegView 64
    ReadRegStr $0 HKLM "${REXXREGKEY}" "UnInstallLocation"
    SetRegView lastused
  ${EndIf}
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${EndIf}
  ; /D= (silent installs) overrides it: NSIS sets $INSTDIR after .onInit
  Call FindPythons
  ${GetParameters} $1
  ClearErrors
  ${GetOptions} $1 "/PYTHON=" $2
  ${IfNot} ${Errors}
    ${StrRep} $2 $2 '"' ''
    StrCpy $PythonsChosen ""
    Call AddPythonList          ; $2: "a.exe;b.exe"
  ${ElseIf} ${Silent}
    Call ChooseAllPythons
  ${EndIf}
  ClearErrors
  ${GetOptions} $1 "/REXXPYTHON=" $2
  ${IfNot} ${Errors}
    ${StrRep} $2 $2 '"' ''
    StrCpy $RexxPython $2
  ${EndIf}
FunctionEnd

; $2 = paths separated by ";" -> appended to $PythonsChosen
Function AddPythonList
  ${Do}
    ${StrLoc} $3 $2 ";" ">"
    ${If} $3 == ""
      StrCpy $4 $2
      StrCpy $2 ""
    ${Else}
      StrCpy $4 $2 $3
      IntOp $3 $3 + 1
      StrCpy $2 $2 "" $3
    ${EndIf}
    ${If} $4 != ""
      StrCpy $PythonsChosen '$PythonsChosen --python "$4"'
    ${EndIf}
  ${LoopUntil} $2 == ""
FunctionEnd

Function ChooseAllPythons
  StrCpy $PythonsChosen ""
  ReadINIStr $0 "${PYINI}" "pythons" "count"
  ${For} $1 1 $0
    ReadINIStr $2 "${PYINI}" "python$1" "exe"
    StrCpy $PythonsChosen '$PythonsChosen --python "$2"'
  ${Next}
FunctionEnd

; The Pythons registered as PEP 514 says (HKCU and HKLM, every company:
; PythonCore for python.org's, Astral for uv's...), 64-bit, 3.9 or later,
; whose python.exe exists: into ${PYINI}, [python1]...[pythonN] exe= label=
Function FindPythons
  WriteINIStr "${PYINI}" "pythons" "count" "0"
  SetRegView 64
  StrCpy $9 "HKCU"
  Call FindPythonsIn
  StrCpy $9 "HKLM"
  Call FindPythonsIn
  SetRegView lastused
FunctionEnd

Function FindPythonsIn          ; $9: HKCU or HKLM
  StrCpy $5 0                   ; company index
  ${Do}
    ${If} $9 == "HKCU"
      EnumRegKey $6 HKCU "Software\Python" $5
    ${Else}
      EnumRegKey $6 HKLM "Software\Python" $5
    ${EndIf}
    ${If} $6 == ""
      ${Break}
    ${EndIf}
    StrCpy $7 0                 ; tag index
    ${Do}
      ${If} $9 == "HKCU"
        EnumRegKey $8 HKCU "Software\Python\$6" $7
      ${Else}
        EnumRegKey $8 HKLM "Software\Python\$6" $7
      ${EndIf}
      ${If} $8 == ""
        ${Break}
      ${EndIf}
      Push "Software\Python\$6\$8"
      Call AddPython
      IntOp $7 $7 + 1
    ${Loop}
    IntOp $5 $5 + 1
  ${Loop}
FunctionEnd

Function AddPython              ; stack: the tag's key; $9: the root
  Exch $0
  Push $1
  Push $2
  Push $3
  Push $4
  ${If} $9 == "HKCU"
    ReadRegStr $1 HKCU "$0\InstallPath" "ExecutablePath"
    ReadRegStr $2 HKCU "$0\InstallPath" ""
    ReadRegStr $3 HKCU "$0" "SysVersion"
    ReadRegStr $4 HKCU "$0" "SysArchitecture"
  ${Else}
    ReadRegStr $1 HKLM "$0\InstallPath" "ExecutablePath"
    ReadRegStr $2 HKLM "$0\InstallPath" ""
    ReadRegStr $3 HKLM "$0" "SysVersion"
    ReadRegStr $4 HKLM "$0" "SysArchitecture"
  ${EndIf}
  ${If} $1 == ""
  ${AndIf} $2 != ""
    StrCpy $1 "$2\python.exe"
    ${StrRep} $1 $1 "\\" "\"
  ${EndIf}
  ${If} $3 == ""                ; PythonCore leaves SysVersion out: the tag is X.Y[-32]
    ${GetFileName} $0 $3
  ${EndIf}
  ${If} $1 == ""
  ${OrIfNot} ${FileExists} $1
    Goto done
  ${EndIf}
  ${If} $4 == "32bit"
    Goto done
  ${EndIf}
  ${StrLoc} $2 $3 "-32" ">"
  ${If} $2 != ""
    Goto done
  ${EndIf}
  Push $3
  Call AtLeast39
  Pop $2
  ${If} $2 != "1"
    Goto done
  ${EndIf}
  ; once per executable (HKCU and HKLM, or two companies, may name the same)
  ReadINIStr $2 "${PYINI}" "pythons" "count"
  ${For} $4 1 $2
    ReadINIStr $0 "${PYINI}" "python$4" "exe"
    ${If} $0 == $1
      Goto done
    ${EndIf}
  ${Next}
  IntOp $2 $2 + 1
  WriteINIStr "${PYINI}" "pythons" "count" $2
  WriteINIStr "${PYINI}" "python$2" "exe" $1
  WriteINIStr "${PYINI}" "python$2" "label" "Python $3  ($1)"
  done:
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd

Function AtLeast39              ; stack: "X.Y..." -> "1" if >= 3.9, else "0"
  Exch $0
  Push $1
  Push $2
  ${StrLoc} $1 $0 "." ">"
  ${If} $1 == ""
    StrCpy $0 "0"
  ${Else}
    StrCpy $2 $0 $1             ; major
    IntOp $1 $1 + 1
    StrCpy $0 $0 "" $1          ; minor and the rest
    IntOp $0 $0 + 0             ; leading digits only
    ${If} $2 > 3
    ${OrIf} $0 >= 9
    ${AndIf} $2 == 3
      StrCpy $0 "1"
    ${Else}
      StrCpy $0 "0"
    ${EndIf}
  ${EndIf}
  Pop $2
  Pop $1
  Exch $0
FunctionEnd

Function RexxFolderLeave
  ${IfNot} ${FileExists} "$INSTDIR\rexx.exe"
  ${OrIfNot} ${FileExists} "$INSTDIR\rexx.dll"
    MessageBox MB_OK|MB_ICONEXCLAMATION "There is no ooRexx in $INSTDIR \
      (rexx.exe and rexx.dll).$\r$\n$\r$\nChoose the folder of ooRexx 5.3.0 \
      r13297 or later, or install ooRexx first (sourceforge.net/projects/oorexx)."
    Abort
  ${EndIf}
FunctionEnd

Function PythonsPage
  !insertmacro MUI_HEADER_TEXT "Pythons" "The Pythons that will import oorexx"
  nsDialogs::Create 1018
  Pop $Dialog
  ${If} $Dialog == error
    Abort
  ${EndIf}
  ReadINIStr $0 "${PYINI}" "pythons" "count"
  ${If} $0 == 0
    ${NSD_CreateLabel} 0 0 100% 24u "No Python 3.9 or later (64-bit) is registered \
      on this computer. Install one from python.org, or give the python.exe of \
      another one below."
  ${Else}
    ${NSD_CreateLabel} 0 0 100% 12u "Make the bridge importable from these Pythons:"
  ${EndIf}
  Pop $1
  StrCpy $2 14
  ${For} $1 1 $0
    ${If} $1 > 6                ; room for six; the rest with /PYTHON= or rexx install
      ${Break}
    ${EndIf}
    ReadINIStr $3 "${PYINI}" "python$1" "label"
    ${NSD_CreateCheckbox} 0 $2u 100% 11u $3
    Pop $4
    ${NSD_Check} $4
    WriteINIStr "${PYINI}" "python$1" "hwnd" $4
    IntOp $2 $2 + 11
  ${Next}
  IntOp $2 $2 + 3
  ${NSD_CreateLabel} 0 $2u 100% 10u "Another Python (its python.exe), optional:"
  Pop $1
  IntOp $2 $2 + 11
  ${NSD_CreateFileRequest} 0 $2u 80% 12u $OtherPython
  Pop $OtherField
  ${NSD_CreateBrowseButton} 82% $2u 18% 12u "Browse..."
  Pop $1
  ${NSD_OnClick} $1 BrowsePython
  ; the Python that Rexx programs use: the one an earlier install chose
  ; (if registered), else the first
  IntOp $2 $2 + 16
  ${NSD_CreateLabel} 0 $2u 35% 10u "Rexx programs use:"
  Pop $1
  ${NSD_CreateDropList} 35% $2u 65% 80u ""
  Pop $RexxPyList
  Call ReadConfiguredPython     ; -> $R0
  StrCpy $3 ""
  ${For} $1 1 $0
    ReadINIStr $4 "${PYINI}" "python$1" "label"
    ${NSD_CB_AddString} $RexxPyList $4
    ReadINIStr $5 "${PYINI}" "python$1" "exe"
    ${If} $3 == ""
    ${OrIf} $5 == $R0
      StrCpy $3 $4
    ${EndIf}
  ${Next}
  ${NSD_CB_AddString} $RexxPyList "Another Python (above)"
  ${If} $3 == ""
    StrCpy $3 "Another Python (above)"
  ${EndIf}
  ${NSD_CB_SelectString} $RexxPyList $3
  nsDialogs::Show
FunctionEnd

Function TrimR0                 ; blanks off both ends of $R0
  ${Do}
    StrCpy $R4 $R0 1
    ${If} $R4 != " "
      ${Break}
    ${EndIf}
    StrCpy $R0 $R0 "" 1
  ${Loop}
  ${Do}
    StrCpy $R4 $R0 1 -1
    ${If} $R4 != " "
      ${Break}
    ${EndIf}
    StrCpy $R0 $R0 -1
  ${Loop}
FunctionEnd

; the python = line of $INSTDIR\oorexxpython.cfg (an earlier install), or ""
Function ReadConfiguredPython
  StrCpy $R0 ""
  ClearErrors
  FileOpen $R1 "$INSTDIR\oorexxpython.cfg" r
  ${If} ${Errors}
    Return
  ${EndIf}
  ${Do}
    ClearErrors
    FileRead $R1 $R2
    ${If} ${Errors}
      ${Break}
    ${EndIf}
    StrCpy $R3 $R2 6
    ${If} $R3 == "python"
      ${StrLoc} $R3 $R2 "=" ">"
      ${If} $R3 != ""
        IntOp $R3 $R3 + 1
        StrCpy $R0 $R2 "" $R3
        ${StrRep} $R0 $R0 "$\r" ""
        ${StrRep} $R0 $R0 "$\n" ""
        Call TrimR0
      ${EndIf}
    ${EndIf}
  ${Loop}
  FileClose $R1
FunctionEnd

Function BrowsePython
  nsDialogs::SelectFileDialog open "" "python.exe|python.exe"
  Pop $0
  ${If} $0 != ""
    ${NSD_SetText} $OtherField $0
  ${EndIf}
FunctionEnd

Function PythonsLeave
  StrCpy $PythonsChosen ""
  ReadINIStr $0 "${PYINI}" "pythons" "count"
  ${For} $1 1 $0
    ReadINIStr $3 "${PYINI}" "python$1" "hwnd"
    ${If} $3 != ""
      ${NSD_GetState} $3 $4
      ${If} $4 == ${BST_CHECKED}
        ReadINIStr $2 "${PYINI}" "python$1" "exe"
        StrCpy $PythonsChosen '$PythonsChosen --python "$2"'
      ${EndIf}
    ${EndIf}
  ${Next}
  ${NSD_GetText} $OtherField $OtherPython
  ${If} $OtherPython != ""
    ${IfNot} ${FileExists} $OtherPython
      MessageBox MB_OK|MB_ICONEXCLAMATION "There is no $OtherPython."
      Abort
    ${EndIf}
    StrCpy $PythonsChosen '$PythonsChosen --python "$OtherPython"'
  ${EndIf}
  ${If} $PythonsChosen == ""
    MessageBox MB_OK|MB_ICONEXCLAMATION "Choose at least one Python."
    Abort
  ${EndIf}
  ${NSD_GetText} $RexxPyList $3
  StrCpy $RexxPython ""
  ${If} $3 == "Another Python (above)"
    ${If} $OtherPython == ""
      MessageBox MB_OK|MB_ICONEXCLAMATION "Rexx programs use $\"another Python$\": give its python.exe."
      Abort
    ${EndIf}
    StrCpy $RexxPython $OtherPython
  ${Else}
    ${For} $1 1 $0
      ReadINIStr $4 "${PYINI}" "python$1" "label"
      ${If} $4 == $3
        ReadINIStr $RexxPython "${PYINI}" "python$1" "exe"
      ${EndIf}
    ${Next}
  ${EndIf}
FunctionEnd

Section "Install"
  ${IfNot} ${FileExists} "$INSTDIR\rexx.exe"
    StrCpy $Problem "There is no ooRexx (rexx.exe) in $INSTDIR."
    Goto failed
  ${EndIf}
  ${If} $PythonsChosen == ""
    StrCpy $Problem "No Python 3.9 or later (64-bit) found: give one with /PYTHON=."
    Goto failed
  ${EndIf}

  ; the package, unpacked where install.rex can take it from
  SetOutPath "$PLUGINSDIR\oorexxpython"
  File /r "${PKGDIR}\*.*"
  SetOutPath "$PLUGINSDIR"      ; not the folder about to be deleted

  StrCpy $1 ""
  ${If} $RexxPython != ""
    StrCpy $1 ' --rexx-python "$RexxPython"'
  ${EndIf}
  ; install.rex's output goes to ${LOG} (a silent install shows nothing),
  ; then into the details
  FileOpen $2 "${LOG}" w
  FileWrite $2 "${LONGNAME}$\r$\nooRexx: $INSTDIR$\r$\nPythons found:$\r$\n"
  ReadINIStr $3 "${PYINI}" "pythons" "count"
  ${For} $4 1 $3
    ReadINIStr $5 "${PYINI}" "python$4" "label"
    FileWrite $2 "  $5$\r$\n"
  ${Next}
  FileWrite $2 "rexx install.rex install$PythonsChosen$1$\r$\n$\r$\n"
  FileClose $2
  DetailPrint "rexx install.rex install$PythonsChosen$1"
  nsExec::Exec '"$SYSDIR\cmd.exe" /c ""$INSTDIR\rexx.exe" "$PLUGINSDIR\oorexxpython\install.rex" install$PythonsChosen$1 >> "${LOG}" 2>&1"'
  Pop $0
  FileOpen $2 "${LOG}" r
  ${Do}
    ClearErrors
    FileRead $2 $3
    ${If} ${Errors}
      ${Break}
    ${EndIf}
    ${StrRep} $3 $3 "$\r" ""
    ${StrRep} $3 $3 "$\n" ""
    DetailPrint $3
  ${Loop}
  FileClose $2
  ${If} $0 != 0
    StrCpy $Problem "install.rex ended with $0: see the details above, or ${LOG}."
    Goto failed
  ${EndIf}

  ; the books, next to ooRexx's
  SetOutPath "$INSTDIR\doc"
  File "${DOCDIR}\pythonref.pdf"
  File "${DOCDIR}\pythoncookbook.pdf"

  CreateDirectory "${SMDOCS}"
  CreateShortCut "${SMDOCS}\ooRexx and Python - Reference.lnk" "$INSTDIR\doc\pythonref.pdf"
  CreateShortCut "${SMDOCS}\ooRexx and Python - Cookbook.lnk" "$INSTDIR\doc\pythoncookbook.pdf"
  CreateDirectory "${SMSAMPLES}"
  CreateShortCut "${SMSAMPLES}\ooRexx and Python samples.lnk" "$INSTDIR\samples\python\slides\samples.html"
  CreateShortCut "${SMFOLDER}\Uninstall ooRexx-Python.lnk" "$INSTDIR\${UNINSTALLER}"

  WriteUninstaller "$INSTDIR\${UNINSTALLER}"
  WriteRegStr HKLM "${REGKEY}" "DisplayName" "${LONGNAME}"
  WriteRegStr HKLM "${REGKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${REGKEY}" "Publisher" "The ooRexx/Python project"
  WriteRegStr HKLM "${REGKEY}" "URLInfoAbout" "https://github.com/JosepMariaBlasco/oorexxpython"
  WriteRegStr HKLM "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${REGKEY}" "DisplayIcon" "$INSTDIR\rexx.exe"
  WriteRegStr HKLM "${REGKEY}" "UninstallString" '"$INSTDIR\${UNINSTALLER}"'
  WriteRegStr HKLM "${REGKEY}" "QuietUninstallString" '"$INSTDIR\${UNINSTALLER}" /S'
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR\doc" "/M=python*.pdf /S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${REGKEY}" "EstimatedSize" $0
  DetailPrint "${LONGNAME}: installed"
  Return

  failed:
  DetailPrint "ooRexx/Python: $Problem"
  ${IfNot} ${Silent}
    MessageBox MB_OK|MB_ICONSTOP "The installation failed.$\r$\n$\r$\n$Problem"
  ${EndIf}
  SetErrorLevel 2
  Abort
SectionEnd

;-------------------------------------------------------------------------------
; Uninstaller: install.rex's own uninstaller removes what it installed (its
; manifest); then what this installer added

Function un.onInit
  SetShellVarContext all
FunctionEnd

Section "Uninstall"
  ${If} ${FileExists} "$INSTDIR\oorexxpython-uninstall.rex"
    nsExec::ExecToLog '"$INSTDIR\rexx.exe" "$INSTDIR\oorexxpython-uninstall.rex"'
    Pop $0
    ${If} $0 != 0
      DetailPrint "oorexxpython-uninstall.rex ended with $0"
    ${EndIf}
  ${EndIf}
  Delete "$INSTDIR\doc\pythonref.pdf"
  Delete "$INSTDIR\doc\pythoncookbook.pdf"
  Delete "${SMDOCS}\ooRexx and Python - Reference.lnk"
  Delete "${SMDOCS}\ooRexx and Python - Cookbook.lnk"
  Delete "${SMSAMPLES}\ooRexx and Python samples.lnk"
  Delete "${SMFOLDER}\Uninstall ooRexx-Python.lnk"
  RMDir "${SMDOCS}"             ; only if empty (a portable ooRexx has no menu)
  RMDir "${SMSAMPLES}"
  RMDir "${SMFOLDER}"
  RMDir "$INSTDIR\doc"
  DeleteRegKey HKLM "${REGKEY}"
  Delete "$INSTDIR\${UNINSTALLER}"
SectionEnd
