#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A2-RV · 修复版上机后的真机复测（必须在「一键 UAC 部署 + 重启一次」之后跑）

前置：
  1. 双击 C:\\Users\\Public\\o9\\RB-fix-deploy.cmd（一次 UAC）
     -> 日志 o9_fix_deploy_result.txt 出现 "RESULT: OK - fixed sys <pin> deployed"
  2. 重启一次并交互登录
  3. 跑本脚本 python a2_reverify_after_reboot.py

判据（比 125 号缺陷压测更严）：
  * 运行镜像 SHA256 必须 == 部署日志里的 SIGNED sha（否则直接 FAIL，不测）
  * 回写态并发扫描 1/2/4/8/16 线程 × 3 次 = 15 次，**全部必须 exit=0**
    （125 号：4 线程 2/3 失败、8/16 线程 3/3 失败）
  * 直写态 16 线程对照必须 exit=0
  * 驱动日志不得出现 "FLUSH IRP failed"（新旧文案都查）
  * LostDirtyBytes 恒 0；关回写后所有文件 --verify-only 逐字节 + SHA256 PASS
  * 配置逐字段恢复、DirtyBytes=0、SafeMode=0

阳性对照（无法在真机注入下层写失败）由用户态 core 回归
`src/core/test_a2flush.c` 场景 B 覆盖：注入 cfg.writeback 失败时
fail_seq 必须增长且判据必须 FAIL。Release/ASan 均已 PASS。
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
DEPLOY_LOG = Path(r"C:\Users\Public\o9\o9_fix_deploy_result.txt")
SYS32 = Path(r"C:\Windows\System32\drivers\RAMBlockCache.sys")
LOGF = Path(r"C:\ProgramData\RAMBlockCache\logs\driver-20261109.log")
OUT = Path(r"C:\Users\Public\o9\stress")
PREFIX = r"D:\rbc-a2-reverify"
# 修复前的运行镜像。部署日志若仍指向它，说明「一键 UAC 部署」尚未执行，
# 必须 fail-closed 停止，否则会在旧驱动上复现缺陷并误报成修复失败。
PRE_FIX_PIN = "723a46d7f165c38b808af154495b998e10104fb4893551122d7bbbca371d206d"
import hashlib


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


def try_rpc(m, p=None):
    """(body, err)。收尾专用（153 §A.4 的 T2）：恢复段失败不许崩在 traceback 上，
    也不许掩盖 try 段已经抛出的原始异常。"""
    try:
        return rpc(m, p), None
    except Exception as e:
        return {}, "%s: %s" % (type(e).__name__, e)


def cfg():
    return rpc(4)["data"]


def st():
    return rpc(1)["data"]


def oracle(path, threads, seed, verify_only=False):
    cmd = [ORACLE, "--file", path, "--size", "8", "--seed", str(seed),
           "--threads", str(threads), "--pattern", "stripe4k"]
    if verify_only:
        cmd.append("--verify-only")
    p = subprocess.run(cmd, capture_output=True, timeout=300)
    out = p.stdout.decode("utf-8", "replace")
    err = p.stderr.decode("utf-8", "replace")
    return {"exit": p.returncode,
            "ok_threads": len(re.findall(r"WRITE_THREAD=\d+ OK", out)),
            "threads": threads, "seed": seed,
            "fail_msgs": re.findall(r"\[FAIL\][^\r\n]*", err)[:3],
            "sha_ok": "SHA256_EXPECTED=" in out and "MISMATCH_COUNT=0" in out
                      and "RESULT: PASS" in out}


def flush_error_count():
    if not LOGF.exists():
        return 0
    t = LOGF.read_text(encoding="utf-8", errors="replace")
    return t.count("FLUSH IRP failed")


