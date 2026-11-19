#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A2-R · 对 A2 压测中两处异常做 A/B 判别复现

异常 1：回写态 rbc_e2e_verify（8/16 线程）exit=1，但 --verify-only 逐字节
        + SHA256 却 PASS。需区分：
          (a) 回写路径让写线程失败（真实缺陷）
          (b) 工具在多句柄/大并发下的环境限制（与回写无关）
          (c) 数据不一致（已由 verify-only PASS 排除）
异常 2：SET_CONFIG CacheSizeMB=8 + AutoMemory=0 回读正确，但 CurBytes 不降反升
        到 103 MB。需区分：
          (a) 引擎 max_bytes 未收缩（配置副本与引擎运行期分裂）
          (b) 收缩生效但统计口径不同
          (c) AutoMemory 被后台周期重新覆盖

判据：直写/回写同参数 A/B；完整 stdout 不截断。
"""
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

BASE = "http://127.0.0.1:5199"
ORACLE = r"C:\Users\Public\o9\rbc_e2e_verify.exe"
WT = r"D:\rbc-a2-repro-wt.bin"
WB = r"D:\rbc-a2-repro-wb.bin"
OUT = Path(r"C:\Users\Public\o9\stress")


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
    """(body, err)。收尾/轮询专用（153 §A.4 的 T2）：桥接/驱动抖动不该让脚本
    崩在 traceback 上，更不该掩盖 try 段已经抛出的原始异常。"""
    try:
        return rpc(m, p), None
    except Exception as e:
        return {}, "%s: %s" % (type(e).__name__, e)


def cfg():
    return rpc(4)["data"]


def st():
    return rpc(1)["data"]


def run_oracle(path, threads, tag):
    cmd = [ORACLE, "--file", path, "--size", "16", "--seed", "202",
           "--threads", str(threads), "--pattern", "stripe4k"]
    p = subprocess.run(cmd, capture_output=True, timeout=300)
    out = p.stdout.decode("utf-8", "replace")
    err = p.stderr.decode("utf-8", "replace")
    (OUT / ("a2_repro_%s.txt" % tag)).write_text(
        "CMD: %s\nEXIT: %d\n--- STDOUT ---\n%s\n--- STDERR ---\n%s\n" %
        (" ".join(cmd), p.returncode, out, err), encoding="utf-8")
    lines = [l.strip() for l in out.splitlines() if l.strip()]
    ok_threads = [l for l in lines if l.startswith("WRITE_THREAD") and "OK" in l]
    fail_lines = [l for l in lines if "FAIL" in l or "ERROR" in l or
                  "mismatch" in l.lower() or "MISMATCH" in l]
    return {"tag": tag, "exit": p.returncode,
            "ok_thread_count": len(ok_threads),
            "write_thread_lines": ok_threads,
            "fail_lines": fail_lines,
            "verify_lines": [l for l in lines if l.startswith(
                ("TOTAL_BYTES", "BLOCKS_COMPARED", "MISMATCH_COUNT",
                 "SHA256_", "RESULT:", "PHASE="))],
            "full_log": str(OUT / ("a2_repro_%s.txt" % tag))}


def main():
    print("=== A2-R ABNORMALITY A/B REPRO ===", flush=True)
    c0 = cfg()
    assert c0["WriteBack"] == 0
    result = {"baseline": {"WriteBack": c0["WriteBack"],
                           "CurBytes": st()["CurBytes"]}}

    # A: 直写态，同参数（阴性对照）
    result["A_write_through"] = run_oracle(WT, 8, "A_writethrough")

    cwb = dict(c0)
    cwb["WriteBack"] = 1
    rc = 0
    try:
        rpc(2, cwb)
        result["B_enable"] = {"readback": cfg()["WriteBack"],
                              "WriteBackMode": st()["WriteBackMode"]}
        # B: 回写态，同参数
        result["B_writeback"] = run_oracle(WB, 8, "B_writeback")

        # C: 回写态单线程（排除并发因素）
        result["C_writeback_1thread"] = run_oracle(
            r"D:\rbc-a2-repro-wb1.bin", 1, "C_writeback_1thread")

        # D: 缓存上限收缩判别
        csmall = dict(cwb)
        csmall["AutoMemory"] = 0
        csmall["CacheSizeMB"] = 8
        rpc(2, csmall)
        before = st()
        time.sleep(2.0)
        after = st()
        readback = cfg()
        rpc(2, cwb)
        result["D_cache_shrink"] = {
            "config_readback": {k: readback.get(k) for k in
                                ("AutoMemory", "CacheSizeMB", "WriteBack")},
            "CurBytes_before": before["CurBytes"],
            "CurBytes_after_2s": after["CurBytes"],
            "EffectiveCacheMB": after["EffectiveCacheMB"],
            "DynamicLimitMB": after["DynamicLimitMB"],
            "PressureShrinkBytes": after["PressureShrinkBytes"],
            "EvictBytes": after["EvictBytes"],
            "shrunk": after["CurBytes"] < before["CurBytes"]}
    finally:
        _rb, rerr = try_rpc(2, c0)
        _cb, cerr = try_rpc(4)
        _sb, serr = try_rpc(1)
        _c = _cb.get("data") or {}
        _s = _sb.get("data") or {}
        result["recovery"] = {"WriteBack": _c.get("WriteBack"),
                              "WriteBackMode": _s.get("WriteBackMode"),
                              "DirtyBytes": _s.get("DirtyBytes"),
                              "LostDirtyBytes": _s.get("LostDirtyBytes")}
        if rerr or cerr or serr:
            # 恢复段失败必须可观测（153 §A.4）：写进结果并非零声张，
            # 但不抛出——抛出会顶掉 try 段的原始异常，现场只剩"恢复失败"。
            rc = 3                      # 恢复失败 -> 非 0（本文件无 1/2 语义，故用 3）
            result["recovery_error"] = {"restore": rerr, "config": cerr,
                                        "stats": serr}
            print(json.dumps({"RECOVERY_FAILED": result["recovery_error"]},
                             ensure_ascii=False), flush=True)

    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    for f in (WT, WB, r"D:\rbc-a2-repro-wb1.bin"):
        try:
            os.remove(f)
        except OSError:
            pass
    (OUT / "a2_repro_result.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    return rc


if __name__ == "__main__":
    sys.exit(main())
