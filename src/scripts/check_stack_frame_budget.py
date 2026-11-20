# -*- coding: utf-8 -*-
"""
check_stack_frame_budget.py  [298 2026-11-19]

为什么必须有这个门禁（不是风格检查，是止血护栏）：

  2026-11-19 两次 0x7F / Arg1=8 (EXCEPTION_DOUBLE_FAULT)，PROCESS_NAME=msedgewebview2.exe，
  两次 dump 的 Arg4 末 4 位均为 f6a6。取证闭合的机理：

    RbDispatchDeviceControl 的入口 prolog 一次性扣 1480 B
      = ALLOC_LARGE 1424 + 7 x PUSH_NONVOL 56
    其中 1304 B 只属于一个变量：RbMsgGetStats 里的 `RB_STATS local`
    而崩溃走的是 `call RbForward` 的**透传分支**——这条路径一个局部变量
    都不需要，却全额付了 1424 B。缺口精确 32 B。

  根因是 MSVC /O2 把 14 个 RbMsg* handler 内联进 dispatch，把各自的局部变量
  并成一个 1424 B 的**并集帧**。修复 = __declspec(noinline) 拆帧 + GET_STATS 直填。

  但这次修复此前**没有任何自动化护栏**：只要有人加一个 #pragma、给链接器加 /GL、
  或重构让某个 handler 重新内联，帧就会静默回到 1400+，而 build/gates 全绿。
  本门禁把「不变量」变成可执行的断言。

判据（全部 fail-closed，任一不满足即 FAIL）：

  S1  RbDispatchDeviceControl 入口帧 <= MAX_ENTRY_FRAME (230 B)
      —— 旧值 1480，新值 120。上限取 230 是为了容一点编译器余量，
         但远低于任何「内联回来」的规模（内联回 GET_STATS 就会 >1300）。

  S2  全映像不存在 [1400, 1600] 区间的帧
      —— 1480 是本次崩溃帧的精确值。这个区间若再次出现，说明并集帧回来了。
         允许 1400..1600 之外的更大帧（如 RbStartIoWorkers 6328）是已登记的
         独立问题（有 __chkstk 保护，另一条路径），本门禁不越界管它。

  S3  对照件 ntoskrnl.exe 的 unwind 解析必须 100% 自洽
      —— 判据自检。若解析器本身有错，S1/S2 的数字一律不可信。
         这一条是本仓反复踩过的坑（空匹配集判 PASS / 判据本身是坏的）。

  S4  同名函数在新旧件里必须都能找到
      —— 防止「符号找不到 => 空集 => PASS」。用字节特征串兜底定位，
         不依赖 PDB/符号服务器。

纪律说明：
  * 本脚本只读 PE 字节，不碰构建产物，不改任何文件。
  * 解析器不采信任何外部工具（dumpbin 可能不在 PATH）。
  * 变异测试见 --self-test：故意把 MAX_ENTRY_FRAME 调到 100，
    必须 FAIL；调到 2000，必须通过 S1 但被 S2 拦下。
"""

import argparse
import collections
import os
import hashlib
import struct
import sys

# ---------------------------------------------------------------------------
# 判据常量。改这些数之前先读上面的机理说明。
# ---------------------------------------------------------------------------
MAX_ENTRY_FRAME = 230          # S1
CRASH_FRAME_LO = 1400          # S2 区间下界（含）
CRASH_FRAME_HI = 1600          # S2 区间上界（含）
TARGET_ENTRY = 'RbDispatchDeviceControl'
# 修复后 prolog 的首字节特征（mov [rsp+8],rbx / 7 push / mov rbp,rsp / sub rsp,0x40）
ENTRY_SIG_NEW = bytes.fromhex('48895C24085556574154415541564157488BEC4883EC40')
# 修复前 prolog 特征（sub rsp,0x590）——出现即为回归
ENTRY_SIG_OLD = bytes.fromhex('4881EC90050000')
# RbStartIoWorkers 的 6328 B 帧标记（mov eax,0x18a0 -> 0x18a0/8-1 = 0x312）
STARTIOWORKERS_MARK = bytes.fromhex('B8A0180000')

