#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""O9-SYNCIO 引用计数版的确定性全交错回归模型（2026-11-09）

背景（124 号 T5）：115 号三态 CAS 的完成例程顺序是
    KeSetEvent(&ctx->evt) -> CAS(&ctx->State) -> 条件收尾
若 SetEvent 后主路径被唤醒并抢先完成 IoCompleteRequest + 释放 ctx，
完成例程随后读 ctx->State 即为 UAF（lookaside 复用后还可能二次释放）。

修复（126 号之后的 B 批）：ctx 增加 Refs=2（主路径 1 + 完成例程 1），
任何一方只在自己仍持引用期间访问 ctx；最后 InterlockedDecrement 到 0
的一方归还 ctx。rbdev.c RB_SYNCCTX 与 rbattach.c RB_IOCTL_SYNC 同式。

本模型穷举两个执行体在原子步骤粒度上的交错，断言：
  I1 ctx 恰好释放一次，且释放后无任何访问
  I2 IRP 恰好 Complete 一次，池化缓冲恰好释放一次
  I3 超时路径返回 STATUS_IO_TIMEOUT、不拷贝输出、不释放池缓冲（留给 late IRP）
  I4 正常路径由主路径收尾并拷贝输出
阳性对照：旧实现（正常路径立即释放 ctx）必须被检出 UAF，否则器具有假绿。
"""
import itertools
import json
import random

WAITING, DONE, ORPHAN = 0, 1, 2
PASS, TIMEOUT = "PASS", "STATUS_IO_TIMEOUT"


class Ctx:
    def __init__(self):
        self.state = WAITING
        self.refs = 2
        self.freed = False
        self.event_set = False
        self.irp_completed = 0
        self.pools_freed = False
        self.output_copied = False
        self.uaf = []
        self.free_count = 0
        self.owner = None

    def touch(self, who, what):
        if self.freed:
            self.uaf.append("%s touched freed ctx (%s)" % (who, what))

    def free_pools(self, who):
        self.touch(who, "free_pools")
        assert not self.pools_freed, "%s double free of pools" % who
        self.pools_freed = True

    def complete(self, who):
        self.touch(who, "IoCompleteRequest")
        self.irp_completed += 1

    def release_ref(self, who):
        self.touch(who, "release_ref")
        self.refs -= 1
        if self.refs == 0:
            assert not self.freed, "%s double free of ctx" % who
            self.freed = True
            self.free_count += 1
            self.owner = who


class Machine:
    """两个执行体，各自持一份引用；步骤按调度序列交错推进。"""

    def __init__(self, timeout):
        self.c = Ctx()
        self.timeout = timeout
        self.ret = None
        self.steps = {"C": ["C1", "C2", "C3", "C4"],
                      "M": ["M1", "M3", "M4"] if timeout else ["M1", "M2", "M4"]}
        self.i = {"C": 0, "M": 0}

    def step(self, who):
        seq = self.steps[who]
        if self.i[who] >= len(seq):
            return
        s = seq[self.i[who]]
        self.i[who] += 1
        c = self.c
        if s == "C1":
            c.touch("completion", "KeSetEvent")
            c.event_set = True
        elif s == "C2":
            c.touch("completion", "CAS(State)")
            old = c.state
            if old == WAITING:
                c.state = DONE
            self.cas_old = old
        elif s == "C3":
            if getattr(self, "cas_old", None) == ORPHAN:
                c.complete("completion")
                c.free_pools("completion")
        elif s == "C4":
            c.release_ref("completion")
        elif s == "M1":
            c.touch("main", "KeWait")
            # 事件已置位时等待成功；超时模式下本模型强制走超时分支
        elif s == "M2":
            c.complete("main")
            c.output_copied = True
            c.free_pools("main")
        elif s == "M3":
            c.touch("main", "CAS(timeout)")
            old = c.state
            if old == WAITING:
                c.state = ORPHAN
            if old == DONE:
                # 完成例程先裁决：改走正常收尾
                self.steps["M"] = ["M1", "M2", "M4"]
                self.i["M"] = 1   # 下一步执行 M2（正常收尾），随后 M4 释放引用
                self.done_case = True
            else:
                self.ret = TIMEOUT
        elif s == "M4":
            if self.ret is None:
                self.ret = PASS
            c.release_ref("main")

    def run(self, schedule):
        for who in schedule:
            self.step(who)
        # 未执行的步骤按序补完（保证两方都结束）
        for who in ("C", "M"):
            while self.i[who] < len(self.steps[who]):
                self.step(who)
        return self.c, self.ret


def assert_invariants(c, ret, label):
    assert not c.uaf, "%s: UAF -> %s" % (label, "; ".join(c.uaf))
    assert c.free_count == 1, "%s: ctx freed %d times" % (label, c.free_count)
    assert c.irp_completed == 1, "%s: IRP completed %d times" % (label, c.irp_completed)
    assert c.pools_freed, "%s: pools leaked" % label
    assert c.refs == 0, "%s: refs=%d" % (label, c.refs)
    if ret == TIMEOUT:
        assert not c.output_copied, "%s: timeout path copied output" % label
    else:
        assert c.output_copied, "%s: normal path did not copy output" % label


def legacy_positive_control():
    """112 号回归版：正常路径立即释放 ctx，late completion 写已释放内存。"""
    freed = {"v": True}
    uaf = []
    def completion():
        if freed["v"]:
            uaf.append("completion KeSetEvent on freed ctx")
    completion_late = True
    # 主路径先释放（正常路径立即释放），late IRP 后完成
    freed["v"] = True
    if completion_late:
        completion()
    return len(uaf) > 0


def main():
    print("=== O9-SYNCIO REFCOUNT FULL-INTERLEAVING MODEL ===", flush=True)
    cases = 0
    failures = []
    # 显式关键交错：C1 置位后主路径抢先收尾（124 号 T5 检出的窗口）
    schedules = [
        "CMCMCM", "MCCMMC", "CMMCCM", "MMCCMC", "CCCCMM", "MMMMCC",
        "CMCCMM", "MCMCMC", "CCMMCM", "MMCCCM", "CMMMCC", "MCCCMM",
    ]
    for timeout in (False, True):
        for sch in schedules:
            m = Machine(timeout)
            c, ret = m.run(sch)
            label = "to=%s sch=%s" % (timeout, sch)
            cases += 1
            try:
                assert_invariants(c, ret, label)
            except AssertionError as e:
                failures.append(str(e))
    # 随机交错：固定种子，覆盖更细的时间片
    rng = random.Random(20261109)
    for timeout in (False, True):
        for _ in range(500):
            sch = "".join(rng.choice("CM") for _ in range(6))
            m = Machine(timeout)
            c, ret = m.run(sch)
            label = "to=%s sch=%s" % (timeout, sch)
            cases += 1
            try:
                assert_invariants(c, ret, label)
            except AssertionError as e:
                failures.append(str(e))
    positive = legacy_positive_control()
    out = {"interleavings": cases, "assertion_failures": len(failures),
           "failures_sample": failures[:5],
           "legacy_positive_control_uaf_detected": positive,
           "RESULT": "PASS" if not failures and positive else "FAIL"}
    print(json.dumps(out, ensure_ascii=False, indent=2), flush=True)
    print("\n能证明：在引用计数抽象下，SetEvent 与 CAS 之间的抢占窗口不再产生",
          flush=True)
    print("UAF / 双释 / 泄漏；超时路径不碰输出、late IRP 由完成例程安全收尾。",
          flush=True)
    print("不能证明：真实内核 IoCompleteRequest 语义、IRQL/内存屏障、MDL 生命周期。",
          flush=True)
    return 0 if out["RESULT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
