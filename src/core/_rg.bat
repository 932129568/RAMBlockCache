@echo off
call C:/Program1/VC/Auxiliary/Build/vcvars64.bat >nul 2>&1
cd /d %~dp0
cl /nologo /O2 /W4 /utf-8 /I. test_regress.c cache.c /Fe:build/test_regress.exe /Fobuild/
cd build
test_regress.exe
