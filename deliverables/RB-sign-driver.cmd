@echo off
rem ===========================================================================
rem  RB-sign-driver.cmd   [one double-click]
rem
rem  WHY (report 252, 2026-11-15): the A-batch patch (report 251) compiles and
rem  passes core/gates, but the binary is only signable, not signed. Signing
rem  needs the LocalMachine\My private key, i.e. elevation, and signtool must
rem  run from an ASCII stage path on this host. This wrapper does the whole
rem  chain in one elevated window:
rem
rem    sign (machine store) -> verify /pa -> install into dist\ ->
rem    write dist\RAMBlockCache.sys.sha256 -> sync_anchors.ps1 ->
rem    read back all three anchors and the dist file hash.
rem
rem  Safety:
rem    - NO deploy, NO service control, NO registry write, NO reboot.
rem    - The running driver and System32 copy are never touched.
rem    - If any step fails after dist\ was replaced, the previous binary and
rem      sidecar are restored and the anchors are re-synced back.
rem
rem  Exit: 0 = signed + anchors verified / 1 = aborted (dist restored) /
rem        2 = aborted before any change.
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo ==================================================================
echo   DRIVER SIGN + ANCHOR SYNC - elevated
echo   sign only; no deploy; running driver untouched
echo ==================================================================
echo.

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\src\scripts\sign_driver.ps1"
set "RC=%ERRORLEVEL%"

echo.
echo ==================================================================
echo   sign-driver exit code: %RC%
echo ==================================================================
if "%RC%"=="0" goto :ok
if "%RC%"=="2" goto :nochange

echo *** signing aborted (exit=%RC%). dist\ was rolled back if it had been touched. ***
echo *** Send this screen back before trying anything else. ***
echo.
pause
exit /b 1

:nochange
echo *** aborted before any change (environment problem). dist\ and anchors untouched. ***
echo.
pause
exit /b 2

:ok
echo   Signed binary is in dist\ and all three anchors match it.
echo   Next step is a deploy approval, run separately:
echo     src\scripts\verify_deploy_signature.ps1
echo     src\scripts\deploy.ps1   (one UAC, one reboot)
echo.
pause
exit /b 0
