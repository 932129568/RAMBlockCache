#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A4 不变量门禁：RbRegSetClean(1) 唯一调用点 + 卷数门下沉校验。

被钉死的不变量（见 rbdriver.c RbRegSetClean 与 RbBootFailLoad 的 [A4] 注释）：
  1. 写 LastCleanShutdown=1 的 RbRegSetClean(1) 全仓库只允许 1 个调用点；
  2. 「本引导曾挂过卷」的卷数门已下沉进 RbRegSetClean(v) 内部：
     v 为真时才计数，计数 0 且从未挂过卷 -> 写 LastCleanWithheld=1 并 return
     （不落 LastCleanShutdown）。
  因此调用点侧只需一条前置判据（引擎无脏数据），门本身由定义体保证。
  3. [269 A4] RbRegSetClean 体内不得出现 BootFailCount（读或写都不该有）。
  4. [269 A4 读侧] 开机清零必须构成**完整绑定链**，且不得单票：
     a) 持久化点 RbRegSetDW(L"BootFailCount", <var>)，且 <var> 由
        RbRegGetDW(L"BootFailCount") 初始化 —— 拦住「把第二个实参改成常量 0」
        （红队 B1：一个 token 让计数永不落盘，其它判据全绿）；
     b) 清零写点必须是同一个 <var>（排除 `ULONG fails = 0;` 影子声明，红队 B4）；
     c) 清零条件里 clean 不得单独构成一个顶层 `||` 操作数（拦住
        `healthy==1 || ticket==1 || clean==1`，红队 B2），且 clean+ticket 必须都出现；
     d) 清零不得落在 else 分支（条件成立反而不清零）。
  5. [269 A4] L"LastCleanShutdown" 的写点白名单只有两处：RbRegSetClean 体内、
     RbBootFailLoad 的开机复位。其它任何出现（含 RbRegSetValue）⇒ FAIL，
     否则「直接写注册表」可以绕过卷数门与唯一调用点（红队 B6）。
  6. [A4 卷数门完整性] RbBootAttachedAny() 必须是读静态标志（含
     InterlockedCompareExchange，且不得 `return TRUE;`）；RbCountAttachedVolumes()
     体内必须引用 IsAttached 与 Removed —— 否则把门实现改一行就能恒过（红队 B7）。
  7. [A4-d 2026-11-17] 被扣必须让调用方看得见，且扣留结论不许被 ALREADY 洗成干净：
     a) RbRegSetClean 返回 BOOLEAN，体内不得有裸 `return;`（MSVC 只给 C4033，不挡编译）；
        扣留分支必须 `return FALSE;`，写点之后必须 `return TRUE;`；
     b) 唯一调用点必须是 `if (!RbRegSetClean(1))` 形态，块内返回 WHY_WITHHELD，
        且把闩置 DONE（否则闩永久 BUSY，RbShutdownPersistInFlight 恒真）；
     c) 每个 RB_PERSIST_WHY_*（除 _NONE）都要有 case，非 CLEAN 码不得映射到
        RB_SKIP_NONE；why 值与 skip 值都不得重复；
     d) ALREADY 出口之前必须在认领块内用 **Rb2GetDW(L"Parameters", …)** 读
        LastCleanWithheld（RbRegGetDW 是 rbdriver.c 的 static，写它会编译不过）；
     e) 承载 (long)why 的那次 RbLog 必须有 withheld 支（否则 level 与文案都说谎）。

     **已知不覆盖（显式声明，别把全绿当证明）**：把判定搬进辅助函数/宏、
     字符串拼接键名、三目清零、取反 `!(...)`、间接注册表写。这些要覆盖必须做
     表达式求值与跨函数数据流，误杀风险大于收益（G1 教训：不做精确形状匹配）。
     7d 的 I8 是**文本序**判据（放在 `if (0) { … }` 里也能满足）；7c 的 I7/I10 只查
     `#define` 字面值，值由宏表达式生成时覆盖不到。I6 钉的是**形态**：写成
     `BOOLEAN wrote = RbRegSetClean(1); if (!wrote)` 会误报（本仓不采用该写法）。

为什么必须剥注释再匹配：rbdriver.c 的 [A4] 注释里多次出现 "RbRegSetClean(1)"
字面量，不剥离注释会把 1 个真调用点算成多个。块注释替换为等长空白（不删行），
file:line 才与原文一致。

fail-closed：空匹配集 = FAIL（字面量被改写 / 枚举漂移时不得判 PASS），
与 check_repo.py 的 "rule silently dead" 自检同原则。

退出码 0=PASS / 1=FAIL。
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]   # src/scripts/ -> 仓库根
DRV = ROOT / "src" / "driver"

