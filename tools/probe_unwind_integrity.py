# -*- coding: utf-8 -*-
"""
probe_unwind_integrity.py  [298 2026-11-19]

目的：回答"0x7F double fault 的栈展开失败是不是驱动 .pdata/unwind 元数据造成的"。

背景（不可省的路径）：
  两次崩溃 P4 末 4 位均为 f6a6 => 同一故障点，PROCESS_NAME=msedgewebview2.exe，
  bucket 含 VRF（验证器开着）。栈文本显示 double fault 发生在
  nt!RtlpUnwindPrologue 展开链中，其 args 含 RAMBlockCache 的
  base(7cb70000) / 返回地址(+0xe00a) / 另一个地址(+0x2b7d8)。

判据设计的第一原则（本仓反复踩过的坑）：
  单向下结论 = 空跑。必须同时跑一份**已知健康对照**（ntoskrnl.exe），
  若对照都不能接近 100% 合法，说明解析器本身有错，任何"驱动非法"的
  结论都不得采信。

UNWIND_INFO 布局（x64，务必按此，写错会把正常函数判成非法）：
  +0  Version:3 | Flags:5
  +1  SizeOfProlog
  +2  CountOfCodes
  +3  FrameRegister:4 | FrameOffset:4
  +4  UnwindCode[]  每项：CodeOffset(1 byte) + (UnwindOp:4 | OpInfo:4)

只读：打开文件读字节，不改不写。
"""

import hashlib
import struct
import sys
import os

REG = ['RAX', 'RCX', 'RDX', 'RBX', 'RSP', 'RBP', 'RSI', 'RDI',
       'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15']

OPNAME = {0: 'PUSH_NONVOL', 1: 'ALLOC_LARGE', 2: 'ALLOC_SMALL', 3: 'SET_FPREG',
          4: 'SAVE_NONVOL', 5: 'SAVE_NONVOL_FAR', 6: 'EPILOG', 7: 'SPARE',
          8: 'SAVE_XMM128', 9: 'SAVE_XMM128_FAR', 10: 'PUSH_MACHFRAME'}


