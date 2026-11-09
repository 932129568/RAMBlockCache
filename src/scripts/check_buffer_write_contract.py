#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate14 · 定长缓冲自增下标写入点契约（防「漏网之鱼」）

130 号找到的 RbLogSnapshot 越界，本质是「固定容量数组 + 无钳位自增下标写入」。
本门禁把全库所有 `\bARR[VAR++] =` 写入点列成守恒清单，并对每一组要求
对应的上界钳位证据仍存在：

  R1 写入点分组集合守恒：新增/消失任何一组都 FAIL（新增必须先人工审计并登记）
  R2 每组钳位正则必须在对应文件中存在（删钳位即 FAIL）
  R3 已修的 RbLogSnapshot 显式钳位（其写法是 Items[n] 而非 Items[n++]，
     不在扫描集合内，故单独断言）
  R4 CI 自检

用法：python check_buffer_write_contract.py [--root <repo_root>]
      --root 供变异测试在最小骨架上运行。
"""
import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[2]
WRITE_RE = re.compile(r"(\w+)\s*\[\s*(\w+)\s*\+\+\s*\]\s*=")
CAP_RE = re.compile(r"<\s*(?:cap|Capacity|q\.MaxCount|RB_[A-Z0-9_]+|\d+)\b")
HEAD_RE = re.compile(r"^(for|while|if)\s*\(")

# 全库自增下标写入点（按 文件/数组/下标变量 去重后的守恒清单）
EXPECTED_GROUPS = {
    ("src/driver/rbL2.c", "path", "k"),
    ("src/driver/rbattach.c", "letters", "count"),
    ("src/driver/rbattach.c", "snap", "n"),
    ("src/driver/rbattach.c", "out", "j"),
    ("src/driver/rbctl.c", "Items", "n"),
    ("src/driver/rbdriver.c", "Out", "count"),
    ("src/driver/rbdriver.c", "snap", "n"),
    ("src/driver/rbdriver.c", "sd", "i"),
    ("src/driver/rbfeat.c", "path", "n"),
    ("src/driver/rbfeat.c", "sub", "n"),
    ("src/driver/rbfeat.c", "snap", "nsnap"),
    ("src/driver/rbhot.c", "top", "n"),
    ("src/driver/rbpools.c", "s_Obj", "s_N"),
}

# 每组写入点必须具备的上界钳位证据（正则，搜索对应文件全文）
GUARDS = [
    ("src/driver/rbL2.c", r"k \+ 1 < cap", "RbL2Path name/prefix copy clamp"),
    ("src/driver/rbL2.c", r"k \+ 3 < cap", "RbL2Path drive-letter clamp"),
    ("src/driver/rbL2.c", r"RbL2Path\(path, RTL_NUMBER_OF\(path\)", "L2 caller passes capacity"),
    ("src/driver/rbattach.c", r"count < RB_MAX_CRITICAL", "critical letters clamp"),
    ("src/driver/rbattach.c", r"n < RB_MAX_VOLUMES", "DevList snapshot clamp"),
    ("src/driver/rbattach.c", r"j < 30", "resume rebuild clamp"),
    ("src/driver/rbattach.c", r"j < 31", "resume append clamp"),
    ("src/driver/rbctl.c", r"n < q\.MaxCount", "event list clamp"),
    ("src/driver/rbctl.c", r"q\.MaxCount > RB_EVENT_MAX", "event MaxCount cap"),
    ("src/driver/rbdriver.c", r"count < Capacity", "diag ring clamp"),
    ("src/driver/rbdriver.c", r"n < RB_LOG_MAX", "RbLogSnapshot clamp (130)"),
    ("src/driver/rbdriver.c", r"n < RB_LOG_MAX", "RbLogSnapshot 64 clamp (130)"),
    ("src/driver/rbdriver.c", r"n < RB_MAX_VOLUMES", "reaper snapshot clamp"),
    ("src/driver/rbdriver.c", r"i < 8", "sysdrv label clamp"),
    ("src/driver/rbfeat.c", r"n < cap", "Rb2Query path clamp (131)"),
    ("src/driver/rbfeat.c", r"RTL_NUMBER_OF\(path\)", "Rb2Query capacity source"),
    ("src/driver/rbfeat.c", r"nsnap < RB_MAX_VOLUMES", "volstatus snapshot clamp"),
    ("src/driver/rbfeat.c", r"WCHAR sub\[24\]", "Policies\\<letter> fixed 24 wchar"),
    ("src/driver/rbhot.c", r"n < 64", "hot-zone top clamp"),
    ("src/driver/rbpools.c", r"s_N < 64", "worker pool array clamp"),
    ("src/driver/rbpools.c", r"s_Obj\[64\]", "worker object array capacity"),
]


def scan(root):
    groups = set()
    hits = defaultdict(int)
    lines = []
    d = root / "src/driver"
    for f in sorted(d.glob("*.c")):
        rel = "src/driver/" + f.name
        for line in f.read_text(encoding="utf-8-sig", errors="replace").splitlines():
            s = line.strip()
            ms = list(WRITE_RE.finditer(s))
            guarded = (not ms) and bool(HEAD_RE.match(s)) and bool(CAP_RE.search(s))
            if not ms and not guarded:
                continue
            lines.append(s)
            for m in ms:
                groups.add((rel, m.group(1), m.group(2)))
                hits[(rel, m.group(1), m.group(2))] += 1
    return groups, hits, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    args = ap.parse_args()
    root = Path(args.root) if args.root else DEFAULT_ROOT
    fails = []

    groups, hits, lines = scan(root)
    missing = sorted(EXPECTED_GROUPS - groups)
    extra = sorted(groups - EXPECTED_GROUPS)
    if missing:
        fails.append("R1 write groups disappeared: %s" % missing)
    if extra:
        fails.append("R1 UNAUDITED new write groups: %s "
                     "(audit before adding)" % extra)

    # R2 行文本守恒：任何一处钳位被删改都会改变该行文本。
    # 全文正则会被「同文件其它位置仍有同名钳位」骗过，故按行精确比对。
    baseline_path = root / "src/scripts/buffer_write_baseline.txt"
    if not baseline_path.is_file():
        fails.append("R2 missing baseline file src/scripts/buffer_write_baseline.txt")
    else:
        base = [l.strip() for l in baseline_path.read_text(
            encoding="utf-8").splitlines() if l.strip()]
        cb, ca = Counter(base), Counter(lines)
        removed = list((cb - ca).elements())
        added = list((ca - cb).elements())
        if removed:
            fails.append("R2 write lines changed/removed (clamp deleted?): %s" % removed)
        if added:
            fails.append("R2 UNAUDITED new write lines: %s" % added)

    cache = {}
    for rel, pattern, why in GUARDS:
        p = root / rel
        if not p.is_file():
            fails.append("R2 missing file %s" % rel)
            continue
        if rel not in cache:
            cache[rel] = p.read_text(encoding="utf-8-sig", errors="replace")
        if not re.search(pattern, cache[rel]):
            fails.append("R2 %s: guard missing [%s] (%s)" % (rel, pattern, why))

    ci = root / ".github/workflows/ci.yml"
    if not ci.is_file() or "check_buffer_write_contract.py" not in ci.read_text(
            encoding="utf-8"):
        fails.append("R4 CI: Gate14 not registered in ci.yml")

    if fails:
        for f in fails:
            print("[FAIL] %s" % f)
        print("result: FAIL (%d fail, 0 warn)" % len(fails))
        return 1
    print("Gate14 buffer-write contract: PASS "
          "(%d audited write groups, %d writes, %d guards)" %
          (len(groups), sum(hits.values()), len(GUARDS)))
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
