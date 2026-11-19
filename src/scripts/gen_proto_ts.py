#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_proto_ts.py —— 从 src/ctl/rb_proto.h 生成 RB_CONFIG 的 TypeScript 定义。

背景（161 号 §5 ③ / 162 号 G-A）：
  RB_CONFIG 在 TS 侧原有 4 份**手写**副本（proto/types.ts interface、
  bridge/server/proto-decode.ts 解码表、bridge/server/index.ts 编码表、
  src/bridge/http-transport.ts zod 键集）。上一轮把它们纳入 Gate12 做
  "手写 + 事后比对"，仍依赖人肉同步。本生成器把头文件变成唯一事实源：

      rb_proto.h  ──gen_proto_ts.py──▶  rb-config.generated.ts  ──▶  4 份副本派生

  这样"改 C 头忘了改 TS"不再可能发生：忘了重跑生成器 → check_proto_codegen.py
  直接 FAIL（打印 unified diff）。

解析逻辑不另写一套：复用 check_config_decode_contract.py 的
  strip_c_comments / parse_macros / parse_c_config
（Gate12 用的就是同一份解析器，避免生成器与门禁对同一根头文件有两种口径）。

输出约定：
  * 只写 LF、UTF-8（无 BOM）；不写时间戳 / 机器路径 / 版本号，
    保证同样的头文件 → 逐字节相同的输出（幂等）。
  * 字段顺序 = 声明顺序 = 偏移升序；每个字段带 @offset 注释。

用法：
  python -B src/scripts/gen_proto_ts.py            # 写入生成物
  python -B src/scripts/gen_proto_ts.py --stdout   # 只打印不落盘（门禁复用）
