@echo off
if not defined VCVARS set "VCVARS=C:\Program1\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1
set "ROOT=%~dp0"
echo [1/2] ctl
cl /nologo /utf-8 /O2 /W3 "%ROOT%ctl\ramboostctl.c" /Fe:"%ROOT%ctl\ramboostctl.exe" /Fo:"%ROOT%ctl\ramboostctl.obj" /link shell32.lib
echo CTL_EXIT=%errorlevel%
echo [2/2] setup
cl /nologo /utf-8 /O2 /W3 "%ROOT%setup\ramboostsetup.c" /Fe:"%ROOT%setup\RamBoostSetup.exe" /Fo:"%ROOT%setup\ramboostsetup.obj" /link advapi32.lib cfgmgr32.lib comctl32.lib user32.lib gdi32.lib shell32.lib
echo SETUP_CL_EXIT=%errorlevel%
mt -nologo -manifest "%ROOT%setup\RamBoostSetup.exe.manifest" -outputresource:"%ROOT%setup\RamBoostSetup.exe;1"
echo MT_EXIT=%errorlevel%
