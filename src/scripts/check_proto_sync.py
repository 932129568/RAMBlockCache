#!/usr/bin/env python3
"""check_proto_sync.py —— 协议三副本字段级比对 + 版本五联动门禁（本机可跑，无需编译器）。

背景（full-audit-report-2026-10-15.md 优化意见 #3）：
  协议权威源 ctl/rb_proto.h（Protocol 9，ABI 冻结）在 C# 侧有两份手工副本：
    ① src/ui/controller/RAMBlockCache.Controller/Models/Models.cs（当前 UI）
    ② src/ui/controller/.../Services/DriverService.cs（internal structs）。
    [140 B3 2026-11-10] 第三副本 src/ui/wpf/Models/RbProto.cs 已随 legacy WPF
    归档到 archive/legacy-wpf-ui/，本门禁从三副本降为两副本比对。
  "本次一致"是运气也是债务——本脚本把字段级比对固化为门禁，任一偏移/类型/SizeConst
  漂移即 FAIL。

校验内容：
  A. 常量：IOCTL 0x222000 / DataOffset=16 / MsgSize=7704 / MaxVolumes=8 / VolLen=4 /
     Stripes=64 / NameLen=16 / LogMax=64 / LogTextMax=48 / ProtocolVersion=9 /
     消息号 1..12 / 写模式 0/1/2 / RB_DEFAULT_WRITEBACK=0
  B. 结构体字段序列（按 Pack=8 逐字段计算偏移，与 C #pragma pack(8) 同规则，
     支持标量 / 定长数组 / 嵌套结构数组 / 匿名 union 取最大成员）：
     RB_CONFIG / RB_STATS / RB_VOLUME_INFO / RB_VOLUME_LIST / RB_VOLUME_SET /
     RB_VOLUME_ATTACH / RB_VERSION / RB_LOG_REC / RB_LOG_LIST / RB_MSG 信封
  C. sizeof(RB_MSG) 必须等于 7704（16 信封 + 最大联合成员 RB_LOG_LIST=7688）
  D. 版本五联动：version.h ↔ DriverService.ExpectedDriverVer ↔ Version.cs ↔
     Controller.csproj ↔ README.md（+payload README 模板）↔ RAMBlockCache.inf

用法: python scripts/check_proto_sync.py [--strict]
退出码: 0=PASS（可带 WARN）, 1=FAIL
已知限制（WARN 级，不阻断）：旧代 ui/wpf 副本缺 PANIC(11)/PANIC_CLEAR(12) 常量，
  属"旧 WPF 整体归档"决策范围（fix-changelog §5），--strict 可升格为 FAIL。
"""
import re
import sys
from pathlib import Path

# ── 路径定位 ────────────────────────────────────────────────────────────────
HERE = Path(__file__).resolve()
ROOT = HERE.parents[1] if HERE.parent.name == "scripts" else HERE.parent
if (ROOT / "ctl" / "rb_proto.h").exists():
    SRC = ROOT
elif (ROOT / "src" / "ctl" / "rb_proto.h").exists():
    SRC = ROOT / "src"
else:
    print(f"[FATAL] 找不到 ctl/rb_proto.h（cwd={Path.cwd()}）")
    sys.exit(1)

PROTO_H = SRC / "ctl" / "rb_proto.h"
MODELS_CS = SRC / "ui" / "controller" / "RAMBlockCache.Controller" / "Models" / "Models.cs"
DRVSVC_CS = SRC / "ui" / "controller" / "RAMBlockCache.Controller" / "Services" / "DriverService.cs"
# [140 B3] legacy third copy archived; two-copy comparison remains authoritative.
# WPF_CS = SRC / "ui" / "wpf" / "Models" / "RbProto.cs"
VERSION_H = SRC / "common" / "version.h"
VERSION_CS = SRC / "ui" / "controller" / "RAMBlockCache.Controller" / "Version.cs"
CSPROJ = SRC / "ui" / "controller" / "RAMBlockCache.Controller" / "RAMBlockCache.Controller.csproj"
# [140 B3 2026-11-10] version strings live in README.md (canonical doc); the
# tracked root README.txt is now a package-time generated artifact. Check both
# when present: README.md always, template if the repo still carries it.
README = SRC.parent / "README.md"
README_TEMPLATE = SRC / "ui" / "installer" / "RAMBlockCache.Setup" / "Assets" / "README.payload.template.txt"
INF = SRC / "driver" / "RAMBlockCache.inf"
RB_COMMON_H = SRC / "driver" / "rb_common.h"
RB_FEATURE_CS = SRC / "ui" / "controller" / "RAMBlockCache.Controller" / "Models" / "RbFeature.cs"
RB_FEATCTL_C = SRC / "ctl" / "rbfeatctl.c"
# [162 G-B 2026-11-12] Rust 桥接的信封缓冲。161 号 §1.4：本文件从未被任何门禁读过
# （`grep -rn "lib\.rs" src/scripts .github` 零命中），union 成员下次变大时 Rust 侧
# 会静默截断——它手工写着 8224，而 8224 正是本脚本 :344 动态算出的 RB_MSG_TOTAL。
RUST_LIB = SRC / "ui" / "web" / "bridge" / "native" / "src" / "lib.rs"
# [185 A1 2026-11-12] 第二家族：184 §1.2 指出 RB_STATS2 / RB2_VOLSTATUS_LIST /
# RB_BOOTCFG / RB_LOGQ_* 的布局复刻点从未被任何门禁逐字段比过。
RB_COMMON2_H = SRC / "driver" / "rb_common2.h"
RB_BOOTCFG_H = SRC / "ctl" / "rb_bootcfg.h"
RB_LOGQ_H = SRC / "ctl" / "rb_logq.h"
BRIDGE_INDEX_TS = SRC / "ui" / "web" / "bridge" / "server" / "index.ts"
LOG_SINK_TS = SRC / "ui" / "web" / "bridge" / "server" / "log-sink.ts"

fails: list[str] = []
warns: list[str] = []

