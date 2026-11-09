#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate12 static contract: RB_CONFIG <-> web/bridge decodeConfig field parity.

背景（124 号 F9 / T7c 变异测试）：
  check_proto_sync.py --strict 只比对 rb_proto.h 与三份 C# 镜像；
  check_stats_contract.py (Gate8) 只守 decodeStats 的五个末尾字段。
  **没有任何门禁覆盖 decodeConfig**：把 proto-decode.ts 里
  `['TinyLFU', 152],` 删掉后 8 道门禁仍全绿 —— 这正是 66 号
  TinyLFU@152 白屏事故的同款缺陷，至今开放。

本门做字段级比对：
  * 从 src/ctl/rb_proto.h 的 `typedef struct RB_CONFIG_T` 解析所有
    uint32_t 字段及按 pack=8 计算的偏移（VolumeFilter wchar[8][4] 占 64B）；
  * 从 web/bridge/server/proto-decode.ts 的 decodeConfig() 解析
    U32_HEAD（偏移 = i*4）与 U32_TAIL（显式偏移）；
  * 两侧字段名 + 偏移逐一相等、数量相等；
  * VolumeFilter 单独断言（TS 用 readWide 解出，不在两个数组里）；
  * sizeof(RB_CONFIG)=156 契约注释 + TinyLFU 的 tiered length guard；
  * CI 必须调用本脚本。

关键：两边都先剥离注释。124 号 T7c 证明「只加 `//` 前缀」能骗过
不剥离注释的正则，本门必须能检出这类注释式移除。

Exit 0 = contract holds; exit 1 = drift.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PROTO_H = ROOT / "src" / "ctl" / "rb_proto.h"
DECODE_TS = ROOT / "web" / "bridge" / "server" / "proto-decode.ts"
CI_YML = ROOT / ".github" / "workflows" / "ci.yml"
SCRIPT_NAME = "check_config_decode_contract.py"


def strip_c_comments(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"(?m)^\s*//.*$", "", t)


def strip_ts_comments(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"(?m)^[ \t]*//.*$", "", t)


def parse_macros(text):
    out = {}
    for name, val in re.findall(r"#define\s+(RB_\w+)\s+(\d+)", text):
        out[name] = int(val)
    return out


def parse_c_config(text, macros):
    """返回 (u32_fields[(name,off)], volume_filter_off, sizeof)。"""
    m = re.search(r"typedef\s+struct\s+RB_CONFIG_T\s*\{(.*?)\}\s*RB_CONFIG\s*;",
                  text, re.S)
    if not m:
        return None, None, None
    body = strip_c_comments(m.group(1))
    fields, vf_off, off = [], None, 0
    for line in body.splitlines():
        line = line.strip()
        if not line:
            continue
        mu = re.match(r"uint32_t\s+(\w+)\s*;", line)
        if mu:
            fields.append((mu.group(1), off))
            off += 4
            continue
        mv = re.match(r"wchar_t\s+(\w+)\s*\[\s*(\w+)\s*\]\s*\[\s*(\w+)\s*\]\s*;", line)
        if mv:
            def dim(x):
                return int(x) if x.isdigit() else macros.get(x, 0)
            n, k = dim(mv.group(2)), dim(mv.group(3))
            vf_off = off
            off += n * k * 2
            continue
    return fields, vf_off, off


def parse_ts_config(text):
    """返回 (u32_fields[(name,off)], has_volume_filter)."""
    m = re.search(r"export\s+function\s+decodeConfig\s*\(\s*\w+\s*:\s*Buffer\s*\)\s*\{(.*?)\n\}",
                  text, re.S)
    if not m:
        return None, False
    body = m.group(1)
    head = re.search(r"const\s+U32_HEAD\s*=\s*\[(.*?)\]\s*as\s+const", body, re.S)
    tail = re.search(r"const\s+U32_TAIL\s*:\s*Array\s*<\s*\[\s*string\s*,\s*number\s*\]\s*>\s*=\s*\[(.*?)\]\s*;",
                     body, re.S)
    if not head or not tail:
        return None, False
    fields = []
    for i, name in enumerate(re.findall(r"'([A-Za-z_][A-Za-z0-9_]*)'", head.group(1))):
        fields.append((name, i * 4))
    for name, off in re.findall(r"\[\s*'([A-Za-z_][A-Za-z0-9_]*)'\s*,\s*(\d+)\s*\]",
                                tail.group(1)):
        fields.append((name, int(off)))
    has_vf = bool(re.search(r"s\s*\.\s*VolumeFilter\s*=", body))
    return fields, has_vf


def main():
    fails = []
    for p in (PROTO_H, DECODE_TS, CI_YML):
        if not p.is_file():
            fails.append("missing file: " + str(p))
    if fails:
        print("== check_config_decode_contract (Gate12) ==")
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1

    proto_raw = PROTO_H.read_text(encoding="utf-8-sig")
    proto = strip_c_comments(proto_raw)
    decode = strip_ts_comments(DECODE_TS.read_text(encoding="utf-8-sig"))
    ci = CI_YML.read_text(encoding="utf-8")

    c_fields, vf_off, sizeof = parse_c_config(proto, parse_macros(proto_raw))
    t_fields, has_vf = parse_ts_config(decode)
    if c_fields is None:
        fails.append("rb_proto.h: RB_CONFIG struct not found")
    if t_fields is None:
        fails.append("proto-decode.ts: decodeConfig U32_HEAD/U32_TAIL not found")
    if c_fields is not None and t_fields is not None:
        if len(c_fields) != len(t_fields):
            fails.append("field count: C=%d TS=%d" % (len(c_fields), len(t_fields)))
        for i, (c, t) in enumerate(zip(c_fields, t_fields)):
            if c[0] != t[0]:
                fails.append("field[%d] name: C=%s TS=%s" % (i, c[0], t[0]))
            elif c[1] != t[1]:
                fails.append("field[%d] %s offset: C=%d TS=%d" %
                             (i, c[0], c[1], t[1]))
        # 逐字段集合也比对（防止整体平移但 zip 恰好对齐的漏检）
        if set(n for n, _ in c_fields) != set(n for n, _ in t_fields):
            fails.append("field name sets differ")
    if vf_off != 64:
        fails.append("VolumeFilter offset: expected 64, got %r" % vf_off)
    if not has_vf:
        fails.append("proto-decode.ts: s.VolumeFilter assignment missing")
    if sizeof != 156:
        fails.append("sizeof(RB_CONFIG): expected 156, computed %r" % sizeof)
    if "sizeof(RB_CONFIG)=156" not in proto_raw:
        fails.append("rb_proto.h: sizeof(RB_CONFIG)=156 contract comment missing")
    if not re.search(r"p\.length\s*>=\s*156\s*\?", decode):
        fails.append("proto-decode.ts: TinyLFU tiered guard >=156 missing")
    if SCRIPT_NAME not in ci:
        fails.append("ci.yml: %s not registered as a gate" % SCRIPT_NAME)

    print("== check_config_decode_contract (Gate12) ==")
    print("  C  u32 fields : %s" % (len(c_fields) if c_fields else "n/a"))
    print("  TS u32 fields : %s" % (len(t_fields) if t_fields else "n/a"))
    print("  sizeof        : %s  VolumeFilter@%s  TS VolumeFilter=%s" %
          (sizeof, vf_off, has_vf))
    for f in fails:
        print("[FAIL] " + f)
    print("result: %s (%d fail)" % ("FAIL" if fails else "PASS", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
