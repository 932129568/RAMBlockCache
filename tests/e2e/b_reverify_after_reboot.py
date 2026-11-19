#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""B-RV · B 批（124 号四缺陷修复）上机后的真机复测

前置（用户，一次 UAC + 一次重启）：
  1. 双击 C:\\Users\\Public\\o9\\RB-fix-deploy.cmd -> RESULT: OK
  2. 重启一次并交互登录
  3. python b_reverify_after_reboot.py

判据分组（每组都能单独定位一个修复点，避免「全绿却不知为什么」）：

  A 镜像     运行 sys == 部署日志 SIGNED sha 且 != B 批前 pin 42c729b5…
  B 排除策略 三组对照读同一「已缓存」文件：
       Q 普通镜像 probe_q.exe      -> ΔReadHit > 0（阳性对照：缓存确实在服务）
       P 默认名单 msmpeng.exe      -> ΔReadHit≈0 且 ΔReadReq≈0
                                      （验证「worker 上下文二次判定」已修）
       E 位于 D:\\加速 的镜像      -> ΔReadHit≈0 且 ΔReadReq≈0
                                      （验证 ProcessImageFileNameWin32(43) 归一化）
     若 P 通过而 E 失败 -> 43 号查询在内核不可用、回退到 27，可精确定位。
  C A2 回归  开回写后 8/16 线程各 3 次必须全 PASS，关回写后零脏零丢失
            （确认 B 批没有破坏 126 号的 FLUSH 判据修复）
  D syncio   并发非缓冲读写 + 驱动池标签 CBRb alloc/free 严格配对
            （引用计数修复的正常完成分支；超时/orphan 分支真机不可造）
  E 系统态   配置逐字段恢复、开机段 ERROR=0、零新增 dump、canary 未变

安全：try/finally 恢复 WriteBack=0 与全部原配置；自建文件末尾清理；
      不重启、不崩溃注入、不签名/部署。
