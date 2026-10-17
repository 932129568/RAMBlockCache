@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
for %%T in (test_cache test_counter test_geom test_mixed test_parflush test_regress test_release test_v5 test_v6 test_bigread test_flush_many edge_v3 edge_v4 stress_v3 stress_v4 bench_readhit test_evict_wire test_auditfix) do (
  build\%%T.exe > build\rel_%%T.out 2> build\rel_%%T.err
  echo %%T exit=!ERRORLEVEL!
)
