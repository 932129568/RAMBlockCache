@echo off
rem [FIX S-12] REM comments English-ized (batch encoding discipline; .cmd wrapper only, driver sources untouched).
rem [P2-12] Supports environment overrides: VCVARS64 / MSBUILD / NUGET_PACKAGES.
rem The old script hardcoded all three paths with no override point, so a
rem different machine or VS version broke the build.
if not defined VCVARS64 set "VCVARS64=C:\Program1\VC\Auxiliary\Build\vcvars64.bat"
if not defined MSBUILD  set "MSBUILD=C:\Program1\MSBuild\Current\Bin\MSBuild.exe"
if not defined NUGET_PACKAGES set "NUGET_PACKAGES=%~dp0..\..\.nuget\packages"

if not exist "%VCVARS64%" (
  echo [X] vcvars64.bat not found: %VCVARS64%
  echo     Set VCVARS64=C:\path\to\vcvars64.bat and re-run.
  exit /b 1
)
call "%VCVARS64%" >nul 2>&1

if not exist "%MSBUILD%" (
  echo [X] MSBuild.exe not found: %MSBUILD%
  echo     Set MSBUILD=C:\path\to\MSBuild.exe and re-run.
  exit /b 1
)
"%MSBUILD%" "%~dp0RAMBlockCache.vcxproj" /p:Configuration=Release /p:Platform=x64 /t:Rebuild /v:n > "%~dp0build_drv.log" 2>&1
echo EXIT=%errorlevel%