CALL = re.compile(r"\bRbRegSetClean\s*\(\s*1\s*\)")
DEF = re.compile(r"\b(?:VOID|BOOLEAN)\s+RbRegSetClean\s*\(\s*ULONG\s+(\w+)\s*\)\s*\{")
DECL = re.compile(r"^\s*(?:VOID|void|BOOLEAN)\s+RbRegSetClean\s*\(")   # 定义/声明行豁免
BOOT = re.compile(r"\bVOID\s+RbBootFailLoad\s*\(\s*VOID\s*\)\s*\{")
NEAR = 8        # 调用点上方 N 行内必须能看到前置判据
MIN_TU = 20     # src/driver/*.c 实测 23；低于此值判枚举漂移


def strip_comments_keep_lines(text):
    """块注释 -> 等长空白（保留换行），行注释 -> 删除；行号与原文对应。"""
    text = re.sub(r"/\*.*?\*/",
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)),
                  text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def driver_sources():
    if not DRV.is_dir():
        return []
    return sorted(list(DRV.glob("*.c")) + list(DRV.glob("*.h")))


def body_after(code, m):
    """按花括号配平截取函数体，返回 (body, body_start_line)。"""
    i = code.find("{", m.start())
    if i < 0:
        return None, None
    depth = 0
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return code[i:j + 1], code.count("\n", 0, i) + 1
    return None, None


def body_span(code, m):
    """返回函数体的 [start, end) 下标（与 body_after 同一花括号配平逻辑）。"""
    i = code.find("{", m.start())
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return (i, j + 1)
    return None


