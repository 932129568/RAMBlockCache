#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate17: IOCTL 注册表 —— 主功能号唯一性 + 跨文件副本一致性。

背景（161 号 §5.3）：DeviceType 0x22 被 5 个头文件 + 1 个 .c 瓜分，
0x806/0x807/0x80B..0x900 空着，谁先加谁占；`rb_common2.h:9` 的
"若撞 rb_proto.h 功能号整体平移"只是一句注释纪律，不是机制。
`check_proto_sync.py:321-326` 只锚定 RB_IOCTL_TYPE/RB_IOCTL_FUNCTION 算出
0x222000，完全不看 0x808/0x809/0x80A/0x90x；`rbfeatctl.c:23-27` 是
`rb_common2.h:10-19` 的手工第二份，两侧撞号/漂移都无人发现。

本门禁只做两件事（纯新增，不修改任何既有断言，也不判断"该不该用这个号"）：
  A. 同一 (DeviceType, Function) 不被两个**不同**符号占用；
  B. 同一符号在多份文件里重复定义时，展开值必须一致。
另外把 `*_FUNC` 裸功能号也纳入唯一性（它们都各有一个配套 CTL_CODE，
提前一步就能抓住撞号），并打印完整分配表。

`CTL_CODE` 按 Win32 定义手工展开：
  (DeviceType << 16) | (Access << 14) | (Function << 2) | Method

任何无法求值的实参一律 FAIL（fail-closed），不静默跳过——"解析不到"
在门禁里必须比"解析出来不对"更早暴露。

用法: python src/scripts/check_ioctl_registry.py
退出码: 0=PASS, 1=FAIL
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# 被扫描的文件：权威头 + 客户端手工副本（161 号 §5.3 的表）
SOURCES = (
    "src/ctl/rb_proto.h",
    "src/ctl/rb_excl.h",
    "src/ctl/rb_logq.h",
    "src/ctl/rb_bootcfg.h",
    "src/driver/rb_common2.h",
    "src/ctl/rbfeatctl.c",
)

# 已发布的 IOCTL 符号。它们删掉/改名都是 ABI 破坏，必须显式红，
# 而不是"文件里少了几个就少比几个"（同 check_proto_sync 的 MSG_FLOOR 口径）。
PUBLISHED = (
    "IOCTL_RB_REQUEST",
    "IOCTL_RB_EXCLUDE",
    "IOCTL_RB_LOGQ",
    "IOCTL_RB_BOOTCFG",
    "RB2_IOCTL_STATS",
    "RB2_IOCTL_REFRESH",
    "RB2_IOCTL_SHRINK",
    "RB2_IOCTL_SNAPSHOT",
    "RB2_IOCTL_VOLSTATUS",
)

# winioctl.h 的公共常量（文件自身若定义则优先用文件内的值）
BUILTIN = {
    "FILE_DEVICE_UNKNOWN": 0x22,
    "METHOD_BUFFERED": 0,
    "METHOD_IN_DIRECT": 1,
    "METHOD_OUT_DIRECT": 2,
    "METHOD_NEITHER": 3,
    "FILE_ANY_ACCESS": 0,
    "FILE_READ_ACCESS": 1,
    "FILE_WRITE_ACCESS": 2,
}

NUM = re.compile(r"^(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*$")

fails = []


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"(?m)//.*$", "", text)


def join_continuations(text):
    return re.sub(r"\\[ \t]*\r?\n", " ", text)


def eval_arg(tok, macros, where):
    """把 CTL_CODE 的一个实参求成整数；解析不到返回 None（由调用方 FAIL）。"""
    tok = tok.strip().rstrip("uUlL") if NUM.match(tok.strip()) else tok.strip()
    if NUM.match(tok):
        return int(tok, 0)
    if tok in macros:
        return macros[tok]
    if tok in BUILTIN:
        return BUILTIN[tok]
    fails.append("%s: 无法求值的实参 %r（既非数字、非本文件宏、非 winioctl 常量）"
                 % (where, tok))
    return None


