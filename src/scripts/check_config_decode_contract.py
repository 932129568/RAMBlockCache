#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate12 static contract: RB_CONFIG -> TS 副本字段级契约（生成物 + 派生纪律）。

背景（124 号 F9 / T7c 变异测试）：
  check_proto_sync.py --strict 只比对 rb_proto.h 与 C# 镜像；
  **decodeConfig 曾完全裸奔**：删掉 proto-decode.ts 的 `['TinyLFU', 152],`
  后 8 道门禁仍全绿 —— 这正是 66 号 TinyLFU@152 白屏事故的同款缺陷。

[166 2026-11-12] RB_CONFIG 的 TS 侧已改为**代码生成**（WR-06 落地）：
      src/ctl/rb_proto.h --gen_proto_ts.py--> rb-config.generated.ts
  proto/types.ts / proto-decode.ts / bridge/server/index.ts 三处不再手写
  字段名与偏移，而是遍历生成物的 RB_CONFIG_FIELDS。
  校验分工因此调整为：

    Gate18 (check_proto_codegen.py) : C 头 ──生成器──▶ 生成物（逐字节）
    本门 (Gate12)                   : C 头 ──▶ 生成物字段表 逐字段比对
                                      + 三处副本「确实由生成表驱动」的结构断言
                                      + http-transport.ts zod 键集 ⊆ RB_CONFIG

  仍是"先剥离注释再比对"：只加 `//` 前缀的移除必须能被检出。

Exit 0 = contract holds; exit 1 = drift.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PROTO_H = ROOT / "src" / "ctl" / "rb_proto.h"
DECODE_TS = ROOT / "src/ui/web" / "bridge" / "server" / "proto-decode.ts"
CI_YML = ROOT / ".github" / "workflows" / "ci.yml"
SCRIPT_NAME = "check_config_decode_contract.py"

# [162 G-A 2026-11-12] RB_CONFIG 的 TS 侧副本（编码/前端）。
# 161 号 §1.4：解码侧被 Gate12 锁死，编码侧裸奔——"少编码一个字段，驱动
# RbApplyConfig 收到 0 并持久化"（66 号白屏事故的结构性成因）。
#   #4 bridge/server/index.ts  encodeParams(method===2)：全量编码 RB_CONFIG
#   #5 src/bridge/http-transport.ts  configSchema：zod 校验键集
#   #6 src/protocol/types.ts          RbConfig 类型
# [166 2026-11-12] #4/#6 与 decodeConfig 已改为**遍历生成表**（不再手写偏移）；
# 生成物由 src/ctl/rb_proto.h 产出，Gate18 守 C 头 ↔ 生成物，本门守生成物 ↔ 消费方。
INDEX_TS = ROOT / "src/ui/web" / "bridge" / "server" / "index.ts"
HTTP_TS = ROOT / "src/ui/web" / "src" / "bridge" / "http-transport.ts"
TYPES_TS = ROOT / "src/ui/web" / "src" / "protocol" / "types.ts"
CODEGEN_TS = ROOT / "src/ui/web" / "src" / "protocol" / "rb-config.generated.ts"

# configSchema 是**部分**校验面（looseObject：未列出的键原样透传），因此对它是
# "键集必须是 C 字段集的子集"，不是逐项全等。但它当前校验的 5 个键是已发布
# 行为（TinyLFU 的 default(0) 有独立的旧桥接兼容语义），删掉任何一个都是回退：
# 用下界锁住，理由同 check_proto_sync.py 的 MSG_FLOOR（防"删了就没人管")。
SCHEMA_FLOOR = ("Enabled", "WriteBack", "CacheSizeMB", "BlockSize", "TinyLFU")


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
        # [2026-11-12 修 fail-open] 原实现对不认识的字段行静默 `continue`，且
        # **不推进 offset**。后果：往 RB_CONFIG_T 里加一个 uint64_t / uint8_t
        # 字段后 ——
        #   · sizeof(RB_CONFIG) 被低估 ⇒ 本门的 `sizeof != 156` 判据**通过**；
        #   · gen_proto_ts.py 复用本函数 ⇒ 生成物同样缺该字段 ⇒ Gate18 的
        #     逐字节比对也**通过**；
        # 两道门禁双双变绿，而 Web 侧按 156 分配、驱动按真实 sizeof 读写 ⇒
        # 越界/错位。这正是 66 号批白屏的同型事故，只是更隐蔽（那次是漏改
        # 手写副本，这次是漏改后**连门禁都看不见**）。
        # 失败方向必须是"不生成、不通过"：新增非 uint32_t 字段时，先扩展本
        # 解析器的类型支持（含对齐规则），否则这里直接抛错。
        raise ValueError(
            "parse_c_config: RB_CONFIG_T 中存在本解析器不认识的字段行 -> %r；"
            "新增字段必须同步扩展本解析器（含对齐），否则 sizeof/偏移会被低估，"
            "Gate12 与 Gate18 会双双假绿" % line)
    return fields, vf_off, off


