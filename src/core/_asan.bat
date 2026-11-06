@echo off
call C:/Program1/VC/Auxiliary/Build/vcvars64.bat >nul 2>&1
cd /d %~dp0
cl /nologo /O2 /W4 /utf-8 /fsanitize=address /I. test_regress.c cache.c /Fe:build/asan_rg.exe /Fobuild/
cd build
asan_rg.exe