def main():
    print("=== A2-RV POST-REBOOT REVERIFICATION ===", flush=True)
    result = {}

    # --- 0) 运行镜像必须等于已部署的修复版 ---
    if not DEPLOY_LOG.exists():
        print("FATAL: deploy log missing - run RB-fix-deploy.cmd first", flush=True)
        return 2
    txt = DEPLOY_LOG.read_text(encoding="utf-8", errors="replace")
    pins = re.findall(r"SIGNED sha=([0-9a-fA-F]{64})", txt)
    if not pins:
        print("FATAL: no SIGNED sha in deploy log - deploy did not succeed",
              flush=True)
        return 2
    pin = pins[-1].lower()
    live = hashlib.sha256(SYS32.read_bytes()).hexdigest().lower()
    result["image"] = {"deployed_pin": pin, "running_sha256": live,
                       "match": live == pin, "is_pre_fix": pin == PRE_FIX_PIN,
                       "deploy_log_has_OK": "RESULT: OK" in txt}
    print(json.dumps({"IMAGE_CHECK": result["image"]}, ensure_ascii=False),
          flush=True)
    if pin == PRE_FIX_PIN:
        print("VERDICT: FAIL - deploy log still points to the PRE-FIX image; "
              "double-click RB-fix-deploy.cmd (one UAC) and REBOOT ONCE first.",
              flush=True)
        return 1
    if live != pin:
        print("VERDICT: FAIL - running image is NOT the fixed build. "
              "Deploy (one UAC) and REBOOT ONCE, then re-run.", flush=True)
        return 1

    c0 = cfg()
    s0 = st()
    result["baseline"] = {"config": c0, "stats": s0}
    assert c0["WriteBack"] == 0 and s0["SafeMode"] == 0

    created = []
    cwb = dict(c0)
    cwb["WriteBack"] = 1
    rc = 0
    try:
        # 直写态对照
        wt = PREFIX + "-wt.bin"
        created.append((wt, 900))
        result["control_write_through_16t"] = oracle(wt, 16, 900)

        rpc(2, cwb)
        result["enable"] = {"readback": cfg()["WriteBack"],
                            "WriteBackMode": st()["WriteBackMode"]}

        err0 = flush_error_count()
        sweep = {}
        for t in (1, 2, 4, 8, 16):
            rows = []
            for i in range(3):
                path = "%s-t%d-r%d.bin" % (PREFIX, t, i)
                seed = 2000 + t * 10 + i
                created.append((path, seed))
                rows.append(oracle(path, t, seed))
            sweep[str(t)] = {"runs": rows,
                             "pass_runs": sum(1 for r in rows if r["exit"] == 0),
                             "fail_runs": sum(1 for r in rows if r["exit"] != 0)}
        result["sweep_writeback"] = sweep
        err1 = flush_error_count()

        rpc(2, c0)
        s_close = st()
        result["disable"] = {
            "WriteBack": cfg()["WriteBack"],
            "WriteBackMode": s_close["WriteBackMode"],
            "DirtyBytes": s_close["DirtyBytes"],
            "LostDirtyBytes": s_close["LostDirtyBytes"],
            "SafeMode": s_close["SafeMode"]}

        final = []
        for path, seed in created:
            if os.path.isfile(path):
                final.append({"file": path, "seed": seed,
                              "verify": oracle(path, 8, seed, verify_only=True)})
        result["final_disk_oracle"] = final
        result["flush_log"] = {"errors_before": err0, "errors_after": err1,
                               "new_errors": err1 - err0}
    finally:
        _rb, rerr = try_rpc(2, c0)
        _cb, cerr = try_rpc(4)
        _sb, serr = try_rpc(1)
        _c = _cb.get("data") or {}
        _s = _sb.get("data") or {}
        result["recovery"] = {"WriteBack": _c.get("WriteBack"),
                              "DirtyBytes": _s.get("DirtyBytes"),
                              "LostDirtyBytes": _s.get("LostDirtyBytes")}
        if rerr or cerr or serr:
            # 恢复失败可观测但不抛：抛出会顶掉原始异常，且下面的 checks/VERDICT
            # 整段不会执行（153 §A.4）。判据侧会因为 WriteBack/LostDirty != 0 变 FAIL。
            rc = 3                      # 恢复失败优先于判据 FAIL(1)：用 3 区分
            result["recovery_error"] = {"restore": rerr, "config": cerr,
                                        "stats": serr}
            print(json.dumps({"RECOVERY_FAILED": result["recovery_error"]},
                             ensure_ascii=False), flush=True)
        for f, _s2 in created:
            try:
                os.remove(f)
            except OSError:
                pass

    # --- 汇总判据 ---
    all_sweep = [r for v in result["sweep_writeback"].values() for r in v["runs"]]
    checks = {
        "image_matches_deployed": result["image"]["match"],
        "writeback_enabled": result["enable"]["WriteBackMode"] == 1,
        "control_write_through_PASS": result["control_write_through_16t"]["exit"] == 0,
        "all_15_concurrent_runs_PASS": all(r["exit"] == 0 for r in all_sweep),
        "no_FLUSH_failure_log": result["flush_log"]["new_errors"] == 0,
        "final_disk_oracle_all_PASS": all(x["verify"]["exit"] == 0 and
                                          x["verify"]["sha_ok"]
                                          for x in result["final_disk_oracle"]),
        "lost_dirty_zero": result["disable"]["LostDirtyBytes"] == 0 and
                           result["recovery"]["LostDirtyBytes"] == 0,
        "dirty_zero": result["disable"]["DirtyBytes"] == 0 and
                      result["recovery"]["DirtyBytes"] == 0,
        "config_restored": result["recovery"]["WriteBack"] == 0,
    }
    result["checks"] = checks
    result["VERDICT"] = "PASS" if all(checks.values()) else "FAIL"

    print("=== SWEEP (fixed build) ===", flush=True)
    for t, v in result["sweep_writeback"].items():
        print("  threads=%-3s pass=%d/3 fail=%d" %
              (t, v["pass_runs"], v["fail_runs"]), flush=True)
    print(json.dumps({"CHECKS": checks, "VERDICT": result["VERDICT"]},
                     ensure_ascii=False), flush=True)
    print("RESULT: %s" % result["VERDICT"], flush=True)
    (OUT / "a2_reverify_result.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    if result["VERDICT"] != "PASS" and rc == 0:
        rc = 1                          # 判据失败；恢复失败(3)优先级更高，不被覆盖
    return rc


if __name__ == "__main__":
    sys.exit(main())
