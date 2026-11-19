#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T7c · 门禁变异最终版：删除式变异 + 注释盲区对照

T7b 的 V2b 阳性对照失败，原因不是门禁无效，而是「注释掉但保留文本」
仍能通过 check_stats_contract.py 的正则（它不剥离 TS 注释）。
本版：
  V2a 删除 decodeConfig 的 ['TinyLFU', 152] 整行
  V2b 删除 decodeStats 的 EngineBlockSize 赋值整行（阳性对照）
  V2c 只给 decodeStats 赋值行加注释前缀（检出正则的注释盲区）
"""
import json
import shutil
import subprocess
import sys
from pathlib import Path
import t7b_gate_mutation as base

MUT = base.MUT
TS = MUT / "web" / "bridge" / "server" / "proto-decode.ts"


def delete_line(marker):
    orig = TS.read_text(encoding="utf-8-sig")
    lines = orig.splitlines(keepends=True)
    idx = next((i for i, l in enumerate(lines) if marker in l), None)
    if idx is None:
        return None, None
    removed = lines[idx].strip()[:110]
    lines[idx] = ""
    TS.write_text("".join(lines), encoding="utf-8-sig")
    return orig, removed


def comment_line(marker):
    orig = TS.read_text(encoding="utf-8-sig")
    lines = orig.splitlines(keepends=True)
    idx = next((i for i, l in enumerate(lines) if marker in l), None)
    if idx is None:
        return None, None
    lines[idx] = "// [MUTATION] " + lines[idx].lstrip()
    TS.write_text("".join(lines), encoding="utf-8-sig")
    return orig, lines[idx].strip()[:110]


def gate8():
    f = MUT / "src" / "scripts" / "check_stats_contract.py"
    p = subprocess.run([sys.executable, str(f)], capture_output=True,
                       timeout=90)
    txt = (p.stdout + p.stderr).decode("utf-8", "replace")
    return p.returncode, [l for l in txt.splitlines() if l.strip()][-3:]


def main():
    print("=== T7c DELETE-MUTATION + COMMENT-BLINDNESS CONTROL ===", flush=True)
    base.build()
    all0 = base.run_all()
    b = {k: v["exit"] for k, v in all0.items()}
    print(json.dumps({"BASELINE": b}, ensure_ascii=False), flush=True)

    out = {"baseline": b}

    # V2b 阳性对照：删除 decodeStats 字段行
    orig, line = delete_line("s.EngineBlockSize")
    out["V2b_delete_decodeStats"] = {"removed": line, "gates": base.run_all(),
                                     "gate8": gate8()}
    TS.write_text(orig, encoding="utf-8-sig")

    # V2a：删除 decodeConfig 的 TinyLFU 字段行
    orig, line = delete_line("['TinyLFU', 152]")
    out["V2a_delete_decodeConfig"] = {"removed": line, "gates": base.run_all(),
                                      "gate8": gate8()}
    TS.write_text(orig, encoding="utf-8-sig")

    # V2c：注释式变异（正则盲区）
    orig, line = comment_line("s.EngineBlockSize")
    out["V2c_comment_decodeStats"] = {"commented": line, "gate8": gate8()}
    TS.write_text(orig, encoding="utf-8-sig")

    def detected(case):
        g = out[case]["gates"]
        return [n for n in base.GATES
                if b.get(n) == 0 and g.get(n, {}).get("exit") not in (0, None)]

    out["detected_V2a"] = detected("V2a_delete_decodeConfig")
    out["detected_V2b"] = detected("V2b_delete_decodeStats")
    print(json.dumps({
        "V2b_gate8_exit": out["V2b_delete_decodeStats"]["gate8"][0],
        "V2b_detected_by": out["detected_V2b"],
        "V2a_gate8_exit": out["V2a_delete_decodeConfig"]["gate8"][0],
        "V2a_detected_by": out["detected_V2a"],
        "V2c_gate8_exit_comment_only": out["V2c_comment_decodeStats"]["gate8"][0],
        "V2a_all_exits": {k: v["exit"] for k, v in
                          out["V2a_delete_decodeConfig"]["gates"].items()},
    }, ensure_ascii=False), flush=True)
    print("=== VERDICT ===", flush=True)
    print("1) V2b gate8 must FAIL => Gate8 has teeth (positive control).", flush=True)
    print("2) V2a undetected => F9 gap on RB_CONFIG/decodeConfig is STILL OPEN.",
          flush=True)
    print("3) V2c gate8 PASS while implementation is commented out => "
          "static regex does not strip TS comments; a comment-only 'removal' "
          "is a false-green path (new finding).", flush=True)
    with open(r"C:\Users\Public\o9\stress\t7c_result.json", "w",
              encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    shutil.rmtree(MUT, ignore_errors=True)
    print(json.dumps({"cleanup": "skeleton removed; repo untouched"},
                     ensure_ascii=False), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