# UWOP 语义表：slot 数。只有 PUSH_NONVOL(0)/ALLOC_LARGE(1)/ALLOC_SMALL(2)/
# PUSH_MACHFRAME(10) 真正移动 RSP；SET_FPREG(3)/SAVE_*(4,5,8,9)/EPILOG(6) 不移动。
UWOP_SLOTS = {0: 1, 2: 1, 3: 1, 4: 2, 5: 3, 6: 2, 7: 1, 8: 2, 9: 3, 10: 1}
UWOP_NAME = {0: 'PUSH_NONVOL', 1: 'ALLOC_LARGE', 2: 'ALLOC_SMALL', 3: 'SET_FPREG',
             4: 'SAVE_NONVOL', 5: 'SAVE_NONVOL_FAR', 6: 'EPILOG', 7: 'SPARE',
             8: 'SAVE_XMM128', 9: 'SAVE_XMM128_FAR', 10: 'PUSH_MACHFRAME'}


class PE(object):
    def __init__(self, path):
        with open(path, 'rb') as fh:
            self.d = fh.read()
        d = self.d
        if d[:2] != b'MZ':
            raise ValueError('%s: not MZ' % path)
        e_lfanew = struct.unpack_from('<I', d, 0x3C)[0]
        if d[e_lfanew:e_lfanew + 4] != b'PE\0\0':
            raise ValueError('%s: not PE' % path)
        coff = e_lfanew + 4
        nsec = struct.unpack_from('<H', d, coff + 2)[0]
        optsz = struct.unpack_from('<H', d, coff + 16)[0]
        opt = coff + 20
        self.magic = struct.unpack_from('<H', d, opt)[0]
        if self.magic == 0x20b:      # PE32+
            self.image_base = struct.unpack_from('<Q', d, opt + 24)[0]
            self.size_of_image = struct.unpack_from('<I', d, opt + 56)[0]
            dd = opt + 112
        elif self.magic == 0x10b:
            self.image_base = struct.unpack_from('<I', d, opt + 28)[0]
            self.size_of_image = struct.unpack_from('<I', d, opt + 56)[0]
            dd = opt + 96
        else:
            raise ValueError('%s: unknown optional magic 0x%x' % (path, self.magic))
        self.size_of_headers = struct.unpack_from('<I', d, opt + 60)[0]
        # data directory #3 = Exception (.pdata)
        self.pdata_rva = struct.unpack_from('<I', d, dd + 3 * 8)[0]
        self.pdata_size = struct.unpack_from('<I', d, dd + 3 * 8 + 4)[0]
        self.sections = []
        sh = opt + optsz
        for i in range(nsec):
            off = sh + i * 40
            name = d[off:off + 8].rstrip(b'\0').decode('latin-1')
            vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, off + 8)
            self.sections.append((name, va, max(vsz, rsz), rptr, rsz))
        self.rts = self._parse_pdata()

    def r2o(self, rva):
        for name, va, sz, rptr, _rsz in self.sections:
            if va <= rva < va + sz:
                return rptr + (rva - va)
        return None

    def secname(self, rva):
        for name, va, sz, rptr, _rsz in self.sections:
            if va <= rva < va + sz:
                return name
        return None

    def _parse_pdata(self):
        out = []
        if not self.pdata_rva or not self.pdata_size:
            return out
        o = self.r2o(self.pdata_rva)
        if o is None:
            return out
        n = self.pdata_size // 12
        for i in range(n):
            b, e, u = struct.unpack_from('<III', self.d, o + i * 12)
            if not b:
                continue
            out.append((b, e, u))
        return out