# ── 覆盖下界（[185 A2]）────────────────────────────────────────────────────
# 184 §3 D3：期望上界/下界不得由被测数据派生，否则"删掉最后一项"时上下界跟着
# 缩小 → 自指校验恒真。以下常量独立于 rb_proto.h 的当前内容，只随"协议发布"
# 手动提升。
PROTO_STRUCT_FLOOR = 14   # rb_proto.h typedef struct 数（V10.6：14）
MIRROR_ENTRY_FLOOR = 13   # mirror 表条目数（9 既有 + 4 event/invalidate）
# [187] 下界原为 9，而 V10.6 实际是 10 项 ⇒ 删掉任一"非最大成员"（RB_EVENT_QUERY
# 等）后 len=9 仍满足下界，union 大小由 RB_EVENT_LIST 决定也不变 ⇒ 整条变异 PASS。
# 下界必须独立于当前内容且等于已发布成员数。
UNION_MEMBER_FLOOR = 10   # RB_MSG.Data union 成员数（V10.6：10）


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def strip_comments_c(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def strip_comments_cs(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)   # 块注释（协议副本大量使用）
    text = re.sub(r"///.*", "", text)
    return re.sub(r"//.*", "", text)


# ── Pack=8 布局引擎 ─────────────────────────────────────────────────────────
# 字段 key：'u8'/'u4'/'u2' 标量，或 'S:<StructName>' 嵌套结构
TSIZE = {"u8": 8, "u4": 4, "u2": 2, "u1": 1}


def align_up(off: int, al: int) -> int:
    return (off + al - 1) // al * al


class Layout:
    """Pack=8 布局计算器。structs: {name: fields}，fields=[(fname, key, count)]"""

    def __init__(self, structs: dict, pack: int = 8):
        self.structs = structs
        self.pack = pack
        self._memo: dict[str, int] = {}
        self._align: dict[str, int] = {}

    def _field_align(self, key: str) -> int:
        # [187] pack 必须**逐字段**生效：`#pragma pack(n)` 的语义是每个成员的对齐
        # 上限被压到 n。旧实现只把 pack 用在"结构尾部对齐"上，字段对齐永远是自然
        # 对齐 ⇒ Layout(pool, 1) 与 Layout(pool, 8) 算出完全相同的 sizeof，
        # 任何 pack 漂移都静默通过（实测 RB_LOGQ_REC 在 pack=1 下仍是 224）。
        if key.startswith("S:"):
            return min(self._struct_align(key[2:]), self.pack)
        return min(TSIZE[key], self.pack)

    def _struct_align(self, name: str) -> int:
        if name not in self._align:
            inner = max((self._field_align(k) for _, k, _ in self.structs[name]), default=1)
            self._align[name] = min(inner, self.pack)
        return self._align[name]

    def _field_size(self, key: str, count: int) -> int:
        if key.startswith("S:"):
            return self.struct_size(key[2:]) * count
        return TSIZE[key] * count

    def struct_size(self, name: str) -> int:
        if name not in self._memo:
            if name not in self.structs:
                raise KeyError(f"未知结构 {name}")
            off, maxal = 0, 1
            for _, k, cnt in self.structs[name]:
                al = self._field_align(k)
                off = align_up(off, al) + self._field_size(k, cnt)
                maxal = max(maxal, al)
            self._memo[name] = align_up(off, min(maxal, self.pack))
        return self._memo[name]

    def field_layout(self, name: str) -> list[tuple[str, int, str, int]]:
        """[(字段名, 偏移, key, 元素数)]——宽度/数组长度参与比对（184 §3 D5）。"""
        out, off = [], 0
        for fname, k, cnt in self.structs[name]:
            al = self._field_align(k)
            off = align_up(off, al)
            out.append((fname, off, k, cnt))
            off += self._field_size(k, cnt)
        return out

    def field_offsets(self, name: str) -> list[tuple[str, int]]:
        return [(n, o) for n, o, _, _ in self.field_layout(name)]


# ── A0. 通用 C 结构体解析引擎（fail-closed，[185 A1/A3] 2026-11-12）──────────
# 184 §3 D1/D4：上一版解析器 `if not fm: continue`——遇到 uint8_t / 位域 / #if 一律
# 静默丢行且不推进 offset，等于"布局漂移也全绿"。这里改为：无法识别的行 = ParseError
# = FAIL。任何"拿到 0 条字段"也 FAIL（本仓库有 $null 被算 1 元素导致假 PASS 的先例）。
class ParseError(Exception):
    """结构体解析失败：一律记 FAIL，绝不跳过。"""


class UnknownType(ParseError):
    """字段类型尚未出现在标量表/已解析结构表里——可延后重试（可能是后置定义）。"""

    def __init__(self, ty: str):
        super().__init__(ty)
        self.ty = ty


def parse_defines(text: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for m in re.finditer(r"#define\s+(\w+)\s+(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*[ \t]*$",
                         text, re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def _split_top_level(text: str, sep: str) -> list[str]:
    """按 sep 切分，忽略 () [] {} 内部的分隔符（嵌套匿名结构/数组表达式安全）。"""
    out, buf, depth = [], [], 0
    for ch in text:
        if ch in "{([":
            depth += 1
        elif ch in "})]":
            depth -= 1
        if ch == sep and depth == 0:
            out.append("".join(buf))
            buf = []
        else:
            buf.append(ch)
    out.append("".join(buf))
    return out


def _eval_const(expr: str, consts: dict, ctx: str) -> int:
    """仅接受 数字 / 已知常量 / +-*() —— 其余一律 FAIL（不做 Python eval 裸跑）。"""
    expr = expr.strip()
    if not expr:
        raise ParseError(f"{ctx}: 空数组长度")
    if re.fullmatch(r"0[xX][0-9A-Fa-f]+|\d+[uUlL]*", expr):
        return int(expr.rstrip("uUlL"), 0)
    if not re.fullmatch(r"[\w\s*+\-()]+", expr):
        raise ParseError(f"{ctx}: 无法求值的数组长度 {expr!r}")
    parts = []
    for tok in re.findall(r"\w+|\*|\+|-|\(|\)", expr):
        if tok.isdigit():
            parts.append(str(int(tok)))
        elif re.fullmatch(r"\w+", tok):
            if tok not in consts:
                raise ParseError(f"{ctx}: 数组长度引用未知常量 {tok!r}")
            parts.append(str(consts[tok]))
        else:
            parts.append(tok)
    try:
        return int(eval("".join(parts)))          # 字符集已限定：数字/运算符/括号
    except Exception:
        raise ParseError(f"{ctx}: 数组长度表达式求值失败 {expr!r}")


def _dims_count(dims: str, consts: dict, ctx: str) -> int:
    cnt = 1
    for d in re.findall(r"\[([^\]]*)\]", dims):
        cnt *= _eval_const(d, consts, ctx)
    return cnt


def header_pack(text: str, ctx: str) -> int:
    """解析 `#pragma pack(push, N)` / `#pragma pack(N)`，返回该头的对齐值。

    [187] 旧版把 pack=8 写死在 Layout 默认参数里，四个头的 `#pragma pack(push, 8)`
    从未被读取——把它改成 4/1，门禁逐字段结果一字不变（实测 PASS）。ABI 契约是
    Pack=8（C# 侧 StructLayout Pack=8 / 默认 8），所以：出现非 8 的显式 pack 即
    FAIL（布局同时按该值重算，报错里的 sizeof 才是真实的）。无指令时按 Windows
    默认 8（rb_common2.h 自述"两侧默认对齐一致，无需 pack"）。"""
    vals = sorted({int(x) for x in re.findall(
        r"#pragma\s+pack\s*\(\s*(?:push\s*,\s*)?(\d+)\s*\)", text)})
    if len(vals) > 1:
        fails.append(f"{ctx} 同时出现多个 #pragma pack 值 {vals}"
                     f"（无法静态判定，fail-closed）")
        return vals[0]
    if vals and vals[0] != 8:
        fails.append(f"{ctx} #pragma pack({vals[0]}) != 8（ABI 契约 Pack=8；"
                     f"每字段对齐上限随之改变，逐字段偏移整体漂移）")
        return vals[0]
    return 8


def parse_struct_typedefs(text: str) -> dict[str, str]:
    """枚举 `typedef struct [tag] { ... } Name[, *PName];` → {首名字: 内部文本}。
    用括号配对扫描（支持匿名嵌套结构），不用正则截断；配对失败抛 ParseError。
    该枚举**独立于字段解析**：字段解析失败时覆盖检查仍然看得见结构体名。"""
    out: dict[str, str] = {}
    for m in re.finditer(r"typedef\s+struct\b", text):
        open_ = text.find("{", m.end())
        semi = text.find(";", m.end())
        if open_ == -1 or (semi != -1 and semi < open_):
            continue                       # `typedef struct X Y;` 前向声明，无定义体
        depth, j = 0, open_
        while j < len(text):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if depth != 0:
            raise ParseError(f"typedef struct 括号不配对（offset {m.start()}）")
        end = text.find(";", j)
        if end == -1:
            raise ParseError(f"typedef struct 缺 ';'（offset {m.start()}）")
        names = [p.strip().lstrip("*").strip() for p in text[j + 1:end].split(",")]
        names = [n for n in names if re.fullmatch(r"\w+", n)]
        if not names:
            raise ParseError(f"typedef struct 无结构名: {text[m.start():end][:60]!r}")
        out[names[0]] = text[open_ + 1:j]
    return out


def _type_key(ty: str, scalar: dict, pool: dict) -> str:
    base = ty[7:].strip() if ty.startswith("struct ") else ty
    if base in scalar:
        return scalar[base]
    if base in pool:
        return f"S:{base}"
    raise UnknownType(base)


def _declarators(text: str, key: str, consts: dict, ctx: str) -> list[tuple[str, str, int]]:
    out = []
    for piece in _split_top_level(text, ","):
        p = piece.strip()
        m = re.match(r"^(\w+)\s*((?:\[[^\]]*\])*)$", p)
        if not m:
            raise ParseError(f"{ctx}: 无法识别的声明 {p!r}")
        out.append((m.group(1), key, _dims_count(m.group(2), consts, f"{ctx}.{m.group(1)}")))
    return out


def _parse_one_struct(sname: str, body: str, consts: dict, scalar: dict,
                      pool: dict, ctx: str) -> list[tuple[str, str, int]]:
    fields: list[tuple[str, str, int]] = []
    for raw in _split_top_level(body, ";"):
        s = " ".join(raw.split())
        if not s:
            continue
        if s.startswith("#"):
            raise ParseError(f"含预处理器行 {s!r}（#if/#pragma 无法静态求布局）")
        if re.search(r"\w\s*:\s*\d", s):
            raise ParseError(f"含位域 {s!r}（位域宽度/分配顺序不可静态复算）")
        if "=" in s:
            raise ParseError(f"含初始化值 {s!r}")
        if s.startswith(("struct", "union")):
            i = s.find("{")
            if i < 0:
                raise ParseError(f"无法识别的字段行 {s!r}")
            depth, j = 0, i
            while j < len(s):
                if s[j] == "{":
                    depth += 1
                elif s[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if depth != 0:
                raise ParseError(f"匿名嵌套结构括号不配对 {s!r}")
            inner, rest = s[i + 1:j], s[j + 1:].strip()
            nm = re.match(r"(\w+)", rest)
            sub = f"{sname}.{nm.group(1) if nm else 'Anon'}"
            if sub in pool:
                raise ParseError(f"匿名嵌套结构重名 {sub}")
            pool[sub] = _parse_one_struct(sub, inner, consts, scalar, pool, ctx)
            fields.extend(_declarators(rest, f"S:{sub}", consts, ctx))
            continue
        decls = _split_top_level(s, ",")
        m = re.match(r"^([A-Za-z_][\w \t]*?)\s+(\w+)\s*((?:\[[^\]]*\])*)$", decls[0].strip())
        if not m:
            raise ParseError(f"无法识别的字段行 {s!r}（fail-closed，不跳过）")
        ty = " ".join(m.group(1).split())
        key = _type_key(ty, scalar, pool)
        fields.append((m.group(2), key,
                       _dims_count(m.group(3), consts, f"{ctx} {sname}.{m.group(2)}")))
        for extra in decls[1:]:
            fields.extend(_declarators(extra, key, consts, ctx))
    if not fields:
        raise ParseError("结构体 0 字段（空解析不得视为通过）")
    return fields


def parse_c_header_from_text(text: str, scalar: dict, ctx: str):
    """解析一段 C ABI 源文本 → (consts, {struct: fields}, {struct: body}, pack)。"""
    consts = parse_defines(text)
    pack = header_pack(text, ctx)
    # [187] `#if 0` 包住整个 typedef 时，括号扫描仍然看得见它（枚举不退化、字段也
    # 照常解析），但该结构在 C 侧根本不参与编译 ⇒ 门禁对着一段"不存在的 ABI"比对。
    # 头文件里没有合法的 #if 0 用法（只有 include guard 与 _KERNEL_MODE 的 include
    # 选择），所以一律 FAIL 而不是猜。
    if re.search(r"#\s*if\s+0\b", text):
        fails.append(f"{ctx} 含 `#if 0` 条件禁用段（无法静态判定哪些结构参与编译，"
                     f"fail-closed）")
    try:
        bodies = parse_struct_typedefs(text)
    except ParseError as e:
        # [187] 旧版裸抛：括号不配对 / 无结构名会变成 Python traceback（虽然仍非 0
        # 退出，但不带上下文、也不算"门禁结论"）。改为结构化 FAIL。
        fails.append(f"{ctx} 结构体枚举失败（fail-closed）: {e}")
        bodies = {}
    pool: dict[str, list[tuple[str, str, int]]] = {}
    pending = list(bodies.items())
    unknown: dict[str, str] = {}
    while pending:
        nxt = []
        for name, body in pending:
            try:
                pool[name] = _parse_one_struct(name, body, consts, scalar, pool, ctx)
            except UnknownType as e:
                unknown[name] = e.ty
                nxt.append((name, body))
            except ParseError as e:
                fails.append(f"{ctx} 结构 {name} 解析失败（fail-closed）: {e}")
                pool[name] = [("__parse_failed__", "u1", 1)]
        if len(nxt) == len(pending):
            break
        pending = nxt
    for name, _ in pending:
        fails.append(f"{ctx} 结构 {name} 解析失败：字段类型 {unknown.get(name, '?')!r} "
                     f"不在标量表、也不是已定义结构（fail-closed）")
        pool[name] = [("__parse_failed__", "u1", 1)]
    return consts, pool, bodies, pack


def parse_c_header(path: Path, scalar: dict, ctx: str):
    """读取并解析一个 C ABI 头（文件缺失即 FAIL，不做 `if exists else ''`）。"""
    if not path.exists():
        fails.append(f"缺 ABI 权威头: {ctx}（{path.name}）")
        return {}, {}, {}, 8
    return parse_c_header_from_text(strip_comments_c(read(path)), scalar, ctx)


# ── A. C 权威源解析（ctl/rb_proto.h）────────────────────────────────────────
# rb_proto.h 的标量白名单是**契约白名单**：头文件自述"布局仅用
# uint32_t/uint64_t/wchar_t"。因此 uint8_t/uint16_t 一律 FAIL 而不是当 u1/u2
# 放行——放行等于允许未评审的宽度进入冻结 ABI。
SCALAR_PROTO = {"uint32_t": "u4", "int32_t": "u4", "uint64_t": "u8", "wchar_t": "u2"}


def parse_c_proto():
    s = strip_comments_c(read(PROTO_H))
    consts = parse_defines(s)

    # RB_MSG 的 Data 是匿名 union：先摘出成员清单（用于动态算 union 大小 +
    # 覆盖断言），再从待解析文本移除——否则 typedef 扫描会把 union 当嵌套结构
    # 求和而不是取最大。
    union_members: list[str] = []
    um = re.search(r"union\s*\{(.*?)\}\s*Data\s*;", s, re.S)
    if not um:
        fails.append("rb_proto.h RB_MSG: 未找到 `union { ... } Data;`（门禁锚点消失，fail-closed）")
    else:
        for raw in _split_top_level(um.group(1), ";"):
            t = " ".join(raw.split())
            if not t:
                continue
            mm = re.match(r"^([A-Za-z_][\w \t]*?)\s+(\w+)$", t)
            if not mm:
                fails.append(f"rb_proto.h RB_MSG union 成员无法解析: {t!r}（fail-closed）")
                continue
            union_members.append(" ".join(mm.group(1).split()))
        s = s[:um.start()] + s[um.end():]

    _consts2, structs, bodies, pack = parse_c_header_from_text(s, SCALAR_PROTO, "rb_proto.h")
    for ty in union_members:
        base = ty[7:].strip() if ty.startswith("struct ") else ty
        if base not in structs:
            fails.append(f"rb_proto.h RB_MSG union 引用了未定义结构 {base}")
    return consts, structs, bodies, union_members, pack


# ── B. C# 副本解析 ───────────────────────────────────────────────────────────
CS_TMAP = {"uint": "u4", "int": "u4", "ulong": "u8", "long": "u8",
           "ushort": "u2", "short": "u2", "char": "u2", "byte": "u1"}


def parse_cs_structs(path: Path):
    """解析 [StructLayout(...)] struct。支持多名字字段行 / MarshalAs SizeConst /
    嵌套结构数组 / nullable 数组（`T[]?`）。返回 {CsName: fields}。

    [185 A3] 改括号配对扫描：旧正则 `(.*?)\\n\\}` 要求闭合大括号顶格，而
    DriverService.cs 的嵌套 struct 闭合在 4 空格缩进——正则会把后续代码一起吞掉
    或整段漏解析（漏解析 = 静默少一个镜像）。未知类型仍以 `?T` 记录，由比对侧
    FAIL（不静默当 u4）。"""
    s = strip_comments_cs(read(path))
    # 文件内 C# 常量（RbP.Stripes / RbProtoDefs.RB_STRIPES / 硬编码 64 等）
    local: dict[str, int] = {}
    for m in re.finditer(r"const\s+(?:int|uint)\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", s):
        local[m.group(1)] = int(m.group(2), 0)

    found: dict[str, list[tuple[str, str, int]]] = {}

    for m in re.finditer(r"\[StructLayout\(([^)]*)\)\]", s):
        rest = s[m.end():]
        hm = re.match(r"\s*(?:(?:public|internal|private|protected|partial|readonly|"
                      r"unsafe)\s+)*struct\s+(\w+)\s*", rest)
        if not hm:
            continue
        sname = hm.group(1)
        # [187] 旧版把 StructLayout 参数整段丢弃：Pack 改成 4/1、或改成
        # LayoutKind.Explicit，C# 侧真实布局已变而门禁仍按 Sequential+8 复算 ⇒ PASS。
        # C 侧契约是 Pack=8（见 header_pack），这里必须显式核对；未写 Pack 时按
        # 平台默认 8（x64）接受。
        attr = m.group(1)
        if "LayoutKind.Sequential" not in attr:
            fails.append(f"{path.name} struct {sname}: 非 LayoutKind.Sequential"
                         f"（{attr.strip()[:60]}），布局无法逐字节复算（fail-closed）")
        pm = re.search(r"Pack\s*=\s*(\d+)", attr)
        if pm and int(pm.group(1)) != 8:
            fails.append(f"{path.name} struct {sname}: StructLayout Pack={pm.group(1)} "
                         f"!= 8（C 侧按 Pack=8 复算，逐字段偏移会漂移）")
        brace = rest.find("{", hm.end())
        if brace < 0:
            continue
        depth, j = 0, brace
        while j < len(rest):
            if rest[j] == "{":
                depth += 1
            elif rest[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if depth != 0:
            fails.append(f"{path.name} struct {sname}: 大括号不配对（fail-closed）")
            continue
        found[sname] = _parse_cs_body(rest[brace + 1:j], local, found,
                                      f"{path.name} {sname}")
    return found


def _parse_cs_body(body: str, local: dict, found: dict, ctx: str):
    fields: list[tuple[str, str, int]] = []

    def ev(x: str) -> int:
        x = x.strip().rstrip("uUlL")
        if re.fullmatch(r"0x[0-9A-Fa-f]+|\d+", x):
            return int(x, 0)
        short = x.split(".")[-1]
        if short in local:
            return local[short]
        raise KeyError(x)

    for seg in _split_top_level(body, ";"):
        seg = seg.strip()
        if not seg:
            continue
        attrs = ""
        while seg.startswith("["):
            i = seg.find("]")
            if i < 0:
                break
            attrs += seg[:i + 1]
            seg = seg[i + 1:].strip()
        if not seg:
            continue
        # 方法 / 属性 / 表达式体 / 嵌套类型 / 常量：不是布局字段
        if any(t in seg for t in ("(", "{", "=>", "const ", "event ")):
            continue
        sm = re.match(r"(?:(?:public|internal|private|protected|readonly|static|unsafe)\s+)*"
                      r"([\w\.]+)(\[\])?\??\s+(.+)$", seg, re.S)
        if not sm:
            fields.append((f"__unparsed_{len(fields)}__", "?unparsed", 1))
            continue
        ty, is_arr, rest = sm.group(1), sm.group(2), " ".join(sm.group(3).split())
        cnt = 1
        if is_arr:
            sc = re.search(r"SizeConst\s*=\s*([^,\)]+)", attrs)
            if not sc:
                fields.append((rest.split(",")[0].strip(), f"?{ty}", -1))
                continue
            expr = sc.group(1).strip()
            try:
                cnt = 1
                for p in expr.split("*"):
                    cnt *= ev(p)
            except KeyError:
                fields.append((rest.split(",")[0].strip(), f"?{expr}", -1))
                continue
        for one in rest.split(","):
            one = one.split("=")[0].strip()
            if not one:
                continue
            one = one.rstrip("?").strip()
            if ty in CS_TMAP:
                fields.append((one, CS_TMAP[ty], cnt))
            elif ty in found:                  # 嵌套结构（定义顺序即依赖顺序）
                fields.append((one, f"S:{ty}", cnt))
            else:
                fields.append((one, f"?{ty}", cnt))
    return fields


# ── 逐字段比对（C ↔ 非 C 副本）──────────────────────────────────────────────
def cmp_struct(cname: str, lay_c: "Layout", s_c: str, lay_s: "Layout", s_s: str,
               label: str, depth: int = 0) -> int:
    """比 (字段名, 偏移, 宽度/是否嵌套, 元素数) 四项，嵌套结构递归比对。
    返回实际比对的字段组数。任何不一致 → fails.append（带结构体名与文件名）。"""
    if depth > 6:
        fails.append(f"{cname}↔{label}: 嵌套深度 >6，疑似循环引用（fail-closed）")
        return 0
    prefix = f"{cname}↔{label}"
    co = lay_c.field_layout(s_c)
    so = lay_s.field_layout(s_s)
    if len(co) != len(so):
        fails.append(f"{prefix} 字段数不同: C={len(co)} {[x[0] for x in co]} vs "
                     f"C#={len(so)} {[x[0] for x in so]}")
        return 0
    n = 0
    for (cn, coff, ck, ccnt), (sn, soff, sk, scnt) in zip(co, so):
        if cn != sn:
            fails.append(f"{prefix} 字段名/顺序漂移: C[{cn}] vs C#[{sn}]")
        if coff != soff:
            fails.append(f"{prefix} 字段 {cn} 偏移漂移: C={coff} vs C#={soff}")
        if ck != sk:
            if ck.startswith("S:") and sk.startswith("S:"):
                pass
            else:
                fails.append(f"{prefix} 字段 {cn} 宽度/类型漂移: C={ck} vs C#={sk}")
        if ccnt != scnt:
            fails.append(f"{prefix} 字段 {cn} 数组长度漂移: C={ccnt} vs C#={scnt}")
        n += 1
        if ck.startswith("S:") and sk.startswith("S:"):
            n += cmp_struct(f"{cname}.{cn}", lay_c, ck[2:], lay_s, sk[2:], label, depth + 1)
    csz, ssz = lay_c.struct_size(s_c), lay_s.struct_size(s_s)
    if csz != ssz:
        fails.append(f"{prefix} 结构大小漂移: C={csz} vs C#={ssz}")
    return n


# ── 第二家族 ABI 副本（[185 A1] 184 §1.2 #2-#10）────────────────────────────
# 这些头不在 rb_proto.h 里，但都是"权威头 + 手工副本"同型结构：
#   RB_STATS2 / RB2_VOLSTATUS_LIST → rbfeatctl.c + DriverService.cs（C# 可逐字段比）
#   RB_BOOTCFG                     → lib.rs BOOTCFG_SIZE + index.ts Buffer.alloc/偏移
#   RB_LOGQ_REQ/REC/RESP           → lib.rs LOGQ_* + log-sink.ts HDR/REC/手写偏移
# 非 C 侧只有"数值/偏移"可比（Rust 不解析结构、TS 手写 read*LE），字段名不可比。
SCALAR_WIN = {
    "uint32_t": "u4", "int32_t": "u4", "uint64_t": "u8", "wchar_t": "u2",
    "ULONG": "u4", "LONG": "u4", "DWORD": "u4", "ULONG32": "u4", "ULONG64": "u8",
    "LONG64": "u8", "SHORT": "u2", "USHORT": "u2", "UCHAR": "u1", "WCHAR": "u2",
    "BOOLEAN": "u1",
}
ABI2_TABLES = {
    "driver/rb_common2.h": {
        "RB_STATS2": ("DriverService.cs", "RbStats2Native"),
        "RB2_VOLSTATUS_LIST": ("DriverService.cs", "RbVolStatusListNative"),
    },
    "ctl/rb_bootcfg.h": {"RB_BOOTCFG": None},
    "ctl/rb_logq.h": {"RB_LOGQ_REQ": None, "RB_LOGQ_REC": None, "RB_LOGQ_RESP": None},
}
ABI2_FLOORS = {"driver/rb_common2.h": 2, "ctl/rb_bootcfg.h": 1, "ctl/rb_logq.h": 3}
# 显式豁免（逐条理由；豁免也必须列名，不许静默）。理由一律基于"没有第二份副本"，
# 而不是"暂时不想比"——后者应当补比对表。
ABI2_EXEMPT = {
    # [187] 理由原文写"仅 RbPolGetFor/rbfeat.c 使用"，实测另有 rbpref.c:194
    # （RbPolCached）与 rbwback.c:73 各声明 RB2_POL 局部量——但两者都在 driver/，
    # 用户态仍无副本，结论不变，故只订正事实描述、不撤豁免。
    "RB2_POL": "驱动内部跨模块结构（rb_common2.h:107，rbfeat.c/rbpref.c/rbwback.c 使用），"
               "不跨 IOCTL、不落线路，用户态无副本（grep RB2_POL 仅命中 driver/）",
    # [V10.5 2026-11-18] 0x906 只读关机诊断的输出结构。
    # 它确实有第二份副本，但那份是 **C**（ctl/rbfeatctl.c 的 SHUTDIAG），不是 C# ——
    # 本表只能表达「C ↔ C#」家族（非 C 侧只比数值/偏移），所以不适用。
    # 该 C↔C 对由 check_ctl_codes.py（Gate25）P5 逐字段名/顺序/偏移/总长比对，
    # 并在驱动侧另有编译期 24 B 断言 `RB2_SHUTDOWN_DIAG_OUT_IS_24_BYTES`。
    # WPF 侧（DriverService.cs）至今没有 0x906 消费者；**将来若加镜像，必须从本
    # 豁免转登记到 ABI2_TABLES**，否则 C# 侧的字段漂移不在任何门禁视野内。
    "RB2_SHUTDOWN_DIAG_OUT": "0x906 只读诊断输出（V10.5 §3.1）。第二份副本是 C 侧 "
                             "ctl/rbfeatctl.c:SHUTDIAG，不属本表的 C↔C# 家族；该对由 "
                             "Gate25 P5 逐字段/偏移/24B 比对。C# 侧无消费者。",
}


def _rust_const(rb: str, name: str, depth: int = 0) -> int:
    """求值 lib.rs 的 `const NAME: usize = <expr>;`（只认数字/其他 const/+-*/）。"""
    m = re.search(rf"const\s+{name}\s*:\s*\w+\s*=\s*([^;]+);", rb)
    if not m:
        raise KeyError(name)
    expr = m.group(1).strip()
    parts = []
    for tok in re.findall(r"\w+|\d+|\*|\+|-|\(|\)", expr):
        if tok.isdigit():
            parts.append(tok)
        elif re.fullmatch(r"\w+", tok):
            if depth > 5:
                raise KeyError(tok)
            parts.append(str(_rust_const(rb, tok, depth + 1)))
        else:
            parts.append(tok)
    return int(eval("".join(parts)))


def check_abi_copies(cs_drvs: dict):
    """返回 (比对组数, {头文件: (consts, pool, bodies, pack)})——pool 复用给数值比对，
    避免二次解析重复刷 FAIL。"""
    n = 0
    pools: dict[str, tuple] = {}
    all_bodies: set[str] = set()
    for rel, table in ABI2_TABLES.items():
        consts, pool, bodies, pack = parse_c_header(SRC / rel, SCALAR_WIN, rel)
        pools[rel] = (consts, pool, bodies, pack)
        all_bodies |= set(bodies)
        floor = ABI2_FLOORS[rel]
        if len(bodies) < floor:
            fails.append(f"{rel} 只枚举到 {len(bodies)} 个 typedef struct，"
                         f"低于下界 {floor}（枚举退化，fail-closed）")
        for nm in sorted(set(bodies) - set(table) - set(ABI2_EXEMPT)):
            fails.append(f"{rel} 结构 {nm} 未纳入第二家族比对表、也未登记豁免"
                         f"（新增结构不许静默放过）")
        for nm, why in sorted(ABI2_EXEMPT.items()):
            if not why.strip():
                fails.append(f"第二家族豁免 {nm} 缺理由字符串")
        for nm in sorted(table):
            if nm not in bodies:
                fails.append(f"{rel} 比对表登记的 {nm} 不存在（表项过期/改名）")
        lay = Layout(pool, pack)
        for nm, mir in table.items():
            if mir is None or nm not in pool:
                continue
            src, want = mir
            got = cs_drvs.get(want)
            if got is None:
                fails.append(f"{src} 缺 {nm} 的镜像（期望类型 {want}）")
                continue
            bad = [f"{x}->{k}" for x, k, _ in got if k.startswith("?")]
            if bad:
                fails.append(f"{src} {want} 含无法解析的类型/SizeConst: {bad}")
                continue
            n += 1
            cmp_struct(f"{rel}:{nm}", lay, nm, Layout(cs_drvs), want, src)
    # [187] 豁免表决不能只查"理由非空"：头里已经没有这个结构时条目就是死条目，
    # 下次同名结构出现会被它静默豁免（MIRROR_EXEMPT 早已有存在性校验，这里补齐）。
    for nm in sorted(ABI2_EXEMPT):
        if nm not in all_bodies:
            fails.append(f"第二家族豁免表登记的 {nm} 在三个权威头中都不存在"
                         f"（表项过期，先删豁免再看它是否真的没有副本）")
    return n, pools


def check_numeric_copies(pools: dict) -> None:
    """RB_BOOTCFG / RB_LOGQ_*：C sizeof/偏移 ↔ Rust 常量 / TS 常量与手写偏移。"""
    # ---- RB_BOOTCFG ↔ lib.rs BOOTCFG_SIZE、index.ts Buffer.alloc + 读写偏移 ----
    if "ctl/rb_bootcfg.h" in pools:
        _c, pool, _b, _pk = pools["ctl/rb_bootcfg.h"]
        if "RB_BOOTCFG" in pool:
            lay = Layout(pool, _pk)
            sz = lay.struct_size("RB_BOOTCFG")
            off = {fn: (o, k, c) for fn, o, k, c in lay.field_layout("RB_BOOTCFG")}
            if sz != 80:
                fails.append(f"RB_BOOTCFG sizeof 实算={sz}，契约 80（rb_bootcfg.h 布局变了吗）")
            rb = read(RUST_LIB) if RUST_LIB.exists() else ""
            try:
                got = _rust_const(rb, "BOOTCFG_SIZE")
                if got != sz:
                    fails.append(f"lib.rs BOOTCFG_SIZE={got} != sizeof(RB_BOOTCFG)={sz}")
            except KeyError:
                fails.append("lib.rs: const BOOTCFG_SIZE 未找到（门禁锚点消失，fail-closed）")
            ts = read(BRIDGE_INDEX_TS) if BRIDGE_INDEX_TS.exists() else ""
            if not ts:
                fails.append("缺桥接源 ui/web/bridge/server/index.ts")
            else:
                i = ts.find("/rb/bootcfg")
                if i < 0:
                    fails.append("index.ts: 未找到 /rb/bootcfg 路由（门禁锚点消失）")
                else:
                    sec = ts[i:i + 1600]
                    allocs = [int(x) for x in re.findall(r"Buffer\.alloc\((\d+)\)", sec)]
                    if not allocs:
                        fails.append("index.ts bootcfg 段未找到 Buffer.alloc(<数字>)（锚点消失）")
                    for a in allocs:
                        if a != sz:
                            fails.append(f"index.ts bootcfg Buffer.alloc({a}) != "
                                         f"sizeof(RB_BOOTCFG)={sz}")
                    w = [int(x) for x in re.findall(r"writeUInt32LE\([^,]+,\s*(\d+)\)", sec)]
                    want_w = [off["Get"][0], off["AutoAttach"][0], off["ResumeOnBoot"][0]]
                    if sorted(set(w)) != sorted(want_w):
                        fails.append(f"index.ts bootcfg 写偏移 {sorted(set(w))} != "
                                     f"C 侧 {sorted(want_w)}（Get/AutoAttach/ResumeOnBoot）")
                    lo, lk, lc = off["ResumeLetters"]
                    want_utf = f"utf16le', {lo}, {lo} + {TSIZE[lk] * lc})"
                    if want_utf not in sec:
                        fails.append(f"index.ts bootcfg 未按 `{want_utf}` 读 ResumeLetters"
                                     f"（期望偏移 {lo}、{TSIZE[lk] * lc} 字节）")

    # ---- RB_LOGQ_* ↔ lib.rs LOGQ_*、log-sink.ts HDR/REC/TEXT_WCHARS + 手写偏移 ----
    if "ctl/rb_logq.h" in pools:
        consts, pool, _b, _pk = pools["ctl/rb_logq.h"]
        if all(x in pool for x in ("RB_LOGQ_REQ", "RB_LOGQ_REC", "RB_LOGQ_RESP")):
            lay = Layout(pool, _pk)
            rec = lay.struct_size("RB_LOGQ_REC")
            resp = lay.struct_size("RB_LOGQ_RESP")
            roff = {fn: (o, k, c) for fn, o, k, c in lay.field_layout("RB_LOGQ_REC")}
            hdr = {fn: (o, k, c) for fn, o, k, c in lay.field_layout("RB_LOGQ_RESP")}["Items"][0]
            rb = read(RUST_LIB) if RUST_LIB.exists() else ""
            for name, want in (("LOGQ_BATCH", consts.get("RB_LOGQ_BATCH")),
                               ("LOGQ_REC_SIZE", rec), ("LOGQ_RESP_SIZE", resp)):
                try:
                    got = _rust_const(rb, name)
                except KeyError:
                    fails.append(f"lib.rs: const {name} 未找到（门禁锚点消失，fail-closed）")
                    continue
                if want is None:
                    fails.append(f"{name}: rb_logq.h 常量缺失，无法比对")
                elif got != want:
                    fails.append(f"lib.rs {name}={got} != C 侧 {want}")
            # rb_logq.h 自己的硬编码宏（自述 16/224）也必须与实算一致
            raw = read(RB_LOGQ_H) if RB_LOGQ_H.exists() else ""
            for macro, want in (("RB_LOGQ_HDR_SIZE", hdr), ("RB_LOGQ_REC_SIZE", rec)):
                m = re.search(rf"#define\s+{macro}\s+\(\(unsigned long\)(\d+)\)", raw)
                if not m:
                    fails.append(f"rb_logq.h 未找到 #define {macro}（锚点消失）")
                elif int(m.group(1)) != want:
                    fails.append(f"rb_logq.h {macro}={m.group(1)} != 实算 {want}")
            ls = read(LOG_SINK_TS) if LOG_SINK_TS.exists() else ""
            if not ls:
                fails.append("缺桥接源 ui/web/bridge/server/log-sink.ts")
            else:
                text_off, text_key, text_cnt = roff["Text"]
                for name, want in (("HDR", hdr), ("REC", rec), ("TEXT_WCHARS", text_cnt)):
                    m = re.search(rf"const\s+{name}\s*=\s*(\d+)\s*;", ls)
                    if not m:
                        fails.append(f"log-sink.ts 未找到 const {name}（锚点消失）")
                    elif int(m.group(1)) != want:
                        fails.append(f"log-sink.ts {name}={m.group(1)} != C 侧 {want}")
                for fn, (o, k, c) in roff.items():
                    fn_rw = {"Seq": "readBigUInt64LE", "Ticks": "readBigUInt64LE",
                             "Level": "readUInt32LE", "Src": "readUInt32LE",
                             "Status": "readInt32LE"}.get(fn)
                    if fn_rw is None:
                        continue
                    pat = rf"{fn_rw}\(off\)" if o == 0 else rf"{fn_rw}\(off \+ {o}\)"
                    if not re.search(pat, ls):
                        fails.append(f"log-sink.ts decodePage 未按 `{fn_rw}(off + {o})` 读 "
                                     f"RB_LOGQ_REC.{fn}（手写偏移漂移）")
                if not re.search(rf"off \+ {text_off} \+ c \* 2", ls):
                    fails.append(f"log-sink.ts decodePage 未按 `off + {text_off} + c * 2` 读 "
                                 f"RB_LOGQ_REC.Text（手写偏移漂移）")


# ── 主流程 ─────────────────────────────────────────────────────────────────
def main():
    strict = "--strict" in sys.argv
    print("== check_proto_sync ==")
    print(f"   src root = {SRC}")

    consts, cstructs, c_bodies, union_members, pack_c = parse_c_proto()
    c_lay = Layout(cstructs, pack_c)
    print(f"   rb_proto.h: {len(consts)} 常量, {len(cstructs)} 结构体 "
          f"({', '.join(sorted(cstructs))})")

    # ── A. 常量核对 ──
    expect_consts = {
        "RB_MAX_VOLUMES": 8, "RB_VOL_LEN": 4, "RB_STRIPES": 64, "RB_NAME_LEN": 16,
        "RB_LOG_MAX": 64, "RB_LOG_TEXT_MAX": 48, "RB_PROTOCOL_VERSION": 9,
        "RB_WRITEMODE_THROUGH": 0, "RB_WRITEMODE_BACK": 1, "RB_WRITEMODE_GLOBAL": 2,
        "RB_DEFAULT_WRITEBACK": 0,
    }
    for k, v in expect_consts.items():
        if consts.get(k) != v:
            fails.append(f"rb_proto.h 常量 {k}={consts.get(k)}，期望 {v}")
    msgs = {}
    for m in re.finditer(r"#define\s+RB_MSG_(\w+)\s+(\d+)", read(PROTO_H)):
        msgs[m.group(1)] = int(m.group(2))
    # [G10 2026-11-11] 原为 range(1,14) 硬编码上界：14 号消息
    # RB_MSG_GET_EVENTS 自 V10.6/B-7 加入后**从未被校验**。
    #
    # 第一版修法（只用 max(msgs) 派生上界）经变异测试证明是**自指校验**：
    # 删掉最大号后 max 跟着降，expected 随之缩小，missing 恒空 → 仍 PASS。
    # 因此必须同时锚定一个**独立于被测数据**的下界：
    #   MSG_FLOOR = 已发布的协议消息数（含 GET_EVENTS）。
    # 它不随 rb_proto.h 变化，删除任何已发布消息都会被抓住；
    # 而新增消息（> MSG_FLOOR）只需连续，不必改这个常量。
    MSG_FLOOR = 14     # V10.6/B-7：RB_MSG_GET_EVENTS=14 为当前协议上界
    if not msgs:
        fails.append("rb_proto.h 未解析到任何 RB_MSG_* 消息号")
    else:
        top = max(max(msgs.values()), MSG_FLOOR)
        expected = set(range(1, top + 1))
        missing = sorted(expected - set(msgs.values()))
        if missing:
            fails.append(f"rb_proto.h 消息号不连续/缺失: 缺 {missing}"
                         f"（MSG_FLOOR={MSG_FLOOR}，实测上界={max(msgs.values())}）")
        dup = sorted({v for v in msgs.values() if list(msgs.values()).count(v) > 1})
        if dup:
            fails.append(f"rb_proto.h 消息号重复: {dup}")
    ioctl = ((consts.get("RB_IOCTL_TYPE", 0) << 16)
             | (consts.get("RB_FILE_ANY_ACCESS", 0) << 14)
             | (consts.get("RB_IOCTL_FUNCTION_REQUEST", 0) << 2)
             | consts.get("RB_METHOD_BUFFERED", 0))
    if ioctl != 0x222000:
        fails.append(f"IOCTL_RB_REQUEST 计算值 0x{ioctl:X} != 0x222000")

    # ── C. sizeof(RB_MSG) ──
    if "RB_MSG" in cstructs:
        env_sz = c_lay.struct_size("RB_MSG")     # union 摘除后仅剩信封 16
        # [185 A3] union 候选改为**从 rb_proto.h 解析出的成员清单**：旧版是 10 项
        # 硬编码元组，等价于第二张白名单——新增 union 成员但忘了改元组时，门禁
        # 会用它算不出最大值而静默放行（这正是 2026-10-24 那次 8224/7704 失同步的
        # 成因）。现在成员来自头文件本身，只有"解析不到"才 FAIL。
        union_sz, union_max = 0, ""
        for cand in union_members:
            sz = c_lay.struct_size(cand)
            if sz > union_sz:
                union_sz, union_max = sz, cand
        RB_MSG_TOTAL = env_sz + union_sz
        print(f"   sizeof(RB_MSG) = 信封{env_sz} + union{union_sz} = {RB_MSG_TOTAL}"
              f"（最大成员 {union_max}；union {len(union_members)} 项）")
        if len(union_members) < UNION_MEMBER_FLOOR:
            fails.append(f"RB_MSG union 只解析到 {len(union_members)} 项，低于下界 "
                         f"{UNION_MEMBER_FLOOR}（成员清单退化，sizeof 会算小）")
    else:
        RB_MSG_TOTAL = 0
        fails.append("rb_proto.h 未解析到 RB_MSG 信封")

    # ── C2. Rust 桥接信封容量（[162 G-B]）────────────────────────────────
    # BUF_SIZE 是 native 侧唯一一处"把 sizeof(RB_MSG) 写死"的地方；必须等于上面
    # 动态算出的 RB_MSG_TOTAL，否则 union 变大后 Rust 侧静默截断：它按小值申请
    # 缓冲、按小值把长度交给 DeviceIoControl。
    if not RUST_LIB.exists():
        fails.append(f"缺 Rust 桥接源: {RUST_LIB.relative_to(SRC)}")
    else:
        rb = read(RUST_LIB)
        mb = re.search(r"const\s+BUF_SIZE\s*:\s*usize\s*=\s*(\d+)\s*;", rb)
        if not mb:
            fails.append("lib.rs: const BUF_SIZE 未找到（门禁锚点消失，不得静默跳过）")
        elif RB_MSG_TOTAL and int(mb.group(1)) != RB_MSG_TOTAL:
            fails.append(f"lib.rs BUF_SIZE={mb.group(1)} != C 侧 "
                         f"sizeof(RB_MSG)={RB_MSG_TOTAL}")
        else:
            print(f"   lib.rs BUF_SIZE = {mb.group(1)}（== sizeof(RB_MSG)）")

    # ── B. 两副本字段级比对（[140 B3] legacy RbProto.cs 已归档）──
    # C 结构 → (Controller Models.cs 类型, Controller DriverService.cs 类型)
    # [185 A1 2026-11-12] 补 4 条：184 §1.2 #10 指出 Models.cs:144-188 早已存在
    # 这四个镜像，但 mirror 表里没有 ⇒ 从未被逐字段比过。
    mirror = {
        "RB_CONFIG":       ("RbConfigNative", None),
        "RB_STATS":        ("RbStats", "RbStats"),
        "RB_VOLUME_INFO":  ("RbVolumeInfo", "RbVolumeItem"),
        "RB_VOLUME_LIST":  ("RbVolumeList", "RbVolumeList"),
        "RB_VOLUME_SET":   (None, "RbVolumeSet"),
        "RB_VOLUME_ATTACH": ("RbVolumeAttach", "RbAttach"),
        "RB_VERSION":      ("RbVersionNative", "RbVersion"),
        "RB_LOG_REC":      (None, "RbLogRec"),
        "RB_LOG_LIST":     (None, "RbLogList"),
        "RB_INVALIDATE_RANGE": ("RbInvalidateRangeNative", None),
        "RB_EVENT_REC":    ("RbEventRecNative", None),
        "RB_EVENT_QUERY":  ("RbEventQueryNative", None),
        "RB_EVENT_LIST":   ("RbEventListNative", None),
    }
    # 显式豁免：每条都写理由（不许静默排除）
    MIRROR_EXEMPT = {
        "RB_MSG": "16B 信封(rb_proto.h:331-354)：Type/Size/Status/Reserved 与 Data "
                  "union 无独立 C# 镜像——DataOffset=16、MsgSize=sizeof(RB_MSG) 由 C/C2/D "
                  "段逐个常量守，union 成员由 UNION_MEMBER_FLOOR + 成员存在性守",
    }

    # ── B0. 反向全覆盖断言（[185 A2]）──────────────────────────────────
    # 184 §1.3：mirror 是白名单 ⇒ rb_proto.h 加第 15 个结构体既不 FAIL 也不 WARN。
    # 这里用 parse_struct_typedefs 的**独立枚举**（只看括号配对，不依赖字段解析
    # 是否成功）与 mirror ∪ 豁免做差集，差集非空即 FAIL。
    proto_names = set(c_bodies)
    if len(proto_names) < PROTO_STRUCT_FLOOR:
        fails.append(f"rb_proto.h 只枚举到 {len(proto_names)} 个 typedef struct，"
                     f"低于下界 {PROTO_STRUCT_FLOOR}（枚举退化，fail-closed）")
    if len(mirror) < MIRROR_ENTRY_FLOOR:
        fails.append(f"mirror 表只有 {len(mirror)} 条，低于下界 {MIRROR_ENTRY_FLOOR}"
                     f"（覆盖表退化，fail-closed）")
    for nm in sorted(proto_names - set(mirror) - set(MIRROR_EXEMPT)):
        fails.append(f"rb_proto.h 结构 {nm} 未纳入镜像比对、也未登记豁免："
                     f"新增结构体必须补 mirror 表或写豁免理由（不许静默放过）")
    for nm in sorted(set(mirror) - proto_names):
        fails.append(f"mirror 表登记的 {nm} 在 rb_proto.h 中不存在（表项过期或改名）")
    for nm, why in sorted(MIRROR_EXEMPT.items()):
        if not why.strip():
            fails.append(f"豁免表 {nm} 缺理由字符串")
        if nm not in proto_names:
            fails.append(f"豁免表 {nm} 在 rb_proto.h 中不存在（表项过期）")

    if not MODELS_CS.exists():
        fails.append(f"缺 C# 副本: {MODELS_CS.relative_to(SRC)}（不得按空池放行）")
    if not DRVSVC_CS.exists():
        fails.append(f"缺 C# 副本: {DRVSVC_CS.relative_to(SRC)}（不得按空池放行）")
    cs_models = parse_cs_structs(MODELS_CS) if MODELS_CS.exists() else {}
    cs_drvs = parse_cs_structs(DRVSVC_CS) if DRVSVC_CS.exists() else {}
    print(f"   Models.cs: {len(cs_models)} structs; DriverService.cs: {len(cs_drvs)}")

    n_cmp = 0
    for cname, (m_name, d_name) in mirror.items():
        if cname not in cstructs:
            fails.append(f"rb_proto.h 缺结构 {cname}（mirror 表要求存在）")
            continue
        for src, want, pool in (("Models.cs", m_name, cs_models),
                                ("DriverService.cs", d_name, cs_drvs)):
            if want is None:
                continue                      # 该副本按设计无此镜像
            got = pool.get(want)
            if got is None:
                fails.append(f"{src} 缺 {cname} 的镜像（期望类型 {want}）")
                continue
            bad = [f"{n}->{k}" for n, k, _ in got if k.startswith("?")]
            if bad:
                fails.append(f"{src} {want} 含无法解析的 SizeConst/类型: {bad}")
                continue
            try:
                cs_lay_obj = Layout(pool)
                n_cmp += 1
                cmp_struct(cname, c_lay, cname, cs_lay_obj, want, src)
            except KeyError as e:
                fails.append(f"{src} {want} 布局计算失败: {e}")
    print(f"   字段级比对完成: {n_cmp} 组镜像核对")

    # ── B2. 第二家族 ABI 副本（[185 A1] 184 §1.2 #2-#10）───────────────
    n2, pools2 = check_abi_copies(cs_drvs)
    check_numeric_copies(pools2)
    print(f"   第二家族: {n2} 组 C↔C# 镜像 + BOOTCFG/LOGQ 数值副本核对")

    # 信封字段（Type/Size/Status/Reserved/DataOffset）在 C# 侧以常量/RbMsg 表达
    for label, path in (("Models.cs", MODELS_CS),):
        if not path.exists():
            continue                          # 缺失已在 B0 段 FAIL，不重复
        s = read(path)
        off_m = re.search(r"(?:DataOffset|RbMsgDataOffset)\s*=\s*(\d+)", s)
        sz_m = re.search(r"(?:MsgSize|RbMsgSize)\s*=\s*(\d+)", s)
        # [185 A3] 锚点缺失原为 WARN（非 strict 下等于放行）；这两个常量是
        # 信封唯一副本，消失即覆盖归零 → 改 FAIL。
        if not off_m:
            fails.append(f"{label} 未找到 DataOffset 常量（门禁锚点消失）")
        elif int(off_m.group(1)) != 16:
            fails.append(f"{label} DataOffset={off_m.group(1)} != 16")
        if not sz_m:
            fails.append(f"{label} 未找到 MsgSize 常量（门禁锚点消失）")
        elif RB_MSG_TOTAL and int(sz_m.group(1)) != RB_MSG_TOTAL:
            fails.append(f"{label} MsgSize={sz_m.group(1)} != C 侧 sizeof(RB_MSG)={RB_MSG_TOTAL}")
        io = re.search(r"IOCTL_RB_REQUEST\s*=\s*(0x[0-9A-Fa-f]+|\d+)", s)
        if not io:
            fails.append(f"{label} 未找到 IOCTL_RB_REQUEST 常量（门禁锚点消失）")
        elif int(io.group(1), 0) != 0x222000:
            fails.append(f"{label} IOCTL_RB_REQUEST={io.group(1)} != 0x222000")
        have = set()
        for m in re.finditer(r"(?:RB_MSG_|MSG_)?(GET_STATS|SET_CONFIG|FLUSH|GET_CONFIG|"
                             r"EXIT_SAFE_MODE|SET_VOLUME|GET_VOLUMES|ATTACH_VOLUME|"
                             r"GET_LOG|GET_VERSION|PANIC|PANIC_CLEAR|INVALIDATE_RANGE|"
                             r"GET_EVENTS)\s*=\s*(\d+)", s):
            have.add((m.group(1), int(m.group(2))))
        expect_msgs = {("GET_STATS", 1), ("SET_CONFIG", 2), ("FLUSH", 3), ("GET_CONFIG", 4),
                       ("EXIT_SAFE_MODE", 5), ("SET_VOLUME", 6), ("GET_VOLUMES", 7),
                       ("ATTACH_VOLUME", 8), ("GET_LOG", 9), ("GET_VERSION", 10),
                       ("PANIC", 11), ("PANIC_CLEAR", 12),
                       ("INVALIDATE_RANGE", 13),   # [V10.6/B-5] 末尾追加
                       ("GET_EVENTS", 14)}         # [V10.6/B-7] 末尾追加（[FIX 2026-10-24] 补入检查）
        extra = {x for x in have if x not in expect_msgs}
        if extra:
            fails.append(f"{label} 消息号越界: {sorted(extra)}")
        missing = expect_msgs - have
        if missing:
            fails.append(f"{label} 缺消息常量 {sorted(missing)}")

    # DriverService 消息号（私有常量 MSG_*，含 "MSG_A = 1, MSG_B = 2" 同行形式）
    if DRVSVC_CS.exists():
        s = read(DRVSVC_CS)
        have = {int(m.group(1)) for m in re.finditer(r"MSG_\w+\s*=\s*(\d+)", s)}
        # [G10 2026-11-11] 同上：上界从 rb_proto.h 实测最大值派生，不再写死 14。
        # 原 range(1,14) 漏掉 MSG_GET_EVENTS=14，删除该常量不会被发现。
        upper = max(msgs.values()) if msgs else 13
        for n in range(1, upper + 1):
            if n not in have:
                fails.append(f"DriverService.cs 缺消息号 {n}")

    # ── D. 版本五联动 ──
    print("   -- 版本五联动 --")
    vh = read(VERSION_H) if VERSION_H.exists() else ""
    ver = {}
    for k in ("MAJOR", "MINOR", "PATCH", "BUILD"):
        m = re.search(rf"#define\s+RB_VERSION_{k}\s+(\d+)", vh)
        ver[k] = m.group(1) if m else None
    machine = f"{ver['MAJOR']}.{ver['MINOR']}.{ver['PATCH']}.{ver['BUILD']}"
    print(f"   version.h 机器版本串 = {machine}")
    if not all(ver.values()):
        fails.append(f"version.h 版本字段缺失: {ver}")

    rd = (read(README) if README.exists() else "") + \
         (read(README_TEMPLATE) if README_TEMPLATE.exists() else "")
    if machine not in rd:
        fails.append(f"README.md/模板 未见机器版本串 {machine}")
    if "V10.5.0 Build 19" not in rd:
        fails.append("README.md/模板 未见发布名 V10.5.0 Build 19")

    # [G9 2026-11-11] 以下三处原为 `read(X) if X.exists() else ""`：
    # 文件被删/改名时读成空串，后续所有 re.search 全部落空，整段版本联动
    # 检查**静默消失**而门禁仍 PASS。改为先断言文件存在（缺失即 FAIL），
    # 再做内容比对——"文件不在"必须比"内容不对"更早暴露。
    if not VERSION_CS.exists():
        fails.append(f"缺版本联动文件: {VERSION_CS.relative_to(SRC)}")
    vc = read(VERSION_CS) if VERSION_CS.exists() else ""
    if vc:
        m = re.search(r'Version\s*=\s*"([\d.]+)"', vc)
        if not m or m.group(1) != machine:
            fails.append(f"Version.cs AppInfo.Version={m.group(1) if m else '缺失'} != {machine}")
        m = re.search(r'ReleaseName\s*=\s*"([^"]+)"', vc)
        if not m or m.group(1) != "V10.5.0 Build 19":
            fails.append(f"Version.cs ReleaseName={m.group(1) if m else '缺失'} != V10.5.0 Build 19")

    cp = read(CSPROJ) if CSPROJ.exists() else ""
    if cp:
        m = re.search(r"<Version>([\d.]+)</Version>", cp)
        if not m or m.group(1) != machine:
            fails.append(f"Controller.csproj Version={m.group(1) if m else '缺失'} != {machine}")

    inf = read(INF) if INF.exists() else ""
    if inf:
        m = re.search(r"DriverVer\s*=\s*(.+)", inf)
        if not m or machine not in m.group(1):
            fails.append(f"RAMBlockCache.inf DriverVer 未见 {machine}")

    bh = read(RB_COMMON_H) if RB_COMMON_H.exists() else ""
    dmaj = re.search(r"#define\s+RB_DRIVER_MAJOR\s+(\d+)", bh)
    dmin = re.search(r"#define\s+RB_DRIVER_MINOR\s+(\d+)", bh)
    if dmaj and dmin and ver["BUILD"]:
        expect_edv = f"{dmaj.group(1)}.{dmin.group(1)} Build{ver['BUILD']}"
        ds = read(DRVSVC_CS) if DRVSVC_CS.exists() else ""
        m = re.search(r'ExpectedDriverVer\s*=\s*"([^"]+)"', ds)
        if not m or m.group(1) != expect_edv:
            fails.append(f"DriverService.ExpectedDriverVer={m.group(1) if m else '缺失'} "
                         f"!= 规则值 {expect_edv}（version.h BUILD={ver['BUILD']}）")
        else:
            print(f"   ExpectedDriverVer = {expect_edv}（驱动上报格式，软校验）")

    # XAML 硬编码版本文本残留（P2-10 应收敛到绑定）
    for xaml in (SRC / "ui" / "controller").rglob("*.xaml"):
        s = read(xaml)
        if "V10.5.0 Build 19" in s:
            warns.append(f"{xaml.relative_to(SRC)} 仍含硬编码发布名"
                         f"（应收敛到 ProductVersion 绑定）")

    # VOLSTATUS reason 6-10：数值唯一权威 rb_common.h，UI/CLI 文案同波同步；布局不改。
    reason_defs = {
        "RB_BYPASS_STREAM_GEOMETRY": 6,
        "RB_BYPASS_DIRECTION": 7,
        "RB_BYPASS_IO_TOO_LARGE": 8,
        "RB_BYPASS_ADMIT": 9,
        "RB_BYPASS_RESOURCE": 10,
    }
    # [G9 2026-11-11] 同上：这两份文件缺失会让 reason 6-10 的 UI/CLI 联动
    # 全段静默消失（空串匹配不到任何 `=>` / `case`，但也不会 FAIL）。
    if not RB_FEATURE_CS.exists():
        fails.append(f"缺 VOLSTATUS UI 文案源: {RB_FEATURE_CS.relative_to(SRC)}")
    if not RB_FEATCTL_C.exists():
        fails.append(f"缺 VOLSTATUS CLI 文案源: {RB_FEATCTL_C.relative_to(SRC)}")
    feature_text = read(RB_FEATURE_CS) if RB_FEATURE_CS.exists() else ""
    featctl_text = read(RB_FEATCTL_C) if RB_FEATCTL_C.exists() else ""
    for name, value in reason_defs.items():
        if not re.search(rf"#define\s+{name}\s+{value}\b", bh):
            fails.append(f"VOLSTATUS reason 权威值缺失/漂移: {name} != {value}")
        if not re.search(rf"\b{value}\s*=>", feature_text):
            fails.append(f"UI VolReasonText 缺少 reason {value}")
        if not re.search(rf"case\s+{value}\s*:", featctl_text):
            fails.append(f"CLI VolReasonText 缺少 reason {value}")

    # ── 汇总 ──
    # [G9 2026-11-11] --strict 此前是空开关：`strict = "--strict" in sys.argv`
    # 赋值后全文再无引用，带与不带输出逐字节相同。而 build.ps1:334 与 CI
    # Gate2 都传了 --strict，等于双方都以为 WARN 会升 FAIL，实际不会。
    # 现在实现该语义：strict 模式下 WARN 并入 fails（fail-closed）。
    if strict and warns:
        for w in warns:
            fails.append(f"[strict] {w}")
    for w in warns:
        print(f"[WARN] {w}")
    for f in fails:
        print(f"[FAIL] {f}")
    print(f"result: {'PASS' if not fails else 'FAIL'} ({len(fails)} fail, {len(warns)} warn)")
    if fails:
        print("门禁语义：ABI 漂移类 FAIL 必须闭环；WARN 为已知限制，登记 report/ 跟踪。")
    sys.exit(1 if fails else 0)


def _merged_cs_pool(*pools):
    out = {}
    for p in pools:
        out.update(p)
    return out


if __name__ == "__main__":
    main()
