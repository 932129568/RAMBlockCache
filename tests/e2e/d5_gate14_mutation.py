#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""D5 · Gate14 变异测试（证明守恒清单与钳位断言有牙）

骨架复制全部 src/driver/*.c + ci.yml，真实仓库零改动。
V1..V6 六种回归注入，每种都必须让 Gate14 FAIL；还原后 PASS。
"""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包")
GATE = REPO / "src/scripts/check_buffer_write_contract.py"
OUT = Path(r"C:\Users\Public\o9\stress\d5_result.json")

MUTATIONS = {
    "V1_remove_logsnapshot_clamp": (
        "src/driver/rbdriver.c",
        "for (i = 0; i < valid && n < RB_LOG_MAX; i++)",
        "for (i = 0; i < valid; i++)"),
    "V2_remove_rb2query_clamp": (
        "src/driver/rbfeat.c",
        "while (Sub[k] && n < cap) path[n++] = Sub[k++];",
        "while (Sub[k]) path[n++] = Sub[k++];"),
    "V3_remove_workerpool_clamp": (
        "src/driver/rbpools.c",
        "for (g = 0; g < ng && s_N < 64; g++)",
        "for (g = 0; g < ng; g++)"),
    "V4_new_unaudited_write": (
        "src/driver/rbhot.c",
        "VOID RbHotSnapshot(VOID)\n{",
        "static void d5_probe(void){ int newbuf[4], m = 0; newbuf[m++] = 1; }\nVOID RbHotSnapshot(VOID)\n{"),
    "V5_remove_l2path_clamp": (
        "src/driver/rbL2.c",
        "while (*name && k + 1 < cap) path[k++] = *name++;",
        "while (*name) path[k++] = *name++;"),
    "V6_remove_critical_clamp": (
        "src/driver/rbattach.c",
        "while (i < n && w[i] && count < RB_MAX_CRITICAL) {",
        "while (i < n && w[i]) {"),
}


def run(root):
    p = subprocess.run([sys.executable, str(GATE), "--root", str(root)],
                       capture_output=True, text=True, timeout=60)
    return p.returncode, p.stdout.strip().splitlines()[:3]


def main():
    r = {"gate14": {}, "checks": {}}
    rels = sorted("src/driver/" + p.name for p in (REPO / "src/driver").glob("*.c"))
    rels.append(".github/workflows/ci.yml")
    rels.append("src/scripts/buffer_write_baseline.txt")
    with tempfile.TemporaryDirectory(prefix="d5_gate14_") as td:
        root = Path(td)
        for rel in rels:
            dst = root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, dst)
        rc0, out0 = run(root)
        r["gate14"]["baseline"] = {"exit": rc0, "output": out0}

        for name, (rel, old, new) in MUTATIONS.items():
            p = root / rel
            t = p.read_text(encoding="utf-8-sig", errors="replace")
            if old not in t:
                r["gate14"][name] = {"exit": None, "error": "pattern not found"}
                continue
            p.write_text(t.replace(old, new, 1), encoding="utf-8-sig")
            rc, out = run(root)
            r["gate14"][name] = {"exit": rc, "output": out}
            p.write_text(t, encoding="utf-8-sig")

        rc1, out1 = run(root)
        r["gate14"]["restored"] = {"exit": rc1, "output": out1}

    g = r["gate14"]
    r["checks"]["baseline_pass"] = g["baseline"]["exit"] == 0
    r["checks"]["all_6_mutations_killed"] = all(
        g.get(k, {}).get("exit") not in (0, None) for k in MUTATIONS)
    r["checks"]["restored_pass"] = g["restored"]["exit"] == 0
    r["VERDICT"] = "PASS" if all(r["checks"].values()) else "FAIL"
    OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"GATE14": {k: v.get("exit") for k, v in g.items()}},
                     ensure_ascii=False))
    print(json.dumps({"CHECKS": r["checks"]}, ensure_ascii=False))
    print("RESULT: %s" % r["VERDICT"])
    return 0 if r["VERDICT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
