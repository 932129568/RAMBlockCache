#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate13 · 池/静态区越界写契约（防 00:32 根因族回归）

背景：11-09 00:32 的 0x7E/0xC0000005 池腐坏运行于 c6d36c4c（00:11 代）。
该代同时含两处已定位的越界写，均在当天后续批次修复：
  - 119 号 13:21：Rb2CurrentImagePath 的
        us->MaximumLength = sizeof(buf) - 8
    与「数据区从 +8 个 WCHAR(=16B) 开始」不匹配 ⇒ 多报 8 字节越界；
    正确契约是 sizeof(buf) - sizeof(UNICODE_STRING)。
  - 121 号 14:06：s_ExPidN 无锁「检查/索引/++」TOCTOU ⇒
    s_ExPid[64] 越界写，提交注释明确标注「与 00:32 未点名的 0x7E 同族候选」；
    正确契约是 volatile LONG + Interlocked 原子占槽 + 读侧上界钳到 64。
本门禁静态钉死这些契约，并覆盖另外两处 Unicode/大块池拷贝边界：
  - RbReadRegValue：分配 qs + sizeof(WCHAR)、拷贝 qs、NUL 写在 [qs/2]
  - L2 记录：RB2_L2REC = sizeof(RB2_L2HDR) + RB2_L2BLK，且
    RtlCopyMemory(rec+1, src, RB2_L2BLK) 不得越出该分配

用法：python check_poolcopy_contract.py [--root <repo_root>]
      --root 供变异测试在最小骨架上运行，真实仓库零改动。
"""
import argparse
import re
import sys
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[2]


def strip_c(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", l) for l in text.splitlines())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    args = ap.parse_args()
    root = Path(args.root) if args.root else DEFAULT_ROOT
    fails = []

    paths = {
        "rbfeat": root / "src/driver/rbfeat.c",
        "rbattach": root / "src/driver/rbattach.c",
        "rbL2": root / "src/driver/rbL2.c",
        "ci": root / ".github/workflows/ci.yml",
    }
    for k, p in paths.items():
        if not p.is_file():
            fails.append("missing file: %s" % p)
    if fails:
        print("\n".join(fails))
        print("result: FAIL (%d fail)" % len(fails))
        return 1

    feat = strip_c(paths["rbfeat"].read_text(encoding="utf-8-sig"))
    attach = strip_c(paths["rbattach"].read_text(encoding="utf-8-sig"))
    l2 = strip_c(paths["rbL2"].read_text(encoding="utf-8-sig"))
    ci = paths["ci"].read_text(encoding="utf-8")

    # A1/A2 119 号：MaximumLength 必须与 UNICODE_STRING 头对齐，且旧写法绝迹
    good_ml = "us->MaximumLength = (USHORT)(sizeof(buf) - sizeof(UNICODE_STRING));"
    bad_ml = "sizeof(buf) - 8"
    n_good = feat.count(good_ml)
    if n_good < 2:
        fails.append("A1 rbfeat: sizeof(UNICODE_STRING)-aligned MaximumLength "
                     "must appear on BOTH the primary and the 27-fallback path "
                     "(found %d)" % n_good)
    if bad_ml in feat:
        fails.append("A2 rbfeat: legacy 'sizeof(buf) - 8' MaximumLength "
                     "(8-byte OOB, 119 pre-fix) reintroduced")

    # A3/A4 121 号：原子占槽 + 读侧钳位
    if "static volatile LONG s_ExPidN;" not in feat:
        fails.append("A3 rbfeat: s_ExPidN must be 'static volatile LONG' "
                     "(plain ULONG reintroduces TOCTOU OOB write)")
    if "static ULONG  s_ExPidN;" in feat:
        fails.append("A3b rbfeat: plain ULONG s_ExPidN detected")
    if feat.count("InterlockedCompareExchange((volatile LONG *)&s_ExPidN") < 3:
        fails.append("A4 rbfeat: Interlocked-based slot reservation incomplete "
                     "(need read + CAS-loop + publish)")
    if "if (n > RB2_EXPIID_MAX) n = RB2_EXPIID_MAX;" not in feat:
        fails.append("A4b rbfeat: read-side upper bound clamp missing")

    # A5 RbReadRegValue：分配/拷贝/NUL 三者同界
    for token, why in (
        ("qs + sizeof(WCHAR)", "allocation"),
        ("RtlCopyMemory(copy, qd, qs)", "copy length"),
        ("((PWCHAR)copy)[qs / sizeof(WCHAR)] = L'\\0';", "NUL index"),
    ):
        if token not in attach:
            fails.append("A5 rbattach: RbReadRegValue %s contract missing" % why)

    # A6 L2 记录：头 24B + 64KB，写 rec+1 不越界
    m_blk = re.search(r"#define\s+RB2_L2BLK\s+\(64\s*\*\s*1024\)", l2)
    m_rec = re.search(r"#define\s+RB2_L2REC\s+\(24\s*\+\s*RB2_L2BLK\)", l2)
    m_hdr = re.search(r"typedef struct _RB2_L2HDR\s*\{(.*?)\}\s*RB2_L2HDR;",
                      l2, re.S)
    if not (m_blk and m_rec and m_hdr):
        fails.append("A6 rbL2: RB2_L2BLK / RB2_L2REC / RB2_L2HDR definition missing")
    else:
        fields = m_hdr.group(1)
        size = (8 if "ULONG64 Lba" in fields else 0) + \
               (4 if "ULONG Stamp" in fields else 0) + \
               (4 if "ULONG Crc" in fields else 0) + \
               (8 if "ULONG64 VolId" in fields else 0)
        if size != 24:
            fails.append("A6 rbL2: RB2_L2HDR size is %d, RB2_L2REC assumes 24 "
                         "=> RtlCopyMemory(rec+1,...,64KB) would overflow" % size)
        if "RtlCopyMemory((PCHAR)(rec + 1), src, RB2_L2BLK);" not in l2:
            fails.append("A6b rbL2: L2 record payload copy missing or changed")

    # A7 CI 自检
    if "check_poolcopy_contract.py" not in ci:
        fails.append("A7 CI: Gate13 not registered in ci.yml")

    if fails:
        for f in fails:
            print("[FAIL] %s" % f)
        print("result: FAIL (%d fail, 0 warn)" % len(fails))
        return 1
    print("Gate13 pool-copy contract: PASS "
          "(A1/A2 119 layout, A3/A4 121 atomic slot, A5 reg value, "
          "A6 L2 record, A7 CI)")
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
