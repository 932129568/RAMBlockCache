#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_disk_truth.py — 用「读方向旁路」取磁盘真值，判定陈旧视图 vs 写丢失

为什么需要它（150/N-150-2）：
  d4 与 probe_write_readback_integrity 都观测到「8 MiB 写 + fsync 后非缓冲读回
  返回旧内容，且 ReadHit 显示全部命中缓存」。必须区分两种故障：
    A 陈旧视图 —— 盘上已是新数据，缓存返回旧块（读路径缺陷）
    B 写丢失   —— 盘上仍是旧数据（写路径缺陷）

  第一次尝试用 ATTACH Attach=0 做旁路，实测 **detach 不生效**
  （GET_VOLUMES 仍报 Attached=1），因此那轮结论无效、方法弃用。
  本工具改用 `RB_MSG_SET_VOLUME` 把该卷 `ReadEnabled=0`：dispatch 的读方向
  门（rbio.c:411）会直接旁路缓存，读到的就是磁盘真值；写方向不改变，
  因此不会影响被观测的写入行为。用完立即恢复 ReadEnabled=1。

  附带一次 `RB_MSG_INVALIDATE_RANGE`：若失效后读回正确，
  则进一步证明"错值来自缓存中的块"，而不是盘面。

盘写预算：默认 1 MiB × 1 轮（写 1 次、读 3 次）≈ 4 MiB 总 I/O，
用于保护 SSD 寿命。需要更大样本时显式 `--size-mb` / `--loops`。

