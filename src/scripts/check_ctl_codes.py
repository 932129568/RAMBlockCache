#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate25: 控制面闭合 —— 声明 / 分派 / 消费 / 布局 / 写点 / 码表 六方对齐。

背景（V10.5 文档 ramblockcache_v10_52.md §3.1，交付记录 282）：
本轮新增两个控制码 0x906（只读关机诊断）与 0x907（健康提交）。这五类漂移，
既有门禁没有一个能抓：

  (1) 声明了没人做：`rb_common2.h` 加一行 CTL_CODE + CLI 加一行同值宏 = 两侧
      对称，Gate17 全绿，而驱动根本没 case 它 —— IOCTL 返回
      STATUS_INVALID_DEVICE_REQUEST，"两处一致"一致地错。
  (2) 做了两遍：同一功能码既被转发进 RbFeatDispatch，又在 rbctl.c 直接处理
      = 双分派 / IRP 双重完成。
  (3) ABI 两侧不同步：`RB2_SHUTDOWN_DIAG_OUT` 驱动侧加字段、ctl 手工副本没跟，
      结构体都还是 24 B 时偏移全错，读到别人的值（check_ctl_freshness 的字段级
      契约只覆盖 STATS2）。
  (4) 清零写点扩散：`BootFailCount` 是 BootLoopGuard 的唯一输入，多一个清零点
      就多一条"守护可被静默解除"的路；A4-d 已把 RbRegSetClean 里的副作用删掉
      （Gate13 I1），不能从别处长回来。
  (5) 码表转述漂移：CLI 要给人看 why/skip 的名字，就得抄一份码表。抄错一个值
      不会报错，只会让操作员照着一份错的码表判断关机是否干净。

七条判据（空匹配集一律 FAIL，绝不因"没解析到"而放行）：
  P1 功能号：`RB2_FUNC_*` 存在、取值互异、0x906/0x907 精确在位。
  P2 副本：MIRROR_PAIRS 里每对符号两侧展开值相等；头文件里**全部** CTL_CODE
     符号只要在 ctl 侧同名出现，两侧数值就必须相等（ctl 未镜像的逐一打印）；
     0x906/0x907 的 DeviceType/Method/Access 逐项断言；头文件别名不得悬空。
  P3 分派闭合：头文件每个 CTL_CODE 符号必须在 FORWARDED ∪ DIRECT 内登记；
     转发者必须在 RbFeatDispatch 有 case，直连者必须有处理器（定义 1 次 +
     调用 ≥1 次）且不得出现在 RbFeatDispatch（禁双分派）。
  P4 消费闭合：每个控制码组在 `src/ctl/*.c`（已剥注释）的 Xfer /
     DeviceIoControl **实参位**至少出现一次；仅存在同名宏副本不算下发点。
  P5 布局：`RB2_SHUTDOWN_DIAG_OUT` 与 ctl `SHUTDIAG` 字段名/顺序/偏移/总长
     逐项相等，总长 = 24 B，且头文件保留 `sizeof(...) == 24 ? 1 : -1` 断言
     （断言表达式本身必须校验，不能只查符号名）。
  P6 写点白名单：以「写点」为对象，不认具体写法 —— 注册表写（RbRegSetDW）、
     直接赋值、自增自减、取地址交给清零原语，全部按 (文件, 宿主函数, 语法类)
     登记；未登记的函数 / 语法 / 重复写点一律 FAIL。纯读不约束。
     `RbRegSetClean` 体内不得出现该符号。
  P7 码表镜像：rbctl.c 的 RB_CTL_WHY_NONE / RB_CTL_SKIP_NONE 落在权威表内且
     取值正确；rbfeatctl.c 的 WhyText/SkipText 必须是**名字 → 值**的映射，
     与权威码表逐键逐值相等（只比值集合会让 CLEAN/DIRTY 对调静默通过；
     skip 额外要求 0=unset）。

只读：不编译、不写文件、不起子进程。
用法: python src/scripts/check_ctl_codes.py
退出码: 0=PASS, 1=FAIL
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

HDR = "src/driver/rb_common2.h"
RBCTL = "src/driver/rbctl.c"
FEAT = "src/driver/rbfeat.c"
MSG = "src/driver/rbmsg.c"
COMMON = "src/driver/rb_common.h"
CTL = "src/ctl/rbfeatctl.c"

# 分派注册表：新增控制码必须在这里登记，否则 P3 闭包判据直接红。
# 这是刻意的人为摩擦 —— 加一个 IOCTL 却没人做，是这几类缺陷里最难发现的一种。
FORWARDED = (
    "RB2_IOCTL_STATS",
    "RB2_IOCTL_REFRESH",
    "RB2_IOCTL_SHRINK",
    "RB2_IOCTL_SNAPSHOT",
    "RB2_IOCTL_VOLSTATUS",
)
DIRECT = {
    "IOCTL_RB2_SHUTDOWN_DIAG": "RbCtlShutdownDiag",
    "IOCTL_RB2_HEALTH_COMMIT": "RbCtlHealthCommit",
}

