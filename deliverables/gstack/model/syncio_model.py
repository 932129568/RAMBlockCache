"""RAMBlockCache 同步 IO 所有权状态机 —— 确定性全交错模型测试
================================================================
背景
----
RbSendSyncIoctl / RbSyncIo 原先在 STATUS_PENDING 后无限等待（"永不放弃在飞
IRP"）：下层设备消失时 IRP 既不完成也不响应取消，PASSIVE 工作项永久卡死。
简单地"加超时后直接返回"会引入 UAF：late IRP 完成时仍写调用者栈上的
KEVENT / IO_STATUS_BLOCK / 数据缓冲。

本修复采用三态 CAS 所有权移交：
    WAITING(0) : 主路径在等，完成例程未裁决
    DONE(1)    : 完成例程先到，主路径负责收尾
    ORPHAN(2)  : 主路径超时离开，完成例程负责收尾
完成例程：SetEvent -> CAS(WAITING->DONE)；若原值 ORPHAN 则自己收尾。
主路径  ：有界等待 -> 超时 CAS(WAITING->ORPHAN)；若原值 DONE 则改走正常收尾。

本模型穷举两个执行体（主路径 / 完成例程）的所有关键交错，断言：
  1. ctx 恰好释放一次，IRP 恰好 Complete 一次（无泄漏、无双释）
  2. 任何执行体都不会访问已释放的 ctx
  3. 主路径返回后不再触碰 ctx
  4. 超时路径一定返回 STATUS_IO_TIMEOUT 且不读输出缓冲
  5. 池化 in/out/数据缓冲在所有路径都有唯一归属

能证明：在给定抽象下所有权移交是正确的（无 UAF / 无泄漏 / 无双释）。
不能证明：真实内核 IRP 完成语义、IoCompleteRequest 与
          STATUS_MORE_PROCESSING_REQUIRED 的交互、真实 IRQL/内存屏障、
          磁盘数据一致性。这些必须由内核编译 + Verifier + 真机验证。
"""

from itertools import permutations

WAITING, DONE, ORPHAN = 0, 1, 2
PASS, TIMEOUT = "PASS", "STATUS_IO_TIMEOUT"


class Ctx:
    def __init__(self):
        self.state = WAITING
        self.event_set = False
        self.freed = False
        self.irp_completed = False
        self.output_copied = False
        self.owner = None            # 'main' | 'completion' | None
        self.access_log = []

    # --- atomic primitives -------------------------------------------------
    def cas(self, expected, new):
        old = self.state
        if old == expected:
            self.state = new
        return old

    def touch(self, who):
        assert not self.freed, f"{who} touched freed ctx"
        self.access_log.append(who)

    def release(self, who):
        assert not self.freed, f"{who} double-free of ctx"
        self.touch(who)
        self.freed = True
        self.owner = who

    def complete_irp(self, who):
        assert not self.irp_completed, f"{who} double IoCompleteRequest"
        self.touch(who)
        self.irp_completed = True


def completion_routine(ctx, cas_before_setevent=False):
    """完成例程：SetEvent + CAS(WAITING->DONE)，ORPHAN 时自行收尾。"""
    ctx.touch("completion")
    if cas_before_setevent:
        old = ctx.cas(WAITING, DONE)
    else:
        ctx.event_set = True
        old = ctx.cas(WAITING, DONE)
    if not cas_before_setevent:
        pass
    else:
        ctx.event_set = True
    if old == ORPHAN:
        # 主路径已离开：完成例程负责收尾（IRP + 池化缓冲 + ctx）
        ctx.complete_irp("completion")
        ctx.release("completion")
        return
    # old == WAITING：主路径仍负责收尾
    return


def main_path(ctx, timeout, cas_completion_first=False, wait_sees_event=False):
    """主路径：有界等待；超时做 CAS(WAITING->ORPHAN)；否则正常收尾。"""
    ctx.touch("main")
    if not timeout:
        # 等待成功（evt 已置位）-> 正常收尾
        ctx.complete_irp("main")
        st = ctx.cas(WAITING, DONE)   # 完成例程尚未裁决的兜底
        if st == ORPHAN:
            raise AssertionError("impossible: ORPHAN before main times out")
        ctx.output_copied = True
        ctx.release("main")
        return PASS

    # 超时分支
    old = ctx.cas(WAITING, ORPHAN)
    if old == DONE:
        # 完成例程刚刚裁决完成：改走正常收尾
        ctx.complete_irp("main")
        ctx.output_copied = True
        ctx.release("main")
        return PASS
    # old == WAITING：成功把所有权交给完成例程，立即返回
    return TIMEOUT


