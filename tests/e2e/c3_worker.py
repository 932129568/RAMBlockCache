#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C3 worker · 单个全新 PID：一次非缓冲读 + 内核池 churn（只读，不改配置）"""
import ctypes
import subprocess
import sys
from ctypes import wintypes

UNCACHED = r"C:\Users\Public\o9\rbc_uncached_read.exe"
HOT_D = r"D:\rbc-c3-hot.bin"
COLD_C = r"C:\Users\Public\o9\stress\c3_cold.bin"
HOSTS = r"C:\Windows\System32\drivers\etc\hosts"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                            wintypes.HANDLE]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
GENERIC_READ = 0x80000000
FILE_SHARE = 0x00000001 | 0x00000002
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80
CHURN = 500


def churn():
    ok = 0
    for _ in range(CHURN):
        h = k32.CreateFileW(HOSTS, GENERIC_READ, FILE_SHARE, None,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
        if h == ctypes.c_void_p(-1).value or h is None:
            continue
        k32.CloseHandle(h)
        ok += 1
    return ok


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "D"
    path = HOT_D if mode == "D" else COLD_C
    p = subprocess.run([UNCACHED, "1", path], capture_output=True, timeout=60)
    n = churn()
    print("%s uncached_exit=%d churn=%d" % (mode, p.returncode, n))
    return 0 if p.returncode == 0 and n > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
