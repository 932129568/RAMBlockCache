#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate8 static contract: RB_STATS geometry-rebuild five-field ABI + TS/CLI copies.

Read-only, no subprocess, no build artifacts required. Runs on GitHub's
windows-latest hosted runner and locally. Guards the exact gap B12 found:
  check_proto_sync.py --strict covers the three C# hand copies but NONE of the
  src/ui/web/ TypeScript hand copies (types.ts / statsSchema / proto-decode.ts) nor the
  CLI `statval` keys. A future edit that appends an RB_STATS/RB_CONFIG field to
  rb_proto.h without syncing the TS side is the TinyLFU@152 white-screen class
  of incident (66); this script fails closed on such drift.

Contract (see deliverables/85-system-audit-and-fix-plan-2026-11-07.md B12):
  - five trailing fields at offsets 1280/1284/1288/1292(u32) and 1296(u64),
    sizeof(RB_STATS)=1304, ProtocolVersion unchanged;
  - decodeStats uses TIERED length guards (1280B old reply -> all five 0,
    never RangeError);
  - types.ts RbStats, http-transport.ts statsSchema, ramboostctl.c statval and
    stats-history.test.ts fixture all carry the five fields;
  - CI keeps calling this script as Gate8.

Exit 0 = contract holds; exit 1 = a TS/CLI copy drifted or ABI changed.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

# (name, offset, read-kind, guard-threshold)
FIELDS = [
    ("EngineBlockSize",           1280, "u32", 1284),
    ("GeometryRebuildCount",      1284, "u32", 1288),
    ("GeometryRebuildLastStatus", 1288, "u32", 1292),
    ("GeometryRebuildInProgress", 1292, "u32", 1296),
    ("LastGeometryRebuildUtc",    1296, "u64", 1304),
]
NAMES = [f[0] for f in FIELDS]
SIZEOF = 1304

PROTO_H   = ROOT / "src" / "ctl" / "rb_proto.h"
DECODE_TS = ROOT / "src/ui/web" / "bridge" / "server" / "proto-decode.ts"
TYPES_TS  = ROOT / "src/ui/web" / "src" / "protocol" / "types.ts"
HTTP_TS   = ROOT / "src/ui/web" / "src" / "bridge" / "http-transport.ts"
STAT_TEST = ROOT / "src/ui/web" / "src" / "state" / "stats-history.test.ts"
CTL_C     = ROOT / "src" / "ctl" / "ramboostctl.c"
CI_YML    = ROOT / ".github" / "workflows" / "ci.yml"
# [298 2026-11-19] GET_STATS capacity contract (see check 8).
RBCTL_C   = ROOT / "src" / "driver" / "rbctl.c"
CTL_CONN  = ROOT / "src" / "ctl" / "ctl_conn.c"
STATVAL_KEYS = ["engine_bs", "rebuild_count", "rebuild_status",
                "rebuild_in_progress", "rebuild_utc"]


