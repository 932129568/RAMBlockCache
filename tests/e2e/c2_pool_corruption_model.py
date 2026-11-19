#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C2 · 00:32 池腐坏「为什么」因果模型（纯用户态，单机，无风险）

输入证据（本机 cdb 离线分析 110926-17187-01.dmp）：
  - BugCheck 0x7E / 0xC0000005，异常点 nt!RtlpHpVsSlotCompactChunks+0x210
  - 故障指令 cmp qword ptr [rbx+8],0；rbx = rax = 0x0075006f00280020
  - 栈完全在 ExpHpCompactionRoutine -> RtlpHpVsSlotCompactChunks（我方不在栈）
  - 模块表含 RAMBlockCache，PE timestamp 0x6AF09F34（11-09 00:11:32 GMT+8）
  - 该代 PDB 未归档；根目录 PDB 签名不匹配（!lmi: no symbols loaded）

判据（每组带阳性 / 阴性对照，回答「为什么」而不只是「是什么」）：
  J1 特征值判别：rbx 的 4 个 uint16 是否全为可打印 ASCII（UTF-16 文本），
     与同一上下文里的真实内核指针（高 16 位 ffff）对照。
  J2 UNICODE_STRING 头同构：低 16 位 Length、次 16 位 MaximumLength
     是否满足 0 < Length <= MaximumLength <= 1024 且均为偶数。
  J3 布局越界复现：按 119 号记录的旧实现精确复现
     WCHAR buf[520] / us->Buffer = buf+8(WCHAR) / MaximumLength = sizeof(buf)-8
     与修复版 sizeof(UNICODE_STRING) 的差分；量化越界字节数与内容特征。
  J4 堆压缩 AV 因果：越界的 8 字节若落在相邻池块头前 8 字节，
     堆管理器把它当链表指针时应读到非规范地址；对照「无越界」时块头仍为内核指针。
  J5 候选分配点分类：源码 19 个 ExAllocatePool2 调用点中，哪些可能把
     Unicode 文本写进池（能解释 J1/J2 的字节特征）。

