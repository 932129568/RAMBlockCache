#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate9 (local CLI artifact freshness) -- closes the S2 preflight gap.

Found 2026-11-07: src/ctl/ramboostctl.exe was built 2026-11-06 06:27 while
src/ctl/ramboostctl.c (which adds the five RB_STATS statval keys) was edited
2026-11-06 17:52. build_setup.ps1:139 embeds that exe verbatim, so a Setup
rebuild would have shipped a CLI that cannot answer `statval engine_bs` --
silently breaking acceptance gate E1-c in deliverables/79 (and the whole
point of exposing EngineBlockSize in the first place). No existing gate
caught it: check_repo.py only pairs core/test_*.c with core/build/*.out|exe.

Two independent assertions:
  1) CONTENT (CI-reliable, does not depend on mtimes): the five statval key
     strings MUST be present in ramboostctl.exe. git does not preserve mtimes,
     so this is the only check that works on a hosted runner.
  2) MTIME (local-only): a present binary MUST NOT be older than its sources.
     Useless on a fresh clone (all mtimes equal) but never a false FAIL there.

CI-safe: if a binary is absent (fresh checkout / CI without build artifacts),
that check is skipped with an INFO line, never a FAIL. A present binary is
always fully enforced. Read-only: no builds, no writes, no subprocess.

Usage: python src/scripts/check_ctl_freshness.py
Exit 0 = PASS, 1 = FAIL.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CTL = ROOT / "src" / "ctl"
TOL = 2.0

# [162 G-D 2026-11-12] RB_STATS2 的字段级契约。
# 原来只有 mtime：rbfeatctl.c 与 rb_common2.h 都改过（哪怕只是交换两个字段的
# 顺序）时 mtime 检查必然 PASS，而 ABI 已经错了。这里补两个纯源码断言：
#   1) rbfeatctl.c 的手工 STATS2 副本 <-> rb_common2.h 的 RB_STATS2：
#      字段名 + 顺序 + 宽度 -> 按 pack=8 自然对齐算出偏移，逐项相等；
#   2) 快照 stats2.schema.json（由 --update 从**权威头**生成）<-> 当前权威头：
#      权威头被改动而快照没跟着更新就 FAIL —— 这是唯一能发现"两侧同步改写、
#      但 ABI 相对上一个已发布驱动已经变了"的手段。exe 里没有任何可解析的
#      字段清单（只有代码与字符串），所以快照锚在头文件而非二进制。
STATS2_H = ROOT / "src" / "driver" / "rb_common2.h"
STATS2_C = CTL / "rbfeatctl.c"
STATS2_SNAP = pathlib.Path(__file__).with_name("stats2.schema.json")

STATVAL_KEYS = ["engine_bs", "rebuild_count", "rebuild_status",
                "rebuild_in_progress", "rebuild_utc"]
CTL_SOURCES = ["ramboostctl.c", "bench_policy.c", "bench_policy.h", "rb_proto.h"]
# rbfeatctl.c:30 declares a hand-maintained RB_STATS2 mirror that MUST stay in
# lockstep with src/driver/rb_common2.h. That header changed 2026-11-07 12:25;
# an older rbfeatctl.exe would ship a stale control-plane copy. Relative path
# is resolved below, so this also works from a CI checkout.
FEAT_SOURCES = ["rbfeatctl.c", "../driver/rb_common2.h"]

fails = []
infos = []


def check(exe_name, sources, keys):
    exe = CTL / exe_name
    if not exe.is_file():
        infos.append("%s absent (fresh checkout) -> skipped" % exe_name)
        return
    # 1) content contract
    if keys:
        blob = exe.read_bytes()
        for k in keys:
            if k.encode("ascii") not in blob:
                fails.append("%s missing statval key %r -- STALE OR MISBUILT "
                             "(rebuild with src/ctl/_build_ctl_both.ps1)" % (exe_name, k))
    # 2) local mtime contract
    try:
        exe_m = exe.stat().st_mtime
        for s in sources:
            p = (CTL / s).resolve()
            if not p.is_file():
                continue
            if exe_m < p.stat().st_mtime - TOL:
                fails.append("%s is older than %s -- rebuild the CLI" %
                             (exe_name, p.relative_to(ROOT).as_posix()))
    except OSError as e:
        fails.append("%s mtime read failed: %s" % (exe_name, e))


check("ramboostctl.exe", CTL_SOURCES, STATVAL_KEYS)
check("rbfeatctl.exe", FEAT_SOURCES, [])

# ── [162 G-D] RB_STATS2 字段级契约 ────────────────────────────────────────────
# 宽度表与 check_proto_sync.py 的 Layout 同一口径（pack=8 自然对齐，结构体
# 对齐取成员最大对齐），只是这里只出现 4/8 字节整数。
TYPES = {"ULONG": 4, "ULONG32": 4, "ULONG64": 8,
         "unsigned __int64": 8, "__int64": 8}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"(?m)//.*$", "", text)


def layout(fields):
    """[(name, width)] -> ([(name, off)], sizeof)。pack=8 自然对齐。"""
    off, out, maxal = 0, [], 1
    for name, w in fields:
        al = w if w <= 8 else 8
        off = (off + al - 1) // al * al
        out.append((name, off))
        off += w
        maxal = max(maxal, al)
    return out, (off + maxal - 1) // maxal * maxal


def parse_decls(body, types):
    """把结构体体按 ';' 切开，展开 `TYPE a, b, c;` 多名字声明。"""
    fields = []
    pat = r"(%s)\s+(.+)$" % "|".join(re.escape(t) for t in types)
    for seg in body.split(";"):
        seg = " ".join(seg.split())
        if not seg:
            continue
        m = re.match(pat, seg)
        if not m:
            continue
        for one in m.group(2).split(","):
            one = one.split("=")[0].strip()
            if one:
                fields.append((one, TYPES[m.group(1)]))
    return fields


STATS2_TYPES = ("unsigned __int64", "ULONG64", "ULONG32", "ULONG")


def parse_stats2_header():
    p = STATS2_H
    if not p.is_file():
        fails.append("缺权威头 %s" % p.relative_to(ROOT).as_posix())
        return None
    s = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
    m = re.search(r"typedef\s+struct\s+_RB_STATS2\s*\{(.*?)\}\s*RB_STATS2", s, re.S)
    if not m:
        fails.append("rb_common2.h: 未找到 typedef struct _RB_STATS2")
        return None
    f = parse_decls(m.group(1), STATS2_TYPES)
    if not f:
        fails.append("rb_common2.h: RB_STATS2 未解析出任何字段")
        return None
    return f


def parse_stats2_ctl():
    p = STATS2_C
    if not p.is_file():
        fails.append("缺 CLI 副本 %s" % p.relative_to(ROOT).as_posix())
        return None
    s = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
    m = re.search(r"typedef\s+struct\s*\{(.*?)\}\s*STATS2\s*;", s, re.S)
    if not m:
        fails.append("rbfeatctl.c: 未找到 typedef struct {...} STATS2")
        return None
    f = parse_decls(m.group(1), STATS2_TYPES)
    if not f:
        fails.append("rbfeatctl.c: STATS2 未解析出任何字段")
        return None
    return f


def schema_of(fields):
    offs, size = layout(fields)
    return {"sizeof": size,
            "fields": [[n, o, w] for (n, o), (_, w) in zip(offs, fields)]}


hdr = parse_stats2_header()
ctl = parse_stats2_ctl()
snap_now = schema_of(hdr) if hdr else None

if "--update" in sys.argv:
    if snap_now is None:
        print("[FAIL] cannot update snapshot: header parse failed")
        sys.exit(1)
    STATS2_SNAP.write_text(
        json.dumps({"source": "src/driver/rb_common2.h", "struct": "RB_STATS2",
                    **snap_now}, indent=2) + "\n", encoding="utf-8", newline="\n")
    print("snapshot updated: %s (sizeof=%d, %d fields)"
          % (STATS2_SNAP.name, snap_now["sizeof"], len(snap_now["fields"])))
    sys.exit(0)

if hdr and ctl:
    h_offs, h_size = layout(hdr)
    c_offs, c_size = layout(ctl)
    if len(h_offs) != len(c_offs):
        fails.append("RB_STATS2 <-> STATS2 字段数不同: header=%d ctl=%d"
                     % (len(h_offs), len(c_offs)))
    for i, (a, b) in enumerate(zip(h_offs, c_offs)):
        if a[0] != b[0]:
            fails.append("RB_STATS2 <-> STATS2 field[%d] name: header=%s ctl=%s"
                         % (i, a[0], b[0]))
        elif a[1] != b[1]:
            fails.append("RB_STATS2 <-> STATS2 field %s offset: header=%d ctl=%d"
                         % (a[0], a[1], b[1]))
    if h_size != c_size:
        fails.append("RB_STATS2 <-> STATS2 sizeof: header=%d ctl=%d"
                     % (h_size, c_size))

if snap_now is not None:
    if not STATS2_SNAP.is_file():
        fails.append("缺字段快照 %s（生成: python -B %s --update）"
                     % (STATS2_SNAP.name, pathlib.Path(__file__).name))
    else:
        try:
            snap = json.loads(STATS2_SNAP.read_text(encoding="utf-8"))
        except Exception as e:
            fails.append("字段快照不可解析: %s" % e)
            snap = None
        if snap is not None:
            old = [[n, o, w] for n, o, w in snap.get("fields", [])]
            new = snap_now["fields"]
            if snap.get("sizeof") != snap_now["sizeof"] or old != new:
                idx = next((i for i, (a, b) in enumerate(zip(old, new)) if a != b),
                           min(len(old), len(new)))
                fails.append(
                    "RB_STATS2 权威头与快照不一致（ABI 已变，需重新基线并登记）: "
                    "sizeof %r -> %r, first diff [%d] %r -> %r"
                    % (snap.get("sizeof"), snap_now["sizeof"], idx,
                       old[idx] if idx < len(old) else None,
                       new[idx] if idx < len(new) else None))

print("== check_ctl_freshness (Gate9) ==  root=%s" % ROOT)
if hdr and ctl:
    print("  RB_STATS2: %d fields, sizeof=%d (header == ctl mirror)"
          % (len(hdr), layout(hdr)[1]))
print("  field snapshot: %s" % STATS2_SNAP.name)
for i in infos:
    print("[INFO] " + i)
if not fails:
    print("[ok] ramboostctl.exe carries all %d RB_STATS statval keys" % len(STATVAL_KEYS))
for f in fails:
    print("[FAIL] " + f)
print("result: %s (%d fail, 0 warn)" % ("PASS" if not fails else "FAIL", len(fails)))
sys.exit(1 if fails else 0)