def strip_ts_comments(text):
    """[FIX O9-COMMENT 2026-11-09] 静态契约必须先剥离 TS 注释再匹配。
    124 号 T7c 变异测试证明：只给 decodeStats 的字段赋值行加 `//` 前缀，
    原实现仍能在注释文本里正则命中并按 PASS 放行（假绿）。此处移除
    块注释与「行首（含缩进）//」行注释；不误伤字符串里的 `//`（如 URL）。
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"(?m)^[ \t]*//.*$", "", text)


def main():
    fails = []
    paths = (PROTO_H, DECODE_TS, TYPES_TS, HTTP_TS, STAT_TEST, CTL_C, CI_YML,
             RBCTL_C, CTL_CONN)
    for p in paths:
        if not p.is_file():
            fails.append("missing file: %s" % p.relative_to(ROOT).as_posix())
    if fails:
        print("== check_stats_contract (Gate8) ==")
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1

    proto = PROTO_H.read_text(encoding="utf-8-sig")
    decode = strip_ts_comments(DECODE_TS.read_text(encoding="utf-8-sig"))
    types = TYPES_TS.read_text(encoding="utf-8-sig")
    http = HTTP_TS.read_text(encoding="utf-8-sig")
    test = STAT_TEST.read_text(encoding="utf-8-sig")
    ctl = CTL_C.read_text(encoding="utf-8-sig")
    ci = CI_YML.read_text(encoding="utf-8")
    rbctl = RBCTL_C.read_text(encoding="utf-8-sig")
    conn = CTL_CONN.read_text(encoding="utf-8-sig")

    # --- 1) rb_proto.h: five trailing fields + documented offsets/sizeof ---
    for name, off, _kind, _guard in FIELDS:
        # each field must be declared in the RB_STATS struct
        if not re.search(r"\b%s\b" % re.escape(name), proto):
            fails.append("rb_proto.h: field %s missing" % name)
    for off in (1280, 1284, 1288, 1292, 1296):
        if str(off) not in proto:
            fails.append("rb_proto.h: documented offset %d missing" % off)
    if "sizeof(RB_STATS)=1304" not in proto:
        fails.append("rb_proto.h: sizeof(RB_STATS)=1304 contract comment missing")
    # LastGeometryRebuildUtc must be the 64-bit tail field (u64@1296 -> 1304)
    if not re.search(r"uint64_t\s+LastGeometryRebuildUtc", proto):
        fails.append("rb_proto.h: LastGeometryRebuildUtc must be uint64_t (u64@1296)")

    # --- 2) decodeStats: exact offsets + TIERED length guards (1280B safe) ---
    m = re.search(r"export function decodeStats\(p: Buffer\)\s*\{(.*?)\n\}",
                  decode, re.S)
    if not m:
        fails.append("proto-decode.ts: decodeStats() body not found")
    else:
        body = m.group(1)
        for name, off, kind, guard in FIELDS:
            if kind == "u32":
                read_pat = r"%s\s*=\s*p\.length\s*>=\s*%d\s*\?\s*p\.readUInt32LE\(%d\)\s*:\s*0" % (
                    re.escape(name), guard, off)
            else:
                read_pat = r"%s\s*=\s*p\.length\s*>=\s*%d\s*\?\s*readU64\(p,\s*%d\)\s*:\s*0" % (
                    re.escape(name), guard, off)
            if not re.search(read_pat, body):
                fails.append("proto-decode.ts: decodeStats %s read@%d with guard >=%d missing/mismatched"
                             % (name, off, guard))
        # exactly five tiered guards, one per trailing field
        guards = re.findall(r"p\.length\s*>=\s*(\d+)", body)
        if guards != [str(f[3]) for f in FIELDS]:
            fails.append("proto-decode.ts: decodeStats tiered guards %r, expected %r"
                         % (guards, [str(f[3]) for f in FIELDS]))
        # 1280B old reply must be safe: no unguarded read of any trailing field
        for name, off, _kind, _guard in FIELDS:
            for bad in re.findall(r"%s\s*=\s*p\.(?:readUInt32LE|readBigUInt64LE)\(%d\)" % (re.escape(name), off), body):
                fails.append("proto-decode.ts: decodeStats %s has UNGUARDED read@%d (1280B RangeError)" % (name, off))

    # --- 3) types.ts RbStats interface: five fields typed number ---
    mi = re.search(r"export interface RbStats\s*\{(.*?)\n\}", types, re.S)
    if not mi:
        fails.append("types.ts: RbStats interface not found")
    else:
        ibody = mi.group(1)
        for name in NAMES:
            if not re.search(r"\b%s\s*:\s*number\b" % re.escape(name), ibody):
                fails.append("types.ts: RbStats.%s missing (or not typed number)" % name)

    # --- 4) http-transport.ts statsSchema: five fields with .default(0) ---
    ms = re.search(r"const statsSchema\s*=\s*z\.looseObject\(\{(.*?)\}\);", http, re.S)
    if not ms:
        fails.append("http-transport.ts: statsSchema not found")
    else:
        sbody = ms.group(1)
        for name in NAMES:
            if not re.search(r"%s\s*:\s*z\.number\(\)\.default\(0\)" % re.escape(name), sbody):
                fails.append("http-transport.ts: statsSchema.%s missing (or no .default(0))" % name)

    # --- 5) ramboostctl.c statval: five CLI keys ---
    for key in STATVAL_KEYS:
        if not re.search(r'strcmp\(argv\[2\],\s*"%s"\)' % re.escape(key), ctl):
            fails.append("ramboostctl.c: statval key %s missing" % key)

    # --- 6) stats-history.test.ts fixture: five fields ---
    for name in NAMES:
        if name not in test:
            fails.append("stats-history.test.ts: fixture field %s missing" % name)

    # --- 7) CI must keep calling Gate8 ---
    if "check_stats_contract.py" not in ci:
        fails.append("ci.yml: Gate8 check_stats_contract.py not registered")

    # --- 8) GET_STATS capacity contract (298, 0x7F stack fix) ---
    #
    # 为什么必须有一条钉这里：0x7F 修复把 RbMsgGetStats 的
    # "1304 B 栈上 local + 按容量截断回拷" 改成了 "直填 SystemBuffer"。
    # 语义等价的前提是**容量必然足够**，而这个前提由分发器的门提供。
    # 本仓此前对这条语义**零门禁**（红队 B B8 实测确认），所以任何一处
    # 门被放宽/删除、或直填前少一道 avail 检查，改坏之后全部门禁仍全绿，
    # 而后果是"Status=SUCCESS + 统计全零"的假成功 —— 运维会把全零误判成
    # "缓存没生效"（本仓已被同型误诊咬过一次，见 rbctl.c 的 [147/N-147-5]）。
    #
    # 四条断言，缺一即 FAIL：
    #   8a 分发器有 outLen < sizeof(RB_MSG) 门（8224，实际兜底的那道）
    #   8b RbCtlMinBuffer 对 GET_STATS 返回 offsetof(Data)+sizeof(RB_STATS)
    #   8c RbMsgGetStats 在不填时给 STATUS_BUFFER_TOO_SMALL（不得静默全零）
    #   8d 用户态消费方按定长 RB_MSG 请求并读满 RB_STATS（消费方，非声明方）
    body_gs = None
    m_gs = re.search(r"RbMsgGetStats\s*\([^)]*\)\s*\{(.*?)\n\}", rbctl, re.S)
    if not m_gs:
        fails.append("rbctl.c: RbMsgGetStats body not found")
    else:
        body_gs = m_gs.group(1)

    # 8a: 分发器 8224 门 —— 必须在 RbDispatchDeviceControl 体内，且不是注释。
    #
    # 第一次实现只对全文搜 `outLen < sizeof(RB_MSG)`，M1 变异（把该门改成
    # `outLen < 0`）**survived** —— 因为注释里也写着这串字（rbctl.c:246 的
    # 说明注释），正则照样命中。这正是"钉声明方而不是消费方"的老毛病：
    # 判据必须锚定到执行体的那一行，并且剥掉注释再匹配。
    body_disp = None
    m_disp = re.search(r"RbDispatchDeviceControl\s*\([^)]*\)\s*\{(.*?)\n\}",
                       rbctl, re.S)
    if not m_disp:
        fails.append("rbctl.c: RbDispatchDeviceControl body not found")
    else:
        # 剥 // 行注释与 /* */ 块注释，避免注释里的字面量冒充判据
        body_disp = re.sub(r"/\*.*?\*/", "", m_disp.group(1), flags=re.S)
        body_disp = re.sub(r"//[^\n]*", "", body_disp)
        if not re.search(r"outLen\s*<\s*sizeof\s*\(\s*RB_MSG\s*\)", body_disp):
            fails.append("rbctl.c: dispatch gate `outLen < sizeof(RB_MSG)` missing "
                         "from RbDispatchDeviceControl body (comments do not count) "
                         "-- GET_STATS capacity is no longer guaranteed")

    # 8b: RbCtlMinBuffer 的 GET_STATS case
    m_mb = re.search(r"RbCtlMinBuffer\s*\([^)]*\)\s*\{(.*?)\n\}", rbctl, re.S)
    if not m_mb:
        fails.append("rbctl.c: RbCtlMinBuffer body not found")
    elif not re.search(
            r"RB_MSG_GET_STATS\s*:\s*return\s+offsetof\s*\(\s*RB_MSG\s*,\s*Data\s*\)"
            r"\s*\+\s*sizeof\s*\(\s*RB_STATS\s*\)", m_mb.group(1)):
        fails.append("rbctl.c: RbCtlMinBuffer GET_STATS must return "
                     "offsetof(RB_MSG,Data)+sizeof(RB_STATS)")

    # 8c: 不填时必须显式报错（红队 B B9-建议1 的承重行）
    if body_gs is not None:
        # `\(?ULONG\)?` 里 ULONG 不是可选的 —— 第一次写成那样，把
        # `avail == sizeof(...)`（无强转）判成不匹配，门禁自己报了个假 FAIL。
        if not re.search(r"if\s*\(\s*avail\s*==\s*(?:\(\s*ULONG\s*\)\s*)?"
                         r"sizeof\s*\(\s*RB_STATS\s*\)\s*\)",
                         body_gs):
            fails.append("rbctl.c: RbMsgGetStats must test avail == sizeof(RB_STATS) "
                         "before filling")
        # [298b] 必须剥注释后再判 —— 红队 B D2 实测：删掉 :270 的真实赋值、
        # 只在注释里留 "STATUS_BUFFER_TOO_SMALL" 字样，含注释的子串判断仍 PASS。
        # 8a 上一版踩过同一坑（注释里的 `outLen < sizeof(RB_MSG)` 让变异 survived），
        # 8c 漏了同样的处理。
        body_gs_code = re.sub(r"/\*.*?\*/", "", body_gs, flags=re.S)
        body_gs_code = re.sub(r"//[^\n]*", "", body_gs_code)
        if not re.search(r"\*\s*pst\s*=\s*STATUS_BUFFER_TOO_SMALL", body_gs_code):
            fails.append("rbctl.c: RbMsgGetStats must ASSIGN "
                         "*pst = STATUS_BUFFER_TOO_SMALL when it cannot fill "
                         "(mentions in comments do not count; silent zero stats "
                         "== fake success)")

    # 8d: 消费方按定长请求（不是变长探测）
    #
    # [298b] 第一版写成全文搜 `sizeof(RB_MSG)` —— 红队 B 做了端到端证明：
    # 该串在 ctl_conn.c 里出现 14 次（:81 的 memcpy、:115/:129/... 十处
    # msg.Size 赋值都能满足），把 :88 的**输出长度**改成 1320 甚至 0，
    # 门禁照样 PASS。它宣称守护的正是本次的容量契约，却是个空网。
    # 现在锚定 DeviceIoControl 的第 5 个实参（输出缓冲区大小）：
    #     ok = DeviceIoControl(g_dev, IOCTL_RB_REQUEST,
    #                          reply, sizeof(RB_MSG),
    #                          reply, sizeof(RB_MSG),   <- 这一行
    #                          &ret, NULL);
    # [298d] 不要用"第 N 个实参"来描述位置 —— 0-based 的 parts[5] 是 1-based
    # 的第 6 个，两种数法差 1（QA 实测指出）。照旧注释"修正"成 parts[4] 的人
    # 会让判据去盯 lpOutBuffer 指针，fullmatch 必然 FAIL，然后他会以为判据坏了。
    # 只按参数名定位：parts[5] == nOutBufferSize。
    # [298e] 必须遍历**全部**调用，不能只查第一个。红队实测：在 ctl_conn.c 末尾
    # 追加第二个 DeviceIoControl（输出长度 1320，真正的短缓冲）后，re.search
    # 版本仍然 PASS —— 只查第一个匹配等于给后来者留了后门，而且是最容易被
    # 用上的后门（新增第二个发送函数是极自然的演进）。
    # 这与此前"空匹配集判 PASS"同族：判据的"通过"必须覆盖被测量的全集。
    dio_calls = list(
        re.finditer(r"DeviceIoControl\s*\((?P<args>[^;]*?)\)\s*;", conn, re.S))
    if not dio_calls:
        fails.append("ctl_conn.c: no DeviceIoControl call found -- empty match "
                     "set must never pass")
    for m_dio in dio_calls:
        args = m_dio.group('args')
        # 按逗号切分（本调用无嵌套括号内的逗号，已实测）
        parts = [a.strip() for a in args.split(',')]
        if len(parts) >= 8:
            # parts: 0=hDevice 1=dwIoControlCode 2=lpInBuffer
            #        3=nInBufferSize 4=lpOutBuffer 5=nOutBufferSize
            #        6=lpBytesReturned 7=lpOverlapped
            # 取 nOutBufferSize（输出缓冲区大小），不是 lpOutBuffer（指针）。
            outbuf_size = parts[5]          # == nOutBufferSize
            dio_ok = bool(re.fullmatch(r"sizeof\s*\(\s*RB_MSG\s*\)", outbuf_size))
            if not dio_ok:
                fails.append("ctl_conn.c: DeviceIoControl output buffer size is %r, "
                             "must be sizeof(RB_MSG) -- short request breaks the "
                             "GET_STATS capacity contract" % outbuf_size)
        else:
            fails.append("ctl_conn.c: DeviceIoControl at offset %d has only %d args "
                         "(need 8) -- cannot verify the capacity contract"
                         % (m_dio.start(), len(parts)))
    # [298f] 这里曾有 `if not m_dio: fails.append("... not found")`，已删除。
    # 它有两个问题，第二个是致命的：
    #   ① 语义重复 —— 上面 `if not dio_calls` 已经把"找不到调用"说完了，
    #      而且说得更准确（那时才知道是空集，不是"循环跑完没命中"）。
    #   ② 空匹配集时直接崩 —— `for m_dio in dio_calls` 一次都不执行时，
    #      `m_dio` 从未被绑定，`if not m_dio` 抛
    #          UnboundLocalError (a NameError subclass): cannot access local
    #          variable 'm_dio' where it is not associated with a value
    #      QA 实测（进程内把 CTL_CONN 换成不含 DeviceIoControl 的源）：
    #        A) 保留本行的 as-built  ->  UnboundLocalError traceback
    #        B) 删掉本行，同输入     ->  rc=1，FAIL 行含
    #                                   'empty match set must never pass'
    #      两者 rc 都是非 0，**只看 rc 完全无法区分**，唯一差别是有没有那行文案。
    #      这正是本仓反复踩的"判据崩溃被误读成判据坏了"：最该干净报 FAIL 的
    #      时刻（有人重构了 IOCTL 发送点）反而拿到 traceback，排查方向跑偏。
    # [纪律] 循环变量不得在循环外引用；"空匹配集"必须由**判空前**的那一句
    # 负责，不能靠循环残余变量兜底。同族见 deploy S5 的 allfails / Gate28 的
    # 自比自 —— 都是"先 append 诊断、再写兜底分支"这个结构里把局部名当函数名用。

    print("== check_stats_contract (Gate8) ==")
    print("  RB_STATS trailing : 1280/1284/1288/1292(u32) 1296(u64), sizeof=%d" % SIZEOF)
    print("  decodeStats guards: %s" % [f[3] for f in FIELDS])
    print("  copies guarded    : types.ts / statsSchema / statval / stats-history.test.ts")
    print("  GET_STATS capacity: dispatch 8224 gate + min 1320 + explicit STATUS_BUFFER_TOO_SMALL")
    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d)" % len(fails))
        return 1
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