# P2：头文件 <-> ctl 手工副本的符号对。
# 新码两侧都用 IOCTL_RB2_*（承载 CTL_CODE 的名字必须逐字相同，否则 Gate17 按
# (DeviceType,Function) 分组会判成两个符号撞号）；RB2_IOCTL_* 只是头文件别名。
MIRROR_PAIRS = (
    ("RB2_IOCTL_STATS", "RB2_IOCTL_STATS"),
    ("RB2_IOCTL_REFRESH", "RB2_IOCTL_REFRESH"),
    ("RB2_IOCTL_SHRINK", "RB2_IOCTL_SHRINK"),
    ("RB2_IOCTL_SNAPSHOT", "RB2_IOCTL_SNAPSHOT"),
    ("RB2_IOCTL_VOLSTATUS", "RB2_IOCTL_VOLSTATUS"),
    ("IOCTL_RB2_SHUTDOWN_DIAG", "IOCTL_RB2_SHUTDOWN_DIAG"),
    ("IOCTL_RB2_HEALTH_COMMIT", "IOCTL_RB2_HEALTH_COMMIT"),
)

# P2：新码 ABI 期望（DeviceType 0x22=FILE_DEVICE_UNKNOWN, Method 0=METHOD_BUFFERED）
EXPECT_ABI = {
    "IOCTL_RB2_SHUTDOWN_DIAG": (0x22, 0x906, 0, 0),   # access 0 = FILE_ANY_ACCESS
    "IOCTL_RB2_HEALTH_COMMIT": (0x22, 0x907, 0, 2),   # access 2 = FILE_WRITE_ACCESS
}

# P6：BootFailCount 写点白名单 —— (文件名, 宿主函数) -> 该函数内允许出现的写语法类。
# 语法类：reg     = RbRegSetDW(L"BootFailCount", ...)（注册表清零）
#         assign  = g_Rb.BootFailCount = / 复合赋值（内存清零）
#         incdec  = g_Rb.BootFailCount++ / --
#         address = &g_Rb.BootFailCount 取地址（交给 RtlZeroMemory / memset /
#                   Interlocked* 等清零原语）—— 换写法不等于换语义，必须登记。
# 只登记「允许存在的语法」，不写死次数下限：删掉某个清零点（收紧）不算回归，
# 多出一个（无论是新函数还是同函数换语法）都必须人工登记。同一 (文件, 函数, 语法)
# 至多 1 处：同函数内补一处新写法不能被"次数不变"掩盖。
ALLOWED_WRITE_SITES = {
    ("rbdriver.c", "RbBootFailLoad"): {"reg", "assign"},
    ("rbctl.c", "RbMsgPanicClear"): {"reg"},
    ("rbctl.c", "RbCtlHealthCommit"): {"reg", "assign"},
}

# P6 写点识别：不认具体写法，认「对 BootFailCount 这个计数器产生写副作用」。
# 只匹配 g_Rb.BootFailCount（BootLoopGuard 的唯一输入）与注册表键名，避免把
# 结构体字段 `o->BootFailCount = ...`（输出副本）误当清零点。
WRITE_KINDS = (
    ("reg", re.compile(r'RbRegSetDW\s*\(\s*L"BootFailCount"')),
    ("assign", re.compile(r"(?<![A-Za-z0-9_.>])g_Rb\s*\.\s*BootFailCount\s*"
                          r"(?:(?:<<|>>|[-+*/%&|^])?=(?!=))")),
    ("incdec", re.compile(r"(?:\+\+|--)\s*g_Rb\s*\.\s*BootFailCount\b"
                          r"|\bg_Rb\s*\.\s*BootFailCount\s*(?:\+\+|--)")),
    ("address", re.compile(r"&\s*g_Rb\s*\.\s*BootFailCount\b")),
)

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

NUM = re.compile(r"^(0[xX][0-9A-Fa-f]+|\d+)$")
IOCTL_NAME = re.compile(r"^(?:IOCTL_[A-Z0-9_]+|RB2_IOCTL_[A-Z0-9_]+)$")
fails = []
infos = []


def fail(msg):
    fails.append(msg)


def read(rel):
    p = ROOT / rel
    if not p.is_file():
        fail("缺文件: %s" % rel)
        return ""
    raw = p.read_bytes()
    try:
        txt = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        txt = raw.decode("utf-8", errors="replace")
    # 行尾归一：不做这一步，所有以 $ 结尾的行内正则在 CRLF 文件上全部静默失配
    # （Gate24 首次落地时 13 条假 FAIL 就是这么来的）。
    return txt.replace("\r\n", "\n")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"(?m)//.*$", "", text)


def join_continuations(text):
    return re.sub(r"\\[ \t]*\n", " ", text)


def scalar_macros(text):
    out = {}
    for m in re.finditer(r"(?m)^[ \t]*#define[ \t]+(\w+)[ \t]+"
                         r"(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*[ \t]*$", text):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def ioctl_defs(text):
    """[(symbol, [args], line)] —— CTL_CODE 定义（已接续行、已剥注释）。"""
    line_of = {}
    for m in re.finditer(r"(?m)^[ \t]*#define[ \t]+(\w+)[ \t]+CTL_CODE", text):
        line_of[m.group(1)] = text[:m.start()].count("\n") + 1
    body = join_continuations(strip_comments(text))
    out = []
    for m in re.finditer(r"(?m)^[ \t]*#define[ \t]+(\w+)[ \t]+CTL_CODE[ \t]*\(([^)]*)\)", body):
        out.append((m.group(1), [a.strip() for a in m.group(2).split(",")],
                    line_of.get(m.group(1), 0)))
    return out


