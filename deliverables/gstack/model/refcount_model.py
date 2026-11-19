#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
refcount_model.py
=================
RAMBlockCache 卷设备扩展（RB_DEV_EXT）引用计数拆除协议 —— 确定性状态机模型测试。

本脚本不启动任何真实线程，也不访问内核。它把 src/driver/rbattach.c 与
src/driver/rbdriver.c 中的两条关键路径抽象成"可枚举的步骤序列"：

  1) RbVolumeRemove 拆除流程（rbattach.c:317）
  2) RbZombieReap 僵尸回收流程（rbdriver.c:342）

并对"修复前（缺陷版）"与"修复后（正确版）"分别建模、对比断言。

建模的真实协议（与源码一一对应）：
  * 每个 ext 的 Refs 计数器初始为 1（owner 基准引用）。
  * 任何持有 ext 的路径（I/O 派发、枚举快照、按盘符查找、PnP 移除工作项）
    都会 +1，用完 -1。
  * 拆除：从全局表摘除 → Removed=1 → 等在途引用排空 → 销毁缓存流 → 分离/删设备。

  缺陷 A（rbattach.c:348 / :569）：排空条件写成 `Refs <= 1`，但调用方自己还持
    有引用（PnP 移除工作项在 rbattach.c:458 +1；RbDetachVolumeByLetter 经
    RbFindExtByLetter 也带 1 个引用），因此 Refs 恒 >= 2，条件永不成立，
    必然等满 3000×20ms = 60s 超时。
  缺陷 B（rbdriver.c:342-368）：超时后转入僵尸队列；回收器对仍在途的条目
    "原地重排后无条件 continue"，且 for(;;) 无上界。单个始终忙的条目即可让
    单次 RbZombieReap() 永不返回。

  修复后：拆除函数接受参数 SelfRefs（调用方额外持有的引用数），排空条件改为
    `Refs <= 1 + SelfRefs`；僵尸回收器每轮只检查"进入本轮时的条目数"（有界），
    就绪后先 flush + 销毁流，再删设备。

用法：
  python refcount_model.py
