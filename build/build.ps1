# build.ps1 -- THE single build entry point for the repository.
#
# Product chains (everything else is a shim that calls this script):
#   driver     RAMBlockCache.sys        (cl+link, reads sources.manifest.json)
#   core       tests/core regression    (Release + ASan, build + run)
#   core-asan  ASan only
#   ctl        ramboostctl.exe / rbfeatctl.exe (+ contract string check)
#   setup      ramboostsetup.exe to temp (never overwrites the signed one)
#   ui         dotnet solution build/RAMBlockCache.sln
#   web        npm run build in src/ui/web
#   native-host RAMBlockCache.Host.exe (D3D11+DComp glass host, shaders -> .cso)
#   gates      Gate1..Gate22 + models (local, strict signature)
#   all        driver + core + ctl + setup + ui + web
#   clean      remove intermediate build outputs (never dist/, never signed exes)
#
# Contract:
#   - No source list may be inlined here: it comes from build/sources.manifest.json.
#   - The driver output is UNSIGNED by design. Never deploy without signtool.
#   - Exit code is fail-closed: any non-zero step aborts the target.
#
# ASCII only (PS 5.1 GBK console lesson). Requires PowerShell 5.1+.
[CmdletBinding()]
param(
    [ValidateSet('all', 'driver', 'core', 'core-asan', 'ctl', 'setup', 'ui', 'web', 'native-host', 'gates', 'clean')]
    [string]$Target = 'all',
    [switch]$NoRun,
    [string]$OutFile = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')

$script:Failures = 0

function Fail([string]$Message) {
    Write-Host "[X] $Message" -ForegroundColor Red
    exit 1
}

function Step([string]$Message) {
    Write-Host ""
    Write-Host "=== $Message ===" -ForegroundColor Cyan
}

function Invoke-Native([string]$Exe, [string[]]$Arguments, [string]$LogPath) {
    & $Exe @Arguments > $LogPath 2>&1
    return $LASTEXITCODE
}

# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------
function Build-Driver {
    Step 'driver: RAMBlockCache.sys'
    $t = $Manifest.targets.driver
    $workRel = ($t.workDir -replace '/', '\')
    $wd = Join-Path $RepoRoot $workRel
    $outRel = ($t.outDir.Substring($t.workDir.Length).TrimStart('/') -replace '/', '\')
    $objRel = Join-Path $outRel 'obj'

    Set-ToolchainEnv 'kernel'
    Push-Location $wd
    try {
        New-Item -ItemType Directory -Force -Path $objRel | Out-Null
        # Link pulls $objRel\*.obj implicitly through the explicit list below; a
        # source deleted since the last build must not survive as a stale object.
        # [2026-11-12] Changed from `Get-ChildItem | Remove-Item -Force` to an
        # explicit foreach: on hosts with a safe-delete hook, the wrapped
        # Remove-Item does not accept pipeline input ("command cannot take
        # pipeline input") and the build aborts before compiling anything.
        # Passing -LiteralPath explicitly is semantically identical and also
        # handles the empty-set case that the old pipeline form left implicit.
        foreach ($o in @(Get-ChildItem -LiteralPath $objRel -Filter '*.obj' -File -ErrorAction SilentlyContinue)) {
            Remove-Item -LiteralPath $o.FullName -Force -ErrorAction SilentlyContinue
        }
        # Deterministic link [154]: link.exe REUSES an existing PDB and bumps the
        # CodeView RSDS Age, so the image differs on every rebuild even with
        # /Brepro (measured 2026-11-11: Age 26->27->28 across three builds of
        # identical sources). Start from no PDB so Age is always 1 and the GUID
        # stays content-derived. Verified: two consecutive links byte-identical.
        # Same reason as above: both pdb sweeps are explicit foreach loops.
        foreach ($p in @(Get-ChildItem -LiteralPath $outRel -Filter '*.pdb' -File -ErrorAction SilentlyContinue)) {
            Remove-Item -LiteralPath $p.FullName -Force -ErrorAction SilentlyContinue
        }
        foreach ($p in @(Get-ChildItem -LiteralPath '.' -Filter '*.pdb' -File -ErrorAction SilentlyContinue)) {
            Remove-Item -LiteralPath $p.FullName -Force -ErrorAction SilentlyContinue
        }

        $inc = @()
        foreach ($d in $t.compile.includes) { $inc += "/I" + (Get-RelFrom $t.workDir $d) }
        $defs = @()
        foreach ($d in $t.compile.defines) { $defs += "/D$d" }
        $wds = @()
        foreach ($w in $t.compile.disabledWarnings) { $wds += "/wd$w" }
        $common = @($t.compile.flags) + @($t.compile.standard) + $defs + $wds + $inc

        Write-Host ("  sources: {0} (manifest)  includes: {1}" -f $t.sources.Count, ($t.compile.includes -join ', '))
        $objs = @()
        foreach ($s in $t.sources) {
            $srcRel = $s -replace '/', '\'
            $stem = Get-Stem $srcRel
            $log = Join-Path $objRel "$stem.cl.log"
            $rc = Invoke-Native (Join-Path $Msvc 'bin\Hostx64\x64\cl.exe') ($common + @(("/Fo{0}\" -f $objRel), $srcRel)) $log
            if ($rc -ne 0) {
                Write-Host "  [X] $srcRel (exit $rc)" -ForegroundColor Red
                Get-Content -LiteralPath $log | Select-Object -First 12
                Fail "driver compile failed: $srcRel"
            }
            $obj = Join-Path $objRel "$stem.obj"
            if (-not (Test-Path -LiteralPath $obj)) { Fail "compiler produced no object for $srcRel" }
            $objs += $obj
            Write-Host "  [ok] $srcRel"
        }

        $libRel = (Get-RelFrom $t.workDir ($t.link.libDir))
        $linkArgs = @($t.link.flags) + @("/LIBPATH:$libRel") + $t.link.libs +
                    @("/OUT:$outRel\$($t.output)") + $objs
        $linkLog = Join-Path $outRel 'link.log'
        $rc = Invoke-Native (Join-Path $Msvc 'bin\Hostx64\x64\link.exe') $linkArgs $linkLog
        if ($rc -ne 0) {
            Get-Content -LiteralPath $linkLog | Select-Object -First 20
            Fail "driver link failed (exit $rc)"
        }

        $sys = Join-Path $outRel $t.output
        if (-not (Test-Path -LiteralPath $sys)) { Fail "link produced no $($t.output)" }
        Write-Host "  BUILD OK"
        # [Gate28 2026-11-19] Post-link stack frame budget. This is where the
        # .sys definitely exists, unlike CI (no WDK, no image). The two 0x7F
        # bugchecks came from a 1480 B union frame in RbDispatchDeviceControl
        # that /O2 created by inlining every RbMsg* handler; the failing path
        # (call RbForward) needed no locals at all. Nothing else stops that
        # from coming back silently, so fail the BUILD, not just gates.
        $frameGate = Join-Path $RepoRoot 'src/scripts/check_stack_frame_budget.py'
        if (-not (Test-Path -LiteralPath $frameGate)) { Fail "frame gate missing: $frameGate" }
        $rc = Invoke-Native 'python' @($frameGate, '--sys', $sys) (Join-Path $outRel 'frame.log')
        if ($rc -ne 0) {
            Get-Content -LiteralPath (Join-Path $outRel 'frame.log') -ErrorAction SilentlyContinue | Select-Object -Last 20
            Fail "stack frame budget violated -- see docblock in check_stack_frame_budget.py"
        }
        Write-Host "  [ok] stack frame budget (dispatch <= 230 B, no [1400,1600] frame)"
        $h = Write-Sha256 $sys
        Write-Host "  NOTE: UNSIGNED. Sign before deploy; then re-sync the anchors:"
        foreach ($a in $t.deployAnchors) { Write-Host "        $a" }
        return $h
    }
    finally { Pop-Location }
}

# --------------------------------------------------------------------------
# core regression (Release and/or ASan)
# --------------------------------------------------------------------------
function Build-Core([ValidateSet('both', 'release', 'asan')][string]$Mode = 'both') {
    Step "core: regression ($Mode)"
    $c = $Manifest.targets.core
    $workRel = ($c.workDir -replace '/', '\')
    $wd = Join-Path $RepoRoot $workRel
    # [162 2026-11-12] No hardcoded 'build': the out dir comes from the manifest,
    # same source qa_paths.ps1 uses (targets.core.outDir). 161 section 4.3: a
    # manifest outDir change used to split this script from every QA driver.
    if (-not $c.outDir.StartsWith($c.workDir)) {
        Fail "manifest targets.core.outDir '$($c.outDir)' is not under workDir '$($c.workDir)'"
    }
    $buildRel = ($c.outDir.Substring($c.workDir.Length).TrimStart('/') -replace '/', '\')

    Set-ToolchainEnv 'user'
    Push-Location $wd
    try {
        New-Item -ItemType Directory -Force -Path $buildRel | Out-Null
        # cl resolves quoted #include relative to the SOURCE FILE DIR only when
        # the source is named without a directory prefix; a "..\..\tests\core\"
        # prefix makes /I. fail to cover the include of the sibling stub headers.
        # Pass bare names and put the work dir on the include path (dbg A-D round).
        # NOTE: build with += elements, NOT @('a'+f(), 'b') inline construction:
        # 'string' + Object[] there coerces the array to one space-joined token
        # ("/I..\..\src\core /I.") and cl then loses both include dirs.
        $inc = @()
        $inc += '/I' + (Get-RelFrom $c.workDir 'src/core')
        $inc += '/I.'

        $hosts = @($Manifest.tests.hosts | Where-Object { $_.suite -eq 'regression' })
        $built = @()
        $skipped = 0
        foreach ($h in $hosts) {
            if ($h.skip) { Write-Host "[SKIP] $($h.name) : $($h.skip)"; $skipped++; continue }
            $engine = @()
            foreach ($e in $c.engineSources) { $engine += (Get-RelFrom $c.workDir $e) }
            if ($h.extraSources) { foreach ($e in $h.extraSources) { $engine += (Get-RelFrom $c.workDir $e) } }
            $srcName = (Get-BaseName $h.file)
            $srcArgs = @($srcName) + $engine
            $defs = @()
            if ($h.defines) { foreach ($d in $h.defines) { $defs += "/D$d" } }

            if ($Mode -ne 'asan') {
                $exe = "$buildRel\$($h.name).exe"
                $log = "$buildRel\$($h.name).rc.log"
                Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue
                $rc = Invoke-Native 'cl.exe' (@($c.compile.release) + $inc + $defs + $srcArgs + @("/Fe:$exe", "/Fo$buildRel\")) $log
                if ($rc -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
                    Get-Content -LiteralPath $log | Select-Object -First 10
                    Fail "release build failed: $($h.name)"
                }
                $built += $h.name
                Write-Host "  [ok-rel] $($h.name)"
            }
            if ($Mode -ne 'release') {
                $exe = "$buildRel\asan_$($h.name).exe"
                $log = "$buildRel\asan_$($h.name).rc.log"
                Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue
                $rc = Invoke-Native 'cl.exe' (@($c.compile.asan) + $inc + $defs + $srcArgs + @("/Fe:$exe", "/Fo$buildRel\")) $log
                if ($rc -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
                    Get-Content -LiteralPath $log | Select-Object -First 10
                    Fail "ASan build failed: $($h.name)"
                }
                $built += "asan_$($h.name)"
                Write-Host "  [ok-asan] $($h.name)"
            }
        }
        Write-Host ("  compiled={0} skipped={1}" -f $built.Count, $skipped)

        if ($NoRun) { Write-Host '  -NoRun: skipping execution'; return }
        Step "core: run ($Mode)"
        $runFail = 0
        foreach ($n in $built) {
            $exe = "$buildRel\$n.exe"
            # check_repo pairs the Release log as rel_<t>.out and the ASan log as
            # asan_<t>.out; keep those exact names (R9-15 lesson: a renamed log
            # makes the freshness gate read a stale file forever).
            $out = if ($n -like 'asan_*') { "$buildRel\$n.out" } else { "$buildRel\rel_$n.out" }
            $err = if ($n -like 'asan_*') { "$buildRel\$n.err" } else { "$buildRel\rel_$n.err" }
            & $exe > $out 2> $err
            if ($LASTEXITCODE -ne 0) {
                Write-Host "  [X] $n exit=$LASTEXITCODE" -ForegroundColor Red
                Get-Content -LiteralPath $err -ErrorAction SilentlyContinue | Select-Object -First 8
                $runFail++
            }
        }
        if ($runFail -gt 0) { Fail "$runFail core host(s) exited non-zero" }
        Write-Host "  RUN_FAIL=0"
    }
    finally { Pop-Location }
}

# --------------------------------------------------------------------------
# ctl
# --------------------------------------------------------------------------
function Build-Ctl {
    Step 'ctl: ramboostctl.exe / rbfeatctl.exe'
    $t = $Manifest.targets.ctl
    $workRel = ($t.workDir -replace '/', '\')
    $wd = Join-Path $RepoRoot $workRel
    Set-ToolchainEnv 'user'
    Push-Location $wd
    try {
        foreach ($n in @('ramboostctl.c', 'bench_policy.c', 'rbfeatctl.c')) {
            Assert-Path (Join-Path $wd $n) "ctl source $n"
        }
        foreach ($p in $t.products) {
            $inc = @()
            foreach ($d in $t.compile.includes) { $inc += '/I' + (Get-RelFrom $t.workDir $d) }
            $args = @($t.compile.flags) + $inc + @($p.sources) + @("/Fe:$($p.output)") + @($p.libs)
            $log = "build_$($p.output).log"
            $rc = Invoke-Native 'cl.exe' $args $log
            if ($rc -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $wd $p.output))) {
                Get-Content -LiteralPath $log -ErrorAction SilentlyContinue | Select-Object -First 10
                Fail "ctl build failed: $($p.output)"
            }
            Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
            if ($p.requiredStrings) {
                $txt = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $wd $p.output)))
                foreach ($k in $p.requiredStrings) {
                    if (-not $txt.Contains($k)) { Fail "$($p.output) is missing required string '$k' (stale/misbuilt)" }
                }
                Write-Host ("  [ok] {0} carries {1} contract strings" -f $p.output, $p.requiredStrings.Count)
            }
            Write-Sha256 (Join-Path $wd $p.output) | Out-Null
        }
        Write-Host '  CTL BUILD OK'
    }
    finally { Pop-Location }
}

# --------------------------------------------------------------------------
# setup (verification-only by default)
# --------------------------------------------------------------------------
function Build-Setup {
    Step 'setup: ramboostsetup.exe (temp by default)'
    $t = $Manifest.targets.setup
    $workRel = ($t.workDir -replace '/', '\')
    $wd = Join-Path $RepoRoot $workRel
    $p = $t.products[0]
    Set-ToolchainEnv 'user'

    if ([string]::IsNullOrWhiteSpace($OutFile)) {
        $tmp = Join-Path $env:TEMP ('rbc-setup-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        $dest = Join-Path $tmp $p.output
    }
    else {
        $dest = [IO.Path]::GetFullPath($OutFile)
        $parent = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        Write-Host "  WARNING: -OutFile given; this may overwrite a signed engine." -ForegroundColor Yellow
    }

    Push-Location $wd
    try {
        $inc = @()
        foreach ($d in $t.compile.includes) { $inc += '/I' + (Get-RelFrom $t.workDir $d) }
        $tmpExe = 'rbc_setup_build.exe'
        $args = @($t.compile.flags) + $inc + @($p.sources) +
                @("/Fe:$tmpExe", "/Fo:rbc_setup_build.obj", '/link') + @($p.libs)
        $log = 'rbc_setup_build.log'
        $rc = Invoke-Native 'cl.exe' $args $log
        try {
            if ($rc -ne 0 -or -not (Test-Path -LiteralPath $tmpExe)) {
                Get-Content -LiteralPath $log -ErrorAction SilentlyContinue | Select-Object -First 10
                Fail 'setup build failed'
            }
            Copy-Item -LiteralPath $tmpExe -Destination $dest -Force
        }
        finally {
            Remove-Item -LiteralPath $tmpExe -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath 'rbc_setup_build.obj' -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
        }
        Write-Sha256 $dest | Out-Null
        Write-Host "  OUT=$dest"
        Write-Host '  NOTE: verification-only; the signed delivered engine is untouched.'
    }
    finally { Pop-Location }
}

# --------------------------------------------------------------------------
# ui (.NET solution)
# --------------------------------------------------------------------------
function Build-Ui {
    Step 'ui: dotnet solution'
    $sln = Join-Path $BuildRoot 'RAMBlockCache.sln'
    Assert-Path $sln 'solution file'
    $dotnet = Set-DotnetEnv
    Push-Location $RepoRoot
    try {
        & $dotnet build $sln -c Release --nologo
        if ($LASTEXITCODE -ne 0) { Fail "dotnet build failed (exit $LASTEXITCODE)" }
    }
    finally { Pop-Location }
}

# --------------------------------------------------------------------------
# web
# --------------------------------------------------------------------------
function Build-Web {
    Step 'web: npm run build'
    $t = $Manifest.targets.web
    $wd = Join-Path $RepoRoot ($t.workDir -replace '/', '\')
    Assert-Path $wd 'web project dir'
    Push-Location $wd
    try {
        & npm run build
        if ($LASTEXITCODE -ne 0) { Fail "npm run build failed (exit $LASTEXITCODE)" }
    }
    finally { Pop-Location }
}

# --------------------------------------------------------------------------
# gates
# --------------------------------------------------------------------------
function Invoke-Gates {
    Step 'gates: static contracts + signature (local, strict)'
    Push-Location $RepoRoot
    try {
        $py = @(
            @('src/scripts/check_repo.py', @()),
            @('src/scripts/check_proto_sync.py', @('--strict')),
            @('deliverables/gstack/model/refcount_model.py', @()),
            @('src/scripts/check_xaml_resources.py', @()),
            @('src/scripts/check_node_contract.py', @()),
            @('src/scripts/check_geom_contract.py', @()),
            @('src/scripts/check_stats_contract.py', @()),
            @('src/scripts/check_ctl_freshness.py', @()),
            @('src/scripts/check_blocksize_contract.py', @()),
            @('src/scripts/check_installer_contract.py', @()),
            @('src/scripts/check_config_decode_contract.py', @()),
            @('src/scripts/check_poolcopy_contract.py', @()),
            @('src/scripts/check_buffer_write_contract.py', @()),
            @('src/scripts/check_layout.py', @()),
            @('src/scripts/check_build_manifest.py', @()),
            @('src/scripts/check_ioctl_registry.py', @()),
            @('src/scripts/check_proto_codegen.py', @()),
            # [Gate23 2026-11-14] C <-> TS protocol parity for Web/Bridge layer.
            @('src/scripts/check_ts_proto_sync.py', @()),
            @('src/scripts/check_l2_contract.py', @()),
            # [Gate21 2026-11-13] Whole-repo heuristic sweep: 30 defect classes
            # (C01-08 / T01-08 / S01-08 / P01-06). Fails on HIGH only: MED/LOW are
            # leads for human triage, not build blockers. Rules are heuristic, so
            # HIGH=0 means "no high-confidence defect of these classes", not "clean".
            # Single round: python tools/audit_sweep.py --round C08 [--json out.json]
            # NOTE: this file is ASCII-only on purpose (PS 5.1 GBK console) -- no CJK.
            @('tools/audit_sweep.py', @('--all', '--fail-on-high')),
            # [Gate22 2026-11-13] Deliverable numbering contract: every "next number"
            # claim in AGENTS.md / HANDOVER-CURRENT.md must equal real max + 1.
            # It was stuck at 143/144 while deliverables/ was at 221, and
            # 144-*.md already existed -- i.e. the protocol told readers to
            # overwrite a real report. Docs that instruct wrong actions are gated.
            @('src/scripts/check_docs_numbering.py', @()),
            # [A4 2026-11-16] BootLoopGuard pre-req: RbRegSetClean(1) must have exactly
            # ONE call site (rbmsg.c shutdown path) and the "a volume was attached at
            # least once this boot" gate must stay inside the RbRegSetClean body
            # (count -> v==0 && !BootAttachedAny -> LastCleanWithheld + return).
            # Without it, a boot that never attached a volume still reports clean=1
            # (dirty==0 is trivially true) and fails>=2 can never hold -- 269 A4.
            @('src/scripts/check_a4_clean_gate.py', @()),
            # [v2 2026-11-16] Shutdown persist wiring: RbShutdownPersist must keep
            # the order Quiesce -> Flush -> HotSnapshot -> IdxSaveGuarded ->
            # RegSetClean, the one-shot latch must survive (259 F1 hiberfile
            # restore), RbDispatchShutdownDiag must stop Rb2Tick BEFORE persisting
            # (259 F3) and must de-dup on g_Rb.ControlDevice (268 B6), and
            # RbL2Close must never appear in the persist body. Details in 271.
            @('src/scripts/check_shutdown_persist_order.py', @()),
            # [Gate24 2026-11-17] SUP-01 (why, skip) code table exists in THREE
            # files: the C switch (rbmsg.c), the PS acceptance reader and the
            # Python evidence reader. A4-d added WHY_WITHHELD=7 ->
            # SKIP_CLEAN_WITHHELD=9 and neither reader knew: the Python verdict
            # was built on a boot-reset key, and the PS table printed UNKNOWN
            # for the one outcome A4-d exposes. This gate pins the three copies
            # key-for-key and code-for-code (275).
            @('src/scripts/check_sup01_tables.py', @()),
            # [Gate25 2026-11-18] Control-plane closure for the two new codes
            # 0x906 (read-only shutdown diag) and 0x907 (health commit, V10.5 3.1).
            # Gate17 compares declarations only: a code that is declared on both
            # sides but never dispatched passes it silently. This one pins
            # declare -> dispatch -> user-mode consumer, forbids double dispatch
            # (forward chain vs RbCtl* handler), keeps the 24-byte diag struct
            # layout equal on both sides, whitelists every BootFailCount clear
            # site, and checks the why/skip tables in the CLI mirror against the
            # authoritative ones (rbmsg.c / rb_common.h). 16 mutations, all KILL.
            @('src/scripts/check_ctl_codes.py', @()),
            # [Gate27 2026-11-19] Liquid-glass native host (D3D11 + DirectComposition).
            # Pins the constraints that fail SILENTLY and invisibly: HLSL smin 1/6,
            # discard after ddx_fine (RT-U1), #if/#endif variants, no 2-D groupshared,
            # groupshared capacity >= real index bound (the external proposal shipped
            # an out-of-bounds LDS write here), the four DComp invariants, frameless
            # window messages, DWMSBT_NONE, RbL2Periodic must not seal .idx, and
            # RbUnload must drain PersistInFlight before engine destroy (259 F10).
            # 10 mutations, all KILL, each verified against its OWN failure text.
            @('src/scripts/check_liquid_glass_dcomp_gate.py', @()),
            # [Gate28 2026-11-19] Kernel stack frame budget -- the 0x7F/Arg1=8 fence.
            # Two bugchecks on the same Arg4 low bits (f6a6): RbDispatchDeviceControl
            # allocated 1480 B at entry because /O2 inlined 14 RbMsg* handlers and
            # merged their locals into one 1424 B union frame (1304 B of it was a
            # single RB_STATS local inside RbMsgGetStats), while the failing path was
            # `call RbForward` -- a passthrough that needs no locals at all. Short
            # by exactly 32 B. Fixed with __declspec(noinline) + direct fill, but
            # until now NOTHING stopped a refactor from inlining it all back and
            # silently restoring a 1400+ frame with every gate green.
            # S1 entry frame <= 230 B; S2 no frame in [1400,1600]; S3 parser
            # self-check must be 100% on ntoskrnl; S4 signature string must resolve
            # uniquely (an empty match set must never pass). 7 mutations, all KILL
            # (S1/S2/S4, S5 same-source, S5 cross-source, S5 self-comparison in both
            #  require and skip modes -- the last two exist because disabling the
            #  self-comparison guard must turn --self-test red, or that branch is
            #  untested code pretending to be a check).
            @('src/scripts/check_stack_frame_budget.py', @())
        )
        foreach ($g in $py) {
            if (-not (Test-Path -LiteralPath $g[0])) { Fail "gate script missing: $($g[0])" }
            $rc = Invoke-Native 'python' (@($g[0]) + $g[1]) (Join-Path $env:TEMP ('rbc-gate-' + (Get-Stem $g[0]) + '.log'))
            if ($rc -ne 0) {
                Get-Content -LiteralPath (Join-Path $env:TEMP ('rbc-gate-' + (Get-Stem $g[0]) + '.log')) -ErrorAction SilentlyContinue | Select-Object -Last 15
                Fail "gate FAILED: $($g[0])"
            }
            Write-Host "  [ok] $($g[0])"
        }
        $ps = @('src/scripts/check_ps_syntax.ps1', 'src/scripts/check_signature.ps1')
        foreach ($g in $ps) {
            if (-not (Test-Path -LiteralPath $g)) { Fail "gate script missing: $g" }
            & powershell -NoProfile -ExecutionPolicy Bypass -File $g
            if ($LASTEXITCODE -ne 0) { Fail "gate FAILED: $g" }
            Write-Host "  [ok] $g"
        }
        Write-Host '  ALL GATES PASS'
    }
    finally { Pop-Location }
}

# --------------------------------------------------------------------------
# clean
# --------------------------------------------------------------------------
function Invoke-Clean {
    Step 'clean: intermediates only'
    $targets = @(
        'src/driver/x64',
        $Manifest.targets.core.outDir,
        'src/setup/rbc_setup_build.obj',
        'src/ctl/build_ramboostctl.exe.log',
        'src/ctl/build_rbfeatctl.exe.log'
    )
    foreach ($rel in $targets) {
        $p = Join-Path $RepoRoot ($rel -replace '/', '\')
        if (Test-Path -LiteralPath $p) {
            Remove-Item -LiteralPath $p -Recurse -Force
            Write-Host "  removed $rel"
        }
    }
    Write-Host '  NOTE: dist/, signed .sys, CLI exes and node_modules are preserved.'
}

# --------------------------------------------------------------------------
# native-host: RAMBlockCache.Host.exe (D3D11 + DirectComposition glass host)
# --------------------------------------------------------------------------
# Two hard facts this function exists to encode, both measured on this host:
#   1. wrl/client.h ships under the Windows SDK's winrt/ folder, NOT under the
#      MSVC include dir. WebView2.h ships in the WebView2 NuGet package. Without
#      both, glass_window.cpp fails at its very first include.
#   2. The SDK um/ libs cannot resolve WebView2LoaderStatic's EventRegister /
#      RegOpenKeyExW / OpenProcessToken imports. advapi32 + psapi must be linked
#      explicitly, otherwise LNK2019 x12.
# Shaders are compiled HERE, not at runtime: loading a .cso that was never built
# is a silent black window, and D3DCompile at first frame costs 200ms+.
function Build-NativeHost {
    Step 'native-host: RAMBlockCache.Host.exe'
    # A hyphenated key cannot be reached with dots ($x.native-host parses as
    # subtraction) NOR with a string index on a PSCustomObject in PS7 (that form
    # only ever worked on PS5.1). PSObject.Properties is the one portable route.
    $t = $Manifest.targets.PSObject.Properties['native-host'].Value
    $workRel = ($t.workDir -replace '/', '\')
    $wd = Join-Path $RepoRoot $workRel
    $outRel = ($t.outDir -replace '/', '\')
    $od = Join-Path $RepoRoot $outRel
    New-Item -ItemType Directory -Force -Path $od | Out-Null

    Set-ToolchainEnv 'user'
    $sdkInc = Join-Path $WinSdk "Include\$SdkVer"
    $winrtInc = Join-Path $sdkInc 'winrt'
    Assert-Path $winrtInc 'Windows SDK winrt include (wrl/)'
    # wrl/client.h must be reachable; an empty dir would let the build "succeed"
    # on a machine that merely has a differently-broken SDK.
    Assert-Path (Join-Path $winrtInc 'wrl\client.h') 'wrl/client.h'

    $p = $t.products[0]
    foreach ($s in $p.sources) {
        Assert-Path (Join-Path $wd ($s -replace '/', '\')) "native-host source $s"
    }

    Push-Location $wd
    try {
        $inc = @()
        foreach ($d in $t.compile.includes) { $inc += '/I' + (Join-Path $RepoRoot ($d -replace '/', '\')) }
        $inc += '/I' + $winrtInc
        $wds = @()
        foreach ($w in $t.compile.disabledWarnings) { $wds += "/wd$w" }
        $common = @($t.compile.flags) + @($t.compile.standard) + $wds + $inc

        # ---- shaders ----
        $fxc = Join-Path $WinSdk "bin\$SdkVer\x64\fxc.exe"
        Assert-Path $fxc 'fxc.exe'
        $csoDir = Join-Path $od 'cso'
        New-Item -ItemType Directory -Force -Path $csoDir | Out-Null
        $shaderRoot = Join-Path $RepoRoot ($t.shaderDir -replace '/', '\')
        $shaderIdx = 0
        foreach ($sh in $p.shaders) {
            $entry = $sh[0]; $csoName = $sh[1]; $defs = @($sh[2])
            $shaderIdx++
            # every PSLiquidGlass variant comes from liquid_glass_core.hlsl;
            # CS* from apca_reduce.hlsl. Resolve by entry name, not by guess.
            $src = if ($entry -like 'PSLiquidGlass*') { 'liquid_glass_core.hlsl' }
                   elseif ($entry -like 'PSKawase*') { 'liquid_glass_core.hlsl' }
                   elseif ($entry -like 'CSBlur*') { 'liquid_glass_core.hlsl' }
                   else { 'apca_reduce.hlsl' }
            $profile = if ($entry -like 'PS*') { 'ps_5_0' } else { 'cs_5_0' }
            $shSrc = Join-Path $shaderRoot $src
            Assert-Path $shSrc "shader source $src"
            $shLog = Join-Path $env:TEMP ("rbc-fxc-" + $shaderIdx + ".log")
            $fxcArgs = @('/T', $profile, '/E', $entry, '/O3', '/Zpr') + $defs +
                       @(("/Fo" + (Join-Path $csoDir $csoName)), $shSrc)
            $rc = Invoke-Native $fxc $fxcArgs $shLog
            if ($rc -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $csoDir $csoName))) {
                Get-Content -LiteralPath $shLog -ErrorAction SilentlyContinue | Select-Object -First 8
                Fail "fxc failed for entry '$entry' -> $csoName (a missing .cso is a silent black window at runtime)"
            }
        }
        Write-Host ("  shaders: {0} .cso -> {1}" -f $p.shaders.Count, $csoDir)

        # ---- compile + link ----
        $objs = @()
        foreach ($s in $p.sources) {
            $stem = Get-Stem $s
            $obj = Join-Path $od "$stem.obj"
            $log = Join-Path $env:TEMP ("rbc-nh-" + $stem + ".log")
            $rc = Invoke-Native (Join-Path $Msvc 'bin\Hostx64\x64\cl.exe') `
                  ($common + @(("/Fo" + $obj), ($s -replace '/', '\'))) $log
            if ($rc -ne 0) {
                Write-Host "  [X] $s (exit $rc)" -ForegroundColor Red
                Get-Content -LiteralPath $log -ErrorAction SilentlyContinue | Select-Object -First 12
                Fail "native-host compile failed: $s"
            }
            if (-not (Test-Path -LiteralPath $obj)) { Fail "compiler produced no object for $s" }
            $objs += $obj
        }

        $exe = Join-Path $od $p.output
        $libs = @()
        foreach ($l in $p.libs) { $libs += $l }
        $linkLog = Join-Path $env:TEMP 'rbc-nh-link.log'
        $linkArgs = @(("/OUT:" + $exe), ("/SUBSYSTEM:" + $p.subsystem),
                      ("/ENTRY:" + $p.entry), '/MACHINE:X64', '/OPT:REF', '/OPT:ICF') +
                    $objs + $libs
        if (-not (Test-Path -LiteralPath (Join-Path $od 'WebView2LoaderStatic.lib'))) {
            # WebView2 SDK's lib dir is not on LIB for the user toolchain env
            $env:LIB = "$(Join-Path $RepoRoot '.nuget\packages\microsoft.web.webview2\1.0.4314.35\build\native\x64');$env:LIB"
        }
        $rc = Invoke-Native (Join-Path $Msvc 'bin\Hostx64\x64\link.exe') $linkArgs $linkLog
        if ($rc -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
            Get-Content -LiteralPath $linkLog -ErrorAction SilentlyContinue | Select-Object -First 15
            Fail 'native-host link failed'
        }
        foreach ($o in $objs) { Remove-Item -LiteralPath $o -Force -ErrorAction SilentlyContinue }
        Write-Sha256 $exe | Out-Null
        Write-Host "  OUT=$exe"
        Write-Host '  NATIVE-HOST BUILD OK'
    }
    finally { Pop-Location }
}

switch ($Target) {
    'driver'    { Build-Driver | Out-Null }
    'core'      { Build-Core 'both' }
    'core-asan' { Build-Core 'asan' }
    'ctl'       { Build-Ctl }
    'setup'     { Build-Setup }
    'ui'        { Build-Ui }
    'web'       { Build-Web }
    'native-host' { Build-NativeHost }
    'gates'     { Invoke-Gates }
    'clean'     { Invoke-Clean }
    'all' {
        Build-Driver | Out-Null
        Build-Core 'both'
        Build-Ctl
        Build-Ui
        Build-Web
        Build-NativeHost
        Build-Setup
    }
}

Write-Host ''
Write-Host "BUILD TARGET '$Target' OK"
exit 0
