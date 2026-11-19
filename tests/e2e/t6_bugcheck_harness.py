#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T6 · RbBugCheckCb 两阶段回调协议的可执行 harness（含阳性对照）

I23/P2 说「该分支仅在再次崩溃时才会被真机验证，不得主动制造崩溃」。
本 harness 用用户态等价模型把 crashdmp 的两次回调序列跑出来，使
「为什么三次转储全失败」以及「修复是否覆盖两阶段」可判定，且零内核风险。

逐字复现 rbdriver.c:607-660（修复版）与注释描述的修复前版本。

判据序列：
  S1 探测阶段 OutBuffer=NULL, MaximumAllowed=0
     修复版：只申报 GUID + OutBufferLength，绝不写 NULL；填写 GUID
     修复前：无条件 RtlZeroMemory(NULL,...) -> 写空指针 -> KiPageFault
  S2 写入阶段 OutBuffer=有效, MaximumAllowed=0
     修复版：用 s_CrashWant（首次申报值）写入，不受 MaximumAllowed=0 影响
     修复前：开头 MaximumAllowed 判据直接 return -> 数据永远写不进 dump
  S3 交替 probe/write x3 -> GUID 恒定、长度恒定、s_CrashWant 不漂移
  S4 上限裁剪：probe 时 MaximumAllowed < sizeof(BLOB) -> 降级为 sizeof(HDR)
  S5 防御性边界：write 时缓冲小于申报长度 -> 观察实现是否有容量校验
"""
import json

BLOB = 2048       # sizeof(RB_DIAG_CRASH_BLOB) 的等价常量
HDR = 64          # sizeof(RB_CRASH_HDR) 的等价常量
GUID = "2b91c607-e4d8-4a35-8c17-d690-4b2e-73-a5"


class DumpData:
    def __init__(self, out=None, cap=None, max_allowed=0):
        self.Guid = None
        self.OutBuffer = out          # None 表示探测阶段
        self.OutBufferLength = 0
        self.MaximumAllowed = max_allowed
        self.buf = out                # bytearray 或 None
        self.cap = cap if cap is not None else (len(out) if out is not None else 0)
        self.null_write = False
        self.overflow = False
        self.writes = 0


def old_impl(dd):
    """修复前：无条件 memset(OutBuffer)，且用 MaximumAllowed 判容量。"""
    want = BLOB
    if dd.MaximumAllowed and dd.MaximumAllowed < want:
        want = HDR
    # 原实现无条件写 OutBuffer（探测阶段为 NULL）
    if dd.OutBuffer is None:
        dd.null_write = True          # 等价 KiPageFault / __memset_repmovs
        return
    if dd.MaximumAllowed == 0:
        return                        # 第二次回调恒 0 -> 永远不写
    if want > dd.cap:
        dd.overflow = True
        return
    dd.buf[:want] = b"\0" * want
    dd.writes += 1
    dd.OutBufferLength = want
    # 原实现从不填 Guid


def new_impl(dd, state):
    """修复版 rbdriver.c:607-660。"""
    dd.Guid = GUID
    if dd.OutBuffer is None:
        want = BLOB
        if dd.MaximumAllowed != 0 and dd.MaximumAllowed < want:
            want = HDR
        state["want"] = want
        dd.OutBufferLength = want
        return
    want = state.get("want", 0)
    if want == 0:
        dd.OutBufferLength = 0
        return
    if want > dd.cap:
        # 内核里 OutBuffer 由系统按首次申报长度分配；此处观察实现是否校验
        dd.overflow = True            # 无显式容量校验 -> 潜在越界
        return
    dd.buf[:want] = b"\x5A" * want   # 可辨识的填充，验证确实写入
    dd.writes += 1
    dd.OutBufferLength = want


def scenario(impl, name):
    state = {"want": 0}
    rows = []

    def step(label, out, cap, maxa):
        dd = DumpData(out, cap, maxa)
        if impl == "old":
            old_impl(dd)
        else:
            new_impl(dd, state)
        row = {"step": label, "guid": dd.Guid, "len": dd.OutBufferLength,
               "null_write": dd.null_write, "overflow": dd.overflow,
               "writes": dd.writes}
        rows.append(row)
        return row

    r = {}
    r["S1_probe"] = step("probe", None, 0, 0)
    r["S2_write"] = step("write", bytearray(BLOB), BLOB, 0)
    r["S3_alternating"] = []
    for i in range(3):
        r["S3_alternating"].append(step("probe%d" % i, None, 0, 0))
        r["S3_alternating"].append(
            step("write%d" % i, bytearray(state.get("want", BLOB)),
                 state.get("want", BLOB), 0))
    r["S4_truncate"] = {}
    state["want"] = 0
    r["S4_truncate"]["probe"] = step("probe_capped", None, 0, HDR - 1)
    r["S4_truncate"]["write"] = step("write_capped", bytearray(HDR), HDR, 0)
    r["S5_undersized"] = step("write_short_buffer", bytearray(16), 16, 0)

    verdict = {
        "probe_writes_null": r["S1_probe"]["null_write"],
        "probe_declares_guid": r["S1_probe"]["guid"] == GUID,
        "probe_declares_length": r["S1_probe"]["len"],
        "write_stage_wrote_data": r["S2_write"]["writes"] > 0,
        "write_stage_len": r["S2_write"]["len"],
        "truncate_declared_len": r["S4_truncate"]["probe"]["len"],
        "undersized_buffer_overflow": r["S5_undersized"]["overflow"],
    }
    return {"impl": impl, "name": name, "rows": r, "verdict": verdict}


def main():
    print("=== T6 BUGCHECK SECONDARY-DUMP TWO-PHASE HARNESS ===", flush=True)
    old = scenario("old", "112/119 修复前（转储全失败的实现）")
    new = scenario("new", "119 号修复版（rbdriver.c:607-660）")
    for s in (old, new):
        print(json.dumps({"impl": s["impl"], "name": s["name"],
                          "verdict": s["verdict"]}, ensure_ascii=False),
              flush=True)
    print("=== POSITIVE CONTROL ===", flush=True)
    print("OLD probe_writes_null = %s  (must be True: explains three "
          "volmgr 161=0x00020042 / no MEMORY.DMP)" %
          old["verdict"]["probe_writes_null"], flush=True)
    print("OLD write_stage_wrote_data = %s (must be False: MaximumAllowed=0 "
          "gate)" % old["verdict"]["write_stage_wrote_data"], flush=True)
    print("=== FIXED ===", flush=True)
    print("NEW probe_writes_null=%s guid=%s len=%d" %
          (new["verdict"]["probe_writes_null"],
           new["verdict"]["probe_declares_guid"],
           new["verdict"]["probe_declares_length"]), flush=True)
    print("NEW write wrote=%s len=%d ; truncate->%d" %
          (new["verdict"]["write_stage_wrote_data"],
           new["verdict"]["write_stage_len"],
           new["verdict"]["truncate_declared_len"]), flush=True)
    print("NEW undersized buffer -> overflow flag = %s "
          "(implementation has no explicit OutBuffer capacity check; "
          "kernel allocates per first-phase declaration, so this is a "
          "defensive observation, NOT a proven defect)" %
          new["verdict"]["undersized_buffer_overflow"], flush=True)
    print("LIMIT: user-mode protocol model. Does NOT prove the kernel callback "
          "is registered/reached; that still requires a real bugcheck.", flush=True)
    with open("t6_result.json", "w", encoding="utf-8") as f:
        json.dump({"old": old, "new": new}, f, ensure_ascii=False, indent=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
