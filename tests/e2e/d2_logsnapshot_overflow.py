#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""D2 · 00:32 池腐坏根因的精确复现（用户态，零风险主动激活）

被复现的代码（src/driver/rbdriver.c RbLogSnapshot，00:11 代与 B 批逐字相同）：
    valid = (g_LogHead < RB_LOG_CAP) ? g_LogHead : RB_LOG_CAP;   // <= 512
    for (i = 0; i < valid; i++) {
        ...
        src = &g_Log[idx]; dst = &out->Items[n];                 // n 无上界!
        RtlStringCchCopyW(dst->Text, RB_LOG_TEXT_MAX, src->Text);
        n++;
    }
    out->Count = n; out->Cursor = g_LogHead;

几何：
    RB_LOG_REC   = 120 B   (Ticks8+Level4+Src4+Status4+pad4+Text[48]*2)
    RB_LOG_LIST  = 8 + 64*120 = 7688 B
    RB_MSG       = 16 + 8208(union 最大 RB_EVENT_LIST) = 8224 B
    最坏写入     = 512*120 = 61440 B  => 越界 53240 B

判据（每组带对照，回答「为什么 rbx 是那个值」）：
  J1 当前实现 + 满环  => 越界 53240 B
  J2 越界区含 UTF-16 " (ou" 字节串（来自 rbL2.c:650 的日志文案）
  J3 越界区按 8 字节窗口能读出 0x0075006F00280020（== dump 的 rbx）
  J4 越界区存在大量「每个 uint16 可打印」的 64 位值 ⇒ 堆管理器会把
     它们当空闲块链表指针解引用，正是 RtlpHpVsSlotCompactChunks 的 AV
  J5 修复版（n < RB_LOG_MAX 钳位）=> 0 越界
  J6 当前实现 + 环内 <=64 条      => 0 越界（解释为何不是每次调用都崩）
