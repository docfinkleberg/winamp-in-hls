; NSIS installer for the Winamp HLS input plug-in.
; Compile with scripts\make-installer.ps1 (expects dist\in_hls-win32 from package.ps1).
;
; Winamp and most of its plug-ins use NSIS; this keeps the same installer family.

!define PRODUCT_NAME "HLS Input Plug-in for Winamp"
!define PRODUCT_VERSION "1.1.0"
!define PRODUCT_PUBLISHER "winamp-in-hls"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\in_hls"
!define PACKAGE_DIR "..\dist\in_hls-win32"

Unicode true
RequestExecutionLevel admin
SetCompressor /SOLID lzma
CRCCheck on

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "..\dist\in_hls-${PRODUCT_VERSION}-win32-setup.exe"
InstallDir "$PROGRAMFILES\Winamp\Plugins"
InstallDirRegKey HKCU "Software\in_hls" "InstallDir"
ShowInstDetails show
ShowUnInstDetails show
BrandingText "${PRODUCT_NAME} ${PRODUCT_VERSION}"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_NOAUTOCLOSE
!define MUI_FINISHPAGE_TEXT \
  "Setup has finished installing ${PRODUCT_NAME} on your computer.$\r$\n$\r$\nRestart Winamp if it is running so it can load in_hls.dll.$\r$\n$\r$\nThen open a local .m3u8 file or a URL whose path ends in .m3u8."
!define MUI_FINISHPAGE_LINK "Open installation notes"
!define MUI_FINISHPAGE_LINK_LOCATION "$INSTDIR\README.txt"

!define MUI_DIRECTORYPAGE_TEXT_TOP \
  "Setup will install ${PRODUCT_NAME} into Winamp's Plugins folder.$\r$\n$\r$\nA 32-bit Winamp 5.x install is required. A 64-bit Winamp process cannot load this plug-in."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE.md"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ; Prefer the path recorded by a previous install of this plug-in.
  ReadRegStr $R0 HKCU "Software\in_hls" "InstallDir"
  ${If} $R0 != ""
  ${AndIf} ${FileExists} "$R0"
    StrCpy $INSTDIR $R0
    Return
  ${EndIf}

  ; Classic Winamp install path (default value of HKCU\Software\Winamp).
  ReadRegStr $R0 HKCU "Software\Winamp" ""
  ${If} $R0 != ""
  ${AndIf} ${FileExists} "$R0\winamp.exe"
    StrCpy $INSTDIR "$R0\Plugins"
    Return
  ${EndIf}

  ; 32-bit registry view on 64-bit Windows.
  SetRegView 32
  ReadRegStr $R0 HKLM "Software\Winamp" ""
  ${If} $R0 != ""
  ${AndIf} ${FileExists} "$R0\winamp.exe"
    StrCpy $INSTDIR "$R0\Plugins"
    SetRegView default
    Return
  ${EndIf}

  ReadRegStr $R0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Winamp" "InstallLocation"
  ${If} $R0 != ""
  ${AndIf} ${FileExists} "$R0\winamp.exe"
    StrCpy $INSTDIR "$R0\Plugins"
    SetRegView default
    Return
  ${EndIf}
  SetRegView default

  ${If} ${FileExists} "$PROGRAMFILES\Winamp\Plugins"
    StrCpy $INSTDIR "$PROGRAMFILES\Winamp\Plugins"
  ${ElseIf} ${RunningX64}
  ${AndIf} ${FileExists} "$PROGRAMFILES32\Winamp\Plugins"
    StrCpy $INSTDIR "$PROGRAMFILES32\Winamp\Plugins"
  ${EndIf}
FunctionEnd

Function .onVerifyInstDir
  ; Soft check: warn if the folder does not look like Winamp's Plugins directory.
  ${IfNot} ${FileExists} "$INSTDIR\in_mp3.dll"
  ${AndIfNot} ${FileExists} "$INSTDIR\in_wave.dll"
  ${AndIfNot} ${FileExists} "$INSTDIR\..\winamp.exe"
    MessageBox MB_YESNO|MB_ICONQUESTION \
      "The selected folder does not look like a Winamp Plugins directory.$\r$\n$\r$\nInstall here anyway?" \
      IDYES +2
    Abort
  ${EndIf}
FunctionEnd

Section "Plug-in" SecPlugin
  SectionIn RO
  ; Drop FFmpeg copies left in Plugins\ by 1.0.0 so they cannot clash.
  Delete "$INSTDIR\avutil-61.dll"
  Delete "$INSTDIR\swresample-7.dll"
  Delete "$INSTDIR\swscale-10.dll"
  Delete "$INSTDIR\avcodec-63.dll"
  Delete "$INSTDIR\avformat-63.dll"

  SetOutPath "$INSTDIR\in_hls"
  File "${PACKAGE_DIR}\in_hls\avutil-61.dll"
  File "${PACKAGE_DIR}\in_hls\swresample-7.dll"
  File "${PACKAGE_DIR}\in_hls\swscale-10.dll"
  File "${PACKAGE_DIR}\in_hls\avcodec-63.dll"
  File "${PACKAGE_DIR}\in_hls\avformat-63.dll"

  SetOutPath $INSTDIR
  File "${PACKAGE_DIR}\in_hls.dll"
  File "${PACKAGE_DIR}\README.txt"
  File "${PACKAGE_DIR}\LICENSE.md"
  File "${PACKAGE_DIR}\THIRD-PARTY-NOTICES.md"
  File "${PACKAGE_DIR}\FFMPEG-LGPL-3.0.txt"
  File "${PACKAGE_DIR}\GPL-3.0.txt"

  WriteUninstaller "$INSTDIR\Uninstall-in_hls.exe"

  WriteRegStr HKCU "Software\in_hls" "InstallDir" "$INSTDIR"

  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall-in_hls.exe"'
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\in_hls.dll"
  Delete "$INSTDIR\avutil-61.dll"
  Delete "$INSTDIR\swresample-7.dll"
  Delete "$INSTDIR\swscale-10.dll"
  Delete "$INSTDIR\avcodec-63.dll"
  Delete "$INSTDIR\avformat-63.dll"
  Delete "$INSTDIR\in_hls\avutil-61.dll"
  Delete "$INSTDIR\in_hls\swresample-7.dll"
  Delete "$INSTDIR\in_hls\swscale-10.dll"
  Delete "$INSTDIR\in_hls\avcodec-63.dll"
  Delete "$INSTDIR\in_hls\avformat-63.dll"
  RMDir "$INSTDIR\in_hls"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\LICENSE.md"
  Delete "$INSTDIR\THIRD-PARTY-NOTICES.md"
  Delete "$INSTDIR\FFMPEG-LGPL-3.0.txt"
  Delete "$INSTDIR\GPL-3.0.txt"
  Delete "$INSTDIR\Uninstall-in_hls.exe"
  ; Leave in_hls.ini and in_hls.log (user settings / diagnostics).

  DeleteRegKey HKLM "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKCU "Software\in_hls"
SectionEnd
