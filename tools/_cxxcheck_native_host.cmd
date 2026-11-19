@echo off
setlocal
chcp 437 >nul
rem ===================================================================
rem  tools\_cxxcheck_native_host.cmd   [295 2026-11-19]
rem
rem  Compile-only check for src/ui/native-host. Proves the native host
rem  sources actually compile against the real MSVC + WinSDK + WebView2
rem  SDK on this machine -- the scheme document shipped these files
rem  without ever compiling them (see deliverables/295 section 2).
rem
rem  No linking: this is a syntax/type check, not a build product.
rem  Pure ASCII, no BOM, CRLF.
rem ===================================================================

set "HERE=%~dp0"
set "REPO=%HERE%.."
set "CLROOT=C:\Program1\VC\Tools\MSVC\14.44.35207"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "WV2=C:\Users\%USERNAME%\.nuget\packages\microsoft.web.webview2\1.0.2792.45\build\native\include"

if not exist "%CLROOT%\bin\Hostx64\x64\cl.exe" (
  echo FAIL: cl.exe not found at %CLROOT%
  exit /b 2
)
if not exist "%WV2%\WebView2.h" (
  echo FAIL: WebView2.h not found at %WV2%
  exit /b 2
)

set "INCLUDE=%CLROOT%\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\winrt;%WV2%"
set "LIB=%CLROOT%\lib\x64;%SDK%\Lib\%SDKVER%\um\x64;%SDK%\Lib\%SDKVER%\ucrt\x64"
set "PATH=%CLROOT%\bin\Hostx64\x64;%PATH%"

set "OUT=%TEMP%\rbc-nativehost-check"
if not exist "%OUT%" mkdir "%OUT%"

set "RC=0"
for %%F in (glass_window.cpp desktop_capture.cpp) do (
  echo --- %%F ---
  cl /nologo /c /std:c++20 /EHsc /W3 /permissive- /utf-8 ^
     /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 ^
     /Fo"%OUT%\%%~nF.obj" /Fd"%OUT%\%%~nF.pdb" ^
     /I"%REPO%\src\ui\native-host\src" ^
     "%REPO%\src\ui\native-host\src\%%F"
  if errorlevel 1 set "RC=1"
)

echo.
if "%RC%"=="0" (echo COMPILE-CHECK OK) else (echo COMPILE-CHECK FAILED)
exit /b %RC%