def parse_file(rel):
    """返回 (ioctls, funcs)；ioctls=[dict]，funcs={name: value}。"""
    p = ROOT / rel
    if not p.is_file():
        fails.append("缺源头文件: %s" % rel)
        return [], {}
    raw = p.read_text(encoding="utf-8-sig", errors="replace")
    # 行号必须取自**原始**文本：剥注释/接续行之后行号会前移，报错位置就指不准。
    line_of = {}
    for m in re.finditer(r"(?m)^[ \t]*#define[ \t]+(\w+)[ \t]+CTL_CODE",
                         raw):
        line_of[m.group(1)] = raw[:m.start()].count("\n") + 1
    text = join_continuations(strip_comments(raw))
    # 本文件内的标量宏（含 FILE_DEVICE_UNKNOWN 之类的本地覆盖）
    macros = {}
    for m in re.finditer(r"(?m)^[ \t]*#define[ \t]+(\w+)[ \t]+"
                         r"(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*[ \t]*$", text):
        macros[m.group(1)] = int(m.group(2), 0)

    ioctls = []
    for m in re.finditer(r"(?m)^[ \t]*#define[ \t]+(\w+)[ \t]+CTL_CODE[ \t]*\(([^)]*)\)",
                         text):
        sym, args = m.group(1), [a.strip() for a in m.group(2).split(",")]
        where = "%s:%d" % (rel, line_of.get(sym, 0))
        if len(args) != 4:
            fails.append("%s: %s 的 CTL_CODE 实参个数 %d != 4" % (where, sym, len(args)))
            continue
        vals = [eval_arg(a, macros, where) for a in args]
        if any(v is None for v in vals):
            continue
        dt, func, method, access = vals
        ioctls.append({
            "symbol": sym, "where": where, "dt": dt, "func": func,
            "method": method, "access": access,
            "value": (dt << 16) | (access << 14) | (func << 2) | method,
        })

    funcs = {k: v for k, v in macros.items()
             if re.search(r"_FUNC(_\w+)*$", k)}
    return ioctls, funcs


def main():
    all_ioctls, all_funcs = [], []      # all_funcs: (name, value, file)
    for rel in SOURCES:
        io, fu = parse_file(rel)
        all_ioctls += io
        all_funcs += [(k, v, rel) for k, v in fu.items()]

    # 反静默自检：一个都解析不出来 = 正则/文件结构漂移，不能报 PASS
    if not all_ioctls:
        fails.append("未解析到任何 CTL_CODE 定义（解析器或在扫描文件已漂移）")

    by_sym = {}
    for e in all_ioctls:
        by_sym.setdefault(e["symbol"], []).append(e)

    # A. (DeviceType, Function) 全局唯一
    by_key = {}
    for e in all_ioctls:
        by_key.setdefault((e["dt"], e["func"]), []).append(e)
    for (dt, fn), group in sorted(by_key.items()):
        names = sorted({e["symbol"] for e in group})
        if len(names) > 1:
            where = ", ".join("%s@%s" % (e["symbol"], e["where"]) for e in group)
            fails.append("撞号: DeviceType=0x%02X Function=0x%03X 被 %d 个不同符号占用 "
                         "(%s)" % (dt, fn, len(names), where))

    # B. 同一符号跨文件必须一致
    for sym, group in sorted(by_sym.items()):
        vals = {e["value"] for e in group}
        pairs = {(e["dt"], e["func"]) for e in group}
        if len(vals) > 1 or len(pairs) > 1:
            detail = ", ".join("%s=0x%08X(dt=0x%02X,fn=0x%03X)"
                               % (e["where"], e["value"], e["dt"], e["func"])
                               for e in group)
            fails.append("副本漂移: %s 在多处的展开值不一致 (%s)" % (sym, detail))

    # A'. 裸 *_FUNC 功能号：不同名字不得同值（提前一步抓"谁先加谁占"）
    by_val = {}
    for name, val, rel in all_funcs:
        by_val.setdefault(val, set()).add(name)
    for val, names in sorted(by_val.items()):
        if len(names) > 1:
            fails.append("功能号重复: 0x%03X 被 %s 共用（多份 *_FUNC 定义）"
                         % (val, sorted(names)))

    # 已发布符号不得消失
    for sym in PUBLISHED:
        if sym not in by_sym:
            fails.append("已发布 IOCTL 符号消失: %s（ABI 破坏，改名/删除都必须显式登记）"
                         % sym)

    # ── 分配表 ──
    print("== check_ioctl_registry (Gate17) ==")
    print("  %-34s %-22s %-6s %-10s %-6s %s" %
          ("file", "symbol", "Device", "Function", "Method", "IOCTL"))
    for e in sorted(all_ioctls, key=lambda x: (x["dt"], x["func"], x["symbol"])):
        print("  %-34s %-22s 0x%02X   0x%03X       %d       0x%08X"
              % (e["where"], e["symbol"], e["dt"], e["func"], e["method"], e["value"]))
    print("  ioctl defines: %d unique symbol(s); *_FUNC macros: %d"
          % (len(by_sym), len(all_funcs)))
    for f in fails:
        print("[FAIL] " + f)
    print("result: %s (%d fail, 0 warn)" % ("FAIL" if fails else "PASS", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
