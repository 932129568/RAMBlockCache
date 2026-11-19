@echo off
REM bench-d.cmd (2026-11-01 rev2) - D: cache effect, one click.
REM Console now shows progress (rev1 sent everything to the log file -> black window).
REM Phases: RAW -> P1 cold -> P2 warm/direct-write -> P3 warm/write-back.
chcp 437 >nul
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
    echo [i] Administrator required - requesting UAC elevation...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" >nul 2>&1
    exit /b
)

set OUT=%~dp0build\bench-d.txt
set BENCH=%~dp0src\tools\rbbench\bin\Release\net10.0-windows\win-x64\rbbench.exe
set CTL=%~dp0src\ctl\ramboostctl.exe
set ARGS=--file D:\rbbench.bin --size 1024 --seq4k 256 --ops 16384 --repeat 2

if not exist "%BENCH%" ( echo [x] missing %BENCH% & pause & exit /b 1 )
if not exist "%CTL%"   ( echo [x] missing %CTL%   & pause & exit /b 1 )

echo === rbbench D: %DATE% %TIME% === > "%OUT%"

echo.
echo [1/9] detaching D: (baseline must be uncached) - may take up to 60s ...
echo === [1] detach D: === >> "%OUT%"
"%CTL%" detach D >> "%OUT%" 2>&1
echo       done.

echo [2/9] RAW baseline (1GiB, ~30s) ...
echo === [2] RAW baseline === >> "%OUT%"
"%BENCH%" %ARGS% --tag RAW >> "%OUT%" 2>&1
echo       done.

echo [3/9] attaching D: write-through ...
echo === [3] attach D wm=0 === >> "%OUT%"
"%CTL%" attach D read=1 write=1 wm=0 >> "%OUT%" 2>&1
echo       done.

echo [4/9] P1 cold pass (fills cache, ~30s) ...
echo === [4] P1 cold === >> "%OUT%"
"%BENCH%" %ARGS% --tag P1 >> "%OUT%" 2>&1
echo       done.

echo [5/9] P2 warm pass, write-through (~30s) ...
echo === [5] P2 warm direct-write === >> "%OUT%"
"%BENCH%" %ARGS% --tag P2 >> "%OUT%" 2>&1
echo       done.

echo [6/9] switching to write-back (wm=1) ...
echo === [6] attach D wm=1 === >> "%OUT%"
"%CTL%" detach D >> "%OUT%" 2>&1
"%CTL%" attach D read=1 write=1 wm=1 >> "%OUT%" 2>&1
echo       done.

echo [7/9] P3 warm pass, write-back (~30s) ...
echo === [7] P3 write-back === >> "%OUT%"
"%BENCH%" %ARGS% --tag P3 >> "%OUT%" 2>&1
echo       done.

echo [8/9] restoring safe state (flush dirty + write-through) ...
echo === [8] restore wm=0 === >> "%OUT%"
"%CTL%" detach D >> "%OUT%" 2>&1
"%CTL%" attach D read=1 write=1 wm=0 >> "%OUT%" 2>&1
echo       done.

echo [9/9] stats ...
echo === [9] stats === >> "%OUT%"
"%CTL%" status >> "%OUT%" 2>&1

echo.
echo ==== finished - full log: build\bench-d.txt ====
pause