纪律：J3 是按 119 号报告文字复现的布局缺陷；本机未反汇编内核
NtQueryInformationProcess 的写入语义，因此 J3 只作「结构性解释」，
不作 00:32 根因确证。
"""
import json
import struct
from pathlib import Path

OUT = Path(r"C:\Users\Public\o9\stress\c2_result.json")

RBX = 0x0075006F00280020
NEG = {
    "rdx": 0xFFFF85838E1D8038,
    "rsi": 0xFFFF858F643A9FE0,
    "rip": 0xFFFFF800E1B99210,
    "r10": 0xFFFF858E13107C00,
    "r11": 0x0000000000007FFF,   # 小整数，作为「非文本但非指针」对照
}

RB2_EXDIR_LEN = 260
BUF_WCHARS = RB2_EXDIR_LEN * 2
BUF_BYTES = BUF_WCHARS * 2
HDR_BYTES = 16                 # sizeof(UNICODE_STRING) on x64
DATA_BYTES = BUF_BYTES - HDR_BYTES


def u16s(v):
    return [(v >> (16 * i)) & 0xFFFF for i in range(4)]


def is_utf16_text(v):
    us = u16s(v)
    return all(0x20 <= u <= 0x7E for u in us)


def is_kernel_ptr(v):
    return ((v >> 48) & 0xFFFF) == 0xFFFF


def us_header(v):
    length = v & 0xFFFF
    maxlen = (v >> 16) & 0xFFFF
    plausible = (0 < length <= maxlen <= 1024 and
                 length % 2 == 0 and maxlen % 2 == 0)
    return {"Length": length, "MaximumLength": maxlen,
            "plausible": plausible}


def overflow_case(max_len_bytes, path_units):
    """内核从 buf+HDR 起最多写 max_len_bytes 字节；返回越界字节数与其内容。"""
    end = HDR_BYTES + max_len_bytes
    over = max(0, end - BUF_BYTES)
    tail = b""
    if over:
        # 越界内容是路径文本的最后 over/2 个 WCHAR
        need = over // 2
        tail = b"".join(struct.pack("<H", c) for c in path_units[-need:])
    return {"max_len": max_len_bytes, "write_end": end,
            "buf_bytes": BUF_BYTES, "overflow_bytes": over,
            "overflow_hex": tail.hex(),
            "overflow_is_utf16": is_utf16_text(
                int.from_bytes(tail.ljust(8, b"\0")[:8], "little")) if over else None,
            "av_if_used_as_flink": over > 0 and not is_kernel_ptr(
                int.from_bytes(tail.ljust(8, b"\0")[:8], "little")) if over else False}


def main():
    r = {"evidence": {"rbx": "0x%016X" % RBX,
                      "bugcheck": "0x7E / 0xC0000005",
                      "faulting_ip": "nt!RtlpHpVsSlotCompactChunks+0x210",
                      "instruction": "cmp qword ptr [rbx+8],0",
                      "module_ts": "0x6AF09F34 / 2026-11-09 00:11:32 GMT+8"},
         "checks": {}}

    # J1 UTF-16 特征 vs 真实内核指针
    j1 = {"rbx_units": ["0x%04X" % u for u in u16s(RBX)],
          "rbx_as_text": "".join(chr(u) for u in u16s(RBX)),
          "rbx_is_utf16_text": is_utf16_text(RBX),
          "rbx_is_kernel_ptr": is_kernel_ptr(RBX),
          "negative": {k: {"is_utf16_text": is_utf16_text(v),
                           "is_kernel_ptr": is_kernel_ptr(v)} for k, v in NEG.items()}}
    j1["pass"] = (j1["rbx_is_utf16_text"] and not j1["rbx_is_kernel_ptr"] and
                  all(not v["is_utf16_text"] for v in j1["negative"].values()) and
                  all(j1["negative"][k]["is_kernel_ptr"]
                      for k in ("rdx", "rsi", "rip", "r10")))
    r["J1_utf16_signature"] = j1
    r["checks"]["J1_utf16_not_pointer"] = j1["pass"]

    # J2 UNICODE_STRING 头
    h = us_header(RBX)
    j2 = {"rbx": h,
          "negative": {k: us_header(v) for k, v in NEG.items()}}
    j2["pass"] = h["plausible"] and not any(v["plausible"] for v in j2["negative"].values())
    r["J2_unicode_string_header"] = j2
    r["checks"]["J2_header_plausible_only_for_rbx"] = j2["pass"]

    # J3 旧 / 新布局越界复现
    sample = "\\Device\\HarddiskVolume6\\Users\\Public\\o9\\stress\\python.exe"
    units = [ord(c) for c in sample]
    while len(units) * 2 < 1032:            # 填到旧 MaximumLength 上限
        units += [ord(c) for c in "_padding_"]
    units = units[:516]
    old = overflow_case(BUF_BYTES - 8, units)          # 119 号记录的旧布局
    new = overflow_case(BUF_BYTES - HDR_BYTES, units)  # 修复版
    j3 = {"layout": {"buf_wchars": BUF_WCHARS, "buf_bytes": BUF_BYTES,
                     "unicode_string_header": HDR_BYTES,
                     "data_bytes": DATA_BYTES},
          "old_implementation": old, "fixed_implementation": new,
          "note": "按 119 号报告文字复现；内核写入语义未在本机反汇编验证"}
    j3["pass"] = (old["overflow_bytes"] == 8 and new["overflow_bytes"] == 0 and
                  old["overflow_is_utf16"] is True)
    r["J3_layout_overflow"] = j3
    r["checks"]["J3_old_overflows_8B_fixed_zero"] = j3["pass"]

    # J4 堆压缩 AV 因果
    j4 = {"old_flink_hex": old["overflow_hex"],
          "old_av_expected": old["av_if_used_as_flink"],
          "fixed_flink": "ffff858e`13107c00 (示例真实内核指针)",
          "fixed_av_expected": False,
          "why_late": ("越界写发生在某次路径查询当下；堆管理器只在后续 "
                       "ExpHpCompactionRoutine 遍历空闲块链表时解引用被覆盖的指针，"
                       "故崩溃线程是 System worker、栈上无我方帧、bugcheck 为 0x7E/AV")}
    j4["pass"] = old["av_if_used_as_flink"] and not j4["fixed_av_expected"]
    r["J4_heap_compaction_causality"] = j4
    r["checks"]["J4_corruption_detected_late"] = j4["pass"]

    # J5 候选分配点分类（源码 grep 结果，按「能否写 Unicode 到池」）
    candidates = [
        {"site": "rbfeat.c:118 Rb2Alloc", "writes_unicode": True,
         "payload": "ExcludeDirs / 默认进程名单（WCHAR 路径），由 Rb2LoadExcl 等填充",
         "trigger": "配置 ExcludeDirs + 新进程首次 IO / 配置刷新"},
        {"site": "rbdriver.c:223/237 RbRegQueryValue", "writes_unicode": True,
         "payload": "注册表 REG_SZ / REG_MULTI_SZ 值（盘符、DmpFile 等）",
         "trigger": "驱动启动 / Features 重载"},
        {"site": "rbattach.c:61 RbReadRegValue", "writes_unicode": True,
         "payload": "注册表值 + NUL 终止（qs + sizeof(WCHAR)）",
         "trigger": "引导关键卷硬表构建 / attach"},
        {"site": "rbdev.c:21/169 sync ctx + PoolBuf", "writes_unicode": False,
         "payload": "二进制 IO 缓冲 / 同步 ctx", "trigger": "每 IRP"},
        {"site": "rbattach.c:292/301/306 IOCTL sync pools", "writes_unicode": False,
         "payload": "RB_CONFIG / STATS 二进制结构", "trigger": "控制面 IOCTL"},
        {"site": "rbio.c:900/1882/1907 DseqBuf + FLUSHJOB", "writes_unicode": False,
         "payload": "ULONG 序列 / 刷脏作业", "trigger": "回写 / FLUSH"},
        {"site": "rbdriver.c:807 heap_snap", "writes_unicode": False,
         "payload": "设备扩展指针数组", "trigger": "PnP 枚举"},
    ]
    j5 = {"total_pool_alloc_sites": 19,
          "unicode_capable": [c for c in candidates if c["writes_unicode"]],
          "binary_only": [c for c in candidates if not c["writes_unicode"]],
          "note": ("J1/J2 要求覆盖内容是 UTF-16 文本 ⇒ 二进制-only 分配点不可能产生 "
                   "0x0075006f00280020 这类值，故写入源收敛到 3 处；但 triage dump "
                   "无完整内存、无该代 PDB、栈上无我方帧，仍不能把「被覆盖块的 "
                   "ExAllocatePool2 行号」点名到唯一一处")}
    j5["pass"] = len(j5["unicode_capable"]) == 3
    r["J5_candidate_sites"] = j5
    r["checks"]["J5_candidates_narrowed_to_3"] = j5["pass"]

    r["VERDICT"] = "PASS" if all(r["checks"].values()) else "FAIL"
    OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
    for k, v in r.items():
        if k in ("checks", "VERDICT", "evidence"):
            continue
        print(json.dumps({k: v}, ensure_ascii=False))
    print(json.dumps({"CHECKS": r["checks"]}, ensure_ascii=False))
    print("RESULT: %s" % r["VERDICT"])
    return 0 if r["VERDICT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
