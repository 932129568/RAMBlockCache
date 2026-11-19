#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A2-M · 并发 FLUSH 假失败的根因模型（不碰内核源码）

真机观测（a2_concurrency_sweep）：
  回写态 threads=1/2 -> 0/3 失败；4 -> 2/3；8/16 -> 3/3，err=1117。
  直写态 16 线程 -> 0 失败；关回写后盘面 oracle 全 PASS；LostDirtyBytes 恒 0。
  驱动日志 `ERROR [1426] FLUSH IRP failed: dirty writeback incomplete`。

源码 rbio.c:1401-1427：
  IRP_MJ_FLUSH_BUFFERS -> rb_stream_flush(s, 1)
  -> if (rb_stream_dirty_bytes(s) > 0) persisted = FALSE
  -> STATUS_IO_DEVICE_ERROR

假设：并发写线程在 flush 扫描期间持续产生**新脏块**，而判据用的是
      「flush 返回瞬间的全局脏量」而非「FLUSH 发起时点之前的脏是否已落盘」，
      于是把「新写入导致的脏」误判成「刷脏失败」= 假失败（false negative
      on availability），且并发度越高越必然。

本模型对比三版判据：
  V_current  全局 dirty_bytes 判据（现状）
  V_snapshot 以 FLUSH 发起时的脏序列快照为判据（修复方向 A）
  V_barrier  FLUSH 期间写屏障：新写降级直写（修复方向 B）
阳性对照：注入真实下层写失败，三版都必须 FAIL（证明器具能检出真故障）。
"""
import json
import random

BLOCKS = 256


class Stream:
    def __init__(self):
        self.dirty = set()
        self.seq = 0
        self.on_disk = {}
        self.fail_blocks = set()

    def write(self, b, gen):
        self.dirty.add(b)
        self.seq += 1
        self.on_disk[b] = gen      # 逻辑上：写已吸收进缓存

    def flush(self):
        """返回 (snapshot_ok, snapshot_blocks)。模拟扫描期间仍有新写交错。"""
        snapshot = set(self.dirty)
        failed = False
        for b in snapshot:
            if b in self.fail_blocks:
                failed = True       # 下层写失败：脏保留
            else:
                self.dirty.discard(b)
        return (not failed), snapshot

    def dirty_bytes(self):
        return len(self.dirty)


def simulate(threads, rounds, mode, inject_real_failure=False, seed=0):
    """每一轮：threads 个写线程 + 1 个 FLUSH worker 交错。

    真机语义：发起 FLUSH 的那个写线程已经写完；**只有其它并发线程**
    还会在 FLUSH 扫描期间继续写、产生新脏块。因此
        tail_writers = max(0, threads - 1)
    单线程时 tail=0 -> 不会有新脏 -> 现状判据也不误判（与真机 0/3 一致）。
    """
    rng = random.Random(seed)
    false_fail = 0
    true_fail = 0
    for _ in range(rounds):
        s = Stream()
        tail_writers = max(0, threads - 1)
        bad_block = rng.randrange(BLOCKS)

        # FLUSH 发起前：各线程已写入若干块（脏）
        for t in range(threads):
            for _j in range(4):
                s.write(rng.randrange(BLOCKS), t)
        # 阳性对照：把必然失败的块放进 FLUSH 快照
        if inject_real_failure:
            s.fail_blocks.add(bad_block)
            s.write(bad_block, -1)

        # FLUSH worker 进入：以发起时点的脏块快照为落盘集合
        snapshot = set(s.dirty)
        snap_ok = True
        for b in snapshot:
            if b in s.fail_blocks:
                snap_ok = False          # 下层写失败：该块脏保留
            else:
                s.dirty.discard(b)

        # FLUSH 扫描期间：其它并发线程继续写
        for t in range(tail_writers):
            for _j in range(4):
                if mode == "barrier":
                    # 写屏障：新写降级直写，不产生脏
                    s.on_disk[rng.randrange(BLOCKS)] = 100 + t
                else:
                    s.write(rng.randrange(BLOCKS), 100 + t)

        # 判据
        if mode == "current":
            failed = s.dirty_bytes() > 0      # 现状：看 FLUSH 后瞬时全局脏量
        else:
            failed = not snap_ok              # 快照/屏障：只看发起时快照是否落净

        if inject_real_failure:
            if failed:
                true_fail += 1
        else:
            if failed:
                false_fail += 1
    return {"false_fail": false_fail, "true_fail": true_fail,
            "rounds": rounds}


def main():
    print("=== A2-M CONCURRENT FLUSH JUDGEMENT MODEL ===", flush=True)
    out = {}
    rounds = 400
    for mode in ("current", "snapshot", "barrier"):
        by_t = {}
        for t in (1, 2, 4, 8, 16):
            r = simulate(t, rounds, mode, seed=100 + t)
            by_t[str(t)] = {"false_fail_rate": round(r["false_fail"] / rounds, 3)}
        pos = simulate(8, 200, mode, inject_real_failure=True, seed=999)
        out[mode] = {"by_threads": by_t,
                     "positive_control_real_io_failure_detected":
                         round(pos["true_fail"] / 200, 3)}
    print(json.dumps(out, ensure_ascii=False, indent=2), flush=True)
    print("=== VERDICT ===", flush=True)
    print("V_current must show false-fail rate rising with thread count "
          "(reproduces the machine observation).", flush=True)
    print("V_snapshot / V_barrier must show ~0 false fails while still "
          "detecting a real lower-layer write failure (positive control).", flush=True)
    with open(r"C:\Users\Public\o9\stress\a2_model_result.json", "w",
              encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
