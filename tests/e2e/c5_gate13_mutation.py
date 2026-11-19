#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C5 · Gate13 变异测试 + 单机点名脚本 fail-closed 实测

两组验证，真实仓库零改动：
  A) Gate13 骨架变异：baseline PASS → 5 种回归注入全部 FAIL → 还原 PASS
  B) analyze_bugcheck_dump.py --all 对 o9\\dumps 全部历史 dump 必须
     全部 INSUFFICIENT（triage minidump 一律不假绿），并给出 reason
"""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包")
GATE = REPO / "src/scripts/check_poolcopy_contract.py"
ANALYZER = REPO / "src/scripts/analyze_bugcheck_dump.py"
OUT = Path(r"C:\Users\Public\o9\stress/c5_result.json")
REL = ["src/driver/rbfeat.c", "src/driver/rbattach.c",
       "src/driver/rbL2.c", "src/driver/rbdriver.c",
       "src/ctl/rb_proto.h", ".github/workflows/ci.yml"]

MUTATIONS = {
    "V1_119_legacy_maxlength": (
        "src/driver/rbfeat.c",
        "us->MaximumLength = (USHORT)(sizeof(buf) - sizeof(UNICODE_STRING));",
        "us->MaximumLength = (USHORT)(sizeof(buf) - 8);"),
    "V2_121_plain_ulong": (
        "src/driver/rbfeat.c",
        "static volatile LONG s_ExPidN;",
        "static ULONG  s_ExPidN;"),
    "V3_121_no_read_clamp": (
        "src/driver/rbfeat.c",
        "if (n > RB2_EXPIID_MAX) n = RB2_EXPIID_MAX;",
        "if (n > 999999) n = 999999;"),
    "V4_regvalue_no_nul": (
        "src/driver/rbattach.c",
        "((PWCHAR)copy)[qs / sizeof(WCHAR)] = L'\\0';",
        "/* NUL removed */"),
    "V5_l2_record_short": (
        "src/driver/rbL2.c",
        "#define RB2_L2REC       (24 + RB2_L2BLK)",
        "#define RB2_L2REC       (16 + RB2_L2BLK)"),
    "V6_logsnapshot_no_clamp": (
        "src/driver/rbdriver.c",
        "for (i = 0; i < valid && n < RB_LOG_MAX; i++)",
        "for (i = 0; i < valid; i++)"),
}


def run_gate(root):
    p = subprocess.run([sys.executable, str(GATE), "--root", str(root)],
                       capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout.strip()


def main():
    r = {"gate13": {}, "analyzer": {}, "checks": {}}
    with tempfile.TemporaryDirectory(prefix="c5_gate13_") as td:
        root = Path(td)
        for rel in REL:
            dst = root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, dst)
        rc0, out0 = run_gate(root)
        r["gate13"]["baseline"] = {"exit": rc0, "output": out0}
        for name, (rel, old, new) in MUTATIONS.items():
            p = root / rel
            t = p.read_text(encoding="utf-8")
            if old not in t:
                r["gate13"][name] = {"exit": None, "error": "pattern not found"}
                continue
            p.write_text(t.replace(old, new, 1), encoding="utf-8")
            rc, out = run_gate(root)
            r["gate13"][name] = {"exit": rc,
                                 "output": out.splitlines()[:2]}
            p.write_text(t, encoding="utf-8")
        rc1, out1 = run_gate(root)
        r["gate13"]["restored"] = {"exit": rc1, "output": out1}

    g = r["gate13"]
    r["checks"]["gate_baseline_pass"] = g["baseline"]["exit"] == 0
    r["checks"]["gate_all_6_mutations_killed"] = all(
        g.get(k, {}).get("exit") not in (0, None) for k in MUTATIONS)
    r["checks"]["gate_restored_pass"] = g["restored"]["exit"] == 0

    # B) 单机点名脚本 fail-closed 实测
    p = subprocess.run([sys.executable, str(ANALYZER), "--all"],
                       capture_output=True, text=True, timeout=600)
    r["analyzer"]["stdout_tail"] = p.stdout.strip().splitlines()[-14:]
    r["analyzer"]["exit"] = p.returncode
    data = json.loads(Path(r"C:\Users\Public\o9\stress/c6_dump_analysis.json")
                      .read_text(encoding="utf-8"))
    r["analyzer"]["summary"] = {k: data[k] for k in
                                ("analyzed", "pointed", "insufficient",
                                 "symbol_pdb_dirs")}
    r["analyzer"]["per_dump"] = [
        {"dump": Path(x["dump"]).name, "verdict": x["VERDICT"],
         "triage": x.get("triage"), "bc": x.get("bugcheck"),
         "reasons": len(x.get("reasons", []))}
        for x in data["results"]]
    r["checks"]["analyzer_exit_0"] = p.returncode == 0
    r["checks"]["analyzer_all_insufficient"] = (
        data["pointed"] == 0 and data["insufficient"] == data["analyzed"] > 0)
    r["checks"]["analyzer_reasons_present"] = all(
        len(x.get("reasons", [])) > 0 for x in data["results"])

    r["VERDICT"] = "PASS" if all(r["checks"].values()) else "FAIL"
    OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"GATE13": {k: v.get("exit") for k, v in g.items()}},
                     ensure_ascii=False))
    print(json.dumps({"ANALYZER": r["analyzer"]["summary"]}, ensure_ascii=False))
    print(json.dumps({"CHECKS": r["checks"]}, ensure_ascii=False))
    print("RESULT: %s" % r["VERDICT"])
    return 0 if r["VERDICT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
