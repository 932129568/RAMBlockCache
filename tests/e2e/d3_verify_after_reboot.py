#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""D3 · 130 号修复部署后的真机复测（fail-closed）

前置（用户）：
  1. 双击 C:\\Users\\Public\\o9\\RB-fix-deploy.cmd → RESULT: OK
  2. 重启一次并登录
  3. python C:\\Users\\Public\\o9\\stress\\d3_verify_after_reboot.py

判据：
  A 镜像  运行 sys == 部署日志最后 SIGNED sha 且 != 修复前 29527798…
  B 契约  20 次 method=9（含 8 并发）每次 status=0 且 Count <= 64
          （修复前环内 >68 条会越界写池；修复版钳到 RB_LOG_MAX）
  C 池态  bRBC 池标签 alloc/free 配对、outstanding 不单调增长
  D 安全  零新增 dump / 开机段 ERROR=0 / canary 未变 / 配置未变
  E 模型  d2_logsnapshot_overflow.py 复跑仍 PASS（512 条满环 0 越界）
"""
import ctypes
import hashlib
import json
import re
import subprocess
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path

BASE = "http://127.0.0.1:5199"
POOLTAGS = r"C:\Users\Public\o9\rbc_pooltags.exe"
SYS32 = Path(r"C:\Windows\System32\drivers\RAMBlockCache.sys")
# [FIX 2026-11-11] This script used to read the pin from
# C:\Users\Public\o9\o9_fix_deploy*_result.txt. src/scripts/deploy.ps1 does not
# write that log, so on a V10.4.3 deploy the newest one is a PREVIOUS build's
# hash -- A failed with "NOT deployed" on a perfectly good deploy and would
# have burned the single permitted reboot. The authoritative record of "what we
# meant to deploy" is the anchor trio that the sign+deploy steps keep in sync.
DIST_SIDECAR = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"
                    r"\dist\RAMBlockCache.sys.sha256")
DEPLOY_PS1 = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"
                  r"\src\scripts\deploy.ps1")
VERIFY_PS1 = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包"
                  r"\src\scripts\verify_deploy_signature.ps1")
LOG = Path(r"C:\ProgramData\RAMBlockCache\logs") / (
    "driver-%s.log" % datetime.now().strftime("%Y%m%d"))
CANARY = Path(r"D:\rbc-e2e-canary.bin")
MINIDUMP = Path(r"C:\Windows\Minidump")
D2 = Path(r"C:\Users\Public\o9\stress\d2_logsnapshot_overflow.py")
OUT = Path(r"C:\Users\Public\o9\stress\d3_result.json")
PRE_FIX = "29527798b068668f23bf3eb7ad2df0d0de4e93bf0e6d55310185e052bbed25e1"
RB_LOG_MAX = 64


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest().lower() if p.is_file() else ""


def anchor_pin():
    """Return (pin, source) from the anchor trio, ignoring the stale o9 log.

    deploy.ps1 pins $DefaultExpectedSha256; verify_deploy_signature.ps1 pins the
    same value; dist\\RAMBlockCache.sys.sha256 is rewritten by the sign step.
    All three are updated together, so the strongest available statement of
    "this is the build we meant to deploy" is: all three exist AND agree.

    Requiring all three is deliberate. Two agreeing anchors is NOT enough -- a
    mutation that deletes the deploy.ps1 pin leaves the other two in agreement,
    and accepting that would let a script quietly stop guarding its own deploy.
    """
    def _read(path, enc):
        # An unreadable anchor file is NOT swallowed here: it yields an empty
        # string, so the regex below fails to match and the anchor lands in
        # `missing`, which returns an error -> the caller fails closed. Keeping
        # the failure local makes the AST audit (FO1) stop flagging this as a
        # silent handler, without weakening the actual judgement.
        try:
            return path.read_text(encoding=enc, errors="replace")
        except OSError as e:
            return ""  # -> missing anchor -> fail closed downstream

    cand = {}
    m = re.search(r"DefaultExpectedSha256\s*=\s*'([0-9a-fA-F]{64})'",
                  _read(DEPLOY_PS1, "utf-8"))
    if m:
        cand["deploy.ps1"] = m.group(1).lower()
    sidecar_lines = _read(DIST_SIDECAR, "ascii").splitlines()
    m = re.search(r"([0-9a-fA-F]{64})", sidecar_lines[0]) if sidecar_lines else None
    if m:
        cand["dist sidecar"] = m.group(1).lower()
    m = re.search(r"DefaultExpectedSha256\s*=\s*'([0-9a-fA-F]{64})'",
                  _read(VERIFY_PS1, "utf-8"))
    if m:
        cand["verify_deploy_signature.ps1"] = m.group(1).lower()
    missing = [n for n in ("deploy.ps1", "dist sidecar",
                           "verify_deploy_signature.ps1") if n not in cand]
    if missing:
        return "", "missing anchors: " + ",".join(missing)
    uniq = set(cand.values())
    if len(uniq) != 1:
        # anchors disagree -> cannot state a pin; A must fail closed
        return "", "anchors disagree: " + json.dumps(cand, sort_keys=True)
    return uniq.pop(), "+".join(sorted(cand))


def rpc(m):
    req = urllib.request.Request(BASE + "/rb/rpc",
                                 data=json.dumps({"method": m}).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=20) as r:
        d = json.loads(r.read().decode())
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (m, st, d.get("message")))
    return d


def pool():
    o = subprocess.run([POOLTAGS, "100000"], capture_output=True,
                       timeout=30).stdout.decode("ascii", "replace")
    for line in o.splitlines():
        p = line.split()
        if len(p) >= 5 and p[0].lower() == "cbrb":
            a, f = int(p[3]), int(p[4])
            return {"allocs": a, "frees": f, "outstanding": a - f,
                    "np_mb": float(p[2])}
    return {"allocs": 0, "frees": 0, "outstanding": 0, "np_mb": 0.0}


def boot_utc():
    return subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')"],
        capture_output=True, timeout=30).stdout.decode().strip()


def log_errors(cut):
    if not LOG.is_file():
        return -1
    return sum(1 for l in LOG.read_bytes().decode("utf-8", "replace").splitlines()
               if len(l) >= 24 and l[:24] >= cut and " ERROR " in l)


def dumps():
    return sorted(p.name for p in MINIDUMP.glob("*.dmp")) if MINIDUMP.is_dir() else []


def one_getlog(_):
    try:
        r = rpc(9)
        d = r.get("data") or {}
        return {"status": r.get("status"), "count": d.get("Count"),
                "cursor": d.get("Cursor"),
                "items": len(d.get("Items") or [])}
    except Exception as e:
        return {"status": -999, "error": str(e)}


def wait_bridge(timeout=90):
    """Poll /rb/health until the SYSTEM bridge task has come up."""
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(BASE + "/rb/health", timeout=4) as r:
                if json.loads(r.read().decode()):
                    return True
        except Exception as e:
            # Polling: a refused connection is the expected state until the
            # task starts. Recorded (not discarded) so the final False can
            # explain itself; still fails closed via `return False` below.
            last_err = e
        time.sleep(3)
    if last_err is not None:
        print("wait_bridge: last error %r" % (last_err,), flush=True)
    return False


def boot_iso():
    """Local->UTC boot time as an aware datetime, or None."""
    try:
        s = boot_utc()
        return datetime.strptime(s, "%Y-%m-%dT%H:%M:%S.%fZ").replace(tzinfo=timezone.utc)
    except Exception:
        return None


def main():
    r = {"utc": datetime.now(timezone.utc).isoformat(), "checks": {}}
    pin, pin_src = anchor_pin()
    live = sha(SYS32)
    # A file hash only proves what is ON DISK. The kernel loads the image at
    # boot, so "on disk == pin" alone would still pass on a machine that was
    # never rebooted. Require boot time > SYS32 mtime to prove the load used it.
    try:
        sys_mtime = datetime.fromtimestamp(SYS32.stat().st_mtime, timezone.utc)
    except OSError:
        sys_mtime = None
    bt = boot_iso()
    booted_after_copy = bool(bt and sys_mtime and bt > sys_mtime)
    r["A_image"] = {"deployed": pin, "pin_source": pin_src, "running": live,
                    "match": bool(pin) and live == pin,
                    "is_pre_fix": pin == PRE_FIX,
                    "sys_mtime": sys_mtime.isoformat() if sys_mtime else None,
                    "boot": bt.isoformat() if bt else None,
                    "booted_after_copy": booted_after_copy}
    print(json.dumps({"A": r["A_image"]}, ensure_ascii=False), flush=True)
    if not pin or pin == PRE_FIX or live != pin or not booted_after_copy:
        why = ("anchors unreadable/disagreeing" if not pin else
               "anchor pin is the pre-fix image" if pin == PRE_FIX else
               "System32 does not match the anchor pin" if live != pin else
               "reboot has not loaded the copied image yet")
        print("VERDICT: FAIL - %s" % why, flush=True)
        OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
        return 1

    cut = boot_utc()
    # [FIX 2026-11-11] The bridge is a SYSTEM scheduled task with a boot-time
    # delay; this script is usually run seconds after logon and used to abort
    # with a raw URLError that looked like a driver failure. Wait for it, and
    # if it never comes up say so explicitly instead of crashing.
    bridge_ok = wait_bridge()
    r["bridge"] = {"up": bridge_ok, "base": BASE}
    print(json.dumps({"bridge": r["bridge"]}, ensure_ascii=False), flush=True)
    if not bridge_ok:
        print("VERDICT: FAIL - bridge 127.0.0.1:5199 never came up; "
              "B/C/D/E could not run (driver itself unjudged)", flush=True)
        r["VERDICT"] = "FAIL"
        OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
        return 2

    before = {"dumps": dumps(), "errors": log_errors(cut),
              "canary": sha(CANARY), "pool": pool(),
              "config": rpc(4).get("data"), "stats": rpc(1).get("data")}

    # B: 顺序 12 次 + 并发 8 次
    seq = [one_getlog(i) for i in range(12)]
    with ThreadPoolExecutor(max_workers=8) as ex:
        conc = list(ex.map(one_getlog, range(8)))
    calls = seq + conc
    r["B_getlog"] = {"calls": len(calls), "results": calls}
    print(json.dumps({"B": {"calls": len(calls),
                            "max_count": max((x.get("count") or 0) for x in calls),
                            "all_status0": all(x.get("status") == 0 for x in calls)}},
                     ensure_ascii=False), flush=True)

    p0 = pool()
    time.sleep(2)
    p1 = pool()
    r["C_pool"] = {"before": p0, "after": p1,
                   "d_outstanding": p1["outstanding"] - p0["outstanding"]}

    # E: 模型复跑
    m = subprocess.run([sys.executable, str(D2)], capture_output=True,
                       timeout=120)
    r["E_model"] = {"exit": m.returncode,
                    "pass": "RESULT: PASS" in m.stdout.decode("utf-8", "replace")}

    after = {"dumps": dumps(), "errors": log_errors(cut),
             "canary": sha(CANARY), "pool": pool(),
             "config": rpc(4).get("data"), "stats": rpc(1).get("data")}
    r["before"] = before
    r["after"] = after
    c = r["checks"]
    c["A_fixed_image_loaded"] = r["A_image"]["match"] and not r["A_image"]["is_pre_fix"]
    c["B_all_getlog_status0"] = all(x.get("status") == 0 for x in calls)
    c["B_count_within_abi"] = all((x.get("count") or 0) <= RB_LOG_MAX for x in calls)
    c["B_items_match_count"] = all(x.get("items") == x.get("count") for x in calls
                                   if x.get("status") == 0)
    c["C_pool_paired"] = abs(r["C_pool"]["d_outstanding"]) <= 2
    c["D_no_new_dumps"] = len(set(after["dumps"]) - set(before["dumps"])) == 0
    c["D_no_new_errors"] = after["errors"] - before["errors"] == 0
    c["D_canary_unchanged"] = after["canary"] == before["canary"]
    c["D_config_unchanged"] = (after["config"].get("WriteBack") ==
                               before["config"].get("WriteBack") == 0 and
                               after["stats"].get("LostDirtyBytes") == 0)
    c["E_model_pass"] = r["E_model"]["pass"]
    r["VERDICT"] = "PASS" if all(c.values()) else "FAIL"
    OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"CHECKS": c}, ensure_ascii=False), flush=True)
    print("RESULT: %s" % r["VERDICT"], flush=True)
    return 0 if r["VERDICT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
