@echo off
rem ===========================================================================
rem  RB-bench-miss-coalescing.cmd - double-click entry for the D-01 P0 probe
rem
rem  WHAT THIS DOES:
rem    One double-click -> one UAC prompt -> one `ramboostctl bench -passes 2`
rem    over D:\<da-mo-xing>\28391d06... -> the mean pass-2 interval size is solved
rem    algebraically -> a machine-readable verdict is printed here and left in a
rem    durable log.
rem
rem  WHAT IT ANSWERS:
rem    Does the second bench pass really issue 4 KiB disk reads (D-01's premise,
rem    candidate D worth building), or does it also coalesce into 128 KiB
rem    (misses clustered, D-01 refuted)? It prints r2, the solved mean interval
rem    size, next to pass 1's 131072 so the 32x gap (or its absence) is visible.
rem
rem  WHY THIS WRAPPER DOES NOT PRE-ELEVATE:
rem    RB-bench-miss-coalescing.ps1 elevates itself ONCE with a PowerShell-native
rem    argument list ($PSCommandPath, no -Command string re-parse under the
rem    console codepage, pwsh 7 pinned) and then propagates the child's real exit
rem    code by tailing the durable log for its TERMINAL line. A wrapper that
rem    elevated here would bypass that pin and would have to fabricate an exit
rem    code; it only chooses the interpreter and reports.
rem
rem  RED LINES honoured by the .ps1 itself (hard-coded, NOT options):
rem    * may READ exactly one file under D:\<da-mo-xing>, named in full in the
rem      script. Any other path there is refused, including a sibling model.
rem    * never writes anywhere: no create, no attribute change, no delete
rem    * never touches D:\rb-write-test (that belongs to RB-write-load.cmd)
rem    * read-only on Features\Flags / WriteBack / Verifier / UpperFilters
rem    * -Passes must be exactly 2; anything else is refused BEFORE the UAC
rem      prompt, because with 3+ passes the hit-rate window spans passes 2..N
rem      and the solved r2 would be a mixture, not a measurement
rem    * no deploy, no reboot, no git commit
rem
rem  ESCAPE HATCH: pass "dry" as the first argument to preview only
rem    RB-bench-miss-coalescing.cmd dry
rem
rem  EXIT CODES (mirrors the .ps1 TERMINAL rc; each means exactly one thing):
rem    0 = D01-CONFIRMED (the premise holds; includes MORE-FRAGMENTED)
rem    3 = D01-REFUTED or INVALID (model / gate broken)
rem    4 = RUNABORTED (red line, or the script aborted)
rem    5 = MISSING (a required counter or field could not be read)
rem    6 = D01-INCONCLUSIVE (usable data, in neither band)
rem    1 = unknown: UAC refused/cancelled, or a throw before any log line
rem
rem  Pure ASCII on purpose (repository gate: cmd_chr=0, no non-ASCII in .cmd).
rem  The non-ASCII model directory name is NEVER typed here - the .ps1 owns it.
rem ===========================================================================
setlocal EnableDelayedExpansion
pushd "%~dp0"
set "RC=1"
set "ISDRY=0"
if /i "%~1"=="dry" set "ISDRY=1"

rem Pin pwsh 7 for BOTH roles: this launcher and the elevated child it spawns.
rem The child does [math]::Round / [IO.Path]::GetFullPath / regex on tool output;
rem 5.1 differences here are exactly the class that produces silent wrong numbers.
set "PWEXE=powershell.exe"
if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" set "PWEXE=%ProgramFiles%\PowerShell\7\pwsh.exe"

if not exist "RB-bench-miss-coalescing.ps1" (
    echo [FAIL] missing script: RB-bench-miss-coalescing.ps1 must sit next to this .cmd
    goto :report
)

if "!ISDRY!"=="1" (
    echo [INFO] dry run only - nothing will be read, nothing written
    "%PWEXE%" -NoProfile -ExecutionPolicy Bypass -File "RB-bench-miss-coalescing.ps1" -DryRun
    set "RC=!ERRORLEVEL!"
    goto :report
)