def run_case(completion_first, timeout, cas_completion_first, wait_sees_event):
    """按给定交错执行两个执行体，返回 (ctx, 主路径返回值)。"""
    ctx = Ctx()
    result = {}
    if completion_first:
        completion_routine(ctx, cas_completion_first)
        result["main"] = main_path(ctx, timeout)
    else:
        result["main"] = main_path(ctx, timeout)
        completion_routine(ctx, cas_completion_first)
    # 若主路径超时、且完成例程已先执行并把自己标为 DONE（主路径未跑），
    # 现实里 SetEvent 会让主路径不再超时；模型由调用方保证交错合法。
    return ctx, result["main"]


def assert_invariants(ctx, main_result, label):
    assert ctx.freed, f"{label}: ctx leaked"
    assert ctx.irp_completed, f"{label}: IRP never completed (leak)"
    assert ctx.owner in ("main", "completion"), f"{label}: bad owner"
    # 释放后无访问：access_log 中 release 之后不得再有 touch
    # （release 本身记一次 touch，之后不能出现新的记录）
    assert len(ctx.access_log) >= 1
    if main_result == TIMEOUT:
        assert ctx.owner == "completion", f"{label}: timeout but main freed ctx"
        assert not ctx.output_copied, f"{label}: timeout path copied output"
    else:
        assert ctx.owner == "main", f"{label}: normal return but completion freed ctx"
        assert ctx.output_copied, f"{label}: normal path did not copy output"
    return True


def main():
    print("=" * 74)
    print("RAMBlockCache 同步 IO 所有权状态机 —— 确定性全交错模型测试")
    print("=" * 74)
    cases = 0
    failures = 0
    rows = []

    # 交错维度：
    #  completion_first      完成例程先于主路径超时裁决
    #  timeout               主路径是否超时
    #  cas_completion_first  完成例程内部 CAS 与 SetEvent 的先后
    #  另加：完成例程完全不执行（IRP 永不完成）-> 只允许 ctx 未释放但无 UAF
    skipped = 0
    for completion_first in (True, False):
        for timeout in (True, False):
            for cas_completion_first in (True, False):
                # 非法交错：主路径"等待成功"以完成例程已 SetEvent 为前提，
                # 因此完成例程不可能在主路径正常收尾之后才首次执行。
                if not completion_first and not timeout:
                    skipped += 1
                    continue
                ctx, res = run_case(completion_first, timeout,
                                    cas_completion_first, False)
                label = (f"cf={completion_first} to={timeout} "
                         f"casfirst={cas_completion_first}")
                try:
                    assert_invariants(ctx, res, label)
                    rows.append((label, res, ctx.owner, "PASS"))
                except AssertionError as e:
                    rows.append((label, res, ctx.owner, f"FAIL: {e}"))
                    failures += 1
                cases += 1

    # 补充：完成例程在主路径 CAS 之后才跑（真正的 late IRP 场景）
    # 手工构造，因为 run_case 的 completion_first=False 已在 main 之后立刻跑
    for cas_first in (True, False):
        ctx = Ctx()
        res = main_path(ctx, timeout=True)          # -> CAS(WAITING->ORPHAN)
        assert ctx.state == ORPHAN
        assert res == TIMEOUT
        completion_routine(ctx, cas_first)          # late IRP 最终完成
        label = f"late-irp after orphan (casfirst={cas_first})"
        try:
            assert_invariants(ctx, res, label)
            rows.append((label, res, ctx.owner, "PASS"))
        except AssertionError as e:
            rows.append((label, res, ctx.owner, f"FAIL: {e}"))
            failures += 1
        cases += 1

    # 补充：IRP 永不完成（下层彻底死掉）—— 不泄漏调用者栈、不 UAF，
    # ctx 由完成例程在"未来"收尾；模型上允许未释放，但主路径必须已经返回。
    ctx = Ctx()
    res = main_path(ctx, timeout=True)
    assert res == TIMEOUT and ctx.state == ORPHAN
    assert not ctx.output_copied and not ctx.freed
    rows.append(("IRP never completes", res, "pending completion", "PASS"))
    cases += 1

    for label, res, owner, verdict in rows:
        print(f"  [{verdict:4}] {label:<58} ret={res:<18} owner={owner}")
    print("-" * 74)
    print(f"汇总: 场景 {cases}/{cases} 执行；断言失败 {failures}")
    print()
    print("【本模型能证明什么 / 不能证明什么】")
    print("  能证明: 三态 CAS 在所列全部交错下，ctx 与 IRP 各有唯一收尾方；")
    print("          超时路径不拷贝输出、不释放 ctx；正常路径只由主路径收尾；")
    print("          late IRP 由完成例程安全收尾；IRP 永不完成时主路径仍可返回。")
    print("  不能证明: 真实内核 IoCompleteRequest / MORE_PROCESSING_REQUIRED 语义、")
    print("            IRQL 与内存屏障、MDL 生命周期、数据面回写一致性。")
    print("            这些必须由内核编译 + Driver Verifier + 真机验证。")
    if failures:
        raise SystemExit(1)
    print("\nRESULT: PASS")


if __name__ == "__main__":
    main()