def parse_generated(text):
    """解析 rb-config.generated.ts（#0 生成物）。

    返回 (size, entries[(name, offset, width, kind, count, len)]) 或 (None, None)。
    """
    ms = re.search(r"export\s+const\s+RB_CONFIG_SIZE\s*=\s*(\d+)\s*;", text)
    mf = re.search(r"export\s+const\s+RB_CONFIG_FIELDS\s*:\s*[^=]+=\s*\[(.*?)\];",
                   text, re.S)
    if not ms or not mf:
        return None, None
    body = strip_ts_comments(mf.group(1))
    entries = []
    for blk in re.findall(r"\{([^{}]*)\}", body):
        d = {}
        for k, v in re.findall(r"(\w+)\s*:\s*'?([\w]+)'?", blk):
            d[k] = v
        if "name" not in d or "offset" not in d:
            continue
        entries.append((d["name"], int(d["offset"]), int(d.get("width", 0)),
                        d.get("kind", "u32"), int(d.get("count", 1)),
                        int(d.get("len", 1))))
    return int(ms.group(1)), entries


def generated_imports(text):
    """从 rb-config.generated（可带 .ts 扩展名）导入的符号集合；无导入返回 None。"""
    names, found = set(), False
    for m in re.finditer(
            r"import\s*(?:type\s*)?\{([^}]*)\}\s*from\s*'[^']*rb-config\.generated(?:\.ts)?'",
            text):
        found = True
        for n in m.group(1).split(","):
            n = n.strip()
            if n:
                names.add(n)
    return names if found else None


def check_decoder(text):
    """#1 decodeConfig：必须由 RB_CONFIG_FIELDS 驱动，且不得残留手写偏移表。

    返回 dict(found, driven, vf, guard, hard[(name,off)])。
    """
    m = re.search(r"export\s+function\s+decodeConfig\s*\(\s*\w+\s*:\s*Buffer\s*\)\s*\{(.*?)\n\}",
                  text, re.S)
    if not m:
        return {"found": False}
    body = m.group(1)
    return {
        "found": True,
        "driven": bool(re.search(r"for\s*\(\s*const\s+\w+\s+of\s+RB_CONFIG_FIELDS\s*\)", body)),
        "vf": bool(re.search(r"kind\s*===\s*'wchar2d'", body)),
        "guard": bool(re.search(r"p\.length\s*>=\s*RB_CONFIG_SIZE\s*\?", body)),
        "hard": [(n, int(o)) for n, o in
                 re.findall(r"\[\s*'([A-Za-z_]\w*)'\s*,\s*(\d+)\s*\]", body)],
    }


def check_encoder(text):
    """#4 encodeParams(method===2)：必须用 RB_CONFIG_SIZE + RB_CONFIG_FIELDS。

    返回 dict(found, driven, vf, alloc_sym, alloc_num, hard[(name,off)])。
    """
    m = re.search(r"if\s*\(\s*method\s*===\s*2\s*\)\s*\{(.*?)\n  \}", text, re.S)
    if not m:
        return {"found": False}
    body = m.group(1)
    ma = re.search(r"Buffer\.alloc\(\s*(\d+)\s*\)", body)
    return {
        "found": True,
        "driven": bool(re.search(r"for\s*\(\s*const\s+\w+\s+of\s+RB_CONFIG_FIELDS\s*\)", body)),
        "vf": bool(re.search(r"kind\s*===\s*'wchar2d'", body)),
        "alloc_sym": bool(re.search(r"Buffer\.alloc\(\s*RB_CONFIG_SIZE\s*\)", body)),
        "alloc_num": ma.group(1) if ma else None,
        "hard": [(n, int(o)) for n, o in
                 re.findall(r"\[\s*'([A-Za-z_]\w*)'\s*,\s*(\d+)\s*\]", body)],
    }


def parse_ts_interface(text):
    """#6 types.ts：转发生成物的 RbConfig，且不得再声明同名 interface。

    返回 dict(imports, reexport, local_iface)。
    """
    return {
        "imports": generated_imports(text),
        "reexport": bool(re.search(r"export\s+type\s*\{\s*RbConfig\s*\}", text)),
        "local_iface": bool(re.search(r"(?m)^\s*(?:export\s+)?interface\s+RbConfig\s*\{", text)),
    }


