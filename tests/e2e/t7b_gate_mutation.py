#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T7b · 门禁变异测试（严谨版：先 baseline 再变异，排除骨架假阳性）

T7 首版的 V2 多个门禁 exit=1，原因是最小骨架缺依赖文件（"missing file"），
不是真的检出了 TS 变异。本版补齐全部依赖，并对每个门禁先跑 baseline，
只把「baseline=0 且变异后=1」计为有效检出。

两个独立变异，用来区分门禁到底守哪一侧：
  V2a decodeConfig 删 ['TinyLFU', 152]  -> 66 号白屏的同类缺陷
  V2b decodeStats  删 EngineBlockSize   -> Gate8 明确声明覆盖的一侧（阳性对照）

判定：
  V2b 必须被至少一个门禁检出（否则 Gate8 无效）
  V2a 若无人检出 => F9 缺口在 RB_CONFIG/decodeConfig 侧仍然存在，
       KNOWN_ISSUES F9 的「建议把 RB_CONFIG 尺寸/字段清单纳入门禁」尚未兑现。
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包")
MUT = Path(r"C:\Users\Public\o9\stress\mut2")

# 相对 REPO 的依赖文件（覆盖所有 Gate 的路径常量）
FILES = [
    "src/ctl/rb_proto.h",
    "src/ctl/ramboostctl.c",
    "src/ctl/ramboostctl.exe",
    "src/ctl/rbfeatctl.c",
    "src/ctl/rbfeatctl.exe",
    "src/common/version.h",
    "src/driver/RAMBlockCache.inf",
    "src/driver/rb_common.h",
    "src/driver/rb_common2.h",
    "src/ui/controller/RAMBlockCache.Controller/Models/Models.cs",
    "src/ui/controller/RAMBlockCache.Controller/Models/RbFeature.cs",
    "src/ui/controller/RAMBlockCache.Controller/Services/DriverService.cs",
    "src/ui/controller/RAMBlockCache.Controller/Version.cs",
    "src/ui/controller/RAMBlockCache.Controller/RAMBlockCache.Controller.csproj",
    "src/ui/wpf/Models/RbProto.cs",
    "src/scripts/check_proto_sync.py",
    "src/scripts/check_node_contract.py",
    "src/scripts/check_stats_contract.py",
    "src/scripts/check_blocksize_contract.py",
    "src/scripts/check_installer_contract.py",
    "src/scripts/check_geom_contract.py",
    "src/scripts/check_ctl_freshness.py",
    "src/scripts/check_repo.py",
    "src/scripts/build_setup.ps1",
    "web/package.json",
    "web/package-lock.json",
    "web/bridge/server/proto-decode.ts",
    "web/src/protocol/types.ts",
    "web/src/bridge/http-transport.ts",
    "web/src/state/stats-history.test.ts",
    ".github/workflows/ci.yml",
    "README.txt",
    "README.md",
]

GATES = ["check_proto_sync.py", "check_node_contract.py",
         "check_stats_contract.py", "check_blocksize_contract.py",
         "check_installer_contract.py", "check_geom_contract.py",
         "check_ctl_freshness.py", "check_repo.py"]


def build():
    if MUT.exists():
        shutil.rmtree(MUT)
    for rel in FILES:
        s, d = REPO / rel, MUT / rel
        if s.exists():
            d.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(s, d)
    return MUT


def run_all():
    out = {}
    for name in GATES:
        f = MUT / "src" / "scripts" / name
        if not f.exists():
            out[name] = {"exit": -1, "reason": "missing gate script"}
            continue
        args = [sys.executable, str(f)]
        if name == "check_proto_sync.py":
            args.append("--strict")
        p = subprocess.run(args, capture_output=True, timeout=90)
        txt = (p.stdout + p.stderr).decode("utf-8", "replace")
        out[name] = {"exit": p.returncode,
                     "tail": [l for l in txt.splitlines() if l.strip()][-3:]}
    return out


def mutate_ts(marker):
    ts = MUT / "web" / "bridge" / "server" / "proto-decode.ts"
    orig = ts.read_text(encoding="utf-8-sig")
    lines = orig.splitlines(keepends=True)
    idx = next((i for i, l in enumerate(lines) if marker in l), None)
    if idx is None:
        return None, None
    backup = orig
    lines[idx] = "// [MUTATION] " + lines[idx].lstrip()
    ts.write_text("".join(lines), encoding="utf-8-sig")
    return backup, lines[idx].strip()[:110]