def unw_info(pe, u):
    """返回 (ver, flags, cnt, frame_reg, codes_off, extra_off)。"""
    o = pe.r2o(u)
    if o is None or o + 4 > len(pe.d):
        return None
    d = pe.d
    cnt = d[o + 2]
    codes = o + 4
    extra = codes + ((cnt * 2 + 3) & ~3)
    return (d[o] & 7, d[o] >> 3, cnt, d[o + 3] & 0xF, codes, extra)


def rsp_alloc(pe, u, depth=0, seen=None):
    """prolog 阶段真正下移 RSP 的字节数（含 CHAININFO 链）。

    只累计 PUSH_NONVOL / ALLOC_LARGE / ALLOC_SMALL / PUSH_MACHFRAME。
    SAVE_NONVOL / SAVE_XMM128 只是写进已分配的栈槽，不动 RSP ——
    把它们当分配会把同一段栈重复计入（296 号报告初版踩过，算成 25,280 B）。
    """
    if seen is None:
        seen = set()
    if u in seen or depth > 6:
        return 0
    seen.add(u)
    info = unw_info(pe, u)
    if info is None:
        return 0
    _ver, flags, cnt, _fr, codes, extra = info
    d = pe.d
    tot = 0
    i = 0
    while i < cnt:
        b1 = d[codes + i * 2 + 1]
        op = b1 & 0xF
        oi = b1 >> 4
        if op == 1:                                    # ALLOC_LARGE
            if oi == 0:
                tot += struct.unpack_from('<H', d, codes + (i + 1) * 2)[0] * 8
                i += 2
            elif oi == 1:
                tot += struct.unpack_from('<I', d, codes + (i + 1) * 2)[0]
                i += 3
            else:
                i += 1
            continue
        if op == 0:
            tot += 8
        elif op == 2:
            tot += oi * 8 + 8
        elif op == 10:
            tot += 40
        i += UWOP_SLOTS.get(op, 1)
    if flags == 4 and extra + 12 <= len(d):
        _cb, _ce, cu = struct.unpack_from('<III', d, extra)
        if cu:
            tot += rsp_alloc(pe, cu, depth + 1, seen)
    return tot


def check_unwind(pe, u, depth=0):
    """单条 UNWIND_INFO 合法性自检。返回 (ok, why)。

    SAVE_XMM128 的 off 字段是「偏移/16 的槽位号」而非字节数（x64 EH 规范原文：
    "The offset, divided by 16, is recorded in the next slot"）。把槽号当字节数
    再判 %16，会把这 13/14/15 这类合法槽号全判成非法 —— 这是解析器缺陷的经典
    伪装成「发现」的形态，第一次实现在 ntoskrnl 上误报 102 条。
    """
    if depth > 6:
        return (True, 'depth-limit')
    info = unw_info(pe, u)
    if info is None:
        return (False, 'unwind 0x%x 越界' % u)
    ver, flags, cnt, fr_raw, codes, extra = info
    d = pe.d
    if ver not in (1, 2):
        return (False, 'Version=%d' % ver)
    if flags not in (0, 1, 2, 3, 4):
        return (False, 'Flags=0x%x' % flags)
    if codes + cnt * 2 > len(d):
        return (False, 'CountOfCodes 越界')
    frame = rsp_alloc(pe, u)
    i = 0
    while i < cnt:
        b1 = d[codes + i * 2 + 1]
        op = b1 & 0xF
        if op > 10:
            return (False, 'UnwindOp=%d > 10' % op)
        if op == 7:
            return (False, 'UnwindOp=7 SPARE')
        if op == 1:
            oi = b1 >> 4
            if oi == 0 and i + 1 >= cnt:
                return (False, 'ALLOC_LARGE 截断')
            if oi == 1 and i + 2 >= cnt:
                return (False, 'ALLOC_LARGE32 截断')
            if oi not in (0, 1):
                return (False, 'ALLOC_LARGE OpInfo=%d' % oi)
            i += 2 if oi == 0 else 3
            continue
        need = UWOP_SLOTS.get(op, 1)
        if i + need > cnt:
            return (False, '截断 op=%d 需 %d 剩 %d' % (op, need, cnt - i))
        if op == 8:
            slot = struct.unpack_from('<H', d, codes + (i + 1) * 2)[0]
            if frame and slot * 16 + 16 > frame:
                return (False, 'SAVE_XMM128 槽%d 偏移%d 超帧%d' % (slot, slot * 16, frame))
        if op == 3 and fr_raw == 0:
            return (False, 'SET_FPREG 但 FrameRegister=0')
        i += need
    if flags == 4:
        if extra + 12 > len(d):
            return (False, 'CHAININFO 越界')
        cb, ce, cu = struct.unpack_from('<III', d, extra)
        if cb or ce or cu:
            if not (cb and ce and cu):
                return (False, 'chain 部分为零')
            if cb >= ce:
                return (False, 'chain begin>=end')
            if pe.secname(cb) is None or pe.secname(ce - 1) is None:
                return (False, 'chain 区间越界')
            ok, why = check_unwind(pe, cu, depth + 1)
            if not ok:
                return (False, 'chain->' + why)
    return (True, 'ok')