退出码：0 = OK；1 = 解析失败（头文件结构变化超出本生成器认识的范围）。
"""
import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve()
ROOT = HERE.parents[2]                     # src/scripts/gen_proto_ts.py → 仓库根
sys.path.insert(0, str(HERE.parent))

# 单一解析口径：与 Gate12 共用同一份 C 解析器。
from check_config_decode_contract import (  # noqa: E402
    parse_c_config,
    parse_macros,
    strip_c_comments,
)

PROTO_H = ROOT / "src" / "ctl" / "rb_proto.h"
OUTPUT = ROOT / "src" / "ui" / "web" / "src" / "protocol" / "rb-config.generated.ts"

STRUCT_RE = r"typedef\s+struct\s+RB_CONFIG_T\s*\{(.*?)\}\s*RB_CONFIG\s*;"


def _struct_body(raw: str) -> str:
    m = re.search(STRUCT_RE, raw, re.S)
    if not m:
        raise SystemExit("[FATAL] rb_proto.h: 找不到 typedef struct RB_CONFIG_T")
    return m.group(1)


def _field_docs(raw_body: str) -> dict:
    """抓每个字段行尾的 /* ... */ 注释，转成 TSDoc 文案（保留 C 侧原始说明）。"""
    docs = {}
    for line in raw_body.splitlines():
        m = re.match(r"\s*(?:uint32_t|wchar_t)\s+(\w+)", line)
        if not m:
            continue
        cm = re.search(r"/\*(.*?)\*/", line, re.S)
        if cm:
            docs[m.group(1)] = " ".join(cm.group(1).split())
    return docs


def _volume_filter_dims(raw_body: str, macros: dict):
    """wchar_t Name[D1][D2]; 的维度（D 可能是 RB_* 宏）。"""
    m = re.search(r"wchar_t\s+(\w+)\s*\[\s*(\w+)\s*\]\s*\[\s*(\w+)\s*\]\s*;", raw_body)
    if not m:
        return None
    name, d1, d2 = m.groups()

    def dim(x: str) -> int:
        if x.isdigit():
            return int(x)
        if x not in macros:
            raise SystemExit("[FATAL] rb_proto.h: %s 的维度 %s 无法解析" % (name, x))
        return macros[x]

    return name, dim(d1), dim(d2)


def layout():
    """返回 (size, entries, macros)。

    entries: [{'name','offset','width','kind','type','c_type','count','len','doc'}]
    顺序 = 声明顺序 = 偏移升序（含 wchar 数组，它插在 u32 序列中间）。
    """
    raw = PROTO_H.read_text(encoding="utf-8-sig")
    stripped = strip_c_comments(raw)
    macros = parse_macros(raw)

    # [2026-11-12] parse_c_config 已改为严格模式：遇到不认识的字段行抛 ValueError
    # （原来静默跳过会让 sizeof 被低估，生成物缺字段，而 Gate12/Gate18 双双假绿）。
    # 这里把异常转成可读的 FATAL，避免整段 traceback 淹没真正的错因。
    try:
        u32_fields, vf_off, size = parse_c_config(stripped, macros)
    except ValueError as _e:
        raise SystemExit("[FATAL] %s" % _e)
    if u32_fields is None:
        raise SystemExit("[FATAL] rb_proto.h: RB_CONFIG 解析失败")
    if vf_off is None:
        raise SystemExit("[FATAL] rb_proto.h: RB_CONFIG 内未找到 wchar_t 二维数组")

    body = _struct_body(raw)
    dims = _volume_filter_dims(body, macros)
    if dims is None:
        raise SystemExit("[FATAL] rb_proto.h: wchar_t 二维数组维度解析失败")
    vf_name, vf_count, vf_len = dims
    docs = _field_docs(body)

    u32_doc = {}
    for name, _ in u32_fields:
        u32_doc[name] = "uint32_t"

    entries = []
    vf_entry = {
        "name": vf_name,
        "offset": vf_off,
        "width": vf_count * vf_len * 2,
        "kind": "wchar2d",
        "type": "string[]",
        "c_type": "wchar_t[%d][%d]" % (vf_count, vf_len),
        "count": vf_count,
        "len": vf_len,
        "doc": docs.get(vf_name, ""),
    }
    inserted = False
    for name, off in u32_fields:
        if not inserted and off > vf_off:
            entries.append(vf_entry)
            inserted = True
        entries.append({
            "name": name,
            "offset": off,
            "width": 4,
            "kind": "u32",
            "type": "number",
            "c_type": u32_doc[name],
            "count": 1,
            "len": 1,
            "doc": docs.get(name, ""),
        })
    if not inserted:
        entries.append(vf_entry)

    # 结构自洽：偏移必须严格递增，末字段必须正好结束在 sizeof 处。
    prev_end = 0
    for e in entries:
        if e["offset"] < prev_end:
            raise SystemExit("[FATAL] rb_proto.h: 字段 %s 偏移 %d 与前一字段重叠"
                             % (e["name"], e["offset"]))
        prev_end = e["offset"] + e["width"]
    if prev_end != size:
        raise SystemExit("[FATAL] rb_proto.h: 字段之和 %d != sizeof(RB_CONFIG) %d"
                         % (prev_end, size))
    return size, entries, macros


def _doc_text(e: dict) -> str:
    parts = ["@offset %d" % e["offset"], e["c_type"]]
    if e["kind"] == "wchar2d":
        parts.append("%d B" % e["width"])
    if e["doc"]:
        parts.append(e["doc"].replace("*/", "*\\/"))
    return " · ".join(parts)


def render():
    """返回 (完整 TS 文本, meta)。文本不含时间戳/绝对路径，可逐字节复现。"""
    size, entries, _ = layout()

    lines = [
        "/* AUTO-GENERATED — DO NOT EDIT.",
        " * 来源：src/ctl/rb_proto.h 的 typedef struct RB_CONFIG_T",
        " * 生成器：src/scripts/gen_proto_ts.py（python -B src/scripts/gen_proto_ts.py）",
        " * 漂移门禁：src/scripts/check_proto_codegen.py（磁盘文件与 C 头不一致即 FAIL）",
        " *",
        " * 手改本文件无效：下一次生成会被覆盖，且门禁当场报红。",
        " */",
        "",
        "/** RB_CONFIG 字段的两种 C 形态：标量 u32 与 wchar_t 二维数组。 */",
        "export type RbConfigField =",
        "  | { readonly name: string; readonly offset: number; readonly width: 4;"
        " readonly kind: 'u32' }",
        "  | { readonly name: string; readonly offset: number; readonly width: number;"
        " readonly kind: 'wchar2d'; readonly count: number; readonly len: number };",
        "",
        "/**",
        " * RB_CONFIG —— 与 C 头逐字节一致（#pragma pack(8)）。",
        " * 字段名保留 C 原始 PascalCase 拼写，便于与 rb_proto.h 逐行对照。",
        " */",
        "export interface RbConfig {",
    ]
    for e in entries:
        lines.append("  /** %s */" % _doc_text(e))
        lines.append("  %s: %s;" % (e["name"], e["type"]))
    lines += [
        "}",
        "",
        "/** sizeof(RB_CONFIG)，单位字节。 */",
        "export const RB_CONFIG_SIZE = %d;" % size,
        "",
        "/**",
        " * RB_CONFIG 的全部字段，按声明顺序（= 偏移升序）。",
        " * 解码器 / 编码器 / zod 键集一律遍历本表，不得再手写偏移。",
        " */",
        "export const RB_CONFIG_FIELDS: ReadonlyArray<RbConfigField> = [",
    ]
    for e in entries:
        if e["kind"] == "wchar2d":
            lines.append(
                "  { name: '%s', offset: %d, width: %d, kind: 'wchar2d', count: %d, len: %d },"
                % (e["name"], e["offset"], e["width"], e["count"], e["len"]))
        else:
            lines.append("  { name: '%s', offset: %d, width: %d, kind: 'u32' },"
                         % (e["name"], e["offset"], e["width"]))
    lines += ["];", ""]

    return "\n".join(lines), {"size": size, "fields": entries}


def main(argv=None):
    ap = argparse.ArgumentParser(description="从 rb_proto.h 生成 rb-config.generated.ts")
    ap.add_argument("--stdout", action="store_true", help="只打印，不写文件")
    args = ap.parse_args(argv)

    text, meta = render()
    if args.stdout:
        sys.stdout.write(text)
        return 0

    # 显式写字节：不受 Windows 文本模式的 \n → \r\n 转换影响，保证幂等。
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(text.encode("utf-8"))

    u32 = sum(1 for f in meta["fields"] if f["kind"] == "u32")
    vf = [f for f in meta["fields"] if f["kind"] == "wchar2d"]
    print("== gen_proto_ts ==")
    print("  source : src/ctl/rb_proto.h")
    print("  output : %s" % OUTPUT.relative_to(ROOT).as_posix())
    print("  fields : %d u32 + %d wchar2d[%d][%d]@%d  sizeof=%d"
          % (u32, len(vf), vf[0]["count"], vf[0]["len"], vf[0]["offset"], meta["size"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
