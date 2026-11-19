#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T7 · 门禁变异测试（F9「TS 协议副本不被门禁覆盖」的动态证明）

F9/69 号称：check_proto_sync.py 只比对 rb_proto.h ↔ 3 份 C# 镜像，
不覆盖 web/bridge 的 TS 副本，这是 66 号批「协议加字段而桥接漏改」
却全门禁放行的根因。本脚本用变异测试（mutation testing）判定：

  V0  基线               -> 门禁应 PASS（否则实验无意义）
  V1  C# 镜像删字段       -> 门禁应 FAIL（阳性对照：器具有检出能力）
  V2  TS 副本删 TinyLFU  -> 门禁若仍 PASS，则确证「无人看守」
  V3  静态引用扫描        -> 全部 CI 门禁脚本中引用 web/bridge / proto-decode
                             / TinyLFU 的行数（若为 0，与 V2 互相印证）

安全：所有变异发生在 C:\\Users\\Public\\o9\\stress\\mut 下的最小仓库骨架，
      绝不改写仓库任何文件；结束后清理。
"""
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包")
MUT = Path(r"C:\Users\Public\o9\stress\mut")
SRC = REPO / "src"
SCRIPTS = SRC / "scripts"

REL_FILES = [
    "ctl/rb_proto.h",
    "ui/controller/RAMBlockCache.Controller/Models/Models.cs",
    "ui/controller/RAMBlockCache.Controller/Services/DriverService.cs",
    "ui/wpf/Models/RbProto.cs",
    "common/version.h",
    "ui/controller/RAMBlockCache.Controller/Version.cs",
    "ui/controller/RAMBlockCache.Controller/RAMBlockCache.Controller.csproj",
    "driver/RAMBlockCache.inf",
    "driver/rb_common.h",
    "ui/controller/RAMBlockCache.Controller/Models/RbFeature.cs",
    "ctl/rbfeatctl.c",
    "scripts/check_proto_sync.py",
    "scripts/check_node_contract.py",
    "scripts/check_blocksize_contract.py",
    "scripts/check_stats_contract.py",
    "scripts/check_installer_contract.py",
    "scripts/check_ctl_freshness.py",
    "scripts/check_geom_contract.py",
    "scripts/check_repo.py",
]


def build_skeleton():
    if MUT.exists():
        shutil.rmtree(MUT)
    (MUT / "src" / "scripts").mkdir(parents=True)
    for rel in REL_FILES:
        src = SRC / rel
        if not src.exists():
            continue
        dst = MUT / "src" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    shutil.copy2(REPO / "README.txt", MUT / "README.txt")
    # TS 副本也放进骨架（门禁若覆盖它，应能读到）
    ts = REPO / "web" / "bridge" / "server" / "proto-decode.ts"
    (MUT / "web" / "bridge" / "server").mkdir(parents=True, exist_ok=True)
    shutil.copy2(ts, MUT / "web" / "bridge" / "server" / "proto-decode.ts")
    # 版本联动还会扫 xaml / ci.yml
    for rel in ["ui/controller/RAMBlockCache.Controller/App.xaml"]:
        s = SRC / rel
        if s.exists():
            d = MUT / "src" / rel
            d.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(s, d)
    return MUT / "src" / "scripts" / "check_proto_sync.py"


def run_gate(script):
    p = subprocess.run([sys.executable, str(script), "--strict"],
                       capture_output=True, timeout=90)
    out = (p.stdout + p.stderr).decode("utf-8", "replace")
    tail = [l for l in out.splitlines() if l.strip()][-6:]
    return {"script": script.name, "exit": p.returncode, "tail": tail}


def static_scan():
    hits = []
    pat = re.compile(r"proto-decode|web[\\/]bridge|TinyLFU|RB_CONFIG", re.I)
    for f in sorted(SCRIPTS.glob("*.py")):
        for i, line in enumerate(f.read_text(encoding="utf-8",
                                            errors="replace").splitlines(), 1):
            if pat.search(line):
                hits.append({"file": f.name, "line": i,
                             "text": line.strip()[:120]})
    ci = REPO / ".github" / "workflows" / "ci.yml"
    ci_hits = []
    if ci.exists():
        for i, line in enumerate(ci.read_text(encoding="utf-8",
                                              errors="replace").splitlines(), 1):
            if pat.search(line):
                ci_hits.append({"line": i, "text": line.strip()[:120]})
    return {"script_files_scanned": len(list(SCRIPTS.glob("*.py"))),
            "hits_in_gate_scripts": hits, "hits_in_ci": ci_hits}


def main():
    print("=== T7 GATE MUTATION TEST (F9 TS-copy gap) ===", flush=True)
    gate = build_skeleton()
    print(json.dumps({"skeleton": str(MUT), "gate": str(gate)},
                     ensure_ascii=False), flush=True)

    r = {}
    r["V0_baseline"] = run_gate(gate)
    print(json.dumps({"V0_baseline": r["V0_baseline"]}, ensure_ascii=False),
          flush=True)

    # V1 阳性对照：C# 镜像里删掉 TinyLFU 字段行
    models = MUT / "src" / "ui" / "controller" / "RAMBlockCache.Controller" / \
        "Models" / "Models.cs"
    orig = models.read_text(encoding="utf-8", errors="replace")
    hit = [l for l in orig.splitlines() if "TinyLFU" in l]
    if hit:
        mutated = orig.replace(hit[0], "// [MUTATION] " + hit[0], 1)
        models.write_text(mutated, encoding="utf-8-sig")
        r["V1_csharp_mutation"] = {"line": hit[0].strip()[:120],
                                   "gate": run_gate(gate)}
        models.write_text(orig, encoding="utf-8-sig")
    else:
        r["V1_csharp_mutation"] = {"error": "no TinyLFU line in Models.cs"}
    print(json.dumps({"V1": r["V1_csharp_mutation"]}, ensure_ascii=False),
          flush=True)

    # V2：TS 副本删掉 TinyLFU 字段（66 号白屏的同类变异）
    ts = MUT / "web" / "bridge" / "server" / "proto-decode.ts"
    torig = ts.read_text(encoding="utf-8", errors="replace")
    tline = [l for l in torig.splitlines() if "'TinyLFU'" in l]
    if tline:
        ts.write_text(torig.replace(tline[0], "// [MUTATION] " + tline[0], 1),
                      encoding="utf-8")
        r["V2_ts_mutation"] = {"line": tline[0].strip()[:120]}
        for name in ["check_proto_sync.py", "check_node_contract.py",
                     "check_blocksize_contract.py", "check_stats_contract.py",
                     "check_installer_contract.py", "check_repo.py"]:
            f = MUT / "src" / "scripts" / name
            if f.exists():
                r["V2_ts_mutation"][name] = run_gate(f)
        ts.write_text(torig, encoding="utf-8")
    else:
        r["V2_ts_mutation"] = {"error": "no TinyLFU line in TS copy"}

    r["V3_static_scan"] = static_scan()
    print(json.dumps({"V2_summary": {
        k: (v.get("exit") if isinstance(v, dict) and "exit" in v else v)
        for k, v in r["V2_ts_mutation"].items()}}, ensure_ascii=False),
        flush=True)
    print(json.dumps({"V3_static_scan": r["V3_static_scan"]},
                     ensure_ascii=False), flush=True)

    # 结论
    v0 = r["V0_baseline"]["exit"]
    v1 = r["V1_csharp_mutation"].get("gate", {}).get("exit")
    v2 = [v.get("exit") for k, v in r["V2_ts_mutation"].items()
          if isinstance(v, dict) and "exit" in v]
    print("=== VERDICT ===", flush=True)
    print("V0 baseline exit=%s (expect 0)" % v0, flush=True)
    print("V1 C# mutation exit=%s (expect !=0 -> instrument has teeth)" % v1,
          flush=True)
    print("V2 TS mutation exits=%s (all 0 => F9 gap CONFIRMED: no gate "
          "watches the bridge decoder)" % v2, flush=True)
    print("V3 gate-script hits on proto-decode/web-bridge/TinyLFU = %d" %
          len(r["V3_static_scan"]["hits_in_gate_scripts"]), flush=True)
    with open(r"C:\Users\Public\o9\stress\t7_result.json", "w",
              encoding="utf-8") as f:
        json.dump(r, f, ensure_ascii=False, indent=2)
    shutil.rmtree(MUT, ignore_errors=True)
    print(json.dumps({"cleanup": "mutation skeleton removed; repo untouched"},
                     ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
