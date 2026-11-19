#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T5 · 同步 IO 三态 CAS 的「SetEvent 后抢占」全交错判别模型

I11/P2 说「115 号超时/orphan 分支无真机覆盖，只有 syncio_model.py 9/9」。
本模型不重复那 9 个交错，而是补上被 9/9 模型**抽象掉**的一个维度：
完成例程的 SetEvent 与 CAS 之间可被抢占。

源码事实（rbdev.c:126-140 / rbattach.c:257-270，逐字）：
    completion:
        C1  KeSetEvent(&ctx->evt, ...)          <-- 访问 ctx
        C2  CAS(&ctx->State, DONE, WAITING)    <-- 访问 ctx
        C3  if C2 saw ORPHAN: IoCompleteRequest + free PoolBuf + free ctx
    main:
        M1  KeWaitForSingleObject(&ctx->evt, ...)
        M2  事件已置位 -> IoCompleteRequest + 读 iosb/copy + free ctx
        M3  超时 -> CAS(&ctx->State, ORPHAN, WAITING)
        M4  CAS 看到 DONE -> 走正常收尾(M2)；否则 log + return TIMEOUT

关键交错：C1 置位事件后主路径被唤醒，M1 返回 SUCCESS -> M2 释放 ctx；
         完成例程随后才执行 C2/C3，此时 ctx 已被释放（甚至被 lookaside
         复用）-> 读 ctx->State 是 UAF；若读到 ORPHAN 还会二次完成/二次释放。

对比三版：
  V0 = 112 号回归版（正常路径立即释放 ctx，late IRP 写已复用池）
       —— 阳性对照：模型必须检出 UAF，否则本器具无区分力
  V1 = 当前 115 号（SetEvent -> CAS -> 条件收尾）
  V2 = 引用计数版（main/completion 各持一引用，最后释放者收尾）
       —— 修复方向验证

