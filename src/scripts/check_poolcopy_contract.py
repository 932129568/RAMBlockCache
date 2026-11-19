#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate13 · 池/静态区越界写契约（防 00:32 根因族回归）

背景：11-09 00:32 的 0x7E/0xC0000005 池腐坏运行于 c6d36c4c（00:11 代）。
该代同时含两处已定位的越界写，均在当天后续批次修复：
  - 119 号 13:21：Rb2CurrentImagePath 的
        us->MaximumLength = sizeof(buf) - 8
    与「数据区从 +8 个 WCHAR(=16B) 开始」不匹配 ⇒ 多报 8 字节越界；
    正确契约是 sizeof(buf) - sizeof(UNICODE_STRING)。
  - 121 号 14:06：s_ExPidN 无锁「检查/索引/++」TOCTOU ⇒
    s_ExPid[64] 越界写，提交注释明确标注「与 00:32 未点名的 0x7E 同族候选」；
    正确契约是 volatile LONG + Interlocked 原子占槽 + 读侧上界钳到 64。
本门禁静态钉死这些契约，并覆盖另外两处 Unicode/大块池拷贝边界：
  - RbReadRegValue：分配 qs + sizeof(WCHAR)、拷贝 qs、NUL 写在 [qs/2]
  - L2 记录：RB2_L2REC = sizeof(RB2_L2HDR) + RB2_L2BLK，且
    RtlCopyMemory(rec+1, src, RB2_L2BLK) 不得越出该分配

用法：python check_poolcopy_contract.py [--root <repo_root>]
      --root 供变异测试在最小骨架上运行，真实仓库零改动。