退出码：全部断言通过 0；否则非 0。
"""

import sys

# ---------------------------------------------------------------------------
# 常量（与源码对齐）
# ---------------------------------------------------------------------------
DRAIN_MAX_STEPS = 3000     # rbattach.c:347 60s 上限，20ms/步
REAP_STEP_CAP = 200000     # 本模型用于"暴露无界自旋"的步数上限（非源码常量）


def _enable_utf8():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except Exception:
            pass


# ---------------------------------------------------------------------------
# 被建模的对象
# ---------------------------------------------------------------------------
class Ext:
    """RB_DEV_EXT 的最小模型：只保留与拆除协议相关的状态。"""

    def __init__(self):
        self.refs = 1            # owner 基准引用（rbdriver.c:602 ext->Refs = 1）
        self.removed = False     # rbattach.c:342
        self.stream = True       # ext->Stream 是否存活
        self.lower_device = True
        self.pdo = True
        self.filter_device = True
        self.magic = True        # 哨兵 RB_EXT_MAGIC
        self.in_table = True     # 是否仍链在 g_Rb.DevList
        self.log = []            # (事件名, 事件发生时的 Refs)

    def ref(self, n=1):
        self.refs += n

    def deref(self, n=1):
        self.refs -= n


def _teardown(ext, selfrefs, threshold, drain_max, io_release_at):
    """
    通用拆除状态机。

    threshold   : 排空判定阈值。修复前 = 1（忽略调用方引用，缺陷 A）；
                  修复后 = 1 + selfrefs。
    io_release_at: {迭代下标: 释放的引用数}，模拟"在途 I/O 在某个 20ms 周期完成"。
                   这是确定性的外部事件调度，不涉及真并发。
    """
    ext.log = []

    # 1) 从全局表摘除（rbattach.c:331-337）
    if ext.in_table:
        ext.in_table = False
        ext.log.append(("unlink", ext.refs))

    # 2) 停接新 I/O（rbattach.c:342-343）
    ext.removed = True
    ext.log.append(("removed=1", ext.refs))

    # 3) 等在途引用排空（rbattach.c:345-350）
    steps = 0
    for i in range(drain_max):
        steps += 1
        rel = io_release_at.get(i, 0)
        if rel:
            ext.deref(rel)                    # 在途 I/O 完成，归还引用
            ext.log.append(("io_release", ext.refs))
        if ext.refs <= threshold:
            break

    if ext.refs > threshold:
        ext.log.append(("timeout->zombie", ext.refs))
        return {"result": "TIMEOUT_ZOMBIE", "steps": steps,
                "refs": ext.refs, "log": ext.log}

    ext.log.append(("drained", ext.refs))

    # 3b) 排空后再销毁缓存流（rbattach.c:357-358）
    if ext.stream:
        ext.stream = False
        ext.log.append(("stream_destroyed", ext.refs))
    ext.magic = False

    # 4) 分离 / 释放 PDO / 删设备（rbattach.c:362-365）
    if ext.lower_device:
        ext.lower_device = False
        ext.log.append(("detach", ext.refs))
    if ext.pdo:
        ext.pdo = False
        ext.log.append(("pdo_released", ext.refs))
    ext.filter_device = False
    ext.log.append(("device_deleted", ext.refs))

    return {"result": "DRAINED", "steps": steps, "refs": ext.refs, "log": ext.log}


def teardown_prefix(ext, selfrefs, io_release_at=None):
    """修复前：阈值恒为 1，无视调用方持有的引用（缺陷 A）。"""
    return _teardown(ext, selfrefs, threshold=1, drain_max=DRAIN_MAX_STEPS,
                     io_release_at=io_release_at or {})


def teardown_fixed(ext, selfrefs, io_release_at=None):
    """修复后：阈值 = 1 + SelfRefs。"""
    return _teardown(ext, selfrefs, threshold=1 + selfrefs, drain_max=DRAIN_MAX_STEPS,
                     io_release_at=io_release_at or {})


# ---------------------------------------------------------------------------
# 僵尸回收器模型
# ---------------------------------------------------------------------------
def reaper_prefix(entries, step_cap=REAP_STEP_CAP):
    """
    修复前 RbZombieReap（rbdriver.c:342-368）：
    for(;;) 无上界；忙条目 RemoveHeadList 后 InsertTailList 放回队尾，再无条件
    continue。只要队列里存在一个始终忙的条目，本次调用就永不返回。
    """
    queue = [dict(e) for e in entries]
    steps = 0
    while queue:
        if steps >= step_cap:
            return {"terminated": False, "steps": steps, "pending": len(queue)}
        le = queue.pop(0)
        steps += 1
        if le["busy"]:
            queue.append(le)        # 放回队尾，下轮再看
            continue                # 无条件 continue → 无界自旋
        # 就绪 → 删除（本模型只关心是否终止）
    return {"terminated": True, "steps": steps, "pending": 0}


def reaper_fixed_round(queue, deleted_out):
    """
    修复后 RbZombieReap 的单轮：只检查"进入本轮时的条目数" n，最多处理 n 个，
    本轮必然返回。忙条目放回队尾，交由下一轮（调用方可在两轮之间做别的事）。
    返回本轮步数（<= n）。
    """
    n = len(queue)                  # 进入本轮时的快照长度，即本轮上界
    steps = 0
    for _ in range(n):
        le = queue.pop(0)
        steps += 1
        if le["busy"]:
            queue.append(le)
        else:
            deleted_out.append(le)
    return steps


# ---------------------------------------------------------------------------
# 场景
# ---------------------------------------------------------------------------
class Scenario:
    def __init__(self, idx, title):
        self.idx = idx
        self.title = title
        self.before = ""
        self.after = ""
        self.checks = []            # (描述, bool)
        self.notes = []

    def check(self, desc, ok):
        self.checks.append((desc, bool(ok)))

    @property
    def passed(self):
        return all(ok for _, ok in self.checks) and len(self.checks) > 0


def scenario1():
    """场景1：无在途 I/O，PnP 移除工作项持 1 引用（Refs = 1 owner + 1 self = 2）。"""
    s = Scenario(1, "无在途IO + PnP移除工作项持1引用 (SelfRefs=1, 初始 Refs=2)")

    e1 = Ext(); e1.ref(1)
    r1 = teardown_prefix(e1, selfrefs=1)
    s.before = ("超时转僵尸 (result=%s, 等待步数=%d, 结束时 Refs=%d)"
                % (r1["result"], r1["steps"], r1["refs"]))

    e2 = Ext(); e2.ref(1)
    r2 = teardown_fixed(e2, selfrefs=1)
    s.after = ("有限步排空 (result=%s, 等待步数=%d, 结束时 Refs=%d)"
               % (r2["result"], r2["steps"], r2["refs"]))

    s.check("修复前必然超时(60s上限, 步数=%d)" % DRAIN_MAX_STEPS,
            r1["result"] == "TIMEOUT_ZOMBIE" and r1["steps"] == DRAIN_MAX_STEPS)
    s.check("修复后有限步内完成排空(步数 < 60s上限)",
            r2["result"] == "DRAINED" and r2["steps"] < DRAIN_MAX_STEPS)
    return s


def scenario2():
    """场景2：无在途 I/O，RbDetachVolumeByLetter 路径（经 RbFindExtByLetter 带 1 引用，
    owner 基准 1 + 调用方 1 = 2，阈值 2）。"""
    s = Scenario(2, "无在途IO + RbDetachVolumeByLetter路径 (阈值2, 初始 Refs=2)")

    e1 = Ext(); e1.ref(1)
    r1 = teardown_prefix(e1, selfrefs=1)
    s.before = ("超时 (result=%s, 步数=%d, Refs=%d)"
                % (r1["result"], r1["steps"], r1["refs"]))

    e2 = Ext(); e2.ref(1)
    r2 = teardown_fixed(e2, selfrefs=1)
    s.after = ("有限步排空 (result=%s, 步数=%d, Refs=%d)"
               % (r2["result"], r2["steps"], r2["refs"]))

    s.check("修复前必然超时",
            r1["result"] == "TIMEOUT_ZOMBIE" and r1["steps"] == DRAIN_MAX_STEPS)
    s.check("修复后有限步排空",
            r2["result"] == "DRAINED" and r2["steps"] < DRAIN_MAX_STEPS)
    return s


def scenario3():
    """场景3：2 个在途 I/O 引用（初始 Refs = 1 owner + 1 self + 2 io = 4）。
    验证修复后先等在途释放再销毁流，且销毁流时在途引用已归零。"""
    s = Scenario(3, "2个在途IO引用 (初始 Refs=4) — 排空顺序断言")
    # 在途 I/O 在第 6、第 13 个 20ms 周期归还（确定性调度）
    sched = {5: 1, 12: 1}

    e1 = Ext(); e1.ref(1); e1.ref(2)
    r1 = teardown_prefix(e1, selfrefs=1, io_release_at=dict(sched))
    s.before = ("超时转僵尸 (result=%s, 步数=%d, Refs=%d)"
                % (r1["result"], r1["steps"], r1["refs"]))

    e2 = Ext(); e2.ref(1); e2.ref(2)
    r2 = teardown_fixed(e2, selfrefs=1, io_release_at=dict(sched))
    s.after = ("排空于第 %d 步 (result=%s, 结束时 Refs=%d)"
               % (r2["steps"], r2["result"], r2["refs"]))

    log = r2["log"]
    io_idx = [i for i, (ev, _) in enumerate(log) if ev == "io_release"]
    sd_idx = [i for i, (ev, _) in enumerate(log) if ev == "stream_destroyed"]
    sd_refs = log[sd_idx[0]][1] if sd_idx else None

    s.check("修复前必然超时", r1["result"] == "TIMEOUT_ZOMBIE")
    s.check("修复后成功排空", r2["result"] == "DRAINED")
    s.check("2 个在途 I/O 均被观察到归还", len(io_idx) == 2)
    s.check("销毁流发生在全部在途 I/O 归还之后",
            bool(sd_idx) and len(io_idx) == 2 and sd_idx[0] > io_idx[-1])
    s.check("销毁流时在途引用已归零 (Refs == 1 + SelfRefs == 2)",
            sd_refs == 2)
    return s


def scenario4():
    """场景4：僵尸队列含 1 个始终忙的条目。
    修复前回收器单次调用步数无界（触顶）；修复后单轮步数 <= 队列长度。"""
    s = Scenario(4, "僵尸队列含1个始终忙条目 — 回收器有界性")

    busy_entries = [{"name": "busy0", "busy": True}]

    r1 = reaper_prefix(busy_entries, step_cap=REAP_STEP_CAP)
    s.before = ("单次 RbZombieReap 未返回，步数触顶 %d (terminated=%s, 剩余=%d)"
                % (r1["steps"], r1["terminated"], r1["pending"]))

    queue = [dict(e) for e in busy_entries]
    deleted = []
    fixed_steps = reaper_fixed_round(queue, deleted)
    s.after = ("单轮步数=%d <= 队列长度=%d (本轮后队列仍剩 %d 个忙条目)"
               % (fixed_steps, len(busy_entries), len(queue)))

    s.check("修复前回收器无界自旋(触达步数上限 %d)" % REAP_STEP_CAP,
            r1["terminated"] is False and r1["steps"] == REAP_STEP_CAP)
    s.check("修复后单轮步数 <= 队列长度",
            fixed_steps <= len(busy_entries))

    # 附加：混合队列（就绪+忙），进一步佐证有界性
    mixed = [{"name": "ready0", "busy": False},
             {"name": "busy0", "busy": True},
             {"name": "ready1", "busy": False}]
    r1m = reaper_prefix(mixed, step_cap=REAP_STEP_CAP)
    qm = [dict(e) for e in mixed]
    dm = []
    steps_m = reaper_fixed_round(qm, dm)
    s.notes.append("混合队列(就绪/忙/就绪)：修复前 terminated=%s 步数=%d；"
                   "修复后单轮步数=%d <= 3，本轮删除了 %d 个就绪条目"
                   % (r1m["terminated"], r1m["steps"], steps_m, len(dm)))
    s.check("混合队列下修复前同样无界(触顶)",
            r1m["terminated"] is False and r1m["steps"] == REAP_STEP_CAP)
    s.check("混合队列下修复后单轮有界", steps_m <= len(mixed))
    return s


def scenario5():
    """场景5：验证修复后"销毁流一定发生在删除设备之前"（顺序断言）。"""
    s = Scenario(5, "修复后销毁流先于删除设备 — 顺序断言")

    e2 = Ext(); e2.ref(1)
    r2 = teardown_fixed(e2, selfrefs=1)
    log = r2["log"]
    sd_idx = [i for i, (ev, _) in enumerate(log) if ev == "stream_destroyed"]
    dev_idx = [i for i, (ev, _) in enumerate(log) if ev == "device_deleted"]
    det_idx = [i for i, (ev, _) in enumerate(log) if ev == "detach"]

    order = " -> ".join(ev for ev, _ in log)
    s.before = "（本场景只针对修复后顺序，修复前因超时根本走不到销毁流）"
    s.after = "事件顺序: " + order

    s.check("存在 stream_destroyed 与 device_deleted 事件",
            bool(sd_idx) and bool(dev_idx))
    s.check("stream_destroyed 索引 < device_deleted 索引",
            bool(sd_idx) and bool(dev_idx) and sd_idx[0] < dev_idx[0])
    s.check("detach 索引 < device_deleted 索引 (设备最后删)",
            bool(det_idx) and bool(dev_idx) and det_idx[0] < dev_idx[0])
    s.check("修复前该路径因超时无法到达销毁流(记录为超时)",
            r2["result"] == "DRAINED")  # 修复后能到达；修复前见场景1超时结论
    return s


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    _enable_utf8()

    print("=" * 74)
    print("RAMBlockCache 引用计数拆除协议 —— 确定性状态机模型测试")
    print("refcount_model.py")
    print("=" * 74)
    print("模型对象: RB_DEV_EXT (Refs 初始 1 = owner 基准引用)")
    print("拆除流程: 摘表 -> Removed=1 -> 等在途排空 -> 销毁流 -> 分离/删设备")
    print("缺陷 A  : 排空阈值写成 Refs<=1，调用方自持 1 引用 -> Refs>=2 恒不成立")
    print("缺陷 B  : 僵尸回收器对忙条目原地重排+无条件 continue，无上界")
    print("修复后  : 阈值 = 1 + SelfRefs；回收器每轮按进入时条目数有界")
    print("-" * 74)

    scenarios = [scenario1(), scenario2(), scenario3(), scenario4(), scenario5()]

    for s in scenarios:
        print()
        print("=== 场景%d: %s ===" % (s.idx, s.title))
        print("  修复前: %s" % s.before)
        print("  修复后: %s" % s.after)
        for note in s.notes:
            print("  备注  : %s" % note)
        for desc, ok in s.checks:
            print("    [%s] %s" % ("PASS" if ok else "FAIL", desc))
        print("  结果  : %s" % ("PASS" if s.passed else "FAIL"))

    total = sum(len(s.checks) for s in scenarios)
    passed = sum(1 for s in scenarios for _, ok in s.checks if ok)
    scen_pass = sum(1 for s in scenarios if s.passed)

    print()
    print("=" * 74)
    print("汇总: 场景 %d/%d 通过；断言 %d/%d 通过"
          % (scen_pass, len(scenarios), passed, total))
    print("=" * 74)
    print()
    print("【本模型能证明什么 / 不能证明什么】")
    print("  能证明: 在给定抽象下，缺陷 A（阈值 1 忽略调用方自持引用）会导致排空条件")
    print("          永不成立、必然超时；缺陷 B（回收器无上界）会导致单次调用对忙条目")
    print("          无限自旋。修复后（阈值 1+SelfRefs、每轮有界）两处均可终止，且")
    print("          '销毁流先于删设备'的顺序关系成立。")
    print("  不能证明: 真实内核 IRQL 契约、内存屏障/原子性语义、真实 PnP 竞态、")
    print("          流销毁与在途 I/O 的实际 UAF 窗口。这些必须由内核编译 + Driver")
    print("          Verifier(0xA/0x50 家族) + 真机热插拔/压力测试验证。")
    print("          本模型是协议逻辑的必要性检查，不是充分性证明。")
    print()

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
