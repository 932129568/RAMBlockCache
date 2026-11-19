#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""D4 · 缓存引擎是否真的接管了卷（触发证据，R7）

为什么需要 d4：
  d3 只证明「镜像已部署、驱动能加载、控制面可用、无新 dump」。它 PASS 时
  缓存引擎完全可能一行数据都没缓存过 —— 实例数为 0、计数全 0 的机器同样
  能让 d3 全绿。所以 d3 PASS 不构成「缓存功能可用」的证据。d4 补这一环：
  主动制造真实卷 I/O，再用驱动自报计数证明请求确实穿过了过滤引擎。

判据（逐项独立，全部进判据表）：
  A bridge_alive          /rb/health 返回 ok=true 且 deviceOpen=true
                          不通 → VERDICT: INCONCLUSIVE，exit 2（不算 FAIL）
  B self_disabled_cleared GET_STATS.InstanceCount >= 1
  C volumes_enumerated    GET_VOLUMES.Count >= 1 且 Count == len(Items)
  D io_triggered          8 MiB 写 + fsync + 全量读回校验后，
                          dReadReq > 0 或 dWriteReq > 0（任一即证明在路径上）
  E cache_resident        采样末 CurBytes > 0；CurBytes == 0 但 ReadHit > 0
                          判 PASS（已命中但已回收）
  F no_regression         LostDirtyBytes == 0 且 PanicActive == 0 且 BootFails <= 1
  G readonly_pose         WriteBackMode == 0（项目默认直写姿态 R3）
                          偏离只判 WARN，不判 FAIL（是姿态偏离，不是数据损坏）

用法：
  python -B tests/e2e/d4_verify_cache_takeover.py
  python -B tests/e2e/d4_verify_cache_takeover.py --no-io          # 只读审计
  python -B tests/e2e/d4_verify_cache_takeover.py --json %TEMP%\\d4.json

退出码：PASS=0 / FAIL=1 / 桥接不可用或设备未打开=2

RPC 报文（以 src/ui/web/bridge/server/index.ts 为准，非推测）：
  POST /rb/rpc  {"method": N}  →  {"status": 0, "data": {...}}（status!=0 为 IOCTL 失败）
  GET  /rb/health →  {"ok", "native", "deviceOpen", "pluginError", "deviceError", ...}
  method 1 = GET_STATS，method 7 = GET_VOLUMES（index.ts DECODERS 表）
