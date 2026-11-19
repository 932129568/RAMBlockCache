# -*- coding: utf-8 -*-
"""[I48c-f 2026-11-10] Gate13 A9-A13 变异验证（补 c5 只覆盖 A1-A8 的空缺）

对每个守卫做一次"回退到旧行为"的定点破坏 → 跑 Gate13 → 必须 FAIL；
还原 → 必须 PASS。破坏限定在对应日志标记之前的窗口内，避免误伤同名守卫。
结果写 C:\\Users\\Public\\o9\\stress\\c5b_result.json。
"""
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(r"C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包")
GATE = REPO / "src" / "scripts" / "check_poolcopy_contract.py"
OUT = Path(r"C:\Users\Public\o9\stress\c5b_result.json")


def run_gate():
    p = subprocess.run([sys.executable, "-B", str(GATE)],
                       capture_output=True, text=True, timeout=120)
    return p.returncode, (p.stdout or "").strip().splitlines()


def mutate_window(path, marker, old, new, window):
    t = path.read_text(encoding="utf-8")
    idx = t.find(marker)
    if idx < 0:
        return None, "marker not found: %s" % marker
    lo = max(0, idx - window)
    seg = t[lo:idx]
    pos = seg.rfind(old)
    if pos < 0:
        return None, "pattern not found in window: %s" % old
    seg2 = seg[:pos] + new + seg[pos + len(old):]
    path.write_text(t[:lo] + seg2 + t[idx:], encoding="utf-8")
    return t, None


def mutate_block(path, old, new):
    t = path.read_text(encoding="utf-8")
    if old not in t:
        return None, "block not found"
    if t.count(old) != 1:
        return None, "block not unique (%d)" % t.count(old)
    path.write_text(t.replace(old, new, 1), encoding="utf-8")
    return t, None


FSCTL_BLOCK = (
    '                if (rb_stream_dirty_bytes(ext->Stream) != 0) {\n'
    '                    rb_stream_set_view_mutating(ext->Stream, 0);\n'
    '                    RbLog(2, __LINE__, STATUS_DEVICE_BUSY,\n'
    '                          L"fsctl: refused - dirty remains after flush (cluster-affecting op)");\n'
    '                    return RbCtlFail(irp, STATUS_DEVICE_BUSY);\n'
    '                }\n'
)
IOCTL_BLOCK = (
    '                if (rb_stream_dirty_bytes(vext->Stream) != 0) {\n'
    '                    rb_stream_set_view_mutating(vext->Stream, 0);\n'
    '                    RbLog(2, __LINE__, STATUS_DEVICE_BUSY,\n'
    '                          L"ioctl: refused - dirty remains after flush (cluster-affecting op)");\n'
    '                    return RbCtlFail(irp, STATUS_DEVICE_BUSY);\n'
    '                }\n'
)


def main():
    ctl = REPO / "src" / "driver" / "rbctl.c"
    att = REPO / "src" / "driver" / "rbattach.c"
    cases = [
        ("A9_downgrade_guard", "win", ctl,
         "cfg transition REFUSED", "dirty_after != 0", "0", 2400),
        ("A10_detach_guard", "win", att,
         "detach: cache view KEPT", "left == 0", "1", 1400),
        ("A11_attach_guard", "win", ctl,
         "attach: cache view KEPT", "dirty_bytes(ext->Stream) == 0", "1", 1600),
        ("A12_setvolume_guard", "win", ctl,
         "write-through switch: cache view KEPT",
         "dirty_bytes(ext->Stream) == 0", "1", 1600),
        ("A13_fsctl_failclosed", "blk", ctl,
         FSCTL_BLOCK,
         '                rb_stream_discard_all(ext->Stream);   /* mutated: old unconditional path */\n',
         None, None),
        ("A13_ioctl_failclosed", "blk", ctl,
         IOCTL_BLOCK,
         '                rb_stream_discard_all(vext->Stream);   /* mutated: old unconditional path */\n',
         None, None),
    ]
    r = {"baseline": None, "mutations": {}, "restored": None, "checks": {}}
    rc, out = run_gate()
    r["baseline"] = {"exit": rc, "output": out[-2:]}
    r["checks"]["baseline_pass"] = rc == 0

    for name, mode, path, a, b, c, w in cases:
        if mode == "win":
            original, err = mutate_window(path, a, b, c, w)
        else:
            original, err = mutate_block(path, a, b)
        if original is None:
            r["mutations"][name] = {"killed": False, "error": err}
            continue
        try:
            rc, out = run_gate()
            r["mutations"][name] = {"exit": rc, "output": out[:3],
                                     "killed": rc != 0}
        finally:
            path.write_text(original, encoding="utf-8")

    rc, out = run_gate()
    r["restored"] = {"exit": rc, "output": out[-2:]}
    r["checks"]["restored_pass"] = rc == 0
    r["checks"]["all_mutations_killed"] = all(
        v.get("killed") for v in r["mutations"].values())
    r["VERDICT"] = "PASS" if all(r["checks"].values()) else "FAIL"
    OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"mutation": {k: v.get("exit") for k, v in r["mutations"].items()}},
                     ensure_ascii=False))
    print(json.dumps({"CHECKS": r["checks"]}, ensure_ascii=False))
    print("RESULT: %s" % r["VERDICT"])
    return 0 if r["VERDICT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