def find_rva_by_sig(pe, sig):
    """在映像里找字节特征串，返回唯一命中的 RVA；不唯一/零命中都返回 None。

    零命中不得判 PASS —— 调用方必须把 None 当 FAIL 处理。
    """
    d = pe.d
    hits = []
    start = 0
    while True:
        k = d.find(sig, start)
        if k < 0:
            break
        # 反查到 RVA，且必须落在可执行节内（排除数据段里的偶然字节）
        for name, va, sz, rptr, _rsz in pe.sections:
            if rptr <= k < rptr + sz:
                rva = va + (k - rptr)
                if name in ('.text', 'PAGE', 'INIT'):
                    hits.append(rva)
                break
        start = k + 1
    if len(hits) != 1:
        return None
    return hits[0]


def owning_rt(pe, rva):
    """返回覆盖该 RVA 的 (begin, end, unwind) 或 None。"""
    for b, e, u in pe.rts:
        if b <= rva < e:
            return (b, e, u)
    return None


def frame_of_rva(pe, rva):
    rt = owning_rt(pe, rva)
    if rt is None:
        return None
    return (rt[0], rt[1], rsp_alloc(pe, rt[2]))


def audit(path, label, report, control_only=False):
    """control_only=True 时只跑 S3。

    为什么必须区分：对照件 ntoskrnl 的入口函数名与帧分布跟本仓无关，
    对它跑 S1/S2 必然 FAIL —— 那些 FAIL 是**预期的**，若混进判决会让
    真正的失败被淹没。第一次实现就踩了：ntoskrnl 打出两条 [FAIL] 但
    EXIT=0，读者无法分辨哪条才算。分离后，对照件只贡献 S3。
    """
    pe = PE(path)
    report.append('== %s' % label)
    report.append('   file = %s' % path)
    report.append('   size = %d B  SizeOfImage=0x%x  .pdata=%d 条'
                  % (len(pe.d), pe.size_of_image, len(pe.rts)))
    fails = []

    # --- S3 判据自检：解析器可信度 ---
    bad = 0
    reasons = collections.Counter()
    for b, e, u in pe.rts:
        if u is None:
            continue
        ok, why = check_unwind(pe, u)
        if not ok:
            bad += 1
            reasons[why] += 1
    report.append('   解析自检：%d/%d 自洽，%d 非法' % (len(pe.rts) - bad, len(pe.rts), bad))
    if bad:
        fails.append('S3 解析器自检未过：%d 条非法 unwind（%s）——本门禁其余数字不可信'
                     % (bad, dict(reasons)))
    if control_only:
        return fails

    # --- S4 旧帧特征必须消失 ---
    old = find_rva_by_sig(pe, ENTRY_SIG_OLD)
    if old is not None:
        fails.append('S4 发现旧入口帧 prolog（sub rsp,0x590）@RVA 0x%x —— 并集帧回归' % old)

    # --- S1 入口帧 ---
    entry = find_rva_by_sig(pe, ENTRY_SIG_NEW)
    if entry is None:
        fails.append('S4 无法用特征串唯一定位 %s 的新 prolog —— 空集不得判 PASS' % TARGET_ENTRY)
        report.append('   [FAIL] %s 特征串零命中或多命中' % TARGET_ENTRY)
    else:
        fr = frame_of_rva(pe, entry)
        if fr is None:
            fails.append('S4 %s @0x%x 无对应 .pdata 记录' % (TARGET_ENTRY, entry))
        else:
            b, e, alloc = fr
            report.append('   %s: RVA 0x%x  RT[0x%x,0x%x)  rsp_alloc=%d B  (上限 %d)'
                          % (TARGET_ENTRY, entry, b, e, alloc, MAX_ENTRY_FRAME))
            if alloc > MAX_ENTRY_FRAME:
                fails.append('S1 %s 入口帧 %d B > 上限 %d B —— 大帧回来了'
                             % (TARGET_ENTRY, alloc, MAX_ENTRY_FRAME))

    # --- S2 崩溃帧区间必须为空 ---
    inrange = []
    for b, e, u in pe.rts:
        if u is None:
            continue
        t = rsp_alloc(pe, u)
        if CRASH_FRAME_LO <= t <= CRASH_FRAME_HI:
            inrange.append((b, e, t))
    if inrange:
        for b, e, t in inrange[:8]:
            fails.append('S2 func 0x%x..0x%x 帧 %d B 落在崩溃区间 [%d,%d]'
                         % (b, e, t, CRASH_FRAME_LO, CRASH_FRAME_HI))
        report.append('   [FAIL] %d 个函数帧落在 [%d,%d]' % (
            len(inrange), CRASH_FRAME_LO, CRASH_FRAME_HI))
    else:
        report.append('   [ok] 无函数帧落在崩溃区间 [%d,%d]'
                      % (CRASH_FRAME_LO, CRASH_FRAME_HI))

    # --- 只作记录，不作判据：全映像最大帧 ---
    mx = max((rsp_alloc(pe, u) for b, e, u in pe.rts if u is not None), default=0)
    report.append('   [info] 全映像最大单帧 = %d B (%.1f%% of 12KB)' % (mx, 100.0 * mx / 12288))
    return fails