"""
import ctypes as C
import json
import struct
from pathlib import Path

OUT = Path(r"C:\Users\Public\o9\stress\d2_result.json")
RB_LOG_MAX = 64
RB_LOGQ_MAX = 512
RB_LOG_REC_SIZE = 120
RB_LOG_LIST_SIZE = 8 + RB_LOG_MAX * RB_LOG_REC_SIZE
RB_MSG_SIZE = 16 + 8208
TEXT_OFFSET_IN_REC = 24
L2_LOG = "l2: store rejected (out of window) [diag O9]"
NEEDLE = b"\x20\x00\x28\x00\x6f\x00\x75\x00"   # " (ou"
RBX = 0x0075006F00280020
# 真正写到相邻池块的字节数：写入总量 8+512*120 减去 msg->Data 可用 8208 B
OVERFLOW_LIMIT = (8 + RB_LOGQ_MAX * RB_LOG_REC_SIZE) - (RB_MSG_SIZE - 16)
OVERFLOW_VS_LOGLIST = (8 + RB_LOGQ_MAX * RB_LOG_REC_SIZE) - RB_LOG_LIST_SIZE


def encode_rec(seq, text, rec_size=RB_LOG_REC_SIZE):
    """按 pack=8 的 RB_LOG_REC 布局编码一条记录。"""
    b = bytearray(rec_size)
    struct.pack_into("<QIIi", b, 0, 1000 + seq, 1, 650, 0)
    w = (text[:47] + "\0").encode("utf-16-le")
    b[TEXT_OFFSET_IN_REC:TEXT_OFFSET_IN_REC + len(w)] = w
    return bytes(b)


def make_ring(n):
    """模拟 g_Log 环：每 7 条放一条 L2 文案，其余为普通日志文案。"""
    out = []
    for i in range(n):
        t = L2_LOG if i % 7 == 3 else "volume attach sweep: letter resolved [diag O9]"
        out.append(encode_rec(i, t))
    return out


def snapshot(ring, clamp):
    """返回 (写入字节流, 越界字节, n)；模拟 out=&msg->Data.LogList。"""
    buf = bytearray(RB_MSG_SIZE)
    # out 位于 msg->Data，偏移 16
    data_off = 16
    n = 0
    for i in range(min(len(ring), RB_LOGQ_MAX)):
        if clamp and n >= RB_LOG_MAX:
            break
        off = data_off + 8 + n * RB_LOG_REC_SIZE
        end = off + RB_LOG_REC_SIZE
        if end <= len(buf):
            buf[off:end] = ring[i]
        else:
            # 越界部分：记录到独立的 overflow 缓冲
            pass
        n += 1
    # 重新按「驱动实际会写多少」计算越界字节
    total = 8 + n * RB_LOG_REC_SIZE
    overflow = max(0, total - (len(buf) - data_off))
    # 构造完整写入流（含越界部分），便于分析其内容
    stream = bytearray()
    stream += b"\0" * 8                     # Count/Cursor
    stream += b"".join(ring[:n])
    return bytes(stream), overflow, n


def utf16ish64(v):
    return all(0x20 <= ((v >> (16 * i)) & 0xFFFF) <= 0x7E for i in range(4))


def main():
    r = {"geometry": {"RB_LOG_REC": RB_LOG_REC_SIZE,
                      "RB_LOG_LIST": RB_LOG_LIST_SIZE,
                      "RB_MSG": RB_MSG_SIZE,
                      "worst_write": 8 + RB_LOGQ_MAX * RB_LOG_REC_SIZE,
                      "overflow_into_pool": OVERFLOW_LIMIT,
                      "overflow_vs_loglist_abi": OVERFLOW_VS_LOGLIST},
         "checks": {}}
    full = make_ring(RB_LOGQ_MAX)

    # V0 当前实现 + 满环（主动激活）
    stream0, ov0, n0 = snapshot(full, clamp=False)
    tail0 = stream0[RB_LOG_LIST_SIZE:]
    vals0 = [struct.unpack_from("<Q", tail0, o)[0]
             for o in range(0, max(0, len(tail0) - 7))]
    r["V0_current_full_ring"] = {
        "written_records": n0, "written_bytes": len(stream0),
        "overflow_bytes": ov0,
        "needle_offset_in_overflow": tail0.find(NEEDLE),
        "rbx_value_present": RBX in vals0,
        "utf16ish_64bit_values": sum(1 for v in vals0 if utf16ish64(v)),
        "sample_utf16ish": ["0x%016X" % v for v in vals0 if utf16ish64(v)][:5]}
    r["checks"]["J1_overflow_exactly_53240"] = ov0 == OVERFLOW_LIMIT == 53240
    r["checks"]["J2_l2_utf16_needle_in_overflow"] = NEEDLE in tail0
    r["checks"]["J3_dump_rbx_value_reproduced"] = RBX in vals0
    r["checks"]["J4_many_utf16_values_as_fake_pointers"] = (
        sum(1 for v in vals0 if utf16ish64(v)) >= 100)

    # V1 当前实现 + 环内仅 40 条（阴性/潜伏性对照）
    s1, ov1, n1 = snapshot(make_ring(40), clamp=False)
    r["V1_current_short_ring"] = {"records": n1, "overflow_bytes": ov1}
    r["checks"]["J6_short_ring_no_overflow"] = ov1 == 0

    # V2 修复版 + 满环
    s2, ov2, n2 = snapshot(full, clamp=True)
    r["V2_fixed_full_ring"] = {"records": n2, "overflow_bytes": ov2,
                               "count_within_abi": n2 <= RB_LOG_MAX}
    r["checks"]["J5_fixed_zero_overflow"] = ov2 == 0 and n2 == RB_LOG_MAX

    r["interpretation"] = (
        "00:32 的 rbx=0x0075006F00280020 是 RbLogSnapshot 越界写入的 "
        "L\"l2: store rejected (out of window) [diag O9]\" 中 \" (ou\" 四个 WCHAR。"
        "旧 GET_LOG(method 9) 把最多 512 条 120B 记录写进 64 条容量的 RB_MSG "
        "信封，越界 53240B 覆盖相邻池块；堆压缩线程随后把这些文本当成空闲块"
        "链表指针解引用 → nt!RtlpHpVsSlotCompactChunks+0x210 AV。"
        "环内日志 <=64 条时不越界，这解释了缺陷的潜伏性与 00:32 的偶发性。"
        if all(r["checks"].values()) else "模型未通过，需复核")
    r["VERDICT"] = "PASS" if all(r["checks"].values()) else "FAIL"
    OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"GEOMETRY": r["geometry"]}, ensure_ascii=False))
    print(json.dumps({"V0": r["V0_current_full_ring"]}, ensure_ascii=False))
    print(json.dumps({"V1": r["V1_current_short_ring"],
                      "V2": r["V2_fixed_full_ring"]}, ensure_ascii=False))
    print(json.dumps({"CHECKS": r["checks"]}, ensure_ascii=False))
    print(r["interpretation"])
    print("RESULT: %s" % r["VERDICT"])
    return 0 if r["VERDICT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