判据：任一执行体访问已释放 ctx = UAF；ctx 释放次数 != 1；IRP 完成次数 != 1。
"""
import itertools
import json
from dataclasses import dataclass, field

WAITING, DONE, ORPHAN = 0, 1, 2


@dataclass
class Ctx:
    alive: bool = True
    freed: int = 0
    irp_completed: int = 0
    state: int = WAITING
    event_set: bool = False
    refs: int = 0
    uaf: list = field(default_factory=list)
    owner: str = ""


def touch(ctx, who, what):
    if not ctx.alive:
        ctx.uaf.append(f"{who} touched freed ctx ({what})")


def do_free(ctx, who):
    touch(ctx, who, "free")
    if ctx.alive:
        ctx.alive = False
        ctx.freed += 1
        ctx.owner = who


def do_complete(ctx, who):
    touch(ctx, who, "IoCompleteRequest")
    ctx.irp_completed += 1


def cas(ctx, expected, new, who):
    touch(ctx, who, "CAS(State)")
    old = ctx.state
    if old == expected:
        ctx.state = new
    return old


# ---------------- V0: 112 号回归版（阳性对照） ----------------
def run_v0(schedule):
    c = Ctx()
    main_done = comp_done = False
    for step in schedule:
        if step == "M" and not main_done:
            # 正常路径立即收尾，不等完成例程
            do_complete(c, "main")
            do_free(c, "main")
            main_done = True
        elif step == "C" and not comp_done:
            # late IRP: 无条件 SetEvent 到 ctx->evt
            touch(c, "completion", "KeSetEvent")
            c.event_set = True
            comp_done = True
    if not main_done:
        do_complete(c, "main"); do_free(c, "main")
    if not comp_done:
        touch(c, "completion", "KeSetEvent")
    return c


# ---------------- V1: 当前实现（SetEvent -> CAS） ----------------
def run_v1(schedule):
    """schedule 是原子步骤序列：C1/C2/C3/M1/M2/M3/M4。
    依赖：C2 在 C1 后；C3 在 C2 后；M2 在 M1(SUCCESS) 或 M4(DONE) 后。"""
    c = Ctx()
    main_ret = None
    cas_old = None
    event_ready = False
    cas_done = False
    for s in schedule:
        if s == "C1":
            touch(c, "completion", "KeSetEvent")
            c.event_set = True
            event_ready = True
        elif s == "C2" and not cas_done:
            cas_old = cas(c, WAITING, DONE, "completion")
            cas_done = True
        elif s == "C3" and cas_done and cas_old == ORPHAN:
            do_complete(c, "completion")
            do_free(c, "completion")
        elif s == "M1":
            # 事件已置位 -> 等待成功（否则超时）
            pass
        elif s == "M2":
            do_complete(c, "main")
            do_free(c, "main")
            main_ret = "PASS"
        elif s == "M3":
            old = cas(c, WAITING, ORPHAN, "main")
            if old == DONE:
                do_complete(c, "main"); do_free(c, "main"); main_ret = "PASS"
            else:
                main_ret = "TIMEOUT"      # 不碰 ctx，立即返回
        elif s == "M4":
            pass
    return c, main_ret


# ---------------- V2: 引用计数版（修复方向） ----------------
def run_v2(schedule):
    c = Ctx(refs=2)
    main_left = comp_left = False
    finished = {"irp": False, "free": False}

    def release(who):
        touch(c, who, "release-ref")
        c.refs -= 1
        if c.refs == 0 and not finished["free"]:
            do_complete(c, who)
            do_free(c, who)
            finished["free"] = True

    for s in schedule:
        if s == "C1" and not comp_left:
            touch(c, "completion", "KeSetEvent")
            c.event_set = True
        elif s == "C2" and not comp_left:
            old = cas(c, WAITING, DONE, "completion")
            # 完成例程裁决后释放自己的引用
            release("completion")
            comp_left = True
        elif s == "M2" and not main_left:
            release("main")
            main_left = True
        elif s == "M3" and not main_left:
            cas(c, WAITING, ORPHAN, "main")
            release("main")
            main_left = True
    if not comp_left:
        release("completion")
    if not main_left:
        release("main")
    return c


def main():
    print("=== T5 SYNC-IO SETEVENT/CAS PREEMPTION MODEL ===", flush=True)
    report = {}

    # V0 阳性对照：late completion 必须被检出 UAF
    v0 = run_v0(["M", "C"])
    report["V0_112_regression_positive_control"] = {
        "uaf": v0.uaf, "freed": v0.freed,
        "detected_uaf": len(v0.uaf) > 0}
    print(json.dumps(report["V0_112_regression_positive_control"],
                     ensure_ascii=False), flush=True)

    # V1 关键交错：C1 置位 -> 主路径 M1/M2 抢先释放 -> C2/C3 才执行
    v1_schedules = {
        "C1_M1_M2_C2_C3": ["C1", "M1", "M2", "C2", "C3"],
        "C1_C2_M1_M2_C3": ["C1", "C2", "M1", "M2", "C3"],
        "C1_M1_M2_C2": ["C1", "M1", "M2", "C2"],
        "M3_C1_C2_C3_timeout": ["M3", "C1", "C2", "C3"],
        "C1_M3_C2_C3": ["C1", "M3", "C2", "C3"],
    }
    v1 = {}
    for name, sched in v1_schedules.items():
        c, ret = run_v1(sched)
        v1[name] = {"main_ret": ret, "uaf": c.uaf, "freed": c.freed,
                    "irp_completed": c.irp_completed,
                    "verdict": "FAIL(UAF/ownership)" if
                    (c.uaf or c.freed != 1 or c.irp_completed != 1) else "PASS"}
    report["V1_current_115"] = v1
    for k, v in v1.items():
        print("V1 %-24s ret=%-8s freed=%d irp=%d uaf=%s -> %s" %
              (k, v["main_ret"], v["freed"], v["irp_completed"],
               "; ".join(v["uaf"]) or "none", v["verdict"]), flush=True)

    # V2 修复方向：同样交错
    v2 = {}
    for name, sched in {
        "C1_M2_C2": ["C1", "M2", "C2"],
        "C1_C2_M2": ["C1", "C2", "M2"],
        "M3_C1_C2": ["M3", "C1", "C2"],
        "C1_M3_C2": ["C1", "M3", "C2"],
    }.items():
        c = run_v2(sched)
        v2[name] = {"uaf": c.uaf, "freed": c.freed,
                    "irp_completed": c.irp_completed,
                    "verdict": "FAIL" if (c.uaf or c.freed != 1 or
                                          c.irp_completed != 1) else "PASS"}
    report["V2_refcount_direction"] = v2
    for k, v in v2.items():
        print("V2 %-24s freed=%d irp=%d uaf=%s -> %s" %
              (k, v["freed"], v["irp_completed"],
               "; ".join(v["uaf"]) or "none", v["verdict"]), flush=True)

    print("=== VERDICT ===", flush=True)
    print("V0 detected_uaf must be True (instrument sensitivity).", flush=True)
    print("V1: if any schedule shows UAF, the current 115 implementation has a",
          flush=True)
    print("    deterministic lifetime race that syncio_model.py's 9/9 abstraction",
          flush=True)
    print("    cannot see (it assumed completion CAS is indivisible with SetEvent).",
          flush=True)
    print("V2: refcount keeps ctx alive until both actors leave -> no UAF.",
          flush=True)
    print("LIMIT: model-level evidence only. It does NOT prove the 00:32 pool",
          flush=True)
    print("       corruption came from this window; that needs a Verifier catch",
          flush=True)
    print("       or a full kernel dump with an exact-PDB stack.", flush=True)
    with open("t5_result.json", "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
