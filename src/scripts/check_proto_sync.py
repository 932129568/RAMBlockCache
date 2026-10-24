#!/usr/bin/env python3
"""check_proto_sync.py —— 协议三副本字段级比对 + 版本五联动门禁（本机可跑，无需编译器）。

背景（full-audit-report-2026-10-15.md 优化意见 #3）：
  协议权威源 ctl/rb_proto.h（Protocol 9，ABI 冻结）在 C# 侧有两份手工副本：
    ① src/ui/controller/RAMBlockCache.Controller/Models/Models.cs（当前 UI）
    ② src/ui/wpf/Models/RbProto.cs（旧代 UI，归档决策前保留）
    另有一份内部镜像 src/ui/controller/.../Services/DriverService.cs（internal structs）。
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
     Controller.csproj ↔ README.txt ↔ RAMBlockCache.inf

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
WPF_CS = SRC / "ui" / "wpf" / "Models" / "RbProto.cs"
VERSION_H = SRC / "common" / "version.h"
VERSION_CS = SRC / "ui" / "controller" / "RAMBlockCache.Controller" / "Version.cs"
CSPROJ = SRC / "ui" / "controller" / "RAMBlockCache.Controller" / "RAMBlockCache.Controller.csproj"
README = SRC.parent / "README.txt"
INF = SRC / "driver" / "RAMBlockCache.inf"
RB_COMMON_H = SRC / "driver" / "rb_common.h"

fails: list[str] = []
warns: list[str] = []


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def strip_comments_c(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


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

    def __init__(self, structs: dict):
        self.structs = structs
        self._memo: dict[str, int] = {}
        self._align: dict[str, int] = {}

    def _field_align(self, key: str) -> int:
        if key.startswith("S:"):
            return self._struct_align(key[2:])
        return TSIZE[key]

    def _struct_align(self, name: str) -> int:
        if name not in self._align:
            self._align[name] = max((self._field_align(k) for _, k, _ in self.structs[name]),
                                    default=1)
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
            self._memo[name] = align_up(off, maxal)
        return self._memo[name]

    def field_offsets(self, name: str) -> list[tuple[str, int]]:
        out, off = [], 0
        for fname, k, cnt in self.structs[name]:
            al = self._field_align(k)
            off = align_up(off, al)
            out.append((fname, off))
            off += self._field_size(k, cnt)
        return out


# ── A. C 权威源解析（ctl/rb_proto.h）────────────────────────────────────────
def parse_c_proto():
    s = strip_comments_c(read(PROTO_H))
    consts: dict[str, int] = {}
    for m in re.finditer(r"#define\s+(RB_\w+)\s+(0x[0-9A-Fa-f]+|\d+)", s):
        v = m.group(2)
        consts[m.group(1)] = int(v, 16 if v.lower().startswith("0x") else 10)

    # RB_MSG 的 Data 是匿名 union：先整体摘除（union 大小由主流程按最大成员计算），
    # 否则 typedef 正则会被 union 的右括号提前截断。
    s_no_union = re.sub(r"union\s*\{.*?\}\s*\w+\s*;", "", s, flags=re.S)

    scalar = {"uint32_t": "u4", "int32_t": "u4", "uint64_t": "u8", "wchar_t": "u2"}

    # 迭代到不动点：基础标量结构先解析，含嵌套结构的随后（RB_VOLUME_INFO→RB_VOLUME_LIST 等）
    structs: dict[str, list[tuple[str, str, int]]] = {}
    pending = []
    for m in re.finditer(r"typedef\s+struct\s+\w+\s*\{(.*?)\}\s*(\w+)\s*;", s_no_union, re.S):
        pending.append((m.group(2), m.group(1)))
    changed = True
    while pending and changed:
        changed = False
        rest = []
        for name, body in pending:
            fields, ok = [], True
            for line in body.split(";"):
                line = line.strip()
                if not line:
                    continue
                fm = re.match(r"(\w+)\s+(\w+)\s*(?:\[([^\]]+)\])?(?:\[([^\]]+)\])?\s*$", line)
                if not fm:
                    continue
                ty, fname, d1, d2 = fm.groups()
                cnt = 1
                if d1:
                    def cv(x):
                        x = x.strip()
                        return consts[x] if x in consts else int(x, 0)
                    cnt = cv(d1) * (cv(d2) if d2 else 1)
                if ty in scalar:
                    fields.append((fname, scalar[ty], cnt))
                elif ty in structs:            # 嵌套结构（可带数组）
                    fields.append((fname, f"S:{ty}", cnt))
                else:
                    ok = False
                    break
            if ok and fields:
                structs[name] = fields
                changed = True
            else:
                rest.append((name, body))
        pending = rest
    if pending:
        for name, _ in pending:
            fails.append(f"rb_proto.h 结构 {name} 解析失败（依赖未满足）")
    return consts, structs


# ── B. C# 副本解析 ───────────────────────────────────────────────────────────
CS_TMAP = {"uint": "u4", "int": "u4", "ulong": "u8", "long": "u8",
           "ushort": "u2", "short": "u2", "char": "u2", "byte": "u1"}


def parse_cs_structs(path: Path):
    """解析 [StructLayout(Pack=8)] struct。支持多名字字段行 / MarshalAs SizeConst /
    嵌套结构数组。返回 {CsName: fields}。定义顺序即依赖顺序（协议副本现状）。"""
    s = strip_comments_cs(read(path))
    # 文件内 C# 常量（RbP.Stripes / RbProtoDefs.RB_STRIPES / 硬编码 64 等）
    local: dict[str, int] = {}
    for m in re.finditer(r"const\s+(?:int|uint)\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", s):
        local[m.group(1)] = int(m.group(2), 0)

    found: dict[str, list[tuple[str, str, int]]] = {}

    for m in re.finditer(
            r"\[StructLayout\([^)]*\)\]\s*(?:public|internal)\s+struct\s+(\w+)\s*\{(.*?)\n\}",
            s, re.S):
        cs_name, body = m.group(1), m.group(2)
        fields: list[tuple[str, str, int]] = []
        # 按 ; 切段；每段可能带前导 [MarshalAs(...)] 属性
        for seg in body.split(";"):
            seg = seg.strip()
            if not seg:
                continue
            sm = re.match(r"(?:\[[^\]]*\]\s*)*(?:public|internal)\s+(\w+)(\[\])?\s+(.+)$",
                          seg, re.S)
            if not sm:
                continue
            ty, is_arr, rest = sm.groups()
            rest = " ".join(rest.split())          # 折行展开
            # 跳过方法 / expression-bodied 属性（含 ( { => 的声明不是字段）
            if any(ch in rest for ch in ("(", "{", "=>")):
                continue
            # SizeConst（仅数组有意义）
            cnt = 1
            if is_arr:
                sc = re.search(r"SizeConst\s*=\s*([^\)]+)\)", seg)
                if not sc:
                    fields.append((rest.split(",")[0].strip(), f"?{ty}", -1))
                    continue
                expr = sc.group(1).strip()

                def ev(x):
                    x = x.strip().rstrip("u")
                    if re.fullmatch(r"0x[0-9A-Fa-f]+|\d+", x):
                        return int(x, 0)
                    short = x.split(".")[-1]
                    if short in local:
                        return local[short]
                    if short.isdigit():
                        return int(short)
                    raise KeyError(x)

                try:
                    if "*" in expr:
                        cnt = 1
                        for p in expr.split("*"):
                            cnt *= ev(p)
                    else:
                        cnt = ev(expr)
                except KeyError:
                    fields.append((rest.split(",")[0].strip(), f"?{expr}", -1))
                    continue
            # 多名字展开（去掉可能的 "= 初值"）
            for one in rest.split(","):
                one = one.split("=")[0].strip()
                if not one:
                    continue
                if ty in CS_TMAP:
                    fields.append((one, CS_TMAP[ty], cnt))
                elif ty in found:                  # 嵌套结构（迭代到不动点由外层循环保证）
                    fields.append((one, f"S:{ty}", cnt))
                else:
                    fields.append((one, f"?{ty}", cnt))
        if fields:
            found[cs_name] = fields
    return found


# ── 主流程 ─────────────────────────────────────────────────────────────────
def main():
    strict = "--strict" in sys.argv
    print("== check_proto_sync ==")
    print(f"   src root = {SRC}")

    consts, cstructs = parse_c_proto()
    c_lay = Layout(cstructs)
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
    for n in range(1, 14):   # [V10.6/B-5] 消息 13 = RB_MSG_INVALIDATE_RANGE（末尾追加）
        if n not in msgs.values():
            fails.append(f"rb_proto.h 缺消息号 {n}")
    ioctl = ((consts.get("RB_IOCTL_TYPE", 0) << 16)
             | (consts.get("RB_FILE_ANY_ACCESS", 0) << 14)
             | (consts.get("RB_IOCTL_FUNCTION_REQUEST", 0) << 2)
             | consts.get("RB_METHOD_BUFFERED", 0))
    if ioctl != 0x222000:
        fails.append(f"IOCTL_RB_REQUEST 计算值 0x{ioctl:X} != 0x222000")

    # ── C. sizeof(RB_MSG) ──
    if "RB_MSG" in cstructs:
        env_sz = c_lay.struct_size("RB_MSG")     # union 摘除后仅剩信封 16
        union_sz = 0
        for cand in ("RB_LOG_LIST", "RB_STATS", "RB_CONFIG", "RB_VOLUME_LIST",
                     "RB_VOLUME_SET", "RB_VOLUME_ATTACH", "RB_VERSION"):
            if cand in cstructs:
                union_sz = max(union_sz, c_lay.struct_size(cand))
        total = env_sz + union_sz
        print(f"   sizeof(RB_MSG) = 信封{env_sz} + union{union_sz} = {total}"
              f"（最大成员 RB_LOG_LIST）")
        if total != 7704:
            fails.append(f"sizeof(RB_MSG) 计算值 {total} != 7704（信封/union 漂移）")
    else:
        fails.append("rb_proto.h 未解析到 RB_MSG 信封")

    # ── B. 三副本字段级比对 ──
    # C 结构 → (Controller Models.cs 类型, Controller DriverService.cs 类型, WPF RbProto.cs 类型)
    mirror = {
        "RB_CONFIG":       ("RbConfigNative", None, "RbConfig"),
        "RB_STATS":        ("RbStats", "RbStats", "RbStats"),
        "RB_VOLUME_INFO":  ("RbVolumeInfo", "RbVolumeItem", "RbVolumeInfo"),
        "RB_VOLUME_LIST":  ("RbVolumeList", "RbVolumeList", "RbVolumeList"),
        "RB_VOLUME_SET":   (None, "RbVolumeSet", "RbVolumeSet"),
        "RB_VOLUME_ATTACH": ("RbVolumeAttach", "RbAttach", "RbVolumeAttach"),
        "RB_VERSION":      ("RbVersionNative", "RbVersion", "RbVersion"),
        "RB_LOG_REC":      (None, "RbLogRec", "RbLogRec"),
        "RB_LOG_LIST":     (None, "RbLogList", "RbLogList"),
    }
    cs_models = parse_cs_structs(MODELS_CS) if MODELS_CS.exists() else {}
    cs_drvs = parse_cs_structs(DRVSVC_CS) if DRVSVC_CS.exists() else {}
    cs_wpf = parse_cs_structs(WPF_CS) if WPF_CS.exists() else {}
    print(f"   Models.cs: {len(cs_models)} structs; DriverService.cs: {len(cs_drvs)}; "
          f"RbProto.cs: {len(cs_wpf)}")

    n_cmp = 0
    for cname, (m_name, d_name, w_name) in mirror.items():
        if cname not in cstructs:
            fails.append(f"rb_proto.h 缺结构 {cname}")
            continue
        c_lay_offsets = c_lay.field_offsets(cname)
        c_sz = c_lay.struct_size(cname)
        copies = [("Models.cs", m_name, cs_models),
                  ("DriverService.cs", d_name, cs_drvs),
                  ("RbProto.cs", w_name, cs_wpf)]
        for src, want, pool in copies:
            if want is None:
                continue                      # 该副本按设计无此镜像
            got = pool.get(want)
            if got is None:
                fails.append(f"{src} 缺 {cname} 的镜像（期望类型 {want}）")
                continue
            if any(k.startswith("?") for _, k, _ in got):
                bad = [f"{n}->{k}" for n, k, _ in got if k.startswith("?")]
                fails.append(f"{src} {want} 含无法解析的 SizeConst/类型: {bad}")
                continue
            n_cmp += 1
            # 每个副本用【自己的池】算布局（嵌套结构在各自文件内解析，互不污染）
            try:
                cs_lay_obj = Layout(pool)
                cs_offsets = cs_lay_obj.field_offsets(want)
                cs_sz = cs_lay_obj.struct_size(want)
            except KeyError as e:
                fails.append(f"{src} {want} 布局计算失败: {e}")
                continue
            if len(c_lay_offsets) != len(cs_offsets):
                fails.append(f"{cname}↔{src} 字段数不同: C={len(c_lay_offsets)} "
                             f"{c_lay_offsets} vs C#={len(cs_offsets)} {cs_offsets}")
                continue
            for (cn, coff), (sn, soff) in zip(c_lay_offsets, cs_offsets):
                if cn != sn:
                    fails.append(f"{cname}↔{src} 字段名/顺序漂移: C[{cn}] vs C#[{sn}]")
                if coff != soff:
                    fails.append(f"{cname}↔{src} 字段 {cn} 偏移漂移: "
                                 f"C={coff} vs C#={soff}")
            if c_sz != cs_sz:
                fails.append(f"{cname}↔{src} 结构大小漂移: C={c_sz} vs C#={cs_sz}")
    print(f"   字段级比对完成: {n_cmp} 组镜像核对")

    # 信封字段（Type/Size/Status/Reserved/DataOffset）在 C# 侧以常量/RbMsg 表达
    for label, path in (("Models.cs", MODELS_CS), ("RbProto.cs", WPF_CS)):
        if not path.exists():
            continue
        s = read(path)
        off_m = re.search(r"(?:DataOffset|RbMsgDataOffset)\s*=\s*(\d+)", s)
        sz_m = re.search(r"(?:MsgSize|RbMsgSize)\s*=\s*(\d+)", s)
        if off_m and int(off_m.group(1)) != 16:
            fails.append(f"{label} DataOffset={off_m.group(1)} != 16")
        if sz_m and int(sz_m.group(1)) != 7704:
            fails.append(f"{label} MsgSize={sz_m.group(1)} != 7704")
        if not off_m:
            warns.append(f"{label} 未见 DataOffset 常量")
        io = re.search(r"IOCTL_RB_REQUEST\s*=\s*(0x[0-9A-Fa-f]+|\d+)", s)
        if io and int(io.group(1), 0) != 0x222000:
            fails.append(f"{label} IOCTL_RB_REQUEST={io.group(1)} != 0x222000")
        have = set()
        for m in re.finditer(r"(?:RB_MSG_|MSG_)?(GET_STATS|SET_CONFIG|FLUSH|GET_CONFIG|"
                             r"EXIT_SAFE_MODE|SET_VOLUME|GET_VOLUMES|ATTACH_VOLUME|"
                             r"GET_LOG|GET_VERSION|PANIC|PANIC_CLEAR|INVALIDATE_RANGE)\s*=\s*(\d+)", s):
            have.add((m.group(1), int(m.group(2))))
        expect_msgs = {("GET_STATS", 1), ("SET_CONFIG", 2), ("FLUSH", 3), ("GET_CONFIG", 4),
                       ("EXIT_SAFE_MODE", 5), ("SET_VOLUME", 6), ("GET_VOLUMES", 7),
                       ("ATTACH_VOLUME", 8), ("GET_LOG", 9), ("GET_VERSION", 10),
                       ("PANIC", 11), ("PANIC_CLEAR", 12),
                       ("INVALIDATE_RANGE", 13)}   # [V10.6/B-5] 末尾追加
        extra = {x for x in have if x not in expect_msgs}
        if extra:
            fails.append(f"{label} 消息号越界: {sorted(extra)}")
        missing = expect_msgs - have
        if missing:
            if label == "RbProto.cs":
                (fails if strict else warns).append(
                    f"RbProto.cs（旧代 WPF 副本）缺消息常量 {sorted(missing)}"
                    f"——属旧 WPF 归档决策范围，建议补齐以消审计矩阵 ⚠️")
            else:
                fails.append(f"{label} 缺消息常量 {sorted(missing)}")

    # RbProto.cs 的信封结构体 RbMsg（Type/Size/Status/Reserved，16 字节）
    if "RbMsg" in cs_wpf:
        try:
            env = Layout(cs_wpf)
            offs = env.field_offsets("RbMsg")
            names = [n for n, _ in offs]
            if names != ["Type", "Size", "Status", "Reserved"]:
                fails.append(f"RbProto.cs RbMsg 信封字段漂移: {names}")
            if env.struct_size("RbMsg") != 16:
                fails.append(f"RbProto.cs RbMsg 信封大小 {env.struct_size('RbMsg')} != 16")
        except KeyError as e:
            fails.append(f"RbProto.cs RbMsg 布局计算失败: {e}")

    # DriverService 消息号（私有常量 MSG_*，含 "MSG_A = 1, MSG_B = 2" 同行形式）
    if DRVSVC_CS.exists():
        s = read(DRVSVC_CS)
        have = {int(m.group(1)) for m in re.finditer(r"MSG_\w+\s*=\s*(\d+)", s)}
        for n in range(1, 14):   # [V10.6/B-5] 消息 13 = MSG_INVALIDATE_RANGE
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

    rd = read(README) if README.exists() else ""
    if machine not in rd:
        fails.append(f"README.txt 未见机器版本串 {machine}")
    if "V10.5.0 Build 19" not in rd:
        fails.append("README.txt 未见发布名 V10.5.0 Build 19")

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

    # ── 汇总 ──
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
