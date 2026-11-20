@echo off
setlocal
chcp 437 >nul
rem ===================================================================
rem  probe_native_host.cmd   [299 2026-11-19]
rem
rem  Launch the native D3D11 + DirectComposition host and print what
rem  the process actually reports, so a human can judge by eye whether
rem  the glass is real refraction or just an opaque scrim.
rem
rem  Why this exists: every claim in 295 was made from source reading.
rem  Nothing in the repo had ever started a process that captures the
rem  desktop and bends it through a shader. "Compiles" is not "works";
rem  a black window and a perfect refraction look identical to a test.
rem
rem  What to look for, in order of what can go wrong:
rem    1. Window appears and is TRANSPARENT (you can see desktop behind
rem       the app). If it is opaque black, LoadShaders failed and the
rem       renderer silently downgraded -- check cso\ exists.
rem    2. Move the pointer across a glass panel: the ripple must bend
rem       the DESKTOP image, not paint a white blob on top.
rem       A white blob means the CPU path is still drawing (class
rem       rbc-host-dcomp never got set, i.e. handshake failed).
rem    3. Drag the window and look at the panel edge: the bezel region
rem       must show a compressed/inverted image (lens refraction).
rem    4. Check the tile boundaries for vertical stripes: r<4 used to
rem       read uninitialised LDS. Fixed in 299; if stripes appear the
rem       shader fix did not take.
rem
rem  Pure ASCII, no BOM, CRLF. Must stay under tools\ : a RB-*.cmd in
rem  the repo root trips the KNOWN_WRAPPERS check.
rem ===================================================================

set "HERE=%~dp0"
set "EXE=%HERE%..\src\ui\native-host\x64\Release\RAMBlockCache.Host.exe"
set "CSO=%HERE%..\src\ui\native-host\x64\Release\cso"

echo.
echo == native-host probe ==
echo.

if not exist "%EXE%" (
    echo [X] EXE not built:
    echo     %EXE%
    echo     Run:  build\build.ps1 -Target native-host
    goto :fail
)

if not exist "%CSO%" (
    echo [X] cso\ missing:
    echo     %CSO%
    echo     The host loads .cso only; without them it downgrades to a
    echo     flat colour and looks like "nothing happened".
    goto :fail
)

set /a CSO_N=0
for %%f in ("%CSO%\*.cso") do set /a CSO_N+=1
echo [ok] EXE : %EXE%
echo [ok] CSO : %CSO_N% shaders
if %CSO_N% LSS 14 (
    echo [X] expected 14 .cso ^(8 glass variants + kawase x2 + blur x2 + apca + wave^)
    goto :fail
)

echo.
echo Starting host. Close the window or press Ctrl+C to stop.
echo Press Ctrl+Shift+P inside the app for presentation mode ^(so screen
echo capture can see the glass; WDA_EXCLUDEFROMCAPTURE hides it otherwise^).
echo.

"%EXE%"
set RC=%ERRORLEVEL%
echo.
echo Host exited with %RC%
if %RC% NEQ 0 goto :fail
goto :done

:fail
echo.
echo PROBE FAILED - see messages above.
exit /b 1

:done
echo.
echo PROBE DONE - judge by eye against the 4 checks in the header.
exit /b 0