"""
import argparse
import re
import sys
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[2]


def strip_c(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", l) for l in text.splitlines())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    args = ap.parse_args()
    root = Path(args.root) if args.root else DEFAULT_ROOT
    fails = []

    paths = {
        "rbfeat": root / "src/driver/rbfeat.c",
        "rbattach": root / "src/driver/rbattach.c",
        "rbL2": root / "src/driver/rbL2.c",
        "rbdriver": root / "src/driver/rbdriver.c",
        "rblog": root / "src/driver/rblog.c",
        "rbproto": root / "src/ctl/rb_proto.h",
        "ci": root / ".github/workflows/ci.yml",
    }
    for k, p in paths.items():
        if not p.is_file():
            fails.append("missing file: %s" % p)
    if fails:
        print("\n".join(fails))
        print("result: FAIL (%d fail)" % len(fails))
        return 1

    feat = strip_c(paths["rbfeat"].read_text(encoding="utf-8-sig"))
    attach = strip_c(paths["rbattach"].read_text(encoding="utf-8-sig"))
    l2 = strip_c(paths["rbL2"].read_text(encoding="utf-8-sig"))
    driver = strip_c(paths["rbdriver"].read_text(encoding="utf-8-sig"))
    rlog = strip_c(paths["rblog"].read_text(encoding="utf-8-sig"))
    ci = paths["ci"].read_text(encoding="utf-8")

    # A1/A2 119 号：MaximumLength 必须与 UNICODE_STRING 头对齐，且旧写法绝迹
    good_ml = "us->MaximumLength = (USHORT)(sizeof(buf) - sizeof(UNICODE_STRING));"
    bad_ml = "sizeof(buf) - 8"
    n_good = feat.count(good_ml)
    if n_good < 2:
        fails.append("A1 rbfeat: sizeof(UNICODE_STRING)-aligned MaximumLength "
                     "must appear on BOTH the primary and the 27-fallback path "
                     "(found %d)" % n_good)
    if bad_ml in feat:
        fails.append("A2 rbfeat: legacy 'sizeof(buf) - 8' MaximumLength "
                     "(8-byte OOB, 119 pre-fix) reintroduced")

    # A3/A4 121 号：原子占槽 + 读侧钳位
    if "static volatile LONG s_ExPidN;" not in feat:
        fails.append("A3 rbfeat: s_ExPidN must be 'static volatile LONG' "
                     "(plain ULONG reintroduces TOCTOU OOB write)")
    if "static ULONG  s_ExPidN;" in feat:
        fails.append("A3b rbfeat: plain ULONG s_ExPidN detected")
    if feat.count("InterlockedCompareExchange((volatile LONG *)&s_ExPidN") < 3:
        fails.append("A4 rbfeat: Interlocked-based slot reservation incomplete "
                     "(need read + CAS-loop + publish)")
    if "if (n > RB2_EXPIID_MAX) n = RB2_EXPIID_MAX;" not in feat:
        fails.append("A4b rbfeat: read-side upper bound clamp missing")

    # A5 RbReadRegValue：分配/拷贝/NUL 三者同界
    for token, why in (
        ("qs + sizeof(WCHAR)", "allocation"),
        ("RtlCopyMemory(copy, qd, qs)", "copy length"),
        ("((PWCHAR)copy)[qs / sizeof(WCHAR)] = L'\\0';", "NUL index"),
    ):
        if token not in attach:
            fails.append("A5 rbattach: RbReadRegValue %s contract missing" % why)

    # A6 L2 记录：头 24B + 64KB，写 rec+1 不越界
    m_blk = re.search(r"#define\s+RB2_L2BLK\s+\(64\s*\*\s*1024\)", l2)
    m_rec = re.search(r"#define\s+RB2_L2REC\s+\(24\s*\+\s*RB2_L2BLK\)", l2)
    m_hdr = re.search(r"typedef struct _RB2_L2HDR\s*\{(.*?)\}\s*RB2_L2HDR;",
                      l2, re.S)
    if not (m_blk and m_rec and m_hdr):
        fails.append("A6 rbL2: RB2_L2BLK / RB2_L2REC / RB2_L2HDR definition missing")
    else:
        fields = m_hdr.group(1)
        size = (8 if "ULONG64 Lba" in fields else 0) + \
               (4 if "ULONG Stamp" in fields else 0) + \
               (4 if "ULONG Crc" in fields else 0) + \
               (8 if "ULONG64 VolId" in fields else 0)
        if size != 24:
            fails.append("A6 rbL2: RB2_L2HDR size is %d, RB2_L2REC assumes 24 "
                         "=> RtlCopyMemory(rec+1,...,64KB) would overflow" % size)
        if "RtlCopyMemory((PCHAR)(rec + 1), src, RB2_L2BLK);" not in l2:
            fails.append("A6b rbL2: L2 record payload copy missing or changed")

    # A8 00:32 根因（130 号）：RbLogSnapshot 输出容量钳位。
    # 环容量 512、RB_LOG_LIST.Items[64]；无钳位时写 61440 B 到 7688 B
    # 信封，越界 53240 B，写入内容即日志 Unicode 文本。
    # [142] 日志环整体迁 rblog.c（rbdriver.c 已无该段），契约锚点随符号走。
    if "for (i = 0; i < valid && n < RB_LOG_MAX; i++)" not in rlog:
        fails.append("A8 rblog: RbLogSnapshot missing 'n < RB_LOG_MAX' "
                     "clamp - 00:32 pool corruption (53KB OOB) would return")
    if "for (i = 0; i < valid && n < maxCount; i++)" not in rlog:
        fails.append("A8b rblog: RbLogQueryPair must keep 'n < maxCount' "
                     "clamp (0x809 LOGQ output)")
    if "#define RB_LOG_MAX       64" not in paths["rbproto"].read_text(
            encoding="utf-8-sig"):
        fails.append("A8c rb_proto.h: RB_LOG_MAX=64 ABI changed; "
                     "re-verify snapshot capacity")

    # A7 CI 自检
    if "check_poolcopy_contract.py" not in ci:
        fails.append("A7 CI: Gate13 not registered in ci.yml")

    # A9 [I48 2026-11-10] WB/Enabled 降档必须 fail-closed（数据安全门）：
    #   旧实现忽略 flush 结果并无条件 discard（core 侧 count_lost=1 计入
    #   lost_dirty_bytes），AV 在位时 flush 连续失败 → 一次降档即静默丢弃
    #   7.87MB 已 ack 脏数据（e4/e5 探针实证）。断言：丢弃点之前必须有
    #   dirty_after 守卫，且守卫分支必须返回 STATUS_IO_DEVICE_ERROR。
    rbctl_txt = root / "src/driver/rbctl.c"
    rbcfg_txt = root / "src/driver/rbconfig.c"
    if not rbctl_txt.is_file() or not rbcfg_txt.is_file():
        fails.append("A9 rbctl.c/rbconfig.c missing")
    else:
        ctl = rbctl_txt.read_text(encoding="utf-8-sig")
        cfgconf = rbcfg_txt.read_text(encoding="utf-8-sig")
        didx = cfgconf.find("RbDiscardAllCachedData();")
        if didx < 0:
            fails.append("A9 rbconfig: RbDiscardAllCachedData call site missing")
        else:
            guard = cfgconf[max(0, didx - 2400):didx]
            if "dirty_after != 0" not in guard:
                fails.append("A9 rbconfig: discard without 'dirty_after != 0' guard "
                             "- data-loss regression (I48)")
            if "STATUS_IO_DEVICE_ERROR" not in guard:
                fails.append("A9 rbconfig: transition refusal return missing "
                             "- fail-open regression (I48)")

    # A10 [I48b 2026-11-10] 软 detach 路径同样必须 fail-closed（第二条丢弃通道：
    #   rbattach.c 热停用时 flush 后无条件 discard_all，AV 在场时实测同样丢脏）。
    att = attach
    didx2 = att.find("rb_stream_discard_all(")
    if didx2 < 0:
        fails.append("A10 rbattach: detach discard site missing")
    else:
        g2 = att[max(0, didx2 - 1400):didx2]
        if "left == 0" not in g2 and "dirty_bytes(st) == 0" not in g2:
            fails.append("A10 rbattach: detach discard without dirty==0 guard "
                         "- data-loss regression (I48b)")

    # A11/A12 [I48c/I48d 2026-11-10] 第三条/第四条丢弃通道：attach 热开启与
    #   SET_VOLUME 切直写同样"flush 失败仍无条件 discard_all"（真机 e5 一次
    #   attach 丢 7,872,512 B）。断言：两处丢弃点前必须有 dirty==0 守卫，
    #   否则必须走 rb_stream_discard_clean（只丢干净块）并留 KEPT 日志。
    for tag, marker in (("A11 attach", "attach: cache view KEPT"),
                        ("A12 set-volume", "write-through switch: cache view KEPT")):
        midx = ctl.find(marker)
        if midx < 0:
            fails.append("%s rbctl: KEPT marker missing - data-loss regression (I48c/d)" % tag)
            continue
        g = ctl[max(0, midx - 1600):midx]
        if "discard_clean" not in g:
            fails.append("%s rbctl: discard path lacks rb_stream_discard_clean "
                         "- data-loss regression (I48c/d)" % tag)
        if "dirty_bytes(ext->Stream) == 0" not in g:
            fails.append("%s rbctl: discard without dirty==0 guard "
                         "- data-loss regression (I48c/d)" % tag)

    # A13 [I48e/I48f 2026-11-10] 簇重排 FSCTL/IOCTL：flush 未净时必须
    #   fail-closed（拒绝转发），不得 discard_all（丢脏）也不得把脏留在
    #   旧 LBA 上继续回写（重排后以旧盖新）。
    for tag, marker in (("A13 fsctl", "fsctl: refused - dirty remains after flush (cluster-affecting op)"),
                        ("A13 ioctl", "ioctl: refused - dirty remains after flush (cluster-affecting op)")):
        midx = ctl.find(marker)
        if midx < 0:
            fails.append("%s rbctl: fail-closed marker missing (I48e/f)" % tag)
            continue
        g = ctl[max(0, midx - 900):midx]
        if "STATUS_DEVICE_BUSY" not in g or "RbCtlFail(irp, STATUS_DEVICE_BUSY)" not in ctl:
            fails.append("%s rbctl: cluster-affecting op not fail-closed "
                         "- data-loss/corruption regression (I48e/f)" % tag)

    if fails:
        for f in fails:
            print("[FAIL] %s" % f)
        print("result: FAIL (%d fail, 0 warn)" % len(fails))
        return 1
    print("Gate13 pool-copy contract: PASS "
          "(A1/A2 119 layout, A3/A4 121 atomic slot, A5 reg value, "
          "A6 L2 record, A8 log snapshot clamp, A7 CI, "
          "A9/A10/A11/A12/A13 data-loss guards)")
    print("result: PASS (0 fail, 0 warn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