退出码：0 = 本次未复现（全等）；2 = 复现（附归因）。
"""
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import sys
import time
import urllib.request
from ctypes import wintypes

BASE = "http://127.0.0.1:5199"
FILE_FLAG_NO_BUFFERING = 0x20000000
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                            wintypes.HANDLE]
k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                         ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.CloseHandle.argtypes = [wintypes.HANDLE]


def rpc(payload, base=BASE):
    """[152/153 A17] 必须校验信封 status —— 原实现只 json.loads 就返回，
    调用方把返回值整体丢弃（`:116` `set_read(...)`、`:122` `invalidate(...)`），
    于是"开关没生效"与"开关生效"在代码里长得一模一样：`:118` 那句"旁路读"
    实际仍走缓存，`bypass_identical` 失真 ⇒ A/B 归因结论直接错，而退出码
    还是 0/2（一个"成功"的假结论）。
    桥接约定（src/ui/web/bridge/server/index.ts:510-520）：成功 = HTTP 200 +
    `status:0` + `data`；失败 = HTTP 200 + `status:-1` + 只有 `message` 没有 `data`。
    所以非 0 一律抛异常，由调用方决定是终止还是记 degraded——绝不静默。"""
    req = urllib.request.Request(base + "/rb/rpc", data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        d = json.loads(r.read().decode("utf-8"))
    st = d.get("status")
    if st != 0:
        raise RuntimeError("rpc method=%s status=%s message=%s"
                           % (payload.get("method"), st, d.get("message")))
    return d


def unbuf_sha(path, chunk=1 << 20):
    h = k32.CreateFileW(path, 0x80000000, 3, None, 3, FILE_FLAG_NO_BUFFERING, None)
    if h == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "CreateFileW")
    d = hashlib.sha256()
    n = 0
    t0 = time.perf_counter()
    try:
        raw = ctypes.create_string_buffer(chunk + 4096)
        aligned = (ctypes.addressof(raw) + 4095) & ~4095
        buf = ctypes.cast(aligned, ctypes.c_void_p)
        while True:
            got = wintypes.DWORD(0)
            if not k32.ReadFile(h, buf, chunk, ctypes.byref(got), None):
                raise OSError(ctypes.get_last_error(), "ReadFile")
            if got.value == 0:
                break
            d.update(ctypes.string_at(aligned, got.value))
            n += got.value
    finally:
        k32.CloseHandle(h)
    return n, d.hexdigest(), (time.perf_counter() - t0) * 1000.0


def set_read(base, letter, on):
    return rpc({"method": 6, "Letter": letter, "ReadEnabled": 1 if on else 0,
                "WriteEnabled": 1, "WriteMode": 2}, base)


def invalidate(base, letter, off, length):
    return rpc({"method": 13, "Letter": letter, "Reserved": 0,
                "Offset": off, "Length": length}, base)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--drive", default="D")
    ap.add_argument("--size-mb", type=int, default=1, help="默认 1 MiB，保护磁盘寿命")
    ap.add_argument("--loops", type=int, default=1)
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--keep", action="store_true", help="保留测试文件（默认删除）")
    a = ap.parse_args()
    letter = a.drive.upper()
    size = a.size_mb << 20
    root = "%s:\\rbc-truth-%d" % (letter, os.getpid())
    os.makedirs(root, exist_ok=True)
    rows = []
    try:
        for i in range(a.loops):
            path = os.path.join(root, "t%d.bin" % i)
            data = os.urandom(size)
            sha_w = hashlib.sha256(data).hexdigest()
            with open(path, "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            del data
            row = {"loop": i, "sha_write": sha_w}
            n1, s1, ms1 = unbuf_sha(path)
            row.update({"sha_cached": s1, "cache_identical": s1 == sha_w, "cache_ms": round(ms1, 1)})
            try:
                set_read(a.base, letter, False)
                time.sleep(0.3)
                n2, s2, ms2 = unbuf_sha(path)
                row.update({"sha_bypass": s2, "bypass_identical": s2 == sha_w,
                            "bypass_ms": round(ms2, 1)})
            except Exception as e:
                # [152/153 A17] 开关下发失败 ⇒ 下一次读**仍走缓存**，此时拿到的
                # sha 与"旁路读到盘面真值"毫无关系。绝不能当成 bypass 结果用。
                row["bypass_error"] = "%s: %s" % (type(e).__name__, e)
            finally:
                try:
                    set_read(a.base, letter, True)
                    time.sleep(0.2)
                except Exception as e:
                    # 恢复失败不许掩盖上面的异常（原实现里 finally 抛错会顶掉原始
                    # 错误，现场只剩"恢复失败"，真正的成因消失）。
                    row["restore_error"] = "%s: %s" % (type(e).__name__, e)
                    print("WARN loop %d: restore ReadEnabled=1 failed: %s" % (i, e),
                          file=sys.stderr)
            try:
                invalidate(a.base, letter, 0, size)
                time.sleep(0.2)
                n3, s3, ms3 = unbuf_sha(path)
                row.update({"sha_after_invalidate": s3, "invalidate_identical": s3 == sha_w})
            except Exception as e:
                row["invalidate_error"] = "%s: %s" % (type(e).__name__, e)

            if "bypass_error" in row:
                # 最高优先级：拿不到真正的旁路读数，后面任何 A/B 判断都无意义
                row["verdict"] = ("ERR - read-bypass switch did not take effect; "
                                  "attribution INVALID (was silently reported as result before 152)")
            elif row.get("bypass_identical") is True and row["cache_identical"] is False:
                row["verdict"] = ("A - stale cache view: disk already holds the new bytes, "
                                  "the cached read returned an older generation")
            elif row.get("bypass_identical") is False:
                row["verdict"] = "B - write lost: disk still holds older bytes"
            elif row["cache_identical"]:
                row["verdict"] = "PASS"
            else:
                row["verdict"] = "INCONCLUSIVE"
            rows.append(row)
            print(json.dumps(row, ensure_ascii=False), flush=True)
    finally:
        if not a.keep:
            shutil.rmtree(root, ignore_errors=True)
    bad = [r for r in rows if r["verdict"] != "PASS"]
    out = {"drive": letter, "size_mb": a.size_mb, "rows": rows, "bad": len(bad),
           "VERDICT": "PASS" if not bad else bad[0]["verdict"]}
    p = os.path.join(os.environ.get("TEMP", "."), "rbc_disk_truth.json")
    with open(p, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("VERDICT: %s  (bad=%d/%d) json=%s" % (out["VERDICT"], len(bad), len(rows), p))
    return 0 if not bad else 2


if __name__ == "__main__":
    sys.exit(main())
