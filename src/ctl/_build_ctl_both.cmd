@echo off
rem Build both CLIs with the machine's non-standard MSVC install path.
set "VCT=C:\Program1\VC\Tools\MSVC\14.44.35207"
set "KIT=C:\Program Files (x86)\Windows Kits\10"
set "INCLUDE=%VCT%\include;%KIT%\Include\10.0.26100.0\ucrt;%KIT%\Include\10.0.26100.0\um;%KIT%\Include\10.0.26100.0\shared"
set "LIB=%VCT%\lib\x64;%KIT%\Lib\10.0.26100.0\um\x64;%KIT%\Lib\10.0.26100.0\ucrt\x64"
set "PATH=%VCT%\bin\Hostx64\x64;%PATH%"
cd /d "%~dp0"
rem [S2 2026-11-07] bench_policy.c is REQUIRED: ramboostctl.c calls three
rem RbBench* helpers (CliFlushFileRange / cli_bench). Without it cl emits
rem LNK2019 x3 + LNK1120 and no exe is produced. See _build_ctl_both.ps1
rem for the authoritative PowerShell entry (cmd<->PS interop is blocked here).
cl /nologo /utf-8 /O2 /W3 ramboostctl.c bench_policy.c /Fe:ramboostctl.exe advapi32.lib bcrypt.lib
cl /nologo /utf-8 /O2 /W3 rbfeatctl.c /Fe:rbfeatctl.exe advapi32.lib
exit /b %ERRORLEVEL%