def parse_zod_keys(text, var):
    """#5 zod schema 的键名（按声明顺序）。"""
    m = re.search(r"const\s+%s\s*=\s*z\.\w+\(\s*\{(.*?)\}\s*\)\s*;" % var, text, re.S)
    if not m:
        return None
    return re.findall(r"(?m)^\s*([A-Za-z_]\w*)\s*:\s*z\.", strip_ts_comments(m.group(1)))


def cmp_named(label, base, other, base_label="C"):
    """逐项比 (name, offset)：数量/名字/顺序/偏移全等。返回 fail 列表。"""
    out = []
    if len(base) != len(other):
        out.append("%s: field count %s=%d %s=%d" %
                   (label, base_label, len(base), label, len(other)))
    for i, (a, b) in enumerate(zip(base, other)):
        if a[0] != b[0]:
            out.append("%s: field[%d] name %s=%s %s=%s (order drift)" %
                       (label, i, base_label, a[0], label, b[0]))
        elif a[1] != b[1]:
            out.append("%s: field[%d] %s offset %s=%d %s=%d" %
                       (label, i, a[0], base_label, a[1], label, b[1]))
    return out


def main():
    fails = []
    for p in (PROTO_H, DECODE_TS, INDEX_TS, HTTP_TS, TYPES_TS, CODEGEN_TS, CI_YML):
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

    macros = parse_macros(proto_raw)
    # [2026-11-12] 严格模式：不认识字段行 -> FAIL（原来静默跳过 = sizeof 低估 =
    # 本门 sizeof 判据与 Gate18 字节比对双双假绿，Web 侧少一个字段）。
    try:
        c_fields, vf_off, sizeof = parse_c_config(proto, macros)
    except ValueError as _e:
        fails.append(str(_e))
        c_fields, vf_off, sizeof = None, None, None
    gen_size, gen_entries = parse_generated(
        strip_ts_comments(CODEGEN_TS.read_text(encoding="utf-8-sig")))
    dec = check_decoder(decode)
    index_raw = INDEX_TS.read_text(encoding="utf-8-sig")
    enc = check_encoder(strip_ts_comments(index_raw))
    types_raw = TYPES_TS.read_text(encoding="utf-8-sig")
    tys = parse_ts_interface(types_raw)
    schema = parse_zod_keys(HTTP_TS.read_text(encoding="utf-8-sig"), "configSchema")

    if c_fields is None:
        fails.append("rb_proto.h: RB_CONFIG struct not found")
    if gen_entries is None:
        fails.append("rb-config.generated.ts: RB_CONFIG_SIZE / RB_CONFIG_FIELDS not found"
                     "（重跑 python -B src/scripts/gen_proto_ts.py）")

    # ── A. C 头 ↔ 生成物：名/顺序/偏移/宽度 ──
    # 生成物是"机器产出"而非手写副本，但仍可能被手改或忘了重跑；这里独立再比一遍
    # （Gate18 比的是整份文件字节，本门比的是字段语义，两者不是重复而是交叉）。
    gen_u32 = ([(n, o) for n, o, w, k, c, l in gen_entries if k == "u32"]
               if gen_entries is not None else None)
    if c_fields is not None and gen_u32 is not None:
        fails += cmp_named("rb-config.generated.ts", c_fields, gen_u32)
        for n, o, w, k, cnt, ln in gen_entries:
            if k == "u32" and w != 4:
                fails.append("generated: %s width=%d != 4（u32）" % (n, w))
            elif k == "wchar2d" and w != cnt * ln * 2:
                fails.append("generated: %s width=%d != count*len*2=%d"
                             % (n, w, cnt * ln * 2))
        vfs = [e for e in gen_entries if e[3] == "wchar2d"]
        if len(vfs) != 1:
            fails.append("generated: wchar2d 字段数 %d != 1" % len(vfs))
        else:
            n, o, w, k, cnt, ln = vfs[0]
            if o != vf_off:
                fails.append("generated: %s offset %d != C vf_off %r" % (n, o, vf_off))
            want_dims = (macros.get("RB_MAX_VOLUMES"), macros.get("RB_VOL_LEN"))
            if (cnt, ln) != want_dims:
                fails.append("generated: %s 维度 [%d][%d] != C 宏 [%s][%s]"
                             % (n, cnt, ln, want_dims[0], want_dims[1]))
        if gen_size != sizeof:
            fails.append("generated: RB_CONFIG_SIZE=%r != C sizeof(RB_CONFIG)=%r"
                         % (gen_size, sizeof))

    if vf_off != 64:
        fails.append("VolumeFilter offset: expected 64, got %r" % vf_off)
    if sizeof != 156:
        fails.append("sizeof(RB_CONFIG): expected 156, computed %r" % sizeof)
    if "sizeof(RB_CONFIG)=156" not in proto_raw:
        fails.append("rb_proto.h: sizeof(RB_CONFIG)=156 contract comment missing")
    if SCRIPT_NAME not in ci:
        fails.append("ci.yml: %s not registered as a gate" % SCRIPT_NAME)

    # ── B. 三处副本必须由生成表驱动（不再手写偏移）──
    # 这些断言守的是"不得退回手写副本"，不是"手写副本是否与 C 一致"：
    # 一旦有人把偏移表抄回来，即使数值暂时正确也 FAIL。
    if not dec["found"]:
        fails.append("proto-decode.ts: decodeConfig() not found")
    else:
        if not dec["driven"]:
            fails.append("proto-decode.ts: decodeConfig 未遍历 RB_CONFIG_FIELDS"
                         "（禁止手写偏移表）")
        if not dec["vf"]:
            fails.append("proto-decode.ts: VolumeFilter 的 wchar2d 分支缺失")
        if not dec["guard"]:
            fails.append("proto-decode.ts: TinyLFU 分级长度守卫 "
                         "`p.length >= RB_CONFIG_SIZE ?` 缺失")
        if dec["hard"]:
            fails.append("proto-decode.ts: decodeConfig 残留手写偏移表 %r"
                         % (dec["hard"][:3],))
    dimp = generated_imports(decode)
    if not dimp or not {"RB_CONFIG_FIELDS", "RB_CONFIG_SIZE"} <= dimp:
        fails.append("proto-decode.ts: 未从 rb-config.generated 导入 "
                     "RB_CONFIG_FIELDS / RB_CONFIG_SIZE")

    if not enc["found"]:
        fails.append("index.ts: encodeParams(method===2) not found")
    else:
        if not enc["driven"]:
            fails.append("index.ts: encodeParams(method===2) 未遍历 RB_CONFIG_FIELDS")
        if not enc["vf"]:
            fails.append("index.ts: VolumeFilter 的 wchar2d 分支缺失")
        if not enc["alloc_sym"]:
            fails.append("index.ts: 必须 Buffer.alloc(RB_CONFIG_SIZE)（实测 alloc(%r)）"
                         % (enc["alloc_num"],))
        if enc["hard"]:
            fails.append("index.ts: encodeParams(method===2) 残留手写偏移表 %r"
                         % (enc["hard"][:3],))
    iimp = generated_imports(index_raw)
    if not iimp or not {"RB_CONFIG_FIELDS", "RB_CONFIG_SIZE"} <= iimp:
        fails.append("index.ts: 未从 rb-config.generated 导入 "
                     "RB_CONFIG_FIELDS / RB_CONFIG_SIZE")

    if not tys["reexport"]:
        fails.append("types.ts: 未 `export type { RbConfig }` 转发生成物")
    if tys["local_iface"]:
        fails.append("types.ts: 仍声明本地 interface RbConfig —— 与生成物并存即漂移源")
    if not tys["imports"] or "RbConfig" not in tys["imports"]:
        fails.append("types.ts: 未从 rb-config.generated 导入 RbConfig")

    # ── C. #5 zod schema：仍是手写"部分校验面"，键集必须是 C 字段集的子集 ──
    c_full = [n for n, _ in c_fields] if c_fields else []
    if c_fields is not None and vf_off is not None and vf_off % 4 == 0:
        c_full = c_full[:vf_off // 4] + ["VolumeFilter"] + c_full[vf_off // 4:]

    if schema is None:
        fails.append("http-transport.ts: configSchema zod object not found")
    else:
        unknown = [k for k in schema if k not in c_full]
        if unknown:
            fails.append("http-transport.ts: configSchema keys not in RB_CONFIG: %s" % unknown)
        lost = [k for k in SCHEMA_FLOOR if k not in schema]
        if lost:
            fails.append("http-transport.ts: configSchema dropped published key(s): %s" % lost)

    print("== check_config_decode_contract (Gate12) ==")
    print("  C  u32 fields : %s" % (len(c_fields) if c_fields else "n/a"))
    print("  generated     : %s entries (%s u32) sizeof=%s  VolumeFilter@%s" %
          (len(gen_entries) if gen_entries else "n/a", len(gen_u32) if gen_u32 else "n/a",
           gen_size, vf_off))
    print("  decode/index  : driven=%s/%s  VF=%s/%s  allocSym=%s" %
          (dec.get("driven"), enc.get("driven"), dec.get("vf"), enc.get("vf"),
           enc.get("alloc_sym")))
    print("  types.ts      : reexport=%s local_iface=%s; schema keys=%s" %
          (tys["reexport"], tys["local_iface"], len(schema) if schema else "n/a"))
    for f in fails:
        print("[FAIL] " + f)
    print("result: %s (%d fail)" % ("FAIL" if fails else "PASS", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
