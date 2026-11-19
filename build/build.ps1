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
    [ValidateSet('all', 'driver', 'core', 'core-asan', 'ctl', 'setup', 'ui', 'web', 'gates', 'clean')]
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
            @('src/scripts/check_liquid_glass_dcomp_gate.py', @())
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

switch ($Target) {
    'driver'    { Build-Driver | Out-Null }
    'core'      { Build-Core 'both' }
    'core-asan' { Build-Core 'asan' }
    'ctl'       { Build-Ctl }
    'setup'     { Build-Setup }
    'ui'        { Build-Ui }
    'web'       { Build-Web }
    'gates'     { Invoke-Gates }
    'clean'     { Invoke-Clean }
    'all' {
        Build-Driver | Out-Null
        Build-Core 'both'
        Build-Ctl
        Build-Ui
        Build-Web
        Build-Setup
    }
}

Write-Host ''
Write-Host "BUILD TARGET '$Target' OK"
exit 0
