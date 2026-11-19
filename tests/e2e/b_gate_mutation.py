#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""B 批门禁变异测试：验证 Gate12 / Gate8 修复后确有检出力

零污染：所有变异发生在 C:\\Users\\Public\\o9\\stress\\bmut 的最小仓库骨架，
真实仓库文件只读复制，结束后整目录删除。

变异矩阵：
  V0 baseline                      -> Gate12 PASS, Gate8 PASS
  V1 删除 decodeConfig TinyLFU 行  -> Gate12 FAIL（124 号 T7c 的 F9 缺口）
  V2 注释 decodeConfig TinyLFU 行  -> Gate12 FAIL（注释式绕过也要被拦）
  V3 注释 decodeStats EngineBlockSize 行 -> Gate8 FAIL（124 号 T7c 的注释盲区）
  V4 restore                       -> 全 PASS（证明还原无误）
"""
import json
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包")
MUT = Path(r"C:\Users\Public\o9\stress\bmut")

REL = [
    "src/scripts/check_config_decode_contract.py",
    "src/scripts/check_stats_contract.py",
    "src/ctl/rb_proto.h",
    "web/bridge/server/proto-decode.ts",
    "web/src/protocol/types.ts",
    "web/src/bridge/http-transport.ts",
    "web/src/state/stats-history.test.ts",
    "src/ctl/ramboostctl.c",
    ".github/workflows/ci.yml",
]


def build():
    if MUT.exists():
        shutil.rmtree(MUT)
    for r in REL:
        s, d = REPO / r, MUT / r
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(s, d)
    return MUT / "web" / "bridge" / "server" / "proto-decode.ts"


def gates():
    out = {}
    for name in ("check_config_decode_contract.py", "check_stats_contract.py"):
        f = MUT / "src" / "scripts" / name
        p = subprocess.run([sys.executable, str(f)], capture_output=True,
                           timeout=60)
        tail = (p.stdout + p.stderr).decode("utf-8", "replace").strip().splitlines()
        out[name] = {"exit": p.returncode, "tail": tail[-3:]}
    return out


def mutate(path, needle, mode):
    orig = path.read_text(encoding="utf-8-sig")
    lines = orig.splitlines(keepends=True)
    idx = next((i for i, l in enumerate(lines) if needle in l), None)
    if idx is None:
        return None
    before = lines[idx]
    if mode == "delete":
        lines[idx] = ""
    else:
        lines[idx] = "// [MUTATION] " + lines[idx].lstrip()
    path.write_text("".join(lines), encoding="utf-8-sig")
    return orig, before.strip()[:100]


def main():
    print("=== B-BATCH GATE MUTATION (skeleton, repo untouched) ===", flush=True)
    ts = build()
    res = {"V0_baseline": gates()}
    print(json.dumps({"V0": res["V0_baseline"]}, ensure_ascii=False), flush=True)

    b1 = mutate(ts, "['TinyLFU', 152]", "delete")
    res["V1_delete_config_field"] = {"mutated": b1[1] if b1 else None,
                                     "gates": gates()}
    if b1:
        ts.write_text(b1[0], encoding="utf-8-sig")

    b2 = mutate(ts, "['TinyLFU', 152]", "comment")
    res["V2_comment_config_field"] = {"mutated": b2[1] if b2 else None,
                                      "gates": gates()}
    if b2:
        ts.write_text(b2[0], encoding="utf-8-sig")

    b3 = mutate(ts, "s.EngineBlockSize =", "comment")
    res["V3_comment_stats_field"] = {"mutated": b3[1] if b3 else None,
                                     "gates": gates()}
    if b3:
        ts.write_text(b3[0], encoding="utf-8-sig")

    res["V4_restore"] = gates()

    g12 = "check_config_decode_contract.py"
    g8 = "check_stats_contract.py"
    summary = {
        "V0_both_PASS": all(v[g]["exit"] == 0 for v in [res["V0_baseline"]]
                            for g in (g12, g8)),
        "V1_Gate12_killed": res["V1_delete_config_field"]["gates"][g12]["exit"] != 0,
        "V2_Gate12_killed_comment": res["V2_comment_config_field"]["gates"][g12]["exit"] != 0,
        "V3_Gate8_killed_comment": res["V3_comment_stats_field"]["gates"][g8]["exit"] != 0,
        "V4_restored_PASS": all(res["V4_restore"][g]["exit"] == 0
                                for g in (g12, g8)),
    }
    res["SUMMARY"] = summary
    print(json.dumps({"SUMMARY": summary}, ensure_ascii=False), flush=True)
    for k in ("V1_delete_config_field", "V2_comment_config_field",
              "V3_comment_stats_field"):
        print("%-30s Gate12=%s Gate8=%s" %
              (k, res[k]["gates"][g12]["exit"], res[k]["gates"][g8]["exit"]),
              flush=True)
    print("RESULT: %s" % ("PASS" if all(summary.values()) else "FAIL"), flush=True)
    Path(r"C:\Users\Public\o9\stress\b_gate_mutation_result.json").write_text(
        json.dumps(res, ensure_ascii=False, indent=2), encoding="utf-8")
    shutil.rmtree(MUT, ignore_errors=True)
    return 0 if all(summary.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
