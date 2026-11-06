@echo off
set "VCT=C:\Program1\VC\Tools\MSVC\14.44.35207"
set "KIT=C:\Program Files (x86)\Windows Kits\10"
set "INCLUDE=%VCT%\include;%KIT%\Include\10.0.26100.0\ucrt;%KIT%\Include\10.0.26100.0\um;%KIT%\Include\10.0.26100.0\shared"
set "LIB=%VCT%\lib\x64;%KIT%\Lib\10.0.26100.0\um\x64;%KIT%\Lib\10.0.26100.0\ucrt\x64"
set "PATH=%VCT%\bin\Hostx64\x64;%PATH%"
cd /d "%~dp0"
cl /nologo /utf-8 /O2 /W3 ramboostctl.c bench_policy.c /Fe:ramboostctl.exe advapi32.lib bcrypt.lib
cl /nologo /utf-8 /O2 /W3 rbfeatctl.c /Fe:rbfeatctl.exe advapi32.lib
exit /b %ERRORLEVEL%
