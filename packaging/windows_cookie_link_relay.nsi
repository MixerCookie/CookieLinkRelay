Unicode true
ManifestDPIAware true
RequestExecutionLevel admin

!ifndef VERSION
  !define VERSION "dev"
!endif

!ifndef BUILD_DIR
  !define BUILD_DIR "build"
!endif

!ifndef OUTFILE
  !define OUTFILE "CookieLinkRelay-${VERSION}-windows-x64-installer.exe"
!endif

Name "CookieLinkRelay"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\CookieLinkRelay"

!include "MUI2.nsh"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "CookieLinkRelay" SecCookieLinkRelay
  SetShellVarContext all

  SetOutPath "$INSTDIR"
  File "${BUILD_DIR}\CookieLinkRelay_artefacts\Release\CookieLinkRelay.exe"

  CreateDirectory "$SMPROGRAMS\CookieLinkRelay"
  CreateShortcut "$SMPROGRAMS\CookieLinkRelay\CookieLinkRelay.lnk" "$INSTDIR\CookieLinkRelay.exe"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CookieLinkRelay" "DisplayName" "CookieLinkRelay"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CookieLinkRelay" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CookieLinkRelay" "Publisher" "Cookie Studio"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CookieLinkRelay" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
SectionEnd

Section "Uninstall"
  SetShellVarContext all

  Delete "$SMPROGRAMS\CookieLinkRelay\CookieLinkRelay.lnk"
  RMDir "$SMPROGRAMS\CookieLinkRelay"

  Delete "$INSTDIR\CookieLinkRelay.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CookieLinkRelay"
SectionEnd