def expand(args, macros):
    """CTL_CODE 实参 -> (dt, func, method, access)；解析不到返回 None（调用方 FAIL）。"""
    if len(args) != 4:
        return None
    vals = []
    for a in args:
        if NUM.match(a):
            vals.append(int(a, 0))
        elif a in macros:
            vals.append(macros[a])
        elif a in BUILTIN:
            vals.append(BUILTIN[a])
        else:
            return None
    return tuple(vals)


def ioctl_abi(text, sym):
    """单个符号的 (dt, func, method, access)，仅对 CTL_CODE 定义有效。"""
    macros = scalar_macros(strip_comments(text))
    for s, args, _ in ioctl_defs(text):
        if s == sym:
            return expand(args, macros)
    return None


def name_value_map(text):
    """符号 -> 展开后的 IOCTL 值；含 `#define ALIAS TARGET` 别名（多轮传递）。"""
    macros = scalar_macros(strip_comments(text))
    vals = {}
    for s, args, _ in ioctl_defs(text):
        ex = expand(args, macros)
        if ex is None:
            fail("%s: %s 的 CTL_CODE 实参无法求值 (%s)" % (text_label(text), s, ",".join(args)))
            continue
        dt, func, method, access = ex
        vals[s] = (dt << 16) | (access << 14) | (func << 2) | method
    clean = strip_comments(text)
    for _ in range(4):
        changed = False
        for m in re.finditer(r"(?m)^[ \t]*#define[ \t]+(\w+)[ \t]+(\w+)[ \t]*$", clean):
            a, t = m.group(1), m.group(2)
            if a == t or not IOCTL_NAME.match(a):
                continue
            if a not in vals and t in vals:
                vals[a] = vals[t]
                changed = True
        if not changed:
            break
    return vals


_LABELS = {}


def text_label(text):
    return _LABELS.get(id(text), "?")


def groups_by_value(vals):
    g = {}
    for name, v in vals.items():
        g.setdefault(v, set()).add(name)
    return g


def field_layout(fields):
    """[(type, name, count)] -> ([(name, offset, size)], total)。未知类型返回 (None, None)。"""
    sizes = {"ULONG": (4, 4), "BOOLEAN": (1, 1), "UCHAR": (1, 1), "WCHAR": (2, 2)}
    off = 0
    align = 1
    out = []
    for ty, name, cnt in fields:
        if ty not in sizes:
            return None, None
        sz, al = sizes[ty]
        align = max(align, al)
        off += (-off) % al
        out.append((name, off, sz * cnt))
        off += sz * cnt
    total = off + ((-off) % align)
    return out, total


def parse_struct(text, typedef_name):
    """按 typedef 的**尾部名字**定位结构体（驱动侧带 tag，ctl 侧是匿名 struct）。"""
    clean = strip_comments(text)
    for m in re.finditer(r"typedef\s+struct[^{;]*\{(.*?)\}\s*([^;]*);", clean, re.S):
        names = [n.strip().lstrip("*") for n in m.group(2).split(",")]
        if typedef_name not in names:
            continue
        fields = []
        for line in m.group(1).split("\n"):
            for decl in line.split(";"):
                decl = decl.strip()
                if not decl:
                    continue
                mm = re.match(r"([A-Z_][A-Za-z0-9_]*)\s+(.+)$", decl)
                if not mm:
                    continue
                ty, rest = mm.group(1), mm.group(2)
                for part in rest.split(","):
                    part = part.strip()
                    if not part:
                        continue
                    am = re.match(r"(\w+)\s*\[\s*(\d+)\s*\]$", part)
                    if am:
                        fields.append((ty, am.group(1), int(am.group(2))))
                    elif re.match(r"\w+$", part):
                        fields.append((ty, part, 1))
        return fields
    return None


def function_body(text, name):
    """按大括号配平取函数体（调用方负责先剥注释）。"""
    m = re.search(r"\b%s\s*\([^;{]*\)\s*\{" % re.escape(name), text)
    if not m:
        return None
    i = text.index("{", m.start())
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
    return None


def case_values(text, func):
    body = function_body(text, func)
    if body is None:
        return None
    vals = set()
    for m in re.finditer(r"case\s+(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\s*:", body):
        vals.add(int(m.group(1), 0))
    return vals if vals else None


FUNC_DEF = re.compile(
    r"(?m)^[ \t]*(?:static[ \t]+|inline[ \t]+|__inline[ \t]+)*"
    r"(?:[A-Za-z_]\w*[ \t\*]+)+([A-Za-z_]\w*)[ \t]*\([^;{}]*\)\s*\{")
C_KEYWORDS = {"if", "for", "while", "switch", "else", "do", "return", "sizeof"}


def function_spans(text):
    """[(name, body_start, body_end)] —— 函数体区间（调用方负责先剥注释）。"""
    spans = []
    for m in FUNC_DEF.finditer(text):
        name = m.group(1)
        if name in C_KEYWORDS:
            continue
        i = m.end() - 1
        depth = 0
        for j in range(i, len(text)):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    spans.append((name, i, j))
                    break
    return spans