"""
import argparse
import ctypes
import hashlib
import json
import os
import sys
import tempfile
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

BASE = "http://127.0.0.1:5199"
DEFAULT_JSON = Path(r"C:\Users\Public\o9\stress\d4_result.json")
IO_BYTES = 8 << 20          # 8 MiB
READ_CHUNK = 1 << 20
DRIVE_FIXED = 3             # GetDriveTypeW 返回值

STAT_KEYS = ("InstanceCount", "ReadReq", "WriteReq", "ReadBytes", "WriteBytes",
             "ReadHit", "CurBytes", "ChunkCount", "Enabled", "SafeMode",
             "BootFails", "PanicActive", "WriteBackMode", "LostDirtyBytes")


# --------------------------------------------------------------------------- 桥接


def http_json(url, payload=None, timeout=20):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers,
                                 method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def health():
    return http_json(BASE + "/rb/health", timeout=5)


def rpc(method):
    d = http_json(BASE + "/rb/rpc", {"method": int(method)})
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%d status=%s message=%s"
                           % (method, st, d.get("message")))
    return d.get("data") or {}


def try_rpc(method):
    """(data, err). 桥接/设备抖动不应让脚本崩在 traceback 上。"""
    try:
        return rpc(method), None
    except Exception as e:
        return {}, "%s: %s" % (type(e).__name__, e)


def wait_bridge(timeout=15):
    """桥接是 SYSTEM 计划任务，开机有延迟；短轮询避免把"还没起来"当"坏了"。"""
    deadline = time.time() + timeout
    last = None
    while True:
        try:
            return health(), None
        except Exception as e:
            last = "%s: %s" % (type(e).__name__, e)
        if time.time() >= deadline:
            return None, last
        time.sleep(1)


def pick_stats(data):
    return {k: data.get(k) for k in STAT_KEYS}


# ------------------------------------------------------------------- I/O 目标发现


def fixed_drives():
    """GetLogicalDrives + GetDriveTypeW(DRIVE_FIXED)，不依赖 psutil。"""
    out = []
    try:
        mask = ctypes.windll.kernel32.GetLogicalDrives()
    except Exception:
        return out
    for i in range(26):
        if not (mask >> i) & 1:
            continue
        root = "%s:\\" % chr(65 + i)
        try:
            t = ctypes.windll.kernel32.GetDriveTypeW(ctypes.c_wchar_p(root))
        except Exception:
            t = 0
        if t == DRIVE_FIXED and os.path.isdir(root):
            out.append("%s:" % chr(65 + i))
    return out


def pick_io_target(vols):
    """优先取「GET_VOLUMES 报告已 Attached 的非系统卷」——在没挂过滤器的卷上
    写 8 MiB 什么也证明不了。其次任意非系统固定盘。都不行才退 %TEMP%。"""
    sysd = (os.environ.get("SystemDrive") or "C:").upper().replace("\\", "")
    drives = [d for d in fixed_drives() if d.upper() != sysd]
    prefs, seen = [], set()
    for it in (vols.get("Items") or []):
        letter = str(it.get("Letter") or "").strip().upper().replace("\\", "")
        if len(letter) == 1:
            letter += ":"
        if it.get("Attached") and letter in drives and letter not in seen:
            seen.add(letter)
            prefs.append((letter, "attached via GET_VOLUMES"))
    for d in drives:
        if d not in seen:
            seen.add(d)
            prefs.append((d, "non-system fixed drive"))
    tried = []
    for letter, why in prefs:
        try:
            p = tempfile.mkdtemp(prefix="rbc-d4-", dir=letter + "\\")
            return {"dir": p, "fallback": False, "why": why, "drive": letter,
                    "tried": tried}
        except OSError as e:
            tried.append("%s: %s" % (letter, e))
    p = tempfile.mkdtemp(prefix="rbc-d4-")
    return {"dir": p, "fallback": True,
            "why": "no writable non-system volume", "drive": "",
            "tried": tried}


def target_note(t):
    if t["fallback"]:
        return "io_target=%s (fallback: temp on system volume)" % t["dir"]
    return "io_target=%s (%s)" % (t["dir"], t["why"])


_K32 = ctypes.WinDLL("kernel32", use_last_error=True)
_K32.CreateFileW.restype = ctypes.c_void_p
_K32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                             ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                             ctypes.c_void_p]
_K32.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                          ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
_K32.CloseHandle.argtypes = [ctypes.c_void_p]
_K32.VirtualAlloc.restype = ctypes.c_void_p
_K32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                              ctypes.c_uint32, ctypes.c_uint32]
_K32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]

GENERIC_READ = 0x80000000
FILE_SHARE_RW = 0x00000003
OPEN_EXISTING = 3
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
MEM_COMMIT_RESERVE = 0x3000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04


def unbuffered_sha256(path, chunk=READ_CHUNK):
    """FILE_FLAG_NO_BUFFERING 全量读 + sha256。

    为什么必须这样读：普通 read() 由 Windows 文件缓存服务，**一个设备读都不发**，
    过滤驱动的 ReadReq 恒为 0（实测 8 MiB 读回仅 9 ms、ReadReq 仍是 0），于是
    "缓存是否命中"根本无法观测。缓冲区用 VirtualAlloc 拿页对齐、长度取扇区整数倍，
    才满足 NO_BUFFERING 的对齐要求。
    """
    h = _K32.CreateFileW(path, GENERIC_READ, FILE_SHARE_RW, None, OPEN_EXISTING,
                         FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, None)
    if not h or h == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "CreateFileW(NO_BUFFERING) failed")
    buf = _K32.VirtualAlloc(None, chunk, MEM_COMMIT_RESERVE, PAGE_READWRITE)
    if not buf:
        _K32.CloseHandle(h)
        raise MemoryError("VirtualAlloc failed")
    got = ctypes.c_uint32(0)
    digest = hashlib.sha256()
    n = 0
    t0 = time.time()
    try:
        while True:
            if not _K32.ReadFile(h, buf, chunk, ctypes.byref(got), None):
                raise OSError(ctypes.get_last_error(), "ReadFile failed")
            if got.value == 0:
                break
            digest.update(ctypes.string_at(buf, got.value))
            n += got.value
    finally:
        _K32.VirtualFree(buf, 0, MEM_RELEASE)
        _K32.CloseHandle(h)
    return n, digest.hexdigest(), (time.time() - t0) * 1000.0


def do_io(dirpath, size=IO_BYTES, passes=2):
    """写 8 MiB（fsync 落盘）→ **非缓冲**读回两遍 → sha256 比对。

    两遍读是核心：第一遍必然 miss（填充缓存），第二遍应当命中。两遍耗时之比
    就是"缓存真的在服务读"的直接证据，比任何计数器都难造假。
    内容用 os.urandom：全零块可能被上层稀疏化/压缩，计数就失真了。
    finally 保证任何分支都删掉测试文件。
    """
    path = os.path.join(dirpath, "d4_io_%d.bin" % int(time.time() * 1000))
    buf = os.urandom(size)
    sha_w = hashlib.sha256(buf).hexdigest()
    res = {"path": path, "bytes": size, "sha256_write": sha_w, "unbuffered": True}
    try:
        t0 = time.time()
        with open(path, "wb") as f:
            f.write(buf)
            f.flush()
            os.fsync(f.fileno())
        res["write_ms"] = round((time.time() - t0) * 1000.0, 1)
        del buf
        p = []
        for i in range(max(1, passes)):
            n, sha, ms = unbuffered_sha256(path)
            p.append({"pass": i + 1, "bytes_read": n, "sha256_read": sha,
                      "ms": round(ms, 1), "identical": (n == size and sha == sha_w)})
        res["passes"] = p
        res["bytes_read"] = p[-1]["bytes_read"]
        res["sha256_read"] = p[-1]["sha256_read"]
        res["read_ms"] = p[0]["ms"]
        res["read2_ms"] = p[-1]["ms"]
        if len(p) > 1 and p[0]["ms"] > 0:
            res["speedup_x"] = round(p[0]["ms"] / max(p[-1]["ms"], 0.01), 1)
        res["identical"] = all(x["identical"] for x in p)
    except OSError as e:
        res["error"] = "%s: %s" % (type(e).__name__, e)
        res["identical"] = False
    finally:
        try:
            os.unlink(path)
            res["removed"] = True
        except OSError as e:
            res["removed"] = False
            res["remove_error"] = str(e)
    return res


def sample_after(prev, tries=4, wait=0.8):
    """fsync 后的写计数可能是异步刷下去才记；给几次机会但不无限等。"""
    last = {}
    for i in range(tries):
        time.sleep(wait)
        last, err = try_rpc(1)
        if err or not last:
            continue
        d = (last.get("ReadReq") or 0) - (prev.get("ReadReq") or 0)
        w = (last.get("WriteReq") or 0) - (prev.get("WriteReq") or 0)
        if d > 0 or w > 0:
            last["_poll"] = i + 1
            return last
    last["_poll"] = tries
    return last


# ------------------------------------------------------------------------- 主流程


def main():
    ap = argparse.ArgumentParser(add_help=True, description="D4 cache takeover")
    ap.add_argument("--json", nargs="?", const=str(DEFAULT_JSON),
                    default=str(DEFAULT_JSON), metavar="PATH",
                    help="结果 JSON 落盘路径（默认 %s）" % DEFAULT_JSON)
    ap.add_argument("--no-io", action="store_true",
                    help="跳过 D 段写文件（只读审计场景）")
    args = ap.parse_args()

    r = {"utc": datetime.now(timezone.utc).isoformat(), "no_io": args.no_io,
         "rows": [], "checks": {}}
    rows = r["rows"]

    def row(name, status, value, note):
        rows.append([name, status, value, note])
        print("%-22s  %-4s  %-34s  %s" % (name, status, value, note), flush=True)

    def finish(verdict, code):
        r["VERDICT"] = verdict
        r["checks"] = {n: s for n, s, _, _ in rows}
        out = Path(args.json)
        try:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(json.dumps(r, ensure_ascii=False, indent=2),
                           encoding="utf-8")
            print("INFO json=%s" % out, flush=True)
        except OSError as e:
            print("INFO json_write_failed=%s: %s" % (out, e), flush=True)
        print("VERDICT: %s" % verdict, flush=True)
        return code

    print("INFO base=%s no_io=%s" % (BASE, args.no_io), flush=True)

    # ---- A: 桥接
    h, herr = wait_bridge()
    alive = bool(h and h.get("ok") and h.get("deviceOpen"))
    if h:
        row("A_bridge_alive", "PASS" if alive else "FAIL",
            "ok=%s deviceOpen=%s" % (h.get("ok"), h.get("deviceOpen")),
            "bridge=%s pid=%s native=%s "
            % (BASE, h.get("pid"), h.get("native")) +
            ("device error: %s" % h.get("deviceError") if h.get("deviceError") else "no bridge error"))
    else:
        row("A_bridge_alive", "FAIL", "unreachable", "bridge did not answer: %s" % herr)
    r["health"] = h
    if not alive:
        # 桥接不可用 / 设备未打开：驱动侧状态无从采样，判 INCONCLUSIVE 而非 FAIL，
        # 否则 A 段的传输故障会伪装成 B~G 的驱动故障污染判据语义。
        why = ("bridge down" if not h else
               "device not open (%s)" % (h.get("deviceError") or "unknown"))
        for nm in ("B_self_disabled_cleared", "C_volumes_enumerated",
                   "D_io_triggered", "E_cache_resident", "F_no_regression",
                   "G_readonly_pose"):
            row(nm, "SKIP", "-", "not judged: %s" % why)
        return finish("INCONCLUSIVE - %s" % why, 2)

    # ---- 基线采样
    st_before, err_b = try_rpc(1)
    vols, err_v = try_rpc(7)
    r["stats_before"] = pick_stats(st_before) if st_before else {}
    r["volumes"] = vols
    print("INFO stats_before=%s" % json.dumps(r["stats_before"], sort_keys=True),
          flush=True)

    # ---- C 的前置数据：卷枚举（B 判据的归因需要它）
    cnt = vols.get("Count") if isinstance(vols.get("Count"), int) else 0
    items = len(vols.get("Items") or [])

    # ---- B: 本次开机没有自断环（区分三种状态，别把"未启用"误报成"自断环"）
    # [150/N-150-1] 判据语义收紧（原 R-c）：`InstanceCount` 来自驱动 `DevCount`，
    # 含义是"挂在本过滤器栈上的卷数"，**开机即 7，与缓存是否启用无关**。
    # 原文案 "engine has a live per-volume instance" 把"卷在栈上"说成"引擎已接管"，
    # 会给出过强的结论。B 只证明一件事：本 boot 没有 self-disable（卷确实进栈了）。
    # 真正证明"缓存已启用并服务 I/O"的是 D（计数器增量）+ E（驻留字节）。
    inst = (st_before.get("InstanceCount") or 0)
    attached_n = len([v for v in (vols.get("Items") or []) if v.get("Attached") == 1])
    b_ok = inst >= 1
    if b_ok:
        b_note = ("filter IS on %d volume stack(s); %d volume(s) currently ATTACHed - "
                  "enablement itself is proven by D/E, not by this check" % (inst, attached_n))
    elif cnt >= 1:
        # 过滤器已挂栈、卷也枚举到了，只是引擎仍是惰性的：没有任何卷被启用。
        # 这是"开机不默认全开"的设计，不是故障 —— 必须说清楚动作是什么。
        b_note = ("filter IS attached (%d volumes) but engine is lazy: no volume enabled - "
                  "send ATTACH_VOLUME (method 8, Letter=<data drive>) or set AutoAttach=1" % cnt)
    else:
        b_note = ("no volume on the filter stack: self-disabled this boot or UpperFilters "
                  "rolled back (see driver log [300] / boot_guard)")
    row("B_self_disabled_cleared", "PASS" if b_ok else "FAIL",
        "InstanceCount=%d attached=%d" % (inst, attached_n), b_note)

    # ---- C: 卷枚举
    c_ok = bool(err_v is None and cnt >= 1 and cnt == items)
    row("C_volumes_enumerated", "PASS" if c_ok else "FAIL",
        "Count=%d len(Items)=%d" % (cnt, items),
        "volumes reported by driver" if c_ok else
        ("GET_VOLUMES rpc error: %s" % err_v if err_v else
         "no volume bound to the filter (Count=%d)" % cnt))

    # ---- D: 主动触发真实 I/O
    io = None
    st_after = st_before
    tgt = None
    if args.no_io:
        row("D_io_triggered", "SKIP", "skipped (--no-io)", "read-only audit: no write issued")
    else:
        tgt = pick_io_target(vols)
        r["io_target"] = tgt
        print("INFO io_target=%s" % json.dumps(tgt, ensure_ascii=False), flush=True)
        try:
            io = do_io(tgt["dir"])
            r["io"] = io
            print("INFO io=%s" % json.dumps(
                {k: io.get(k) for k in ("bytes", "bytes_read", "write_ms",
                                        "read_ms", "read2_ms", "speedup_x",
                                        "identical", "removed", "error")},
                ensure_ascii=False), flush=True)
            if io.get("identical"):
                st_after = sample_after(st_before)
            else:
                st_after, _ = try_rpc(1)
        finally:
            # 目录也清掉；文件在 do_io 内部已 finally 清理
            try:
                os.rmdir(tgt["dir"])
                r["io_dir_removed"] = True
            except OSError as e:
                r["io_dir_removed"] = False
                r["io_dir_error"] = str(e)
        d_rr = (st_after.get("ReadReq") or 0) - (st_before.get("ReadReq") or 0)
        d_wr = (st_after.get("WriteReq") or 0) - (st_before.get("WriteReq") or 0)
        d_rb = (st_after.get("ReadBytes") or 0) - (st_before.get("ReadBytes") or 0)
        d_wb = (st_after.get("WriteBytes") or 0) - (st_before.get("WriteBytes") or 0)
        d_ok = (d_rr > 0 or d_wr > 0)
        r["delta"] = {"ReadReq": d_rr, "WriteReq": d_wr, "ReadBytes": d_rb,
                      "WriteBytes": d_wb,
                      "CurBytes": (st_after.get("CurBytes") or 0) -
                                  (st_before.get("CurBytes") or 0)}
        # [150/N-150-6 2026-11-11] 内容一致性不得只写进备注。
        # 旧判据只看"计数器有没有动"，把 `identical=false`（写后读回内容与写入
        # 不符 = 静默数据错误）降级成一句 note 仍判 PASS —— 150 轮正是被这个
        # 假绿掩盖了 P0（对齐整块写跳过失效 → 读回旧内容）。数据错误必须是 FAIL，
        # 与"缓存没工作"同等严重：计数器动只证明有 I/O，不证明 I/O 是对的。
        io_intact = True if not io else bool(io.get("identical"))
        if not io or args.no_io:
            io_intact = True          # 未做 I/O（--no-io）时不做该判据
        r["io_intact"] = io_intact
        note = target_note(tgt)
        if io:
            if io.get("speedup_x"):
                note += "; unbuffered read pass1=%.1fms pass2=%.1fms (%.1fx)" % (
                    io.get("read_ms") or 0, io.get("read2_ms") or 0, io["speedup_x"])
            if not io.get("identical"):
                note += ("; READBACK MISMATCH: %s -> data-integrity failure, not a perf note"
                         % (io.get("error") or "sha256 differs"))
        if not d_ok:
            note += "; no counter delta after %d MiB fsync'd write + unbuffered readback" % (
                IO_BYTES >> 20)
        row("D_io_triggered", "PASS" if (d_ok and io_intact) else "FAIL",
            "dReadReq=%d dWriteReq=%d dReadBytes=%d dWriteBytes=%d intact=%s"
            % (d_rr, d_wr, d_rb, d_wb, io_intact), note)

    r["stats_after"] = pick_stats(st_after) if st_after else {}

    # ---- E: 缓存驻留
    cur = st_after.get("CurBytes") or 0
    hit = st_after.get("ReadHit") or 0
    if cur > 0:
        e_status, e_note = "PASS", "cached bytes resident at sampling time"
    elif hit > 0:
        # 命中过就证明引擎真的服务过请求；CurBytes=0 只是已被回收/淘汰，
        # 机械判负会把"缓存正常工作但回收积极"的机器判成坏机器。
        e_status, e_note = "PASS", "ReadHit>0: served from cache then reclaimed"
    else:
        e_status, e_note = "FAIL", "no resident bytes and never hit: engine idle"
    row("E_cache_resident", e_status, "CurBytes=%d ReadHit=%d" % (cur, hit), e_note)

    # ---- F: 无回归
    lost = st_after.get("LostDirtyBytes") or 0
    panic = st_after.get("PanicActive") or 0
    bootf = st_after.get("BootFails")
    bootf = -1 if bootf is None else bootf
    f_ok = (lost == 0 and panic == 0 and 0 <= bootf <= 1)
    bad = [s for s, c in (("LostDirtyBytes!=0", lost != 0),
                          ("PanicActive!=0", panic != 0),
                          ("BootFails>1", bootf > 1 or bootf < 0)) if c]
    row("F_no_regression", "PASS" if f_ok else "FAIL",
        "LostDirtyBytes=%d PanicActive=%d BootFails=%d" % (lost, panic, bootf),
        "no dirty loss, no panic latch, boot-fail budget intact" if f_ok
        else "regression: " + ",".join(bad))

    # ---- G: 直写姿态（只 WARN）
    wbm = st_after.get("WriteBackMode")
    wbm = -1 if wbm is None else wbm
    g_status = "PASS" if wbm == 0 else "WARN"
    g_note = ("direct-write pose R3 as designed" if wbm == 0
              else "pose deviates from direct-write (R3); not data corruption")
    if inst < 1:
        g_note += "; no live instance, pose not observable"
    row("G_readonly_pose", g_status, "WriteBackMode=%d" % wbm, g_note)

    # WARN/SKIP 不进 PASS/FAIL 汇总：G 是姿态观察项，D 的 SKIP 只出现在 --no-io
    verdicts = [s for _, s, _, _ in rows if s in ("PASS", "FAIL")]
    verdict = "PASS" if verdicts and all(s == "PASS" for s in verdicts) else "FAIL"
    if args.no_io:
        print("INFO --no-io: D_io_triggered skipped, verdict covers A/B/C/E/F only",
              flush=True)
    return finish(verdict, 0 if verdict == "PASS" else 1)


if __name__ == "__main__":
    raise SystemExit(main())