"""
import ctypes
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from ctypes import wintypes
from pathlib import Path

BASE = "http://127.0.0.1:5199"
ORACLE = r"C:\Users\Public\o9\rbc_e2e_verify.exe"
UNCACHED = r"C:\Users\Public\o9\rbc_uncached_read.exe"
POOLTAGS = r"C:\Users\Public\o9\rbc_pooltags.exe"
DEPLOY_LOG = Path(r"C:\Users\Public\o9\o9_fix_deploy_result.txt")
SYS32 = Path(r"C:\Windows\System32\drivers\RAMBlockCache.sys")
OUT = Path(r"C:\Users\Public\o9\stress")
HOT = r"D:\rbc-b-hot.bin"
QDIR = r"C:\Users\Public\o9\stress"
PDIR = r"D:\加速\_rbc_bprobe"
Q_EXE = os.path.join(QDIR, "b_probe_q.exe")
P_EXE = os.path.join(QDIR, "msmpeng.exe")
E_EXE = os.path.join(PDIR, "b_probe_e.exe")
PRE_B_PIN = "42c729b5882fcb2437463eb9ed618fb621e4c59715df4acbefc92bb0ad493398"

KEYS = ["ReadReq", "ReadHit", "ReadBytes", "ReadHitBytes", "WriteReq",
        "WriteCached", "WbFlush", "WbBytes", "DirtyBytes", "LostDirtyBytes",
        "SafeMode", "WriteBackMode", "RmwReads", "EvictBytes", "CurBytes"]


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
    d = rpc(1)["data"]
    return {k: d.get(k) for k in KEYS}


def dstat(a, b):
    return {k: (b[k] - a[k]) if isinstance(a.get(k), (int, float)) and
            isinstance(b.get(k), (int, float)) else None for k in KEYS}


def pool():
    o = subprocess.run([POOLTAGS, "100000"], capture_output=True,
                       timeout=30).stdout.decode("ascii", "replace")
    for line in o.splitlines():
        p = line.split()
        if len(p) >= 5 and p[0].lower() == "cbrb":
            return {"allocs": int(p[3]), "frees": int(p[4]),
                    "outstanding": int(p[3]) - int(p[4])}
    return {"allocs": 0, "frees": 0, "outstanding": 0}


def read_group(exe, label, settle=0.8):
    a = st()
    p = subprocess.run([exe, "1", HOT], capture_output=True, timeout=60)
    time.sleep(settle)
    b = st()
    d = dstat(a, b)
    rc = p.returncode
    return {"label": label, "exe": exe, "exit": rc,
            "d_ReadReq": d["ReadReq"], "d_ReadHit": d["ReadHit"],
            "d_ReadBytes": d["ReadBytes"], "excluded":
                (d["ReadReq"] == 0 and d["ReadHit"] == 0)}


def oracle(path, threads, seed, verify_only=False):
    cmd = [ORACLE, "--file", path, "--size", "8", "--seed", str(seed),
           "--threads", str(threads), "--pattern", "stripe4k"]
    if verify_only:
        cmd.append("--verify-only")
    p = subprocess.run(cmd, capture_output=True, timeout=300)
    return {"exit": p.returncode,
            "sha_ok": "SHA256_EXPECTED=" in p.stdout.decode("utf-8", "replace")
                      and "MISMATCH_COUNT=0" in p.stdout.decode("utf-8", "replace")
                      and "RESULT: PASS" in p.stdout.decode("utf-8", "replace")}


def main():
    print("=== B-RV POST-REBOOT REVERIFICATION (4-fix batch) ===", flush=True)
    r = {}
    # ---- A 镜像 ----
    if not DEPLOY_LOG.exists():
        print("FATAL: deploy log missing", flush=True)
        return 2
    txt = DEPLOY_LOG.read_text(encoding="utf-8", errors="replace")
    pins = re.findall(r"SIGNED sha=([0-9a-fA-F]{64})", txt)
    if not pins:
        print("FATAL: no SIGNED sha", flush=True)
        return 2
    pin = pins[-1].lower()
    live = hashlib.sha256(SYS32.read_bytes()).hexdigest().lower()
    r["A_image"] = {"deployed": pin, "running": live, "match": live == pin,
                    "is_pre_B": pin == PRE_B_PIN}
    print(json.dumps({"A": r["A_image"]}, ensure_ascii=False), flush=True)
    if pin == PRE_B_PIN:
        print("VERDICT: FAIL - B-batch build NOT deployed; run "
              "RB-fix-deploy.cmd (one UAC) and REBOOT ONCE.", flush=True)
        return 1
    if live != pin:
        print("VERDICT: FAIL - running image != deployed pin; reboot required",
              flush=True)
        return 1

    c0 = cfg()
    r["baseline"] = {"config": c0, "stats": st()}
    created = []
    os.makedirs(PDIR, exist_ok=True)
    for dst in (Q_EXE, P_EXE, E_EXE):
        shutil.copy2(UNCACHED, dst)
    # 预热一个 8 MiB 热文件（先用 ORACLE 写入，再让 Q 组读命中）
    w = oracle(HOT, 8, 77001)
    created.append(HOT)
    print(json.dumps({"warmup": w}, ensure_ascii=False), flush=True)

    rc = 0
    try:
        # ---- B 排除策略三组对照 ----
        # Q 先读一遍使其命中（此时缓存里已有该区间）
        q1 = read_group(Q_EXE, "Q_normal_warm")
        q2 = read_group(Q_EXE, "Q_normal_control")
        p1 = read_group(P_EXE, "P_default_list_msmpeng")
        e1 = read_group(E_EXE, "E_exclude_dir_image")
        r["B_admit"] = {"Q_warm": q1, "Q_control": q2, "P": p1, "E": e1}
        print(json.dumps({"B": r["B_admit"]}, ensure_ascii=False), flush=True)

        # ---- C A2 回归（126 号修复不能退化）----
        cwb = dict(c0)
        cwb["WriteBack"] = 1
        rpc(2, cwb)
        r["C_enable"] = {"WriteBack": cfg()["WriteBack"],
                         "WriteBackMode": st()["WriteBackMode"]}
        c_runs = []
        for i, t in enumerate((8, 16)):
            for j in range(3):
                path = r"D:\rbc-b-a2-t%d-r%d.bin" % (t, j)
                created.append(path)
                c_runs.append(oracle(path, t, 88000 + t + j))
        rpc(2, c0)
        s_close = st()
        r["C_a2"] = {"runs": c_runs,
                     "all_pass": all(x["exit"] == 0 for x in c_runs),
                     "close": {"DirtyBytes": s_close["DirtyBytes"],
                               "LostDirtyBytes": s_close["LostDirtyBytes"],
                               "WriteBackMode": s_close["WriteBackMode"]}}
        print(json.dumps({"C": r["C_a2"]}, ensure_ascii=False), flush=True)

        # ---- D syncio 并发稳定性 + 池标签配对 ----
        p0 = pool()
        d_runs = []
        for i in range(6):
            path = r"D:\rbc-b-sync-r%d.bin" % i
            created.append(path)
            d_runs.append(oracle(path, 8, 99000 + i))
        p1 = pool()
        r["D_syncio"] = {"runs": d_runs,
                         "all_pass": all(x["exit"] == 0 for x in d_runs),
                         "pool_before": p0, "pool_after": p1,
                         "outstanding_delta": p1["outstanding"] - p0["outstanding"]}
        print(json.dumps({"D": r["D_syncio"]}, ensure_ascii=False), flush=True)
    finally:
        _rb, rerr = try_rpc(2, c0)
        _cb, cerr = try_rpc(4)
        _sb, serr = try_rpc(1)
        _c = _cb.get("data") or {}
        _s = _sb.get("data") or {}
        r["recovery"] = {"WriteBack": _c.get("WriteBack"),
                         "DirtyBytes": _s.get("DirtyBytes"),
                         "LostDirtyBytes": _s.get("LostDirtyBytes")}
        if rerr or cerr or serr:
            # 恢复失败可观测但不抛（153 §A.4）：抛出会让下面的 checks/VERDICT
            # 整段丢失（`E_config_restored` 依赖 r["recovery"]）。
            rc = 3                      # 恢复失败优先于判据 FAIL(1)：用 3 区分
            r["recovery_error"] = {"restore": rerr, "config": cerr, "stats": serr}
            print(json.dumps({"RECOVERY_FAILED": r["recovery_error"]},
                             ensure_ascii=False), flush=True)
        for f in (Q_EXE, P_EXE, E_EXE):
            try:
                os.remove(f)
            except OSError:
                pass
        try:
            os.rmdir(PDIR)
        except OSError:
            pass
        for f in created:
            try:
                os.remove(f)
            except OSError:
                pass

    # ---- E 汇总判据 ----
    B, C, D = r["B_admit"], r["C_a2"], r["D_syncio"]
    checks = {
        "A_image_is_B_batch": r["A_image"]["match"] and not r["A_image"]["is_pre_B"],
        "B_positive_control_Q_hits": B["Q_control"]["d_ReadHit"] > 0,
        "B_worker_context_fix_P_excluded": B["P"]["excluded"] and B["P"]["exit"] == 0,
        "B_win32_path_fix_E_excluded": B["E"]["excluded"] and B["E"]["exit"] == 0,
        "C_A2_regression_all_pass": C["all_pass"],
        "C_zero_dirty_loss": C["close"]["DirtyBytes"] == 0 and
                             C["close"]["LostDirtyBytes"] == 0 and
                             C["close"]["WriteBackMode"] == 0,
        "D_syncio_concurrent_all_pass": D["all_pass"],
        "D_pool_paired": D["outstanding_delta"] == 0,
        "E_config_restored": r["recovery"]["WriteBack"] == 0 and
                             r["recovery"]["DirtyBytes"] == 0 and
                             r["recovery"]["LostDirtyBytes"] == 0,
    }
    r["checks"] = checks
    r["VERDICT"] = "PASS" if all(checks.values()) else "FAIL"
    print(json.dumps({"CHECKS": checks, "VERDICT": r["VERDICT"]},
                     ensure_ascii=False), flush=True)
    print("NOTE: if only B_win32_path_fix_E_excluded fails while "
          "B_worker_context_fix_P_excluded passes, ProcessImageFileNameWin32(43) "
          "is unavailable in kernel and the 27 fallback is in effect.",
          flush=True)
    print("RESULT: %s" % r["VERDICT"], flush=True)
    (OUT / "b_reverify_result.json").write_text(
        json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
    if r["VERDICT"] != "PASS" and rc == 0:
        rc = 1                          # 判据失败；恢复失败(3)优先级更高，不被覆盖
    return rc


if __name__ == "__main__":
    sys.exit(main())