def main():
    fails = []
    srcs = driver_sources()
    n_c = len([p for p in srcs if p.suffix == ".c"])
    if n_c < MIN_TU:
        fails.append("src/driver/*.c 只枚举到 %d 个 (< %d) -- 枚举漂移，规则不得判 PASS"
                     % (n_c, MIN_TU))

    hits, code_of = [], {}
    for p in srcs:
        code = strip_comments_keep_lines(p.read_text(encoding="utf-8", errors="replace"))
        code_of[p] = code
        lines = code.splitlines()
        for m in CALL.finditer(code):
            ln = code.count("\n", 0, m.start()) + 1
            if ln - 1 < len(lines) and DECL.match(lines[ln - 1]):
                continue                      # 排除定义/声明
            hits.append((p, ln, lines[ln - 1].strip() if ln - 1 < len(lines) else ""))

    # ---- 1) 调用点必须恰好 1 个（空集 = FAIL） ----
    if not hits:
        fails.append("RbRegSetClean(1) 命中 0 处 -- 空匹配集按 FAIL 处理"
                     "（字面量被改写或扫描范围漂移，不得判 PASS）")
    elif len(hits) > 1:
        fails.append("RbRegSetClean(1) 出现 %d 次，只允许 1 次: %s"
                     % (len(hits), ", ".join(
                         "%s:%d" % (p.relative_to(ROOT).as_posix(), ln)
                         for p, ln, _ in hits)))

    if len(hits) == 1:
        p, ln, _ = hits[0]
        rel = p.relative_to(ROOT).as_posix()

        # ---- 2a) 调用点身份：唯一合法位置是关机电源路径 rbmsg.c ----
        if rel != "src/driver/rbmsg.c":
            fails.append("RbRegSetClean(1) 应只在 src/driver/rbmsg.c（关机电源路径），"
                         "实测 %s:%d" % (rel, ln))

        # ---- 2b) 调用点上方 NEAR 行内必须有前置判据（引擎无脏数据） ----
        lines = code_of[p].splitlines()
        near = "\n".join(lines[max(0, ln - 1 - NEAR):ln])
        if not re.search(r"rb_engine_has_dirty\s*\(", near):
            fails.append("%s:%d 上方 %d 行内没有 rb_engine_has_dirty() 前置判据"
                         "（clean 必须在刷盘完成后写）" % (rel, ln, NEAR))

        # ---- 2c) 门本身：校验 RbRegSetClean 定义体 ----
        defs = [(f, m) for f, c in code_of.items() for m in DEF.finditer(c)]
        if len(defs) != 1:
            fails.append("RbRegSetClean 定义体定位到 %d 处（期望 1）-- 定义签名被改写"
                         % len(defs))
        else:
            fp, m = defs[0]
            frel = fp.relative_to(ROOT).as_posix()
            def_ln = code_of[fp].count("\n", 0, m.start()) + 1
            body, _ = body_after(code_of[fp], m)
            if body is None:
                fails.append("%s: 无法从 :%d 截取 RbRegSetClean 函数体" % (frel, def_ln))
            else:
                if not re.search(r"KeGetCurrentIrql\(\)\s*!=\s*PASSIVE_LEVEL", body):
                    fails.append("%s:%d RbRegSetClean 缺 PASSIVE 守卫"
                                 "（注册表写只允许 PASSIVE）" % (frel, def_ln))
                p_v = re.search(r"if\s*\(\s*v\s*\)", body)
                m_asg = re.search(r"(\w+)\s*=\s*RbCountAttachedVolumes\s*\(\s*\)", body)
                p_any = body.find("RbBootAttachedAny(")
                p_reg = body.find('L"LastCleanShutdown"')
                if not p_v:
                    fails.append("%s:%d 没有 `if (v)` 分支 -- 卷数门必须只对写 1 生效"
                                 % (frel, def_ln))
                if not m_asg:
                    fails.append("%s:%d 未用 `X = RbCountAttachedVolumes()` -- 卷数门被摘掉"
                                 % (frel, def_ln))
                if p_any < 0:
                    fails.append("%s:%d 未调用 RbBootAttachedAny() -- 曾挂卷判据缺失"
                                 % (frel, def_ln))
                if m_asg:
                    var = m_asg.group(1)
                    if not (re.search(rf"\b{var}\s*==\s*0\s*&&\s*!\s*RbBootAttachedAny\s*\(\s*\)", body)
                            or re.search(rf"!\s*RbBootAttachedAny\s*\(\s*\)\s*&&\s*{var}\s*==\s*0", body)):
                        fails.append("%s:%d 门判据不是 `%s == 0 && !RbBootAttachedAny()`"
                                     " -- 语义被改写" % (frel, def_ln, var))
                    if p_v and m_asg.start() < p_v.end():
                        fails.append("%s:%d 计数在 `if (v)` 之前 -- v==0 时也计数"
                                     % (frel, def_ln))
                if m_asg and p_any >= 0 and p_any < m_asg.start():
                    fails.append("%s:%d RbBootAttachedAny() 先于计数 -- 判据顺序被调换"
                                 % (frel, def_ln))
                if p_reg < 0:
                    fails.append('%s:%d 找不到 L"LastCleanShutdown" 写点' % (frel, def_ln))
                elif p_any >= 0 and p_reg < p_any:
                    fails.append('%s:%d L"LastCleanShutdown" 写在门之前 -- 门形同虚设'
                                 % (frel, def_ln))
                elif p_v and not re.search(r"\breturn\s+FALSE\s*;", body[p_v.end():p_reg]):
                    fails.append("%s:%d 拒写分支不是 `return FALSE;` -- 门未过仍会落到"
                                 " LastCleanShutdown，或调用方无法区分被扣"
                                 % (frel, def_ln))
                # ---- 3) I1：269 原缺陷不得回潮 ----
                if "BootFailCount" in body:
                    fails.append('%s:%d RbRegSetClean 体内出现 BootFailCount（读或写）-- '
                                 '269 A4 原缺陷回潮，必须人工复核' % (frel, def_ln))

    # ---- 4) I2：清零绑定链（读 -> 清零 -> 落盘 -> 消费） + 不得单票 ----
    boots = [(p, m) for p, c in code_of.items() for m in BOOT.finditer(c)]
    if len(boots) != 1:
        fails.append("RbBootFailLoad 定义定位到 %d 处（期望 1）-- 判据空转，不得判 PASS"
                     % len(boots))
    else:
        bp, bm = boots[0]
        brel = bp.relative_to(ROOT).as_posix()
        bbody, bstart = body_after(code_of[bp], bm)
        if bbody is None:
            fails.append("%s: 无法从 :%d 截取 RbBootFailLoad 函数体" % (brel, bstart or 0))
        else:
            mw = re.search(r'RbRegSetDW\s*\(\s*L"BootFailCount"\s*,\s*([A-Za-z_]\w*)\s*\)',
                           bbody)
            if not mw:
                fails.append('%s: 找不到 RbRegSetDW(L"BootFailCount", <变量>) -- '
                             '持久化点缺失或第二个实参被改成常量（红队 B1）' % brel)
            else:
                var = mw.group(1)
                if not re.search(r'\b%s\s*=\s*RbRegGetDW\s*\(\s*L"BootFailCount"\s*,'
                                 % var, bbody):
                    fails.append('%s: %s 不是由 RbRegGetDW(L"BootFailCount") 初始化 -- '
                                 '绑定链断裂' % (brel, var))
                if not re.search(r'g_Rb\.BootFailCount\s*=\s*%s\s*;' % var, bbody):
                    fails.append("%s: 没有把 %s 写回 g_Rb.BootFailCount -- 落盘链断裂"
                                 % (brel, var))
                if not re.search(r'BootLoopGuard\s*=\s*\(\s*%s\s*>=\s*2\s*\)' % var, bbody):
                    fails.append("%s: BootLoopGuard 不是由 (%s >= 2) 产生 -- 消费链断裂"
                                 % (brel, var))
                # clean / ticket 的变量名同样从注册表读点反推，而不是硬编码：
                # 这样「把 ticket 赋成常量 1」会让绑定消失（红队 B3），而一致的重命名不会误杀。
                mc = re.search(r'([A-Za-z_]\w*)\s*=\s*RbRegGetDW\s*\(\s*L"LastCleanShutdown"',
                               bbody)
                mt = re.search(r'([A-Za-z_]\w*)\s*=\s*RbRegGetDW\s*\(\s*L"BootReachedUser"',
                               bbody)
                if not mc:
                    fails.append('%s: 找不到 XX = RbRegGetDW(L"LastCleanShutdown") -- '
                                 'clean 绑定缺失' % brel)
                if not mt:
                    fails.append('%s: 找不到 XX = RbRegGetDW(L"BootReachedUser") -- '
                                 'ticket 绑定缺失（被赋成常量即命中此条，红队 B3）' % brel)
                cleanvar = mc.group(1) if mc else None
                ticketvar = mt.group(1) if mt else None
                zpat = re.compile(r'(?<![A-Za-z0-9_>.])%s\s*=\s*0(?:UL|u|U|L)*\s*;' % var)
                zs = list(zpat.finditer(bbody))
                if len(zs) != 1:
                    fails.append("%s: `%s = 0;` 命中 %d 处（期望 1）-- 空匹配集/多写点都按 FAIL"
                                 % (brel, var, len(zs)))
                else:
                    zstart = zs[0].start()
                    lstart = bbody.rfind("\n", 0, zstart) + 1
                    prefix = bbody[lstart:zstart]
                    if re.match(r"\s*(?:ULONG|LONG|BOOLEAN|int|unsigned|UINT32|ULONG64|LONG64)\b",
                                prefix):
                        fails.append("%s: `%s = 0;` 是**声明**不是清零写点 -- "
                                     "判据被影子变量绕过（红队 B4）" % (brel, var))
                    else:
                        head = bbody[:zstart]
                        conds = list(re.finditer(r"\bif\s*\(([^;{}]*?)\)\s*\{", head))
                        if not conds:
                            fails.append("%s: 找不到清零所属 if 条件 -- 不得判 PASS" % brel)
                        else:
                            cm = conds[-1]
                            cond = cm.group(1)
                            if re.search(r"\belse\b", head[cm.end():]):
                                fails.append("%s: 清零落在 else 分支 -- 语义反转" % brel)
                            has_clean = (cleanvar is not None
                                         and re.search(r"\b%s\b" % cleanvar, cond) is not None)
                            has_ticket = (ticketvar is not None
                                          and re.search(r"\b%s\b" % ticketvar, cond) is not None)
                            if not (has_clean and has_ticket):
                                miss = [n for n, ok in (("clean", has_clean),
                                                        ("ticket", has_ticket)) if not ok]
                                fails.append("%s: 清零条件缺 %s -- 单票可清零（条件 %r）"
                                             % (brel, " / ".join(miss), cond.strip()[:80]))
                            # 顶层 `||` 的每个操作数都不得「只认 clean」
                            depth = 0
                            parts, cur, i = [], "", 0
                            while i < len(cond):
                                ch = cond[i]
                                if ch == "(":
                                    depth += 1
                                elif ch == ")":
                                    depth -= 1
                                if depth == 0 and cond.startswith("||", i):
                                    parts.append(cur); cur = ""; i += 2; continue
                                cur += ch; i += 1
                            parts.append(cur)
                            for part in parts:
                                if (cleanvar is not None
                                        and re.search(r"\b%s\b" % cleanvar, part)
                                        and not (ticketvar is not None
                                                 and re.search(r"\b%s\b" % ticketvar, part))):
                                    fails.append("%s: 顶层 `||` 操作数只认 clean（%r）-- "
                                                 "单票可清零（红队 B2）" % (brel, part.strip()[:60]))

    # ---- 5) I3：LastCleanShutdown 写点白名单（防「直接写注册表」绕过） ----
    # 白名单带「实参约束」：RbRegSetClean 只许写参数 v；RbBootFailLoad 只许写字面量 0。
    allowed = []          # (file, start, end, kind, param)
    for fp, c in code_of.items():
        for mm in DEF.finditer(c):
            sp = body_span(c, mm)
            if sp:
                allowed.append((fp, sp[0], sp[1], "clean_def", mm.group(1)))
        for mm in BOOT.finditer(c):
            sp = body_span(c, mm)
            if sp:
                allowed.append((fp, sp[0], sp[1], "boot", None))
    WR1 = re.compile(r'RbRegSetDW\s*\(\s*L"LastCleanShutdown"\s*,\s*([^)]*?)\s*\)')
    WR2 = re.compile(r'RbRegSetValue\s*\([^;]*?L"LastCleanShutdown"', re.S)
    for fp, c in code_of.items():
        for mm in WR1.finditer(c):
            rel2 = fp.relative_to(ROOT).as_posix()
            ln2 = c.count("\n", 0, mm.start()) + 1
            owner = [x for x in allowed if x[0] == fp and x[1] <= mm.start() < x[2]]
            if not owner:
                fails.append('%s:%d L"LastCleanShutdown" 的写点不在白名单函数内 -- '
                             '可绕过卷数门与唯一调用点（红队 B6）' % (rel2, ln2))
            else:
                kind, param = owner[0][3], owner[0][4]
                arg = mm.group(1).strip()
                if kind == "clean_def" and arg != param:
                    fails.append('%s:%d RbRegSetClean 写 clean 的实参是 %r（期望参数 %r）-- '
                                 '门可能被绕过' % (rel2, ln2, arg, param))
                if kind == "boot" and arg != "0":
                    fails.append('%s:%d RbBootFailLoad 里写 clean 的实参是 %r（只许 0）-- '
                                 '开机复位被改成伪造 clean（红队 B6）' % (rel2, ln2, arg))
        for mm in WR2.finditer(c):
            rel2 = fp.relative_to(ROOT).as_posix()
            ln2 = c.count("\n", 0, mm.start()) + 1
            owner = [x for x in allowed if x[0] == fp and x[1] <= mm.start() < x[2]]
            if not owner:
                fails.append('%s:%d RbRegSetValue 写 L"LastCleanShutdown" 不在白名单内 -- '
                             '可绕过卷数门（红队 B6）' % (rel2, ln2))

    # ---- 6) I4：卷数门实现不得被改成恒真 ----
    ANYF = re.compile(r"\bBOOLEAN\s+RbBootAttachedAny\s*\(\s*VOID\s*\)\s*\{")
    CNTF = re.compile(r"\bULONG\s+RbCountAttachedVolumes\s*\(\s*VOID\s*\)\s*\{")
    anyf = [(p, m) for p, c in code_of.items() for m in ANYF.finditer(c)]
    cntf = [(p, m) for p, c in code_of.items() for m in CNTF.finditer(c)]
    if len(anyf) != 1:
        fails.append("RbBootAttachedAny 定义定位到 %d 处（期望 1）-- 判据空转" % len(anyf))
    else:
        p, m = anyf[0]
        bd, _ = body_after(code_of[p], m)
        rel = p.relative_to(ROOT).as_posix()
        if bd is None:
            fails.append("%s: 无法截取 RbBootAttachedAny 函数体" % rel)
        else:
            if "InterlockedCompareExchange" not in bd:
                fails.append("%s: RbBootAttachedAny 不再读静态标志（缺 "
                             "InterlockedCompareExchange）" % rel)
            if re.search(r"\breturn\s+(TRUE|1)\s*;", bd):
                fails.append("%s: RbBootAttachedAny 变成恒真（return TRUE）-- "
                             "卷数门恒过（红队 B7）" % rel)
    if len(cntf) != 1:
        fails.append("RbCountAttachedVolumes 定义定位到 %d 处（期望 1）-- 判据空转" % len(cntf))
    else:
        p, m = cntf[0]
        bd, _ = body_after(code_of[p], m)
        rel = p.relative_to(ROOT).as_posix()
        if bd is None:
            fails.append("%s: 无法截取 RbCountAttachedVolumes 函数体" % rel)
        else:
            for need in ("IsAttached", "Removed"):
                if need not in bd:
                    fails.append("%s: RbCountAttachedVolumes 体内不见 %s -- "
                                 "计数判据被改写" % (rel, need))


    # ================= [A4-d 2026-11-17] I5-I10 =================
    why_cases = {}

    # ---- 7) I5：返回类型 + 三态返回值 ----
    defs2 = [(pp, mm) for pp, cc in code_of.items() for mm in DEF.finditer(cc)]
    if len(defs2) != 1:
        fails.append("I5: RbRegSetClean 定义定位到 %d 处（期望 1）-- 判据空转，不得判 PASS"
                     % len(defs2))
    else:
        fp, mm = defs2[0]
        r5 = fp.relative_to(ROOT).as_posix()
        srcc = code_of[fp]
        dline = srcc.splitlines()[srcc.count("\n", 0, mm.start())]
        bd, _ = body_after(srcc, mm)
        if bd is None:
            fails.append("%s: I5 无法截取 RbRegSetClean 函数体" % r5)
        else:
            if not re.match(r"\s*BOOLEAN\b", dline):
                fails.append("%s: RbRegSetClean 返回类型不是 BOOLEAN -- 调用方看不见被扣（A4-d）"
                             % r5)
            if re.search(r"\breturn\s*;", bd):
                fails.append("%s: RbRegSetClean 体内有裸 `return;` -- BOOLEAN 返回值未定义"
                             "（MSVC 仅 C4033，门禁是唯一防线）" % r5)
            pv5 = re.search(r"if\s*\(\s*v\s*\)", bd)
            pr5 = bd.find('L"LastCleanShutdown"')
            if not pv5 or pr5 < 0:
                fails.append("%s: I5 找不到 `if (v)` 或 LastCleanShutdown 写点 -- 判据空转" % r5)
            else:
                if not re.search(r"\breturn\s+FALSE\s*;", bd[pv5.end():pr5]):
                    fails.append("%s: 扣留分支缺 `return FALSE;`（`return;` 不行）" % r5)
                if not re.search(r"\breturn\s+TRUE\s*;", bd[pr5:]):
                    fails.append("%s: 写点之后缺 `return TRUE;`" % r5)

    # ---- 8) I6：调用点必须消费返回值（块内判定） ----
    CONS = re.compile(r"if\s*\(\s*(?:!\s*RbRegSetClean\s*\(\s*1\s*\)"
                      r"|RbRegSetClean\s*\(\s*1\s*\)\s*==\s*FALSE)\s*\)")
    if len(hits) != 1:
        fails.append("I6: RbRegSetClean(1) 调用点 %d 处（期望 1）-- 消费形态判据空转"
                     % len(hits))
    else:
        p6, ln6, _ = hits[0]
        r6 = p6.relative_to(ROOT).as_posix()
        s6 = code_of[p6]
        off6 = sum(len(x) + 1 for x in s6.splitlines()[:ln6 - 1])
        mi6 = s6.find("if", off6)
        mc6 = CONS.match(s6, mi6) if mi6 >= 0 else None
        if not mc6:
            fails.append("%s:%d RbRegSetClean(1) 不是 `if (!...)` / `== FALSE` 形态 -- "
                         "返回值被忽略，A4-d 原样回潮" % (r6, ln6))
        else:
            blk6, _ = body_after(s6, mc6)
            if not blk6:
                fails.append("%s:%d I6 无法截取 if 块（缺花括号？）" % (r6, ln6))
            else:
                if not re.search(r"return\s+RB_PERSIST_WHY_WITHHELD\s*;", blk6):
                    fails.append("%s:%d 扣留分支没返回 RB_PERSIST_WHY_WITHHELD（块内，"
                                 "不看整个函数）" % (r6, ln6))
                if not re.search(r"InterlockedExchange\s*\(\s*&s_PersistDone\s*,\s*"
                                 r"RB_PERSIST_DONE\s*\)", blk6):
                    fails.append("%s:%d 扣留出口没把闩置 DONE -- 闩永久 BUSY，"
                                 "RbShutdownPersistInFlight 恒真" % (r6, ln6))

    # ---- 9) I7：why 码全覆盖 + case 不得返回 NONE + 值互异 ----
    MAC = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(RB_PERSIST_WHY_\w+)[ \t]+(\w+)", re.M)
    SKF = re.compile(r"\bRbPersistSkipReasonOf\s*\(\s*ULONG\s+\w+\s*\)\s*\{")
    macros = []
    for pp, cc in code_of.items():
        for mm in MAC.finditer(cc):
            tok = re.sub(r"[uUlL]+$", "", mm.group(2))
            try:
                val = int(tok, 0)
            except ValueError:
                val = None
            macros.append((mm.group(1), val))
    skf = [(pp, mm) for pp, cc in code_of.items() for mm in SKF.finditer(cc)]
    if not macros:
        fails.append("I7: 未枚举到任何 RB_PERSIST_WHY_* 宏 -- 判据空转，不得判 PASS")
    elif len(skf) != 1:
        fails.append("I7: RbPersistSkipReasonOf 定位到 %d 处（期望 1）-- 判据空转" % len(skf))
    else:
        pp, mm = skf[0]
        r7 = pp.relative_to(ROOT).as_posix()
        sbd, _ = body_after(code_of[pp], mm)
        if sbd is None:
            fails.append("%s: I7 无法截取 RbPersistSkipReasonOf 函数体" % r7)
        else:
            for cm in re.finditer(r"case\s+(RB_PERSIST_WHY_\w+)\s*:\s*return\s+(\w+)\s*;", sbd):
                why_cases[cm.group(1)] = cm.group(2)
            for name, _v in macros:
                if name == "RB_PERSIST_WHY_NONE":
                    continue          # 哨兵：按设计由 default 覆盖，无 case
                if name not in why_cases:
                    fails.append("I7: %s 无 case -- 静默落 default（RB_SKIP_NONE = 假「没跳过」）"
                                 % name)
                elif name == "RB_PERSIST_WHY_CLEAN":
                    pass          # CLEAN -> NONE 是设计（"没跳过"就是干净）
                elif why_cases[name] == "RB_SKIP_NONE":
                    fails.append("I7: %s 的 case 返回 RB_SKIP_NONE -- 与「没跳过」不可区分"
                                 % name)
    seen = {}
    for name, val in macros:
        if val is None:
            fails.append("I7: %s 的值无法解析为整数 -- 判据不得静默跳过" % name)
            continue
        seen.setdefault(val, []).append(name)
    for val, ns in seen.items():
        if len(ns) > 1:
            fails.append("I7: why 值 %d 被复用：%s -- 原因码串味" % (val, " / ".join(ns)))

    # ---- 10) I8：ALREADY 之前必须读 LastCleanWithheld（钉住原语与子键） ----
    SPN = re.compile(r"\bULONG\s+RbShutdownPersist\s*\(\s*VOID\s*\)\s*\{")
    spn = [(pp, mm) for pp, cc in code_of.items() for mm in SPN.finditer(cc)]
    if len(spn) != 1:
        fails.append("I8: RbShutdownPersist 定义定位到 %d 处（期望 1；原型不带 `{` 不计）"
                     " -- 判据空转，不得判 PASS" % len(spn))
    else:
        pp, mm = spn[0]
        r8 = pp.relative_to(ROOT).as_posix()
        sbd, _ = body_after(code_of[pp], mm)
        if sbd is None:
            fails.append("%s: I8 无法截取 RbShutdownPersist 函数体" % r8)
        else:
            ma8 = re.search(r"return\s+RB_PERSIST_WHY_ALREADY\s*;", sbd)
            mc8 = re.search(r"if\s*\(\s*prev\s*!=\s*RB_PERSIST_IDLE\s*\)", sbd)
            mr8 = re.search(r'Rb2GetDW\s*\(\s*L"Parameters"\s*,\s*L"LastCleanWithheld"', sbd)
            if not ma8:
                fails.append("%s: 找不到 `return RB_PERSIST_WHY_ALREADY;` -- 判据空转" % r8)
            elif not mc8 or mc8.start() > ma8.start():
                fails.append("%s: ALREADY 之前没有 `if (prev != RB_PERSIST_IDLE)` 认领块"
                             " -- 判据空转" % r8)
            elif not (mr8 and mc8.start() < mr8.start() < ma8.start()):
                fails.append("%s: ALREADY 之前未在认领块内读 LastCleanWithheld（原语必须是 "
                             "Rb2GetDw(L\"Parameters\", ...)；RbRegGetDW 是 rbdriver.c 的 static）"
                             % r8)

    # ---- 11) I9：外层日志必须有 withheld 独立支 ----
    DDI = re.compile(r"\bRbDispatchShutdownDiag\s*\([^;)]*\)\s*\{")
    ddi = [(pp, mm) for pp, cc in code_of.items() for mm in DDI.finditer(cc)]
    if len(ddi) != 1:
        fails.append("I9: RbDispatchShutdownDiag 定义定位到 %d 处（期望 1）-- 判据空转"
                     % len(ddi))
    else:
        pp, mm = ddi[0]
        r9 = pp.relative_to(ROOT).as_posix()
        sbd, _ = body_after(code_of[pp], mm)
        if sbd is None:
            fails.append("%s: I9 无法截取 RbDispatchShutdownDiag 函数体" % r9)
        else:
            if not re.search(r"withheld\s*=\s*\(\s*BOOLEAN\s*\)\s*\(\s*why\s*==\s*"
                             r"RB_PERSIST_WHY_WITHHELD\s*\)", sbd):
                fails.append("%s: 缺 `withheld = (BOOLEAN)(why == RB_PERSIST_WHY_WITHHELD)`"
                             " -- why=7 会落进「NOT clean」假文案" % r9)
            iw = sbd.find("(long)why")
            if iw < 0:
                fails.append("%s: 找不到 `(long)why` 的 RbLog -- 判据空转" % r9)
            else:
                st = sbd.rfind("RbLog", 0, iw)
                en = sbd.find(");", iw)
                seg = sbd[st:en + 2] if st >= 0 and en > 0 else ""
                if "withheld" not in seg:
                    fails.append("%s: 承载 (long)why 的那次 RbLog 等级/文案里没有 withheld"
                                 " -- level 与文案都会说谎" % r9)

    # ---- 12) I10：skip 码互异 + 9 号存在且被引用 ----
    SKD = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(RB_SKIP_\w+)[ \t]+(\w+)", re.M)
    skips = []
    for pp, cc in code_of.items():
        for mm in SKD.finditer(cc):
            tok = re.sub(r"[uUlL]+$", "", mm.group(2))
            try:
                skips.append((mm.group(1), int(tok, 0)))
            except ValueError:
                fails.append("I10: %s 的值无法解析为整数 -- 判据不得静默跳过" % mm.group(1))
    if not skips:
        fails.append("I10: 未枚举到任何 RB_SKIP_* 宏 -- 判据空转，不得判 PASS")
    else:
        byv = {}
        for nm, vv in skips:
            byv.setdefault(vv, []).append(nm)
        for vv, ns in byv.items():
            if len(ns) > 1:
                fails.append("I10: skip 值 %d 被复用：%s -- 跳过码串味" % (vv, " / ".join(ns)))
        got = dict(skips)
        if "RB_SKIP_CLEAN_WITHHELD" not in got:
            fails.append("I10: 缺 RB_SKIP_CLEAN_WITHHELD -- A4-d 的跳过码未定义")
        elif got["RB_SKIP_CLEAN_WITHHELD"] != 9:
            fails.append("I10: RB_SKIP_CLEAN_WITHHELD = %d（期望 9）-- 与既有码串味"
                         % got["RB_SKIP_CLEAN_WITHHELD"])
        elif why_cases.get("RB_PERSIST_WHY_WITHHELD") != "RB_SKIP_CLEAN_WITHHELD":
            fails.append("I10: WHY_WITHHELD 的 case 没返回 RB_SKIP_CLEAN_WITHHELD（实测 %r）"
                         % why_cases.get("RB_PERSIST_WHY_WITHHELD"))

    print("check_a4_clean_gate: RbRegSetClean(1) single call site + volume gate")
    print("  scanned: %d files under src/driver (comment-stripped, line-preserving)" % len(srcs))
    for p, ln, txt in hits:
        print("  call site: %s:%d  %s" % (p.relative_to(ROOT).as_posix(), ln, txt))
    if fails:
        for f in fails:
            print("[FAIL] " + f)
        print("result: FAIL (%d fail, 0 warn)" % len(fails))
        return 1
    print('[OK] exactly 1 RbRegSetClean(1) call site; volume gate present in '
          'RbRegSetClean body (count -> v==0 && !BootAttachedAny -> withheld+return)')
    print('[OK] I1: RbRegSetClean body free of BootFailCount')
    print('[OK] I2: zeroing binding chain (read -> zero -> persist -> BootLoopGuard) + '
          'clean needs ticket at top level; shape only, see docstring for uncovered forms')
    print('[OK] I3: L"LastCleanShutdown" only written inside RbRegSetClean / boot reset')
    print('[OK] I4: attachment ticket is a real static flag (not always-true) and the '
          'volume count still filters IsAttached && !Removed')
    print('[OK] I5: RbRegSetClean returns BOOLEAN; no bare `return;`; withheld branch '
          'returns FALSE and the write point returns TRUE')
    print('[OK] I6: the single call site CONSUMES the result (if (!RbRegSetClean(1))) and '
          'the withheld exit sets the latch DONE')
    print('[OK] I7: every RB_PERSIST_WHY_* (except _NONE) has a case in '
          'RbPersistSkipReasonOf, none maps a non-CLEAN code to RB_SKIP_NONE, values unique')
    print('[OK] I8: the ALREADY exit consults Parameters\\LastCleanWithheld via Rb2GetDW '
          'first (hiberfile-stale latch cannot claim clean)')
    print('[OK] I9: shutdown diag has a dedicated WITHHELD branch (level + wording)')
    print('[OK] I10: RB_SKIP_* values unique; RB_SKIP_CLEAN_WITHHELD == 9 and is what '
          'WHY_WITHHELD maps to')
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
