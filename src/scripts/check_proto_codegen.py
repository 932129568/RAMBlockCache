#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_proto_codegen.py —— rb-config.generated.ts 与 rb_proto.h 的新鲜度门禁（Gate18）。

背景（161 号 §5 ③ / 162 号 G-A）：
  上一轮把 RB_CONFIG 的 4 份 TS 副本纳入 Gate12 做"手写 + 事后比对"，
  仍是靠人肉同步。本轮改为**从 C 头生成 TS**（gen_proto_ts.py），
  于是门禁要守的不再是"两份手写是否一致"，而是：

      C 头 ──(生成器)──▶ 磁盘上的 rb-config.generated.ts

本门在内存里重跑一遍生成逻辑，与磁盘文件比对：
  * 一致 → PASS；
  * 不一致 → FAIL 并打印 unified diff。

它能抓到的典型事故：改了 src/ctl/rb_proto.h 却没重跑生成器
（新增/改名/重排字段），此时生成物是陈旧的，所有下游 TS 副本跟着错——
正是 66 号 TinyLFU@152 整页白屏的同款成因。

为什么比"两个手写副本互相 diff"更强：生成物本身没有手写成分，
唯一的输入是 C 头，所以不存在"两份手写一起错"的盲区。

换行符：生成器固定写 LF；本门比对前把磁盘文本的 CRLF 归一为 LF——
Windows 下 core.autocrlf=true 的 checkout 会把文件签成 CRLF，
不归一会造成与内容无关的假红（内容比对不受影响）。

Exit 0 = 一致；exit 1 = 漂移。
"""
import difflib
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve()
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))

from gen_proto_ts import OUTPUT, PROTO_H, render  # noqa: E402

CI_YML = ROOT / ".github" / "workflows" / "ci.yml"
SCRIPT_NAME = "check_proto_codegen.py"


def main():
    fails = []
    print("== check_proto_codegen (Gate18) ==")
    print("  source : %s" % PROTO_H.relative_to(ROOT).as_posix())
    print("  output : %s" % OUTPUT.relative_to(ROOT).as_posix())

    if not PROTO_H.is_file():
        fails.append("missing source header: " + PROTO_H.relative_to(ROOT).as_posix())
    if not OUTPUT.is_file():
        fails.append("missing generated file: " + OUTPUT.relative_to(ROOT).as_posix() +
                     "（运行 python -B src/scripts/gen_proto_ts.py）")

    if not fails:
        try:
            expected, meta = render()
        except SystemExit as e:
            fails.append("generator failed: %s" % e)
            expected, meta = None, None

        if expected is not None:
            actual = OUTPUT.read_text(encoding="utf-8").replace("\r\n", "\n")
            if expected != actual:
                diff = list(difflib.unified_diff(
                    actual.splitlines(keepends=True),
                    expected.splitlines(keepends=True),
                    fromfile="disk:" + OUTPUT.name,
                    tofile="expected: gen_proto_ts.py(rb_proto.h)",
                    n=3))
                print("".join(ln if ln.endswith("\n") else ln + "\n" for ln in diff))
                fails.append(
                    "rb-config.generated.ts 与 rb_proto.h 不同步：改了 C 头但没重跑 "
                    "`python -B src/scripts/gen_proto_ts.py`（上方 diff：- 磁盘 / + 应为）")
            else:
                u32 = sum(1 for f in meta["fields"] if f["kind"] == "u32")
                vf = [f for f in meta["fields"] if f["kind"] == "wchar2d"]
                print("  fields : %d u32 + %d wchar2d  sizeof=%d（与 C 头一致）"
                      % (u32, len(vf), meta["size"]))

    # 自注册：门禁被从 CI 里删掉时不能静默通过。
    if CI_YML.is_file() and SCRIPT_NAME not in CI_YML.read_text(encoding="utf-8", errors="replace"):
        fails.append("ci.yml: %s 未注册为门禁步骤" % SCRIPT_NAME)

    for f in fails:
        print("[FAIL] " + f)
    print("result: %s (%d fail)" % ("FAIL" if fails else "PASS", len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
