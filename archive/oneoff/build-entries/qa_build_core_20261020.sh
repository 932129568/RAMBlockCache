#!/usr/bin/env bash
# QA independent build+run of RAMBlockCache core tests (2026-10-20).
# Pinned toolchain: MSVC 14.44.35207 @ C:\Program1 + WinSDK 10.0.26100.0
# Include/LIB passed as cl args (env vars get mangled by MSYS conversion).
set -u
cd "$(dirname "$0")/../.."
ROOT="$(pwd)"
OUT="$ROOT/src/core/build/qa-20261020"
mkdir -p "$OUT"

MSVC="/c/Program1/VC/Tools/MSVC/14.44.35207"
KIT="/c/Program Files (x86)/Windows Kits/10"
export MSYS2_ARG_CONV_EXCL="*"

CL="$MSVC/bin/Hostx64/x64/cl.exe"
INC="/I$MSVC/include /I$KIT/Include/10.0.26100.0/ucrt /I$KIT/Include/10.0.26100.0/um /I$KIT/Include/10.0.26100.0/shared /I$ROOT/src/core /I$ROOT/src/common /I$ROOT/src/core/usertest"
LIBDIR="/LIBPATH:$MSVC/lib/x64 /LIBPATH:$KIT/Lib/10.0.26100.0/ucrt/x64 /LIBPATH:$KIT/Lib/10.0.26100.0/um/x64"
export PATH="$MSVC/bin/Hostx64/x64:$PATH"

CFLAGS="/nologo /O2 /W4 /utf-8 /MT"

echo "=== compiling cache.c (plain) ==="
"$CL" $CFLAGS $INC /c src/core/cache.c /Fo:"$OUT/cache.obj" 2>&1 | head -30
echo "cache_plain_rc=${PIPESTATUS[0]}"
echo "=== compiling cache.c (/DRB_EVICT_HOOK_EXTERNAL) ==="
"$CL" $CFLAGS $INC /DRB_EVICT_HOOK_EXTERNAL /c src/core/cache.c /Fo:"$OUT/cache_ew.obj" 2>&1 | head -30
echo "cache_ew_rc=${PIPESTATUS[0]}"

TESTS="test_auditfix test_bigread test_cache test_counter test_counter_seed test_evict_wire test_flush_many test_geom test_mixed test_parflush test_regress test_release test_v5 test_v6 edge_v3 edge_v4 stress_v3 stress_v4 offchk"

for t in $TESTS; do
  if [ "$t" = "test_evict_wire" ]; then CO="$OUT/cache_ew.obj"; else CO="$OUT/cache.obj"; fi
  "$CL" $CFLAGS $INC /c "src/core/$t.c" /Fo:"$OUT/$t.obj" 2>"$OUT/$t.cl.log"
  crc=$?
  if [ $crc -ne 0 ]; then echo "$t: COMPILE FAIL ($crc)"; tail -8 "$OUT/$t.cl.log"; continue; fi
  "$CL" $CFLAGS $INC $LIBDIR "$OUT/$t.obj" "$CO" /Fe:"$OUT/$t.exe" /Fm:"$OUT/$t.map" >>"$OUT/$t.cl.log" 2>&1
  lrc=$?
  if [ $lrc -ne 0 ]; then echo "$t: LINK FAIL ($lrc)"; tail -8 "$OUT/$t.cl.log"; continue; fi
  w=$(grep -c "warning" "$OUT/$t.cl.log" || true)
  echo "$t: BUILD OK (test-TU warnings=$w)"
done
echo "=== binaries built: $(ls "$OUT"/*.exe 2>/dev/null | wc -l) ==="
