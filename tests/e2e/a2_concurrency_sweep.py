#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A2-S · 并发度扫描：把「FLUSH IRP failed」变成可量化的因果证据

A2 主压测已捕获：
  * rbc_e2e_verify 回写态 8 线程 -> writer thread 2: err=1117 (ERROR_IO_DEVICE)
  * 驱动日志 `ERROR [1426] FLUSH IRP failed: dirty writeback incomplete`
  * 源码 rbio.c:1401-1427：IRP_MJ_FLUSH_BUFFERS 强制 rb_stream_flush(s,1)，
    随后 `rb_stream_dirty_bytes(s) > 0` 即判失败、回 STATUS_IO_DEVICE_ERROR。

假设 H：并发写与强制 FLUSH 交错时，刷脏扫描后仍有新写产生的脏块
        -> 失败率随并发度单调上升。
对照：
  * 直写态 16 线程 -> 应 0 失败（排除工具/环境）
  * 回写态 1 线程 -> 应 0 失败（排除单写路径）
  * 每档 3 次重复 -> 区分必现与偶发

同时判定数据安全性：LostDirtyBytes 是否恒 0、关回写后 --verify-only 是否
逐字节 PASS（区分「拒绝写」与「静默损坏」）。
"""
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

BASE = "http://127.0.0.1:5199"
ORACLE = r"C:\Users\Public\o9\rbc_e2e_verify.exe"
LOGF = Path(r"C:\ProgramData\RAMBlockCache\logs\driver-20261109.log")
OUT = Path(r"C:\Users\Public\o9\stress")
PREFIX = r"D:\rbc-a2-sweep"


def rpc(m, p=None):
    body = {"method": m}
    if p is not None:
        body["params"] = p
    req = urllib.request.Request(BASE + "/rb/rpc",
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=20) as r:
        d = json.loads(r.read().decode())
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (m, st, d.get("message")))
    return d


def cfg():
    return rpc(4)["data"]


def st():
    return rpc(1)["data"]


def log_error_count():
    if not LOGF.exists():
        return 0
    txt = LOGF.read_text(encoding="utf-8", errors="replace")
    return txt.count("FLUSH IRP failed: dirty writeback incomplete")


def run(path, threads, seed, verify_only=False):
    cmd = [ORACLE, "--file", path, "--size", "8", "--seed", str(seed),
           "--threads", str(threads), "--pattern", "stripe4k"]
    if verify_only:
        cmd.append("--verify-only")
    p = subprocess.run(cmd, capture_output=True, timeout=300)
    out = p.stdout.decode("utf-8", "replace")
    err = p.stderr.decode("utf-8", "replace")
    okt = len(re.findall(r"WRITE_THREAD=\d+ OK", out))
    fails = re.findall(r"\[FAIL\][^\r\n]*", err + out)
    errcodes = re.findall(r"err=(\d+)", err)
    return {"exit": p.returncode, "ok_threads": okt, "threads": threads,
            "fail_messages": fails[:3], "err_codes": errcodes[:3],
            "sha_ok": ("SHA256_EXPECTED=" in out and
                       "MISMATCH_COUNT=0" in out and
                       "RESULT: PASS" in out),
            "stdout_tail": out.strip()[-260:]}


def recovery_failed(res, stage, exc):
    """恢复段失败的统一记账：一条响亮日志 + 结果 JSON 字段（退出码由调用点置）。

    不能让「字段缺失」看起来像「恢复成功」，所以显式写 recovery_error。
    """
    msg = repr(exc)[:200]
    res.setdefault("recovery_error", msg)
    res.setdefault("recovery_stage", stage)
    print(json.dumps({"RECOVERY_FAILED": msg, "STAGE": stage,
                      "ACTION": "manually POST method=2 with the baseline config"},
                     ensure_ascii=False), flush=True)


def main():
    print("=== A2-S CONCURRENCY SWEEP ===", flush=True)
    c0 = cfg()
    assert c0["WriteBack"] == 0
    res = {"baseline": {"WriteBack": 0,
                        "flush_error_total": log_error_count()}}
    created = []   # (path, seed, threads) —— 保留写入参数供最终 oracle 复传
    rc = 0

    # 直写态 16 线程对照
    wt = PREFIX + "-wt.bin"
    created.append((wt, 900, 16))
    res["control_write_through_16t"] = run(wt, 16, 900)

    cwb = dict(c0)
    cwb["WriteBack"] = 1
    try:
        rpc(2, cwb)
        res["enable"] = {"readback": cfg()["WriteBack"],
                         "WriteBackMode": st()["WriteBackMode"]}

        levels = [1, 2, 4, 8, 16]
        sweep = {}
        for t in levels:
            rows = []
            for i in range(3):
                path = "%s-t%d-r%d.bin" % (PREFIX, t, i)
                seed = 1000 + t * 10 + i
                created.append((path, seed, t))
                e0 = log_error_count()
                s0 = st()
                r = run(path, t, seed)
                time.sleep(0.5)
                e1 = log_error_count()
                s1 = st()
                r.update({"seed": seed,
                          "new_flush_errors": e1 - e0,
                          "d_LostDirtyBytes": s1["LostDirtyBytes"] -
                          s0["LostDirtyBytes"],
                          "d_DirtyBytes": s1["DirtyBytes"] -
                          s0["DirtyBytes"]})
                rows.append(r)
            failed = sum(1 for r in rows if r["exit"] != 0)
            sweep[str(t)] = {"runs": rows, "fail_runs": failed,
                             "fail_rate": round(failed / len(rows), 2),
                             "flush_errors": sum(r["new_flush_errors"]
                                                 for r in rows)}
        res["sweep_writeback"] = sweep

        # 关回写：自动刷脏 + 丢缓存视图
        rpc(2, c0)
        res["disable"] = {"WriteBack": cfg()["WriteBack"],
                          "WriteBackMode": st()["WriteBackMode"],
                          "DirtyBytes": st()["DirtyBytes"],
                          "LostDirtyBytes": st()["LostDirtyBytes"]}

        # 关回写后逐个做盘面 oracle（视图已丢弃，读必来自盘）
        final = []
        for path, seed, _t in created:
            if not os.path.isfile(path):
                continue
            final.append({"file": path, "seed": seed,
                          "verify": run(path, 8, seed, verify_only=True)})
        res["final_disk_oracle"] = final
    finally:
        try:
            rpc(2, c0)
        except Exception as e:
            rc = 2                      # 恢复失败 -> 非 0
            recovery_failed(res, "restore_rpc", e)
        try:
            res["recovery"] = {"WriteBack": cfg()["WriteBack"],
                               "DirtyBytes": st()["DirtyBytes"],
                               "LostDirtyBytes": st()["LostDirtyBytes"]}
        except Exception as e:
            rc = 2
            recovery_failed(res, "recovery_readback", e)

    for f, _seed, _t in created:
        try:
            os.remove(f)
        except OSError:
            pass

    print("=== SWEEP SUMMARY ===", flush=True)
    print("threads | fail_runs/3 | flush_errors | new_FLUSH_errors", flush=True)
    for t in ("1", "2", "4", "8", "16"):
        s = res["sweep_writeback"][t]
        print("  %-5s | %-11d | %-12d | %s" %
              (t, s["fail_runs"], s["flush_errors"],
               [r["new_flush_errors"] for r in s["runs"]]), flush=True)
    print("control_write_through_16t exit=%s" %
          res["control_write_through_16t"]["exit"], flush=True)
    print(json.dumps(res, ensure_ascii=False), flush=True)
    (OUT / "a2_sweep_result.json").write_text(
        json.dumps(res, ensure_ascii=False, indent=2), encoding="utf-8")
    return rc


if __name__ == "__main__":
    sys.exit(main())
