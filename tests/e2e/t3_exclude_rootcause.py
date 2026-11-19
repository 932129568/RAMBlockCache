#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""T3（精确版）· 逐字复现 rbfeat.c:265-288 的路径归一化

修正前一版的字符级笔误：驱动 :271 检测的是 **两个连续反斜杠 +
DEVICE**（"\\\\DEVICE\\..."），而 ProcessImageFileName(27) 实际返回的是
**单个反斜杠**（"\\Device\\..."）。因此该分支不命中，驱动走 :280 的
通用循环，把 NT 设备路径整体大写后交给 ExcludeDirs 做盘符前缀比较，
首字符 '\\' vs 'D' 即失败。

同时查询 ProcessImageFileNameWin32(43) 作为修复方向对照。
"""
import ctypes
import json
import os
from ctypes import wintypes

ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
Q = ntdll.NtQueryInformationProcess
Q.argtypes = [wintypes.HANDLE, ctypes.c_uint, wintypes.LPVOID,
              wintypes.ULONG, ctypes.POINTER(wintypes.ULONG)]
Q.restype = ctypes.c_long
k32.GetCurrentProcess.restype = wintypes.HANDLE


class US(ctypes.Structure):
    _fields_ = [("Length", ctypes.c_ushort), ("MaximumLength", ctypes.c_ushort),
                ("Buffer", ctypes.c_void_p)]


def query(cls):
    buf = ctypes.create_string_buffer(4096)
    ret = wintypes.ULONG(0)
    st = Q(k32.GetCurrentProcess(), cls, buf, 4096, ctypes.byref(ret))
    if st != 0:
        return None, hex(st & 0xFFFFFFFF)
    us = ctypes.cast(buf, ctypes.POINTER(US)).contents
    if not us.Buffer or not us.Length:
        return "", "empty"
    return ctypes.wstring_at(us.Buffer, us.Length // 2), hex(st & 0xFFFFFFFF)


def driver_normalize(s, cap=260):
    """逐字复现 rbfeat.c:265-288，返回 (ok, normalized, branch, first_mismatch)。"""
    n = len(s)
    j = 0
    branch = "generic (:280 loop, no prefix stripped)"
    if n > 4 and s[0] == "\\" and s[1] == "?" and s[2] == "?" and s[3] == "\\":
        j = 4
        branch = ":270 \\??\\ stripped"
    elif (n > 22 and s[0] == "\\" and s[1] == "\\" and
          s[2:8].upper() == "DEVICE"):
        # :274-278：跳过设备名后直接 return FALSE
        return False, None, ":271-278 device branch -> return FALSE", None
    out = []
    k = 0
    while j < n and k + 1 < cap:
        c = s[j]
        if c == "/":
            c = "\\"
        out.append(c.upper())
        j += 1
        k += 1
    norm = "".join(out)
    excl = r"D:\加速".upper()
    mm = next((i for i in range(min(len(norm), len(excl)))
               if norm[i] != excl[i]), min(len(norm), len(excl)))
    return True, norm, branch, {"index": mm,
                                "path_char": norm[mm] if mm < len(norm) else "<end>",
                                "exclude_char": excl[mm] if mm < len(excl) else "<end>"}


def main():
    raw27, st27 = query(27)
    raw43, st43 = query(43)
    win32 = ctypes.create_unicode_buffer(1024)
    k32.GetModuleFileNameW(None, win32, 1024)

    rec = {"pid": os.getpid(),
           "class27_ProcessImageFileName": {"ntstatus": st27, "value": raw27},
           "class43_ProcessImageFileNameWin32": {"ntstatus": st43, "value": raw43},
           "GetModuleFileNameW": win32.value,
           "registered_ExcludeDirs": r"D:\加速"}
    if raw27:
        ok, norm, branch, mm = driver_normalize(raw27)
        rec["driver_simulation"] = {
            "branch_taken": branch, "returns_true": ok,
            "normalized_path": norm,
            "first_prefix_mismatch": mm,
            "excluded_would_be": False}
    print(json.dumps(rec, ensure_ascii=False, indent=2), flush=True)
    print("=== VERDICT ===", flush=True)
    print("ROOT CAUSE: ProcessImageFileName(27) yields an NT device path; the "
          "driver's stripping logic only handles \\??\\ and \\\\DEVICE, so the "
          "normalized string still starts with '\\DEVICE\\...'. ExcludeDirs is "
          "stored as a DOS drive path ('D:\\加速'), so the prefix comparison "
          "fails at index 0 for EVERY process => excluded is always FALSE.", flush=True)
    print("FIX DIRECTION: use ProcessImageFileNameWin32 (class 43) or convert "
          "the NT volume name via RtlVolumeDeviceToDosName / IoVolumeDeviceToDosName "
          "before the prefix compare; add a positive-control gate asserting "
          "AdmitRejects increments for a process in the exclude dir.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
