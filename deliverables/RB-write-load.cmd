@echo off
rem ===========================================================================
rem  RB-write-load.cmd - double-click entry point for the 0x7F write-path load
rem
rem  WHAT THIS DOES:
rem    One human double-click -> one UAC prompt -> two rounds of >=900 MB
rem    write / copy / delete load on D:\rb-write-test -> machine-readable
rem    verdict printed here and left in a durable log.
rem
rem  WHY THIS WRAPPER NO LONGER PRE-ELEVATES (rewritten 2026-11-20):
rem    The previous version elevated by itself with
rem      powershell -Command "Start-Process -Verb RunAs -ArgumentList @(...)"
rem    Two defects, both fatal on the very first click, neither ever exercised
rem    because no run through this path has ever completed:
rem      1) the script path was interpolated into a -Command STRING. The repo
rem         path contains non-ASCII (yuan ma bao), and a Chinese path embedded
rem         in -Command gets re-parsed under the console codepage -> mojibake
rem         -> the elevated child cannot find the .ps1. This is a known trap
rem         in this repo, hit before.
rem      2) it hardcoded powershell.exe (5.1) as the elevated child, while the
rem         child is exactly the process that runs [IO.File]::Open +
rem         Flush($true) + fsutil output parsing + [math]::Round. The .ps1
rem         itself pins pwsh 7 for this reason (see its ELEVATION comment);
rem         pre-elevating here silently bypassed that pin.
rem    So we hand off unelevated and let RB-write-load.ps1 self-elevate ONCE
rem    with a PowerShell-native argument list ($PSCommandPath, no string
rem    re-parse, pwsh 7 pinned). It also propagates the child's real exit code
rem    by tailing the durable log for the TERMINAL line, instead of a
rem    fabricated 0. This wrapper only supplies the interpreter and reports.
rem
rem  RED LINES honoured by RB-write-load.ps1 itself (hard-coded, not options):
rem    * never touches D:\<da-mo-xing>   (user model files, unrecoverable)
rem    * never writes to a volume root; only D:\rb-write-test\
rem    * never touches the historical rbc-*.bin leftovers at D:\ root
rem    * read-only on Features\Flags / WriteBack / Verifier / UpperFilters
rem    * no deploy, no reboot, no git commit
rem
rem  ESCAPE HATCH: pass "dry" as the first argument to preview only
rem    RB-write-load.cmd dry
rem
rem  EXIT CODES (mirrors RB-write-load.ps1 TERMINAL rc):
rem    0 = PASS   5 = INCONCLUSIVE   3 = FAIL (hard)   4 = RUNABORTED
rem    1 = unknown: UAC refused/cancelled, or a throw before the log existed
rem
rem  Pure ASCII on purpose (repository gate: cmd_chr=0, no non-ASCII in .cmd).
rem ===========================================================================
setlocal EnableDelayedExpansion
pushd "%~dp0"
set "RC=1"
set "ISDRY=0"
if /i "%~1"=="dry" set "ISDRY=1"

rem Pin pwsh 7 for BOTH roles: the launcher and the elevated child it spawns.
set "PWEXE=powershell.exe"
if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" set "PWEXE=%ProgramFiles%\PowerShell\7\pwsh.exe"

if not exist "RB-write-load.ps1" (
    echo [FAIL] missing script: RB-write-load.ps1 must sit next to this .cmd
    goto :report
)

if /i "%~1"=="dry" (
    echo [INFO] dry run only - nothing will be written
    "%PWEXE%" -NoProfile -ExecutionPolicy Bypass -File "RB-write-load.ps1" -DryRun
    set "RC=!ERRORLEVEL!"
    goto :report
)

echo [INFO] interpreter : %PWEXE%
echo [INFO] Script will request elevation ONCE. Choose Yes on the UAC prompt.
echo [INFO] The elevated window shows live progress; this window waits for the
echo [INFO] verdict, then holds so you can read it.
echo.

"%PWEXE%" -NoProfile -ExecutionPolicy Bypass -File "RB-write-load.ps1" -Rounds 2 -SizeMB 900 -TargetDir "D:\rb-write-test"
set "RC=!ERRORLEVEL!"

:report
echo.
echo ===========================================================================
echo EXIT CODE : !RC!
if "!ISDRY!"=="1" (
    echo VERDICT   : DRY RUN - plan printed only, nothing was written, nothing was verified
) else (
if "!RC!"=="0" echo VERDICT   : PASS - both rounds completed, no crash, write bytes crossed the filter
if "!RC!"=="5" echo VERDICT   : INCONCLUSIVE - load ran, one counter could not be proven
if "!RC!"=="3" echo VERDICT   : FAIL - crash, a hard check failed, or no write crossed the filter
if "!RC!"=="4" echo VERDICT   : RUNABORTED - the script died for a reason it knows, see log
if "!RC!"=="1" echo VERDICT   : UNKNOWN - UAC refused/cancelled, or a throw before any log line
)
echo.
echo Durable log is %TEMP%\RB-write-load_yyyyMMdd_HHmmss.log (whole run appended).
echo The path is passed to the reporting commands below through the environment,
echo never through a command line, so the non-ASCII repo path cannot corrupt it.
echo.
set "NEWEST="
for /f "delims=" %%F in ('dir /b /o-d "%TEMP%\RB-write-load_*.log" 2^>nul') do (
    if not defined NEWEST set "NEWEST=%TEMP%\%%F"
)
if defined NEWEST (
    echo LOG FILE : !NEWEST!
    echo ---- last 40 lines ----
    "%PWEXE%" -NoProfile -Command "Get-Content -LiteralPath $env:NEWEST -Tail 40"
    echo ---- machine-readable lines ----
    "%PWEXE%" -NoProfile -Command "Select-String -LiteralPath $env:NEWEST -Pattern '^(TERMINAL|RUNABORTED) ' | ForEach-Object { $_.Line }"
) else (
    echo [WARN] no durable log found - the elevated run never started.
    echo        Most likely the UAC prompt was cancelled. Nothing was written:
    echo        the .ps1 creates no file before elevation is granted.
)
echo ===========================================================================
echo.
echo Send the log above (or the whole file) back for the report.
popd
echo.
if /i "%~1"=="dry" goto :no_pause
if defined RB_NO_PAUSE goto :no_pause
pause
:no_pause
rem ---------------------------------------------------------------- exit code
rem MUST be the real RC, not a blanket 0.  A trailing `exit /b 0` here would
rem make "UAC cancelled (1)" and "all checks passed (0)" indistinguishable to any
rem caller -- the exact "$LASTEXITCODE inherited -> fake success" class this
rem project keeps re-hitting, reintroduced one layer above the .ps1 that was
rem fixed for it.  Verified: with a stub that returns 4, the old `exit /b 0`
rem printed "EXIT CODE : 4" and then handed 0 to the caller.
rem `setlocal` does not need a manual endlocal when `exit /b` carries an rc.
exit /b %RC%