echo [INFO] interpreter : %PWEXE%
echo [INFO] This reads ONE 20.2 GB model file twice (cold, then hot) and solves
echo [INFO] the mean pass-2 read size. Read-only: no write, no delete, no move.
echo [INFO] The script requests elevation ONCE. Choose Yes on the UAC prompt.
echo [INFO] A cold pass over 20.2 GB takes a few minutes; this window waits.
echo.

"%PWEXE%" -NoProfile -ExecutionPolicy Bypass -File "RB-bench-miss-coalescing.ps1" -Passes 2
set "RC=!ERRORLEVEL!"

:report
echo.
echo ===========================================================================
echo EXIT CODE : !RC!
if "!ISDRY!"=="1" (
    echo VERDICT   : DRY RUN - plan printed only, nothing was read, nothing was judged
) else (
if "!RC!"=="0" echo VERDICT   : D01-CONFIRMED - pass 2 reads are fragmented, D-01's premise holds
if "!RC!"=="3" echo VERDICT   : D01-REFUTED or INVALID - misses clustered, or the model/gate broke
if "!RC!"=="4" echo VERDICT   : RUNABORTED - red line or abort, see log
if "!RC!"=="5" echo VERDICT   : MISSING - a counter or field could not be read; run is unusable
if "!RC!"=="6" echo VERDICT   : D01-INCONCLUSIVE - usable data, in neither band
if "!RC!"=="1" echo VERDICT   : UNKNOWN - UAC refused/cancelled, or a throw before any log line
rem Default branch: every documented rc has a line above, so reaching this one
rem means the rc is outside the contract. Printing nothing would read as "no
rem finding" when it actually means "this wrapper does not know what happened".
rem Verified with stubs returning 2, 7 and 9: before these lines they produced an
rem empty VERDICT row. NOTE: GTR must be written WITHOUT quotes here. `if "7"
rem GTR "6"` is a STRING comparison and is false, so the quoted form silently
rem skipped rc=7 and rc=9 -- caught by exactly this test.
if "!RC!"=="2" echo VERDICT   : UNDOCUMENTED rc=2 - inspect the log; expected 0/1/3/4/5/6
if !RC! GTR 6 echo VERDICT   : UNDOCUMENTED rc=!RC! - inspect the log; expected 0/1/3/4/5/6
)
echo.
echo Durable log is %TEMP%\RB-bench-miss_yyyyMMdd_HHmmss.log (whole run appended).
echo The path is passed to the reporting commands below through the environment,
echo never through a command line, so the non-ASCII repo path cannot corrupt it.
echo.
set "NEWEST="
for /f "delims=" %%F in ('dir /b /o-d "%TEMP%\RB-bench-miss_*.log" 2^>nul') do (
    if not defined NEWEST set "NEWEST=%TEMP%\%%F"
)
if defined NEWEST (
    echo LOG FILE : !NEWEST!
    echo ---- judgement lines ----
    "%PWEXE%" -NoProfile -Command "Get-Content -LiteralPath $env:NEWEST -Tail 30"
    echo ---- machine-readable line ----
    "%PWEXE%" -NoProfile -Command "Select-String -LiteralPath $env:NEWEST -Pattern '^(TERMINAL|RUNABORTED) ' | ForEach-Object { $_.Line }"
) else (
    echo [WARN] no durable log found - the run never started.
    echo        Most likely the UAC prompt was cancelled. Nothing was read:
    echo        the .ps1 touches no file before elevation is granted.
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
rem MUST be the real RC. A trailing `exit /b 0` would make "UAC cancelled (1)"
rem and "all checks passed (0)" indistinguishable to any caller -- the exact
rem "$LASTEXITCODE inherited -> fake success" class this project keeps
rem re-hitting, reintroduced one layer above the .ps1 that was fixed for it.
rem Independently verified with a stub returning 4: the report prints
rem "EXIT CODE : 4" and the caller really receives 4.
exit /b %RC%