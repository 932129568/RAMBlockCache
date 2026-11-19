#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T8 · 剩余未闭环项的只读量化与证据可得性判别

覆盖：
  I16/P2  README.txt mojibake 可逆性（字节级实测，不改写）
  I10/P1  池腐坏分配点：驱动池标签 CBRb 的多轮 alloc/free 配对稳定性
  I15/P1  Setup 发布链滞后：dist/payload/Setup/文档基线四方哈希矩阵
  P0      A2 三门槛的可满足性量化（观察期剩余秒数）
  I26/I28 压测后崩溃证据（dump / 日志 ERROR 段）
  D8/I19  64K 口径：源码白名单 vs KNOWN_ISSUES 红线文本的一致性

全部只读；不写仓库、不改配置、不重启、不制造崩溃。
"""
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import time
from pathlib import Path

REPO = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包")
POOLTAGS = r"C:\Users\Public\o9\rbc_pooltags.exe"
LOGF = Path(r"C:\ProgramData\RAMBlockCache\logs\driver-20261109.log")
BOOT_UTC = "2026-11-09T06:14:19.000Z"  # 14:14:19 GMT+8；与日志 24 字符前缀等长
BOOT_LOCAL = dt.datetime(2026, 11, 9, 14, 14, 19)
OBSERVATION_DAYS = 7


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def pool():
    o = subprocess.run([POOLTAGS, "100000"], capture_output=True,
                       timeout=30).stdout.decode("ascii", "replace")
    for line in o.splitlines():
        p = line.split()
        if len(p) >= 5 and p[0].lower() == "cbrb":
            return {"tag": p[0], "np_mb": float(p[2]),
                    "allocs": int(p[3]), "frees": int(p[4])}
    return {"tag": "CBRb", "np_mb": 0.0, "allocs": 0, "frees": 0,
            "absent": True}


def readme_mojibake():
    raw = (REPO / "README.txt").read_bytes()
    text = raw.decode("utf-8", "replace")
    bom = raw[:3] == b"\xef\xbb\xbf"
    q = text.count("?")
    crlf = raw.count(b"\r\n")
    lf = raw.count(b"\n")
    # 逆转预演：UTF-8 中文被按 GBK 解读后再存 UTF-8 -> 反向 encode gbk / decode utf8
    recoded_ok = recoded_bad = 0
    try:
        back = text.encode("gbk", "replace").decode("utf-8", "replace")
        recoded_ok = len(back)
        recoded_bad = back.count("\ufffd")
    except Exception as e:
        back = repr(e)
    # sys 锚点行
    m = re.search(r"DefaultExpectedSha256\s*=\s*([0-9a-f]{64})", text)
    anchor = m.group(1) if m else None
    return {"bytes": len(raw), "bom": bom, "question_marks": q,
            "crlf": crlf, "lf_total": lf,
            "crlf_absorbed_paragraphs": lf - crlf,
            "reverse_preview_len": recoded_ok,
            "reverse_replacement_chars": recoded_bad,
            "sys_anchor_sha": anchor,
            "sys_anchor_matches_dist": anchor == sha(REPO / "dist" / "RAMBlockCache.sys"),
            "reversible_losslessly": (q == 0 and crlf == lf)}


def release_matrix():
    dist = REPO / "dist" / "RAMBlockCache.sys"
    payload = REPO / "src" / "ui" / "installer" / "RAMBlockCache.Setup" / \
        "Assets" / "payload" / "RAMBlockCache.sys"
    setup = REPO / "dist" / "RAMBlockCache-Setup.exe"
    return {
        "dist_sys": {"sha": sha(dist), "bytes": dist.stat().st_size,
                     "mtime": dt.datetime.fromtimestamp(dist.stat().st_mtime)
                     .isoformat(timespec="seconds")},
        "setup_payload_sys": {"sha": sha(payload),
                              "bytes": payload.stat().st_size,
                              "mtime": dt.datetime.fromtimestamp(
                                  payload.stat().st_mtime).isoformat(timespec="seconds")},
        "setup_exe": {"sha": sha(setup), "bytes": setup.stat().st_size,
                      "mtime": dt.datetime.fromtimestamp(
                          setup.stat().st_mtime).isoformat(timespec="seconds")},
        "payload_is_current": sha(payload) == sha(dist),
        "generations_behind": "11-08 payload vs 11-09 dist (5 fix batches: "
                              "110/113/115/116/119/121)"}


def pool_stability(rounds=6, gap=1.5):
    rows = []
    for i in range(rounds):
        p = pool()
        p["t"] = time.strftime("%H:%M:%S")
        p["outstanding"] = p["allocs"] - p["frees"]
        rows.append(p)
        if i + 1 < rounds:
            time.sleep(gap)
    first, last = rows[0], rows[-1]
    return {"rounds": rows,
            "outstanding_start": first["outstanding"],
            "outstanding_end": last["outstanding"],
            "outstanding_delta": last["outstanding"] - first["outstanding"],
            "np_mb_start": first["np_mb"], "np_mb_end": last["np_mb"],
            "paired_under_idle": last["outstanding"] == first["outstanding"]}


def crash_evidence():
    md = Path(r"C:\Windows\Minidump")
    dumps = []
    if md.exists():
        for f in sorted(md.glob("*.dmp")):
            dumps.append({"name": f.name, "bytes": f.stat().st_size,
                          "mtime": dt.datetime.fromtimestamp(
                              f.stat().st_mtime).isoformat(timespec="seconds")})
    memory = Path(r"C:\Windows\MEMORY.DMP")
    err_lines = []
    if LOGF.exists():
        for line in LOGF.read_text(encoding="utf-8", errors="replace").splitlines():
            if line[:24] >= BOOT_UTC and " ERROR " in line:
                err_lines.append(line[:200])
    return {"minidumps": dumps, "minidump_count": len(dumps),
            "newest_dump": dumps[-1] if dumps else None,
            "MEMORY_DMP_present": memory.exists(),
            "boot_segment_ERROR_lines": len(err_lines),
            "boot_segment_ERROR_samples": err_lines[:3]}


def a2_gate():
    now = dt.datetime.now()
    elapsed = int((now - BOOT_LOCAL).total_seconds())
    target = BOOT_LOCAL + dt.timedelta(days=OBSERVATION_DAYS)
    remain = int((target - now).total_seconds())
    return {"boot_local": BOOT_LOCAL.isoformat(timespec="seconds"),
            "now": now.isoformat(timespec="seconds"),
            "elapsed_s": elapsed,
            "observation_target": target.isoformat(timespec="seconds"),
            "remaining_s": remain, "remaining_days": round(remain / 86400, 2),
            "gate2_time_window_met": remain <= 0,
            "gate1_core_total_order": "user-mode models PASS (refcount 17/17, "
                                      "syncio 9/9, expid 84 interleavings) "
                                      "- not a kernel-side proof",
            "gate3_rebuild_boot_mutex": "96 SPEC OK by review; no kernel-side "
                                        "runtime proof on this image",
            "verdict": "FROZEN: gate2 not met, and A2 requires explicit user decision"}


def k64_conflict():
    srcs = {
        "driver rbctl.c": REPO / "src" / "driver" / "rbctl.c",
        "core cache.c": REPO / "src" / "core" / "cache.c",
        "CLI ramboostctl.c": REPO / "src" / "ctl" / "ramboostctl.c",
    }
    found = {}
    for k, p in srcs.items():
        t = p.read_text(encoding="utf-8", errors="replace")
        found[k] = bool(re.search(r"\{4096,\s*8192,\s*16384,\s*32768,\s*65536\}",
                                 t))
    ki = (REPO / "KNOWN_ISSUES.md").read_text(encoding="utf-8", errors="replace")
    claims = {
        "D8 says 64K NO-GO": "64K 解禁 NO-GO" in ki,
        "D8 says clamp back to 4096": "把它钳回 4096" in ki,
        "I19 says forbid 64K unban": "禁 64K 解禁" in ki,
        "B1 says 4-layer block": "64K/128K 白名单四层封堵" in ki,
        "97 doc exists": (REPO / "deliverables" /
                          "97-64k-unban-perf-unfreeze-2026-11-08.md").exists(),
    }
    return {"source_whitelists_contain_65536": found,
            "known_issues_claims": claims,
            "conflict": all(found.values()) and claims["D8 says 64K NO-GO"]
            and claims["I19 says forbid 64K unban"]}


def main():
    print("=== T8 CLOSURE-EVIDENCE BATCH ===", flush=True)
    r = {}
    for name, fn in [("readme_mojibake", readme_mojibake),
                     ("release_matrix", release_matrix),
                     ("crash_evidence", crash_evidence),
                     ("a2_gate", a2_gate),
                     ("k64_conflict", k64_conflict)]:
        r[name] = fn()
        print(json.dumps({name: r[name]}, ensure_ascii=False), flush=True)
    r["pool_stability"] = pool_stability()
    print(json.dumps({"pool_stability": r["pool_stability"]},
                     ensure_ascii=False), flush=True)
    with open(r"C:\Users\Public\o9\stress\t8_result.json", "w",
              encoding="utf-8") as f:
        json.dump(r, f, ensure_ascii=False, indent=2)
    print("=== T8 DONE ===", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