def main():
    print("=== T7b RIGOROUS GATE MUTATION (baseline-controlled) ===", flush=True)
    build()
    base = run_all()
    print(json.dumps({"BASELINE": {k: v["exit"] for k, v in base.items()}},
                     ensure_ascii=False), flush=True)

    res = {"baseline": base}

    # V1 阳性对照：C# 镜像删 TinyLFU
    models = MUT / "src" / "ui" / "controller" / "RAMBlockCache.Controller" / \
        "Models" / "Models.cs"
    mo = models.read_text(encoding="utf-8-sig")
    ml = next((l for l in mo.splitlines() if "TinyLFU" in l), None)
    v1 = None
    if ml:
        models.write_text(mo.replace(ml, "// [MUTATION] " + ml, 1),
                          encoding="utf-8-sig")
        f = MUT / "src" / "scripts" / "check_proto_sync.py"
        p = subprocess.run([sys.executable, str(f), "--strict"],
                          capture_output=True, timeout=90)
        v1 = p.returncode
        models.write_text(mo, encoding="utf-8-sig")
    res["V1_csharp_positive_control_exit"] = v1
    print(json.dumps({"V1_csharp_positive_control_exit": v1},
                     ensure_ascii=False), flush=True)

    # V2a: decodeConfig 删 TinyLFU（66 号白屏同类）
    backup, line = mutate_ts("['TinyLFU', 152]")
    res["V2a_decodeConfig_TinyLFU"] = {"mutated_line": line,
                                       "gates": run_all() if backup else None}
    if backup:
        (MUT / "web" / "bridge" / "server" / "proto-decode.ts").write_text(
            backup, encoding="utf-8-sig")

    # V2b: decodeStats 删 EngineBlockSize（Gate8 应检出，阳性对照）
    marker = "EngineBlockSize"
    backup2, line2 = mutate_ts(marker) if marker else (None, None)
    # mutate_ts 找第一个含 marker 的行；需确认落在 decodeStats 区域
    res["V2b_decodeStats_field"] = {"mutated_line": line2,
                                    "gates": run_all() if backup2 else None}
    if backup2:
        (MUT / "web" / "bridge" / "server" / "proto-decode.ts").write_text(
            backup2, encoding="utf-8-sig")

    # 恢复后的回归，确认还原无误
    res["restore_check"] = {k: v["exit"] for k, v in run_all().items()}

    def detected(case):
        g = res[case]["gates"]
        if not g:
            return []
        return [n for n in GATES
                if base.get(n, {}).get("exit") == 0 and
                g.get(n, {}).get("exit") not in (0, None)]

    res["detected_V2a"] = detected("V2a_decodeConfig_TinyLFU")
    res["detected_V2b"] = detected("V2b_decodeStats_field")
    print(json.dumps({
        "V2a_exits": {k: v["exit"] for k, v in
                      (res["V2a_decodeConfig_TinyLFU"]["gates"] or {}).items()},
        "V2b_exits": {k: v["exit"] for k, v in
                      (res["V2b_decodeStats_field"]["gates"] or {}).items()},
        "detected_V2a": res["detected_V2a"],
        "detected_V2b": res["detected_V2b"],
        "restore_check": res["restore_check"]}, ensure_ascii=False), flush=True)
    print("=== VERDICT ===", flush=True)
    print("V1 (C# mutation) must be exit 1 -> instrument sensitivity.", flush=True)
    print("V2b (decodeStats mutation) must be detected by Gate8.", flush=True)
    print("V2a (decodeConfig/TinyLFU mutation) detection list = %s" %
          res["detected_V2a"], flush=True)
    print("If V2a list is empty, F9's RB_CONFIG/decodeConfig gap is STILL OPEN.",
          flush=True)
    with open(r"C:\Users\Public\o9\stress\t7b_result.json", "w",
              encoding="utf-8") as f:
        json.dump(res, f, ensure_ascii=False, indent=2)
    shutil.rmtree(MUT, ignore_errors=True)
    print(json.dumps({"cleanup": "skeleton removed; repo untouched"},
                     ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