class PE(object):
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            d = f.read()
        self.d = d
        if d[:2] != b'MZ':
            raise ValueError('not MZ')
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe + 4] != b'PE\x00\x00':
            raise ValueError('not PE')
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        opt = pe + 24
        magic = struct.unpack_from('<H', d, opt)[0]
        self.is64 = (magic == 0x20b)
        sec = opt + struct.unpack_from('<H', d, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = sec + i * 40
            name = d[o:o + 8].rstrip(b'\x00').decode('ascii', 'replace')
            vsz, va, rsz, ra = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((name, va, vsz, ra, rsz))
        self.size_of_image = struct.unpack_from('<I', d, opt + 56)[0]
        self.image_base = struct.unpack_from('<Q', d, opt + 24)[0] if self.is64 else \
            struct.unpack_from('<I', d, opt + 28)[0]
        dd = opt + (112 if self.is64 else 96)
        self.exc_rva, self.exc_size = struct.unpack_from('<II', d, dd + 3 * 8)
        self.rts = []
        if self.exc_rva and self.exc_size:
            n = self.exc_size // 12
            for i in range(n):
                o = self.r2o(self.exc_rva + i * 12)
                if o is None:
                    continue
                b, e, u = struct.unpack_from('<III', d, o)
                self.rts.append((b, e, u))

    def r2o(self, rva):
        for name, va, vsz, ra, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                return ra + (rva - va)
        return None

    def secname(self, rva):
        for name, va, vsz, ra, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                return name
        return None


def check_unwind(pe, u, depth=0, seen=None):
    """返回 (ok, reason)。u=0 视为叶子函数（合法）。"""
    if seen is None:
        seen = set()
    d = pe.d
    if u == 0:
        return (True, 'leaf')
    if u in seen:
        return (False, 'chain 循环 @0x%x' % u)
    seen.add(u)
    o = pe.r2o(u)
    if o is None or o + 4 > len(d):
        return (False, 'unwind 指针 0x%x 越界' % u)
    ver = d[o] & 7
    flags = d[o] >> 3
    cnt = d[o + 2]
    fr = d[o + 3]
    if ver not in (1, 2):
        return (False, 'Version=%d (合法仅 1/2)' % ver)
    # Flags 合法取值（第一版漏了 3，导致对照件 ntoskrnl 被大面积误判）：
    #   0 = NHANDLER, 1 = EHANDLER, 2 = UHANDLER,
    #   3 = EHANDLER|UHANDLER（链接器在同帧既有 EH 又有 UH 时就是这么发的，
    #       实测 ntoskrnl 里 0x1a 字节遍地都是，属正常），
    #   4 = CHAININFO。
    # 5/6/7 等是 EHANDLER/UHANDLER 与 CHAININFO 互斥后的非法组合。
    if flags not in (0, 1, 2, 3, 4):
        return (False, 'Flags=0x%x (合法 0/1/2/3/4)' % flags)
    # [298b 2026-11-19] 删掉原「CHAININFO 且 CountOfCodes=0 且 FrameRegister!=0
    # => 非法」。这是误报：FrameRegister/FrameOffset 只在出现 SET_FPREG 时才有
    # 意义，cnt==0 表示本记录没有 op，此时那两个字段是**未使用**的残留位，链接器
    # 会留垃圾（实测 ntoskrnl u=0x66074: frameReg_raw=0x45，而真实 op 全在
    # chain 指向的 u=0x6604c, count=14 里）。据此误判了 ntoskrnl 20 条。
    codes = o + 4
    if codes + cnt * 2 > len(d):
        return (False, 'CountOfCodes=%d 越界' % cnt)
    i = 0
    while i < cnt:
        b1 = d[codes + i * 2 + 1]
        op = b1 & 0xF
        oi = b1 >> 4
        if op > 10:
            return (False, 'UnwindOp=%d (>10 非法)' % op)
        if op == 7:
            return (False, 'UnwindOp=7 SPARE (保留值不得出现)')
        if op == 1:  # ALLOC_LARGE，长度取决于 OpInfo
            if oi == 0:
                if i + 1 >= cnt:
                    return (False, 'ALLOC_LARGE 截断')
                i += 2
            elif oi == 1:
                if i + 2 >= cnt:
                    return (False, 'ALLOC_LARGE32 截断')
                i += 3
            else:
                return (False, 'ALLOC_LARGE OpInfo=%d (合法仅 0/1)' % oi)
            continue
        need = {0: 1, 2: 1, 3: 1, 4: 2, 5: 3, 6: 2, 8: 2, 9: 3, 10: 1}[op]
        if i + need > cnt:
            return (False, '末尾节点截断 op=%d(%s) 需 %d slot 剩 %d'
                    % (op, OPNAME[op], need, cnt - i))
        if op == 8:
            # [298b 2026-11-19] 修误报。规范原文（x64 exception handling,
            # UWOP_SAVE_XMM128=8）："The offset, divided by 16, is recorded in
            # the next slot in the UNWIND_INFO structure."
            # ⇒ 下一槽里存的是 off/16 的**槽位号**，字节偏移 = 槽号 × 16。
            # 旧实现把槽号当字节数再判 `% 16`，于是 13/14/15 这类**合法**
            # 槽号全被判「未按 16 对齐」。这不是驱动缺陷，是本解析器缺陷。
            # 后果：曾经据此写出「驱动 3 条非法 unwind」——该结论作废。
            # 正确判据：槽号 × 16 + 16 <= 本帧字节数（存进已分配的栈槽）。
            slot = struct.unpack_from('<H', d, codes + (i + 1) * 2)[0]
            fr_now = rsp_alloc(pe, u)
            if fr_now and slot * 16 + 16 > fr_now:
                return (False, 'SAVE_XMM128 槽号=%d -> 偏移%d+16 > 帧%d'
                        % (slot, slot * 16, fr_now))
        if op == 3:
            # SET_FPREG：FrameRegister 必须非 0
            if (fr & 0xF) == 0:
                return (False, 'SET_FPREG 但 FrameRegister=0')
        i += need
    # handler / chain 起始偏移（第二处错，v1 写成 4+((cnt+1)//2)*2）：
    # UnwindCode 数组占 cnt*2 字节，随后按 4 字节对齐：
    #   align(cnt*2, 4) = (cnt*2 + 3) & ~3
    # cnt=2 时 -> 4，总偏移 codes_off = 4 + 4 = 8。
    # v1 算成 4+((2+1)//2)*2 = 6 —— 差 2 字节，把 chain 读成乱码，
    # 导致 ntoskrnl 里大量正常 chained unwind 被判 "begin>=end"。
    extra = codes + ((cnt * 2 + 3) & ~3)
    if flags == 4:
        if extra + 12 > len(d):
            return (False, 'CHAININFO 越界')
        cb, ce, cu = struct.unpack_from('<III', d, extra)
        if cb or ce or cu:
            if not (cb and ce and cu):
                return (False, 'chain 字段部分为零')
            if cb >= ce:
                return (False, 'chain begin=0x%x >= end=0x%x' % (cb, ce))
            if pe.secname(cb) is None or pe.secname(ce - 1) is None:
                return (False, 'chain 区间不在映像内')
            if depth < 6:
                ok, why = check_unwind(pe, cu, depth + 1, seen)
                if not ok:
                    return (False, 'chain->' + why)
    elif flags in (1, 2):
        if extra + 4 > len(d):
            return (False, 'EXCEPTION_HANDLER 越界')
        h = struct.unpack_from('<I', d, extra)[0]
        if h and pe.secname(h) is None:
            return (False, 'handler RVA 0x%x 越界' % h)
    return (True, 'ok')


def rsp_alloc(pe, u, depth=0, seen=None):
    """计算一个函数（含 CHAININFO 链）在 prolog 阶段真正把 RSP 下移多少字节。

    为什么必须单独有这个函数（本轮踩坑，必须留证）：
    296 号报告第一版把「SAVE_NONVOL / SAVE_XMM128」也算进栈帧，于是
    func 0xb164 被算成 25,280 B、再加 chain 变成 37,944 B（308% of 12KB）。
    那不是发现，是算错 —— SAVE_NONVOL 只是把非易失寄存器**存到 rsp+offset**，
    它不移动 RSP。直接把它的 offset 当分配累加，会把同一段栈重复计入。

    真正改变 RSP 的只有：
      PUSH_NONVOL   +8
      ALLOC_LARGE   OpInfo=0 -> imm16*8 ; OpInfo=1 -> imm32
      ALLOC_SMALL   OpInfo*8 + 8
      PUSH_MACHFRAME +40
    """
    if seen is None:
        seen = set()
    if u in seen or depth > 5:
        return 0
    seen.add(u)
    d = pe.d
    o = pe.r2o(u)
    if o is None:
        return 0
    cnt = d[o + 2]
    flags = d[o] >> 3
    codes = o + 4
    tot = 0
    i = 0
    while i < cnt:
        b1 = d[codes + i * 2 + 1]
        op = b1 & 0xF
        oi = b1 >> 4
        if op == 0:                                   # PUSH_NONVOL
            tot += 8
            i += 1
        elif op == 1:                                 # ALLOC_LARGE
            if oi == 0:
                tot += struct.unpack_from('<H', d, codes + (i + 1) * 2)[0] * 8
                i += 2
            elif oi == 1:
                tot += struct.unpack_from('<I', d, codes + (i + 1) * 2)[0]
                i += 3
            else:
                i += 2
        elif op == 2:                                 # ALLOC_SMALL
            tot += oi * 8 + 8
            i += 1
        elif op == 3:                                 # SET_FPREG  不改 RSP
            i += 1
        elif op == 4:                                 # SAVE_NONVOL 不改 RSP
            i += 2
        elif op == 5:                                 # SAVE_NONVOL_FAR
            i += 3
        elif op == 6:                                 # EPILOG
            i += 1
        elif op == 7:
            i += 1
        elif op == 8:                                 # SAVE_XMM128
            i += 2
        elif op == 9:                                 # SAVE_XMM128_FAR
            i += 3
        elif op == 10:                                # PUSH_MACHFRAME
            tot += 40
            i += 1
        else:
            i += 1
    if flags & 4:
        coff = codes + 4 + ((cnt * 2 + 3) // 4) * 4 - 4
        cb, ce, cu = struct.unpack_from('<III', d, coff)
        if cu:
            tot += rsp_alloc(pe, cu, depth + 1, seen)
    return tot


def dump_one(pe, u, indent=0):
    d = pe.d
    pad = '  ' * indent
    print(pad + 'UNWIND_INFO @0x%x' % u)
    o = pe.r2o(u)
    if o is None:
        print(pad + '  !! 越界')
        return
    ver = d[o] & 7
    flags = d[o] >> 3
    cnt = d[o + 2]
    fr = d[o + 3]
    print(pad + '  ver=%d flags=%d count=%d frameReg=%s+%d'
          % (ver, flags, cnt, REG[fr & 0xF], fr >> 4))
    codes = o + 4
    i = 0
    while i < cnt:
        co = d[codes + i * 2]
        b1 = d[codes + i * 2 + 1]
        op = b1 & 0xF
        oi = b1 >> 4
        print(pad + '  [%02d] off=0x%02x op=%-2d %-16s opinfo=%d'
              % (i, co, op, OPNAME.get(op, '??'), oi))
        need = {0: 1, 2: 1, 3: 1, 4: 2, 5: 3, 6: 2, 8: 2, 9: 3, 10: 1, 1: 1}
        i += need.get(op, 1)
    if flags == 4:
        extra = codes + ((cnt * 2 + 3) & ~3)
        cb, ce, cu = struct.unpack_from('<III', d, extra)
        print(pad + '  CHAIN -> b=0x%x e=0x%x unwind=0x%x' % (cb, ce, cu))
        if cu and indent < 4:
            dump_one(pe, cu, indent + 2)


def sweep(path, label):
    print('')
    print('#' * 72)
    print('# ' + label)
    print('#' * 72)
    if not os.path.exists(path):
        print('  [SKIP] 文件不存在')
        return None
    pe = PE(path)
    print('  file      = %s' % path)
    print('  size      = %d B   ImageBase=0x%x  SizeOfImage=0x%x'
          % (len(pe.d), pe.image_base, pe.size_of_image))
    print('  .pdata    RVA=0x%x size=0x%x -> %d 个 RUNTIME_FUNCTION'
          % (pe.exc_rva, pe.exc_size, len(pe.rts)))
    if not pe.rts:
        print('  [FAIL] .pdata 为空：x64 非叶子函数无展开信息，栈回溯必崩')
        return pe, [], []
    good, bad = [], []
    for b, e, u in pe.rts:
        if e <= b:
            bad.append((b, e, u, 'Begin>=End'))
            continue
        ok, why = check_unwind(pe, u)
        (good if ok else bad).append((b, e, u, why))
    tot = len(pe.rts)
    print('  合法 unwind = %d / %d = %.2f%%    非法 = %d'
          % (len(good), tot, 100.0 * len(good) / tot, len(bad)))
    if bad:
        print('  --- 非法样例（前 15） ---')
        for b, e, u, w in bad[:15]:
            print('    func 0x%-7x..0x%-7x unwind=0x%-7x  %s' % (b, e, u, w))
    return pe, good, bad


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        try:
            sys.stdout.reconfigure(encoding='utf-8')
        except Exception:
            pass

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)

    # [298 2026-11-19] 原为写死的 2dce9d72 / 202,976 B。修复件重新签名后 dist 件
    # 必然换哈希与尺寸；写死会让报告标题与被分析的字节不是同一份（F-01 形态）。
    # 现改为实读；缺失时明确报出，而不是打印一个过期常量冒充观察值。
    dist_sys = os.path.join(root, 'dist', 'RAMBlockCache.sys')
    if os.path.exists(dist_sys):
        with open(dist_sys, 'rb') as fh:
            live_sha = hashlib.sha256(fh.read()).hexdigest()
        live_note = '%s (%d B)' % (live_sha, os.path.getsize(dist_sys))
    else:
        live_sha = None
        live_note = 'MISSING -- dist/ 下无现场件'
    print('=' * 72)
    print('probe_unwind_integrity  [298]')
    print('现场驱动 sha256 = %s' % live_note)
    print('=' * 72)

    results = {}
    results['live'] = sweep(dist_sys,
                            'RAMBlockCache.sys 现场部署件 (dist, %d B)'
                            % (os.path.getsize(dist_sys) if os.path.exists(dist_sys) else -1))
    results['nt'] = sweep(os.path.join(os.environ.get('SystemRoot', r'C:\Windows'),
                                       'System32', 'ntoskrnl.exe'),
                          'ntoskrnl.exe  [对照，必须接近 100% 合法]')
    local = os.path.join(root, 'src', 'driver', 'x64', 'Release', 'RAMBlockCache.sys')
    if os.path.exists(local):
        results['local'] = sweep(local, 'RAMBlockCache.sys 本地 Release (%d B)'
                                 % os.path.getsize(local))

    print('')
    print('=' * 72)
    print('判据自检（顺序不可颠倒：先验对照，再看嫌疑）')
    print('=' * 72)

    nt = results.get('nt')
    if nt:
        tot = len(nt[0].rts)
        good = len(nt[1])
        ratio = 100.0 * good / tot if tot else 0
        print('  对照 ntoskrnl 合法率 = %.2f%%' % ratio)
        if ratio < 99.0:
            print('  [FAIL] 解析器自身有错（对照应近乎全合法）—— 下面关于驱动的结论一律作废')
            verdict = 'PARSER_BROKEN'
        else:
            print('  [ok] 解析器可信（对照近乎全合法）')
            verdict = 'PARSER_OK'
    else:
        verdict = 'NO_CONTROL'

    live = results.get('live')
    if verdict == 'PARSER_OK' and live:
        pe, good, bad = live
        tot = len(pe.rts)
        print('')
        print('  嫌疑件 dist/RAMBlockCache.sys: 合法 %d/%d, 非法 %d' % (len(good), tot, len(bad)))
        # 0xe00a 所属函数
        print('')
        print('  --- 栈底帧 RAMBlockCache+0xe00a 所属函数 ---')
        hits = [r for r in pe.rts if r[0] <= 0xe00a < r[1]]
        if not hits:
            print('    [!!] 0xe00a 不在任何 RUNTIME_FUNCTION 区间内 -> 该 PC 无展开信息')
            verdict = 'NO_PDATA_FOR_PC'
        else:
            for b, e, u in hits:
                print('    func 0x%x..0x%x  unwind=0x%x' % (b, e, u))
                ok, why = check_unwind(pe, u)
                print('    校验 -> ok=%s reason=%s' % (ok, why))
                if not ok:
                    verdict = 'BAD_UNWIND'
                dump_one(pe, u, 2)
                # 栈参数里那个 0x2b7d8
                print('    --- 栈参数中的 0x2b7d8 ---')
                sec = pe.secname(0x2b7d8)
                inrt = [r for r in pe.rts if r[0] <= 0x2b7d8 < r[1]]
                print('      落节 = %s   所属 RUNTIME_FUNCTION = %s'
                      % (sec, inrt if inrt else 'NONE'))
                o2 = pe.r2o(0x2b7d8)
                if o2 is not None:
                    vf = pe.d[o2]
                    print('      若当作 UNWIND_INFO 读: ver=%d flags=%d count=%d -> %s'
                          % (vf & 7, vf >> 3, pe.d[o2 + 2],
                             '非法(常见于把代码地址误当元数据)' if (vf & 7) not in (1, 2) else '形态合法'))

    # ---------------------------------------------------------------
    # 帧大小审计（[297] 新增，本轮最重要的一条常设判据）
    #
    # 为什么必须有：296 号报告凭「0xb0ee 紧邻 RbRwWorker 起点 0xb0c0」推断
    # 6304 B 帧属于 RbRwWorker，这是**相邻推断**，不是证据。实测：
    #   dist 0xb0c0 = RbRwWorker，23 B，帧 40 B（48 83 ec 28 / call / 48 83 c4 28 / c3）
    #   dist 0xb0e0 = 另一个函数，帧 6328 B，头部紧跟 KeQueryActiveProcessorCountEx(0xFFFF)
    # 即 6304 B 帧根本不在 RbRwWorker 里，而在 RbStartIoWorkers（rbio_worker.c:1049），
    # 来源 auxT[RB_PCPU_MAX=256] * sizeof(RB_AFFIN_TARGET=24) = 6144 B + 其它局部。
    # 教训：函数归属要靠符号/首字节特征，不能靠「离谁近」。
    # ---------------------------------------------------------------
    if verdict == 'PARSER_OK' and live:
        pe = live[0]
        LIMIT = 4096
        big = []
        for b, e, u in pe.rts:
            try:
                t = rsp_alloc(pe, u)
            except Exception:
                continue
            if t >= LIMIT:
                big.append((b, e, t))
        print('')
        print('=' * 72)
        print('  栈帧审计：RSP 分配 >= %d B 的函数（12KB 内核栈的安全线）' % LIMIT)
        print('=' * 72)
        print('  注意：只累计真正移动 RSP 的 unwind op，SAVE_NONVOL/SAVE_XMM128 不计。')
        print('')
        if not big:
            print('  [ok] 无单帧 >= %d B 的函数' % LIMIT)
        else:
            # 合并连续同类区间，避免同一内联体被重复罗列
            for b, e, t in big:
                print('    func 0x%-6x..0x%-6x (代码 %5d B)  RSP 分配 %6d B = %5.1f%% of 12KB'
                      % (b, e, e - b, t, 100.0 * t / 12288))
            print('')
            print('  [WARN] 存在 >= %d B 的单帧，占了内核栈 %.1f%%，与卷过滤栈叠加有溢出风险'
                  % (LIMIT, 100.0 * max(t for _, _, t in big) / 12288))

    print('')
    print('READ-ONLY：只读文件字节，未改任何东西。')
    print('')
    if verdict in ('BAD_UNWIND', 'NO_PDATA_FOR_PC'):
        print('结论 = 驱动 unwind 元数据存在缺陷（栈展开在此必然失败）')
        return 2
    if verdict == 'PARSER_BROKEN':
        print('结论 = 无法判定（解析器未通过自检）')
        return 3
    print('结论 = 未发现 unwind 元数据缺陷')
    return 0


if __name__ == '__main__':
    sys.exit(main())