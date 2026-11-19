@echo off
rem ===========================================================================
rem  RB-t4-signature-verify.cmd
rem  T4 elevated real-machine signature retest. READ ONLY. One UAC.
rem
rem  Background: report 229 found two Authenticode checks UNREADABLE inside an
rem  ELEVATED nested process, while the same T1 fix returned Valid outside it.
rem  Root cause was never proven. This wrapper collects the missing evidence.
rem
rem  What it does (elevated):
rem    1. WinVerifyTrust on dist/RAMBlockCache.sys and on the System32 copy,
rem       directly from the Chinese repository path.
rem    2. Copies both to an ASCII stage dir using FileShare.ReadWrite streaming
rem       (src/scripts/copy_rb_file_readwrite_share.ps1) and verifies again.
rem    3. Writes a UTC-stamped log to C:\ProgramData\rb-deploy\.
rem    4. Runs ramboostctl sigverify on both files (native WinVerifyTrust,
rem       no PowerShell) and appends independent evidence to
rem       C:\ProgramData\rb-deploy\t4-native-verify.log.
rem
rem  READ ONLY: no driver source/build/sign/deploy, no registry write, no
rem  config change, no write-back / WBCOAL (R4/R9), no reboot, no rollback.
rem  The staged copies are new files under C:\ProgramData\rb-deploy\t4stage\.
rem
rem  Reading the result:
rem    DIRECT Valid + STAGE Valid  -> T1 fix also holds when elevated; T4 closed.
rem    DIRECT UNREADABLE + STAGE Valid -> path/share handling is the root cause.
rem    Anything else -> T4 remains NOT VERIFIED. Never claim closure without
rem    the log (red line R7: no trigger evidence is no verification).
rem    NATIVE Valid on both is supporting evidence only; the verdict
rem    still needs the main log. Never claim closure from NATIVE alone.
rem ===========================================================================
setlocal

set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
if not exist "%PWSH%" set "PWSH=powershell.exe"
set "DEPLOY=C:\ProgramData\rb-deploy"
set "HELPER=%~dp0..\src\scripts\copy_rb_file_readwrite_share.ps1"
set "REPO=%~dp0.."

net session >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo *** NOT ELEVATED *** requesting UAC ...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b 0
)

echo ==================================================================
echo   RAMBlockCache T4 SIGNATURE RE-TEST - elevated, READ ONLY
echo ==================================================================
echo.
if not exist "%HELPER%" goto :no_helper
if not exist "%DEPLOY%" mkdir "%DEPLOY%"

"%PWSH%" -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Continue'; . $env:HELPER; $stage=Join-Path $env:DEPLOY 't4stage'; New-Item -ItemType Directory -Force -Path $stage | Out-Null; $log=Join-Path $env:DEPLOY ('t4-verify-'+(Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')+'.log'); function Show-Sig($label,$path){ if(Test-Path -LiteralPath $path){ $s=Get-AuthenticodeSignature -LiteralPath $path; return ($label+' path='+$path+' status='+$s.Status+' msg='+$s.StatusMessage) } else { return ($label+' path='+$path+' MISSING') } }; $out=@(); $out+='T4 elevated re-test UTC='+((Get-Date).ToUniversalTime().ToString('o')); $out+='whoami='+([Security.Principal.WindowsIdentity]::GetCurrent().Name); $dist=Join-Path $env:REPO 'dist\RAMBlockCache.sys'; $sys32=Join-Path $env:windir 'System32\drivers\RAMBlockCache.sys'; $out+=Show-Sig 'DIST_DIRECT' $dist; $out+=Show-Sig 'SYSTEM32_DIRECT' $sys32; $pairs=@(@($dist,(Join-Path $stage 'from-dist.sys')),@($sys32,(Join-Path $stage 'from-system32.sys'))); foreach($p in $pairs){ if(Test-Path -LiteralPath $p[0]){ try { Copy-RbFileWithReadWriteShare -SourcePath $p[0] -DestinationPath $p[1]; $out+=Show-Sig ('STAGE_'+(Split-Path $p[1] -Leaf)) $p[1] } catch { $out+='STAGE_COPY_FAILED '+$p[0]+' : '+$_.Exception.Message } } }; $out | Tee-Object -FilePath $log; Write-Host ('log: '+$log); Write-Host 'Send this log back for the T4 verdict. No closure is claimed here.'"
set "VRC=%ERRORLEVEL%"
echo.
echo ==================================================================
echo   T4 PROBE EXIT = %VRC%
echo ==================================================================
echo log: C:\ProgramData\rb-deploy\t4-verify-YYYYMMDDTHHMMSSZ.log
echo   (exact path printed above by the elevated PowerShell step)
echo.
call :native
pause
exit /b %VRC%

:no_helper
echo *** MISSING HELPER *** %HELPER%
echo Nothing was changed. Re-run after restoring the helper script.
pause
exit /b 2

:native
rem Native read-only WinVerifyTrust evidence. No PowerShell, no registry write.
if not exist "%REPO%\src\ctl\ramboostctl.exe" (
  echo NATIVE_MISSING: ctl exe absent
  exit /b 0
)
echo === native sigverify === >> "%DEPLOY%\t4-native-verify.log" 2>&1
echo NATIVE_DIST >> "%DEPLOY%\t4-native-verify.log" 2>&1
"%REPO%\src\ctl\ramboostctl.exe" sigverify "%REPO%\dist\RAMBlockCache.sys" >> "%DEPLOY%\t4-native-verify.log" 2>&1
echo NATIVE_SYSTEM32 >> "%DEPLOY%\t4-native-verify.log" 2>&1
"%REPO%\src\ctl\ramboostctl.exe" sigverify "%WINDIR%\System32\drivers\RAMBlockCache.sys" >> "%DEPLOY%\t4-native-verify.log" 2>&1
echo native log: %DEPLOY%\t4-native-verify.log
exit /b 0