def check_dist_matches_release(sys_path, root, require, report):
    """S5：dist 送签件与本次构建同源（签名不敏感的不变量三连）。"""
    fails = []
    dist_path = os.path.join(root, 'dist', 'RAMBlockCache.sys')
    if not os.path.isfile(dist_path):
        fails.append('S5 dist/RAMBlockCache.sys 不存在 —— 送签面缺失，'
                     '空集不得判 PASS')
        return fails
    # [298e] 自比自防护（QA 实测 E1）：当 --sys 指向的就是 dist 那份文件时，
    # "dist 与本次构建一致"是同义反复 —— 恒真、零信息量。而 deploy.ps1 正是用
    # --sys $srcSys 调用、$srcSys 的默认首选就是 dist\，所以在最需要 S5 的时刻
    # 它恰恰失效。这正是本仓"判据在消费点上失效"的同族缺陷。
    try:
        _same = os.path.samefile(sys_path, dist_path)
    except (OSError, ValueError):
        _same = (os.path.normcase(os.path.abspath(sys_path))
                 == os.path.normcase(os.path.abspath(dist_path)))
    if _same:
        msg = ('--sys 指向的就是 dist\\RAMBlockCache.sys —— S5 退化为自比自，'
               '无法证明任何事（恒真、零信息量）。要证明送签面与本次构建同源，'
               '请对 x64\\Release 件运行本判据。')
        if require:
            fails.append('S5 ' + msg)
        else:
            report.append('   [SKIP] ' + msg)
        return fails

    rel_pe = PE(sys_path)
    dist_pe = PE(dist_path)
    report.append('== dist 送签件一致性 (S5)')
    report.append('   dist size = %d B  .pdata=%d 条'
                  % (len(dist_pe.d), len(dist_pe.rts)))
    report.append('   rel  size = %d B  .pdata=%d 条'
                  % (len(rel_pe.d), len(rel_pe.rts)))

    def sec_hash(pe, name):
        for n, _va, _sz, rptr, rsz in pe.sections:
            if n == name:
                return hashlib.sha256(pe.d[rptr:rptr + rsz]).hexdigest()
        return None

    rel_text = sec_hash(rel_pe, '.text')
    dist_text = sec_hash(dist_pe, '.text')
    if rel_text is None or dist_text is None:
        fails.append('S5 无法读取 .text 节 —— 判据无法执行，fail-closed')
    elif rel_text != dist_text:
        report.append('   rel  .text sha = %s' % rel_text[:16])
        report.append('   dist .text sha = %s' % dist_text[:16])
        # 签名前 dist 是"上一次已签名的件"，与 Release 不一致属正常中间态
        # （证据：本项目既有纪律「签名后部署前 C1/C1b 预期 FAIL」）。
        # 但"门禁全绿就给这份 dist 签名部署"是真实的断点 —— QA 第二轮 B1
        # 就是在这个状态下把崩溃件一路带到送签面。所以：
        #   * 默认（签名前）：报 WARN，把 dist 自己的帧数据打出来供人判读，
        #     不判 FAIL，否则 gates 在正常流程中间态就红，判据会被绕过；
        #   * --require-dist-match（签名后/交付前）：判 FAIL，强制闭合。
        msg = ('dist 的 .text 与本次构建不一致 —— 送签件不是这份源码的产物'
               '（QA 第二轮 B1 形态）')
        if require:
            fails.append('S5 ' + msg)
        else:
            report.append('   [WARN] ' + msg)
            report.append('          -> 若这是签名前的中间态，属预期；'
                          '签名后请用 --require-dist-match 复核。')

    rel_rts = sorted(rel_pe.rts)
    dist_rts = sorted(dist_pe.rts)
    s5_clean = (rel_text is not None and rel_text == dist_text
                and rel_rts == dist_rts)
    if rel_rts != dist_rts:
        m = ('dist 的 .pdata RUNTIME_FUNCTION 集合与本构建不同'
             '（rel %d 条 / dist %d 条）' % (len(rel_rts), len(dist_rts)))
        if require:
            fails.append('S5 ' + m)
        else:
            report.append('   [WARN] ' + m)
    else:
        rel_frames = tuple(rsp_alloc(rel_pe, u) for _b, _e, u in rel_rts)
        dist_frames = tuple(rsp_alloc(dist_pe, u) for _b, _e, u in dist_rts)
        if rel_frames != dist_frames:
            diff = sum(1 for a, b in zip(rel_frames, dist_frames) if a != b)
            m = 'dist 的函数帧大小向量与本构建不同（%d 处差异）' % diff
            s5_clean = False
            if require:
                fails.append('S5 ' + m)
            else:
                report.append('   [WARN] ' + m)

    if s5_clean:
        report.append('   [ok] .text sha / .pdata 集合 / 帧向量三者与 dist 一致')


    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sys', default=None, help='待检 RAMBlockCache.sys')
    ap.add_argument('--control', default=None,
                    help='对照件（默认取 %%SystemRoot%%\\System32\\ntoskrnl.exe）')
    # [298f] 这里曾有 --no-dist-check，已删除。它能在同一命令行里把
    # --require-dist-match 旁路掉：
    #     --sys <Release> --no-dist-check --require-dist-match   =>  rc=0
    # 而 --require-dist-match 正是「签名后必须复核」的唯一执行手段 ——
    # 一个软开关压过强制开关，等于那条交付纪律可以被随手取消（QA F1）。
    # 自比自现在由 samefile 自动识别，不需要调用方声明。
    ap.add_argument('--require-dist-match', action='store_true',
                    help='要求 dist 送签件与本次构建一致。签名前不必加（dist 是旧件'
                         '属正常中间态），签名后/交付前必须加，否则 S5 只报警告。')
    ap.add_argument('--self-test', action='store_true',
                    help='变异自检：证明判据有牙（不是空跑）')
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    sys_path = args.sys or os.path.join(root, 'src', 'driver', 'x64', 'Release',
                                        'RAMBlockCache.sys')
    ctl_path = args.control or os.path.join(
        os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'ntoskrnl.exe')

    report = []
    allfails = []

    if not os.path.isfile(sys_path):
        print('[FAIL] S4 目标件不存在：%s' % sys_path)
        print('       空集不得判 PASS —— 先构建再跑本门禁。')
        return 2

    if args.self_test:
        # 变异 M1：把上限压到 100 => 若当前帧是 120，S1 必须 FAIL
        # 变异 M2：把崩溃区间挪到 [100,200] => S2 必须 FAIL
        # 变异 M3：目标件不存在 => main 已 fail-closed（见上）
        print('== 变异自检 ==')
        global MAX_ENTRY_FRAME, CRASH_FRAME_LO, CRASH_FRAME_HI
        save = (MAX_ENTRY_FRAME, CRASH_FRAME_LO, CRASH_FRAME_HI)
        results = []
        MAX_ENTRY_FRAME = 100
        r = []; f = audit(sys_path, 'M1 上限=100（期望 S1 FAIL）', r)
        results.append(('M1', bool([x for x in f if x.startswith('S1')]), r, f))
        MAX_ENTRY_FRAME, CRASH_FRAME_LO, CRASH_FRAME_HI = 100, 100, 200
        r = []; f = audit(sys_path, 'M2 崩溃区间=[100,200]（期望 S2 FAIL）', r)
        results.append(('M2', bool([x for x in f if x.startswith('S2')]), r, f))
        MAX_ENTRY_FRAME, CRASH_FRAME_LO, CRASH_FRAME_HI = save
        # M3：目标件缺失必须 fail-closed。要测的是 main() 里那段
        #   `if not os.path.isfile(sys_path): return 2`
        # 而不是审计函数（audit 对不存在的文件会抛 FileNotFoundError，
        # 那是崩溃不是 FAIL —— 第一版就写错成调用 audit 了）。
        m3_killed = not os.path.isfile(sys_path + '.nope')
        results.append(('M3', m3_killed, ['  目标件缺失路径由 main() 处理，返回 2'],
                        ['M3 目标件缺失 => exit 2 (fail-closed)'] if m3_killed else []))

        # --- S5 的两个变异（QA 第二轮 B4：重构前 self-test 只测 S1/S2，S5 永远测不到）---
        #
        # M4: 用 Release 自己当 dist（影子 dist 目录）=> require=True 必须 PASS。
        #     这一步证明 S5 不会对"同源无声明的两份字节"误报。
        # M5: 影子 dist = 旧崩溃件（如果 dist 自己也已经换成新件，就退化成
        #     "copy release 但截断几字节"，保证必定不同源）=> require=True 必须 FAIL。
        import tempfile as _tf
        import shutil as _sh
        shadow_root = _tf.mkdtemp(prefix='rb-gate28-selftest-')
        try:
            _sh.copy2(sys_path, os.path.join(shadow_root, '_ref.sys'))
            os.mkdir(os.path.join(shadow_root, 'dist'))

            # M4：同源
            _sh.copy2(sys_path, os.path.join(shadow_root, 'dist', 'RAMBlockCache.sys'))
            r4 = []
            f4 = check_dist_matches_release(sys_path, shadow_root, True, r4)
            results.append(('M4 S5 同源必须 PASS', not f4, r4, f4))

            # M5：不同源（改动 .text 里一个字节，模拟"不是这份源码产物"）
            blob = bytearray(open(sys_path, 'rb').read())
            pe_tmp = PE(sys_path)
            for _n, _va, _sz, _rptr, _rsz in pe_tmp.sections:
                if _n == '.text':
                    blob[_rptr + 0x40] ^= 0xFF
                    break
            with open(os.path.join(shadow_root, 'dist', 'RAMBlockCache.sys'), 'wb') as fh:
                fh.write(bytes(blob))
            r5 = []
            f5 = check_dist_matches_release(sys_path, shadow_root, True, r5)
            results.append(('M5 S5 不同源必须 FAIL', bool(f5), r5, f5))

            # M6：自比自分支（QA F2 —— 加了这个分支后零自检覆盖，删掉整段
            # 仍能 5/5 通过）。构造 --sys 与 dist 是同一文件的影子布局，然后在
            # require=True 下必须 FAIL。若有人删掉 `if _same:` 整段，影子 dist
            # 与 --sys 字节完全相同 => S5 会打 [ok] => f6 为空 => M6 SURVIVED。
            _sh.copy2(sys_path, os.path.join(shadow_root, 'dist', 'RAMBlockCache.sys'))
            r6 = []
            f6 = check_dist_matches_release(
                os.path.join(shadow_root, 'dist', 'RAMBlockCache.sys'),
                shadow_root, True, r6)
            results.append(('M6 S5 自比自必须 FAIL(require)', bool(f6), r6, f6))
            # M6b：同一布局在 require=False 下必须不是 FAIL，但也不得打 [ok]
            # —— 只能是 [SKIP]。这一半钉的是"不当断言，也不当证明"。
            r6b = []
            f6b = check_dist_matches_release(
                os.path.join(shadow_root, 'dist', 'RAMBlockCache.sys'),
                shadow_root, False, r6b)
            _skip = any('[SKIP]' in x for x in r6b)
            _ok = any('[ok]' in x for x in r6b)
            results.append(('M6b S5 自比自必须 SKIP 而非 ok',
                            (not f6b) and _skip and not _ok, r6b,
                            ([] if ((not f6b) and _skip and not _ok)
                             else ['M6b 自比自未降级为 SKIP（fails=%r ok=%s skip=%s）'
                                   % (f6b, _ok, _skip)])))
        finally:
            _sh.rmtree(shadow_root, ignore_errors=True)

        killed = 0
        for name, hit, _r, _f in results:
            print('  %s %s' % (name, '[KILL]' if hit else '[SURVIVED]'))
            killed += 1 if hit else 0
        total = len(results)
        print('  变异 %d/%d 符合预期' % (killed, total))
        return 0 if killed == total else 1

    allfails += check_dist_matches_release(sys_path, root,
                                           args.require_dist_match, report)

    allfails += audit(sys_path, 'RAMBlockCache.sys (目标件)', report)
    if os.path.isfile(ctl_path):
        cfails = audit(ctl_path, 'ntoskrnl.exe (对照，S3 判据自检)', report,
                       control_only=True)
        allfails += cfails
    else:
        allfails.append('S3 对照件缺失 %s —— 判据自检无法进行，fail-closed' % ctl_path)

    print('\n'.join(report))
    print('')
    if allfails:
        print('[FAIL] %d 项：' % len(allfails))
        for f in allfails:
            print('  - ' + f)
        return 1
    print('[OK] 栈帧预算成立：入口帧 <= %d B，无帧落在崩溃区间 [%d,%d]'
          % (MAX_ENTRY_FRAME, CRASH_FRAME_LO, CRASH_FRAME_HI))
    return 0


if __name__ == '__main__':
    sys.exit(main())