def host_function(pos, spans):
    """pos 所在的最近一层函数名；解析不到返回 None（调用方必须 FAIL，不许跳过）。"""
    best = None
    for name, a, b in spans:
        if a < pos < b and (best is None or a > best[1]):
            best = (name, a)
    return best[0] if best else None


def switch_name_values(text, func):
    """switch 分支 -> {返回的字符串名: case 值}。名字取字面量首段（"NONE(x)" -> NONE）。

    只比值集合会让 rbmsg.c 里 CLEAN(0)/DIRTY(1) 对调后仍然 PASS —— 名字与值必须
    成对比较，所以这里保留名字。
    """
    body = function_body(text, func)
    if body is None:
        return None
    out = {}
    for m in re.finditer(r"case\s+(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\s*:\s*"
                         r"return\s+\"([^\"]*)\"\s*;", body):
        name = re.sub(r"\s*\(.*$", "", m.group(2)).strip()
        if name in out:
            return None
        out[name] = int(m.group(1), 0)
    return out if out else None


def main():
    hdr = read(HDR)
    ctl = read(CTL)
    rbctl = read(RBCTL)
    feat = read(FEAT)
    msg = read(MSG)
    common = read(COMMON)
    _LABELS.update({id(hdr): HDR, id(ctl): CTL, id(rbctl): RBCTL,
                    id(feat): FEAT, id(msg): MSG, id(common): COMMON})

    hdr_vals = name_value_map(hdr)
    ctl_vals = name_value_map(ctl)

    # ---------- P1 功能号 ----------
    funcs = {k: v for k, v in scalar_macros(strip_comments(hdr)).items()
             if re.match(r"RB2_FUNC_\w+$", k)}
    if not funcs:
        fail("P1: rb_common2.h 未解析到任何 RB2_FUNC_* （解析器或文件结构已漂移）")
    dup = {}
    for k, v in funcs.items():
        dup.setdefault(v, []).append(k)
    for v, names in sorted(dup.items()):
        if len(names) > 1:
            fail("P1: 功能号 0x%03X 被 %s 共用" % (v, sorted(names)))
    ok_neq = True
    for sym, want in (("RB2_FUNC_SHUTDOWN_DIAG", 0x906), ("RB2_FUNC_HEALTH_COMMIT", 0x907)):
        if sym not in funcs:
            fail("P1: 缺 %s（V10.5 §3.1）" % sym)
            ok_neq = False
        elif funcs[sym] != want:
            fail("P1: %s = 0x%03X，期望 0x%03X" % (sym, funcs[sym], want))
            ok_neq = False
    infos.append("P1 功能号 %d 个 / 互异 %d 个 / 0x906+0x907 在位=%s"
                 % (len(funcs), len(dup), ok_neq))

    # ---------- P2 副本一致性 + ABI ----------
    if not hdr_vals:
        fail("P2: 头文件未解析到任何 CTL_CODE 值（空集不得判 PASS）")
    if not ctl_vals:
        fail("P2: ctl 未解析到任何 CTL_CODE 值（空集不得判 PASS）")
    n_pair = 0
    for hs, cs in MIRROR_PAIRS:
        hv, cv = hdr_vals.get(hs), ctl_vals.get(cs)
        if hv is None:
            fail("P2: 头文件未定义/无法展开 %s" % hs)
            continue
        if cv is None:
            fail("P2: ctl 侧缺副本 %s（新增控制码必须两侧同值）" % cs)
            continue
        if hv != cv:
            fail("P2: %s / %s 两侧不同值 头=0x%08X ctl=0x%08X" % (hs, cs, hv, cv))
        else:
            n_pair += 1
    # 闭包：不只看 MIRROR_PAIRS 这 7 对。头文件里每一个 CTL_CODE 符号，只要在
    # ctl 侧同名出现，两侧数值就必须相等 —— 否则新增的第 8 个码可以「两侧都有、
    # 值不同」而完全绕过 P2。ctl 侧没有同名符号的容许（由 P4 兜底），但必须打印。
    clean_ctl = strip_comments(ctl)
    clean_hdr = strip_comments(hdr)
    hdr_syms = [s for s, _, _ in ioctl_defs(hdr)]
    mirrored = 0
    absent = []
    for s in hdr_syms:
        hv = hdr_vals.get(s)
        if not re.search(r"\b%s\b" % re.escape(s), clean_ctl):
            absent.append(s)
            continue
        cv = ctl_vals.get(s)
        if hv is None:
            fail("P2: 头文件 %s 无法展开，闭包判据无值可比" % s)
            continue
        if cv is None:
            fail("P2: ctl 侧出现 %s 却无法展开出同值（副本/别名已漂移）" % s)
            continue
        if hv != cv:
            fail("P2: %s 两侧不同值 头=0x%08X ctl=0x%08X" % (s, hv, cv))
        else:
            mirrored += 1
    # 头文件里的 IOCTL 别名不得悬空（悬空 = 声明了却展开不到值）
    for m in re.finditer(r"(?m)^[ \t]*#define[ \t]+(\w+)[ \t]+(\w+)[ \t]*$", clean_hdr):
        a, t = m.group(1), m.group(2)
        if not IOCTL_NAME.match(a):
            continue
        if a not in hdr_vals:
            fail("P2: 别名 %s -> %s 悬空（别名或被指向的符号无法展开）" % (a, t))
    for sym, want in EXPECT_ABI.items():
        ex = ioctl_abi(hdr, sym)
        if ex is None:
            fail("P2: %s 的 CTL_CODE 无法展开" % sym)
            continue
        if ex != want:
            fail("P2: %s ABI = (dt=0x%02X, func=0x%03X, method=%d, access=%d)，"
                 "期望 (0x22, 0x%03X, 0, %d)" % (sym, ex[0], ex[1], ex[2], ex[3], want[1], want[3]))
    infos.append("P2 副本对 %d/%d 相等；头文件 CTL_CODE 符号 %d 个（ctl 同值镜像 %d，"
                 "ctl 未镜像 %s）；ABI 断言 %d 条"
                 % (n_pair, len(MIRROR_PAIRS), len(hdr_syms), mirrored,
                    absent or "无", len(EXPECT_ABI)))

    # ---------- P3 分派闭合 ----------
    defined = {s for s, _, _ in ioctl_defs(hdr)}
    if not defined:
        fail("P3: 头文件一个 CTL_CODE 符号都没有，闭包判据无意义")
    registry = set(FORWARDED) | set(DIRECT)
    for s in sorted(defined - registry):
        fail("P3: %s 已声明但未在分派注册表登记（新增控制码必须显式登记 FORWARDED/DIRECT）" % s)
    for s in sorted(registry - defined):
        fail("P3: 注册表登记的 %s 在头文件里已不存在（登记过期）" % s)

    rbctl_vals = set()
    for m in re.finditer(r"(?m)\bcode\s*==\s*(\w+)", strip_comments(rbctl)):
        v = hdr_vals.get(m.group(1))
        if v is None:
            v = rbctl_vals_raw(rbctl, m.group(1))
        if v is None:
            fail("P3: rbctl.c 里 code == %s 无法求值" % m.group(1))
            continue
        rbctl_vals.add(v)
    feat_vals = set()
    for m in re.finditer(r"(?m)^\s*case\s+(IOCTL_\w+|RB2_IOCTL_\w+)\s*:", strip_comments(feat)):
        v = hdr_vals.get(m.group(1))
        if v is None:
            fail("P3: rbfeat.c 里 case %s 无法求值" % m.group(1))
            continue
        feat_vals.add(v)
    if not rbctl_vals:
        fail("P3: rbctl.c 未解析到任何 code == <ioctl> 分派点（空集不得判 PASS）")
    if not feat_vals:
        fail("P3: rbfeat.c 未解析到任何 case <ioctl>（空集不得判 PASS）")

    for sym in FORWARDED:
        v = hdr_vals.get(sym)
        if v is None:
            continue
        if v not in rbctl_vals:
            fail("P3: 转发符号 %s 不在 rbctl.c 的转发链里" % sym)
        if v not in feat_vals:
            fail("P3: 转发符号 %s 在 RbFeatDispatch 里没有 case（转发即丢包）" % sym)
    clean_rbctl = strip_comments(rbctl)
    chain_vals, chain_err = forward_chain_vals(clean_rbctl, hdr_vals)
    if chain_err:
        # 空匹配集一律 FAIL：锚点被改写 / 守卫块解析不出时，静默返回 set() 会让
        # 「直连码被塞进转发链」和「转发链整段消失」都变成无声 PASS。
        fail("P3: forward_chain_vals: %s" % chain_err)
    for sym, handler in DIRECT.items():
        v = hdr_vals.get(sym)
        if v is None:
            continue
        if v not in rbctl_vals:
            fail("P3: 直连符号 %s 在 rbctl.c 里没有分派点" % sym)
        if v in feat_vals:
            fail("P3: 直连符号 %s 同时出现在 RbFeatDispatch —— 双分派（IRP 双重完成）" % sym)
        if v in chain_vals:
            # 只在转发链里加一个 || 不改 switch：驱动侧看起来"分派了"，实际
            # RbFeatDispatch 的 default 返回 STATUS_INVALID_DEVICE_REQUEST，
            # 而直连处理器永远不会被调用。M6 变异就是为了打这一枪。
            fail("P3: 直连符号 %s 被塞进 RbFeatDispatch 转发链 —— 与直连处理器双路径" % sym)
        # 定义行本身会命中 \bhandler( —— 只查"出现过"等价于只查定义，调用点缺失
        # 永远发现不了。要求：① 有 static NTSTATUS 定义；② 至少一处出现在
        # **别的函数体**内（原型 + 定义不算调用点）。
        spans = function_spans(clean_rbctl)
        uses = len(re.findall(r"\b%s\s*\(" % re.escape(handler), clean_rbctl))
        callers = set()
        for hm in re.finditer(r"\b%s\s*\(" % re.escape(handler), clean_rbctl):
            fn = host_function(hm.start(), spans)
            if fn is not None and fn != handler:
                callers.add(fn)
        if not re.search(r"static\s+NTSTATUS\s+%s\s*\(" % re.escape(handler), clean_rbctl):
            fail("P3: 处理器 %s 未在 rbctl.c 中定义" % handler)
        elif not callers:
            fail("P3: 处理器 %s 没有调用点（全文件出现 %d 次，均在自身定义/原型内；"
                 "期望至少 1 处位于别的函数体）—— 无调用点 = 死代码，直连码实际无人处理"
                 % (handler, uses))
    infos.append("P3 声明 %d / 转发 %d / 直连 %d；rbctl 分派值 %d，feat case 值 %d"
                 % (len(defined), len(FORWARDED), len(DIRECT), len(rbctl_vals), len(feat_vals)))

    # ---------- P4 消费闭合 ----------
    ctl_files = sorted((ROOT / "src" / "ctl").glob("*.c"))
    if not ctl_files:
        fail("P4: src/ctl 下没有 .c 文件，消费闭合无从判断")
    corpus = ""
    for p in ctl_files:
        raw = p.read_bytes()
        try:
            t = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            t = raw.decode("utf-8", errors="replace")
        # 必须剥注释：P2 强制 ctl 侧保留同名宏副本（#define RB2_IOCTL_X ...），
        # 不剥的话「名字出现过」恒真，与有没有下发点无关。
        corpus += strip_comments(t.replace("\r\n", "\n")) + "\n"
    grp = groups_by_value(hdr_vals)
    if not grp:
        fail("P4: 头文件控制码分组为空（空集不得判 PASS）")
    for v, names in sorted(grp.items()):
        hit = None
        for n in sorted(names):
            # 下发点 = 出现在 Xfer / DeviceIoControl 的**实参位**。只认宏定义/字符串
            # 不算；[^;()] 防止跨语句或吞进嵌套调用。
            if re.search(r"(?:Xfer|DeviceIoControl)\s*\([^;()]*\b%s\b" % re.escape(n),
                         corpus):
                hit = n
                break
        if hit is None:
            fail("P4: 控制码组 %s（0x%08X）在 src/ctl/*.c 里没有任何下发点"
                 "（须在 Xfer/DeviceIoControl 实参位出现；同名宏定义不算）"
                 % (sorted(names), v))
    infos.append("P4 ctl 语料 %d 个 .c，覆盖控制码组 %d 个" % (len(ctl_files), len(grp)))

    # ---------- P5 结构体布局 ----------
    hf = parse_struct(hdr, "RB2_SHUTDOWN_DIAG_OUT")
    cf = parse_struct(ctl, "SHUTDIAG")
    if not hf:
        fail("P5: 头文件里找不到 typedef ... RB2_SHUTDOWN_DIAG_OUT")
    if not cf:
        fail("P5: ctl 里找不到 typedef ... SHUTDIAG（手工副本）")
    if hf and cf:
        hl, ht = field_layout(hf)
        cl, ct = field_layout(cf)
        if hl is None or cl is None:
            fail("P5: 结构体含未知字段类型，无法计算布局")
        else:
            if ht != 24:
                fail("P5: 驱动侧 RB2_SHUTDOWN_DIAG_OUT 总长 %s B，期望 24 B" % ht)
            if ht != ct:
                fail("P5: 总长不一致 驱动=%s B ctl=%s B" % (ht, ct))
            if len(hl) != len(cl):
                fail("P5: 字段数不一致 驱动=%d ctl=%d" % (len(hl), len(cl)))
            else:
                for a, b in zip(hl, cl):
                    if a != b:
                        fail("P5: 字段布局不一致 驱动=%r ctl=%r" % (a, b))
            # 只查 token 存在 = 断言写错也过（`? 1 : 1` 恒真、`== 25` 照样编译）。
            # 必须校验表达式本体：sizeof(RB2_SHUTDOWN_DIAG_OUT) == 24 且失败分支 -1。
            assert_m = re.search(r"RB2_SHUTDOWN_DIAG_OUT_IS_24_BYTES\s*\[(.*?)\]\s*;",
                                 clean_hdr, re.S)
            if not assert_m:
                fail("P5: 头文件缺 24 B 编译期断言（改字段时无法在编译期炸）")
            else:
                expr = assert_m.group(1)
                if not re.search(r"sizeof\s*\(\s*RB2_SHUTDOWN_DIAG_OUT\s*\)\s*==\s*24",
                                 expr):
                    fail("P5: 24 B 断言未校验 sizeof(RB2_SHUTDOWN_DIAG_OUT) == 24，"
                         "表达式 = %r" % " ".join(expr.split()))
                if not re.search(r"\?\s*1\s*:\s*-1", expr):
                    fail("P5: 24 B 断言的失败分支不是 -1（`? 1 : 1` 恒真 = 断言失效），"
                         "表达式 = %r" % " ".join(expr.split()))
            infos.append("P5 驱动=%dB ctl=%dB 字段=%d 末字段偏移=%s"
                         % (ht, ct, len(hl), hl[-1][1] if hl else "?"))

    # ---------- P6 BootFailCount 写点白名单 ----------
    # 判据对象是「写点」，不是「正则命中次数」：换语法（InterlockedExchange /
    # RtlZeroMemory / memset / --）必须同样可见；同文件「删一处 + 补一处」也必须红。
    sites = {}
    orphans = []
    for p in sorted((ROOT / "src" / "driver").glob("*.c")):
        raw = p.read_bytes()
        try:
            txt = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            txt = raw.decode("utf-8", errors="replace")
        txt = strip_comments(txt.replace("\r\n", "\n"))
        spans = function_spans(txt)
        for m in re.finditer(r"\bBootFailCount\b", txt):
            if host_function(m.start(), spans) is None:
                orphans.append("%s@%d" % (p.name, txt[:m.start()].count("\n") + 1))
        for kind, rx in WRITE_KINDS:
            for m in rx.finditer(txt):
                fn = host_function(m.start(), spans)
                if fn is None:
                    fail("P6: %s 的 BootFailCount 写点（%s）找不到宿主函数，"
                         "无法登记 —— 必须人工确认" % (p.name, kind))
                    continue
                key = (p.name, fn, kind)
                sites[key] = sites.get(key, 0) + 1
    if orphans:
        fail("P6: 以下 BootFailCount 出现点找不到宿主函数（不得跳过）: %s" % orphans)
    if not sites:
        fail("P6: 未解析到任何 BootFailCount 写点（空集不得判 PASS）")
    for (fname, fn, kind), n in sorted(sites.items()):
        allowed = ALLOWED_WRITE_SITES.get((fname, fn))
        if allowed is None:
            fail("P6: %s::%s 出现 BootFailCount 写点（%s）—— 该函数未登记，"
                 "多一个清零点就多一条静默解除 BootLoopGuard 的路" % (fname, fn, kind))
        elif kind not in allowed:
            fail("P6: %s::%s 出现未登记的写语法 %s（已登记 %s）—— 换写法不等于新语义"
                 % (fname, fn, kind, sorted(allowed)))
        elif n > 1:
            fail("P6: %s::%s 的 %s 写点 %d 处，登记上限 1 处（同函数内补点不得被次数掩盖）"
                 % (fname, fn, kind, n))
    reg_disp = {k[0]: sum(v for kk, v in sites.items() if kk[0] == k[0] and kk[2] == "reg")
                for k in sites if k[2] == "reg"}
    mem_disp = {k[0]: sum(v for kk, v in sites.items() if kk[0] == k[0] and kk[2] != "reg")
                for k in sites if k[2] != "reg"}
    drv = strip_comments(read("src/driver/rbdriver.c"))
    body = function_body(drv, "RbRegSetClean")
    if body is None:
        fail("P6: 找不到 RbRegSetClean 函数体（Gate13 I1 的落点）")
    elif "BootFailCount" in body:
        fail("P6: RbRegSetClean 体内出现 BootFailCount —— A4-d 已删除的写侧副作用复活")
    infos.append("P6 写点 %d 处 %s / 注册表写 %s / 内存写 %s / RbRegSetClean 体内干净=%s"
                 % (sum(sites.values()), sorted(sites), reg_disp, mem_disp,
                    body is not None and "BootFailCount" not in body))

    # ---------- P7 码表镜像 ----------
    # 名字 -> 值。只比值集合时，rbmsg.c 里 CLEAN(0)/DIRTY(1) 对调后集合不变 ⇒ PASS，
    # 操作员会照着对调后的名字读关机是否干净。名字和值必须成对对齐。
    why_auth = {m.group(1)[len("RB_PERSIST_WHY_"):]: int(m.group(2), 0)
                for m in re.finditer(r"(?m)^\s*#define\s+(RB_PERSIST_WHY_\w+)\s+"
                                     r"(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*",
                                     strip_comments(msg))}
    if not why_auth:
        fail("P7: rbmsg.c 未解析到 RB_PERSIST_WHY_*（权威码表锚点丢失）")
    skip_auth = {m.group(1)[len("RB_SKIP_"):]: int(m.group(2), 0)
                 for m in re.finditer(r"(?m)^\s*#define\s+(RB_SKIP_\w+)\s+"
                                      r"(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*",
                                      strip_comments(common))}
    if not skip_auth:
        fail("P7: rb_common.h 未解析到 RB_SKIP_*（权威码表锚点丢失）")
    why_vals = set(why_auth.values())
    skip_vals = set(skip_auth.values())

    mir = dict(re.findall(r"(?m)^\s*#define\s+(RB_CTL_WHY_NONE|RB_CTL_SKIP_NONE)\s+"
                          r"(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*", strip_comments(rbctl)))
    if not mir:
        fail("P7: rbctl.c 未解析到 RB_CTL_* 镜像常量")
    else:
        wv = int(mir["RB_CTL_WHY_NONE"], 0) if "RB_CTL_WHY_NONE" in mir else None
        sv = int(mir["RB_CTL_SKIP_NONE"], 0) if "RB_CTL_SKIP_NONE" in mir else None
        if wv is None:
            fail("P7: rbctl.c 缺 RB_CTL_WHY_NONE")
        elif wv != 0xFF or wv not in why_vals:
            fail("P7: RB_CTL_WHY_NONE=0x%X，必须 = 0xFF 且落在 RB_PERSIST_WHY_* 表内" % wv)
        if sv is None:
            fail("P7: rbctl.c 缺 RB_CTL_SKIP_NONE")
        elif sv != 0xFFFFFFFF or sv not in skip_vals:
            fail("P7: RB_CTL_SKIP_NONE=0x%X，必须 = 0xFFFFFFFF 且落在 RB_SKIP_* 表内" % sv)

    wt = switch_name_values(clean_ctl, "WhyText")
    st = switch_name_values(clean_ctl, "SkipText")
    if wt is None:
        fail("P7: rbfeatctl.c 找不到 WhyText 的 switch 分支（或分支名重复）")
    elif wt != why_auth:
        key_bad = {k: (wt[k], why_auth[k]) for k in set(wt) & set(why_auth)
                   if wt[k] != why_auth[k]}
        fail("P7: WhyText 名字->值 与权威码表不一致（值错=%s 多=%s 缺=%s）"
             % (key_bad, sorted(set(wt) - set(why_auth)),
                sorted(set(why_auth) - set(wt))))
    skip_ok = False
    if st is None:
        fail("P7: rbfeatctl.c 找不到 SkipText 的 switch 分支（或分支名重复）")
    else:
        st_cmp = dict(st)
        unset = st_cmp.pop("unset", None)
        if unset is None:
            fail("P7: SkipText 缺 0=\"unset\" 分支（skip 额外要求 0=未设置）")
        elif unset != 0:
            fail("P7: SkipText 的 unset 分支值 = 0x%X，必须 = 0" % unset)
        if st_cmp != skip_auth:
            key_bad = {k: (st_cmp[k], skip_auth[k]) for k in set(st_cmp) & set(skip_auth)
                       if st_cmp[k] != skip_auth[k]}
            fail("P7: SkipText 名字->值 与权威码表不一致（值错=%s 多=%s 缺=%s）"
                 % (key_bad, sorted(set(st_cmp) - set(skip_auth)),
                    sorted(set(skip_auth) - set(st_cmp))))
        skip_ok = (unset == 0 and st_cmp == skip_auth)
    infos.append("P7 why 表 %d 名 / skip 表 %d 名 / WhyText=%s / SkipText=%s"
                 % (len(why_auth), len(skip_auth),
                    "OK" if wt == why_auth else "MISMATCH",
                    "OK" if skip_ok else "MISMATCH"))

    # ---------- 输出 ----------
    print("== check_ctl_codes (Gate25) ==")
    for i in infos:
        print("  [ok] " + i)
    for f in fails:
        print("[FAIL] " + f)
    print("result: %s (%d fail, 0 warn)" % ("FAIL" if fails else "PASS", len(fails)))
    return 1 if fails else 0


