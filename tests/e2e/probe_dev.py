#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""探测 RB2 控制设备是否可从普通会话读取（用于 AdmitRejects 触发证据）。"""
import ctypes
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
CreateFileW = k32.CreateFileW
CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                        wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                        wintypes.HANDLE]
CreateFileW.restype = wintypes.HANDLE
DeviceIoControl = k32.DeviceIoControl
DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID,
                            wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
DeviceIoControl.restype = wintypes.BOOL

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80
INVALID = ctypes.c_void_p(-1).value
FILE_DEVICE_UNKNOWN = 0x22
METHOD_BUFFERED = 0
FILE_ANY_ACCESS = 0


def ctl(fn):
    return (FILE_DEVICE_UNKNOWN << 16) | (FILE_ANY_ACCESS << 14) | (fn << 2) | METHOD_BUFFERED


class STATS2(ctypes.Structure):
    _fields_ = [("Version", wintypes.ULONG), ("Flags", wintypes.ULONG),
                ("CpuGroups", wintypes.ULONG), ("Workers", wintypes.ULONG),
                ("Hits", ctypes.c_uint64), ("Misses", ctypes.c_uint64),
                ("BytesHit", ctypes.c_uint64), ("BytesMiss", ctypes.c_uint64),
                ("PrefIssued", ctypes.c_uint64), ("PrefFillOk", ctypes.c_uint64),
                ("WbQueued", ctypes.c_uint64), ("WbDone", ctypes.c_uint64),
                ("WbThrottled", ctypes.c_uint64), ("L2Stores", ctypes.c_uint64),
                ("L2Hits", ctypes.c_uint64), ("L2Miss", ctypes.c_uint64),
                ("HotZones", wintypes.ULONG), ("L2State", wintypes.ULONG),
                ("ThermalStops", wintypes.ULONG), ("AdmitRejects", wintypes.ULONG)]


def try_path(path, access):
    h = CreateFileW(path, access, 0, None, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, None)
    if h == INVALID:
        return None, ctypes.get_last_error()
    s = STATS2()
    ret = wintypes.DWORD(0)
    ok = DeviceIoControl(h, ctl(0x901), None, 0, ctypes.byref(s),
                         ctypes.sizeof(s), ctypes.byref(ret), None)
    err = ctypes.get_last_error() if not ok else 0
    k32.CloseHandle(h)
    if not ok:
        return None, err
    return {"AdmitRejects": s.AdmitRejects, "Hits": s.Hits,
            "Misses": s.Misses, "Version": s.Version,
            "ThermalStops": s.ThermalStops, "Flags": s.Flags}, 0


for p in [r"\\.\RAMBlockCache", r"\\.\GLOBALROOT\Device\RAMBlockCache",
          r"\\?\GLOBALROOT\Device\RAMBlockCache"]:
    for a, label in [(GENERIC_READ, "R"), (GENERIC_READ | GENERIC_WRITE, "RW")]:
        v, e = try_path(p, a)
        print("%-50s %-3s -> %s err=%s" % (p, label, v, e))
