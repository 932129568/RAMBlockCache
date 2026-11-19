########################################################################
#  !!!  HISTORICAL ARCHIVE - DO NOT USE FOR INSTALL/DEPLOYMENT  !!!
#
#  This whole directory (src/dist/RAMBlockCache-Build16) is a Build 16
#  snapshot kept only as a ROLLBACK REFERENCE. It is read-only and must
#  NOT be deleted (refactor spec E.6 / P1-5).
#
#  Its deploy.ps1 is the old V10.2 flow: it has NO verify-before-change
#  gate (no expected SHA256 pin, no Authenticode check before any system
#  mutation) and its post-copy verification is advisory only (a failure
#  prints a warning and the script still reports "Deploy complete").
#
#  Current supported flows (see the main src/ tree):
#    install   : setup\ramboostsetup.ps1 (or RamBoostSetup.exe)
#    deploy    : scripts\deploy.cmd -> deploy.ps1 (V10.4.3 fail-closed)
#    verify    : scripts\verify_deploy_signature.ps1
#    uninstall : scripts\uninstall.cmd -> uninstall.ps1
########################################################################

RAMBlockCache Build 16 - Install Package
=========================================

CONTENTS
  RamBoostSetup.exe       Setup wizard (run this first)
  install.bat             Silent install (admin PowerShell)
  uninstall.bat           Remove (admin)
  driver\RAMBlockCache.sys  Signed driver (SHA256: 19231AED...E4CADAB)
  ctl\ramboostctl.exe    CLI control
  ctl\rbfeatctl.exe      Feature flags/stats/watch
  scripts\                Deploy + rollback scripts
  report\                 Final audit txt

QUICK START (admin)
  1. Right-click install.bat -> Run as administrator
  2. Reboot (must use full restart, NOT shutdown+start - Win11 fast startup skips DriverEntry)
  3. After boot, run: ctl\rbfeatctl.exe stats
  4. Attach a volume: ctl\ramboostctl.exe attach F:

ROLLBACK (if boot fails)
  - Boot Guard automatically restores UpperFilters to volsnap-only on failed boot
  - Worst case: run scripts\offline_rollback.cmd from WinRE

SIGNING
  Self-signed cert CN=RAMBoostFS Root (SHA1: DF1A0ADA2F2CD6E5778EA937C079CB0A035BA431)
  Target machine needs test signing on or cert imported to trusted root.

GUI CONTROL PANEL
  ui\RAMBoost控制面板.exe  WPF GUI (68MB self-contained, run as admin)