def rbctl_vals_raw(text, sym):
    """rbctl.c 自身若定义了 CTL_CODE 宏（当前没有），从它取值；否则 None。"""
    ex = ioctl_abi(text, sym)
    if ex is None:
        return None
    dt, func, method, access = ex
    return (dt << 16) | (access << 14) | (func << 2) | method


def forward_chain_vals(text, name_map):
    """RbFeatDispatch 转发链上的功能值集合：返回 (vals, err)。

    err 非 None 时调用方必须 FAIL —— 本脚本的判据是「空匹配集一律 FAIL」，
    锚点被改写或守卫块解析不出时返回空 set 就等于静默放行。

    守卫条件不再用 rfind("if (") 这类位置启发式（链内出现巢状 if 就把窗口指错），
    改为从锚点向前做括号配平，取**真正包住调用**的最近一个 `if (...)`；并要求
    该块的开括号与调用之间没有任何语句（否则中间插一个 fail-fast if 就能让
    直连码看起来"已在链上"）。
    """
    m = None
    for cand in re.finditer(r"RbFeatDispatch\s*\(\s*irp\s*\)", text):
        m = cand
    if m is None:
        return set(), "找不到 RbFeatDispatch(irp) 锚点"
    pos = m.start()

    depth = 0
    brace = -1
    i = pos - 1
    while i >= 0:
        c = text[i]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                brace = i
                break
            depth -= 1
        i -= 1
    if brace < 0:
        return set(), "RbFeatDispatch(irp) 不在任何 { } 块内"

    between = text[brace + 1:pos].strip()
    if between != "return":
        return set(), ("RbFeatDispatch(irp) 与守卫块起始之间不是单纯的 `return`（%r）—— "
                       "守卫条件无法确定" % between[:80])

    j = brace - 1
    while j >= 0 and text[j].isspace():
        j -= 1
    if j < 0 or text[j] != ")":
        return set(), "RbFeatDispatch(irp) 的守卫块不是 if (...) { 形式"

    depth = 0
    k = j
    while k >= 0:
        if text[k] == ")":
            depth += 1
        elif text[k] == "(":
            depth -= 1
            if depth == 0:
                break
        k -= 1
    if k < 0:
        return set(), "守卫条件括号不配平"
    if not re.search(r"\bif\s*$", text[:k]):
        return set(), "RbFeatDispatch(irp) 最近一层守卫不是 if 语句"
    cond = text[k + 1:j]

    vals, unresolved = set(), []
    for mm in re.finditer(r"\bcode\s*==\s*(\w+)", cond):
        v = name_map.get(mm.group(1))
        if v is None:
            unresolved.append(mm.group(1))
        else:
            vals.add(v)
    if unresolved:
        return vals, "转发链条件里 %s 无法求值" % sorted(set(unresolved))
    if not vals:
        return set(), "转发链条件里没解析到任何 code == <ioctl>（空集不得判 PASS）"
    return vals, None


if __name__ == "__main__":
    sys.exit(main())
