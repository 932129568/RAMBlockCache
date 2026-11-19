#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""单机内核转储点名脚本（fail-closed）

用途：把 128 号 §5 的结论做成可运行链路 —— 发生池腐坏后，**不需要双机
WinDbg**，在本机用 cdb 离线打开完整内核转储即可读 Verifier Pool Tracking
记录的分配调用栈，点名 ExAllocatePool2 调用点。

用法：
  python analyze_bugcheck_dump.py [dump_path]
  python analyze_bugcheck_dump.py --all          # 批量分析 o9\\dumps 全部 dump
  python analyze_bugcheck_dump.py --out <json>   # 指定结果文件

判据（fail-closed，triage 一律不宣称点名）：
  POINTED     完整转储 + !verifier 3 有 Verifier 列表 + 出现我方分配栈帧
  INSUFFICIENT 其余全部，附带逐条 reason 与下一步指引

安全：只读，不重启、不部署、不改配置、不触发崩溃。
"""
import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

CDB = Path(r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe")
PDB_ROOT = Path(r"C:\Users\Public\o9\pdb")
DUMP_COPY = Path(r"C:\Users\Public\o9\dumps")
FULL_DUMP = Path(r"C:\Windows\MEMORY.DMP")
MINIDUMP = Path(r"C:\Windows\Minidump")
OUT_DIR = Path(r"C:\Users\Public\o9\stress")
SYM_STORE = r"srv*C:\symbols*https://msdl.microsoft.com/download/symbols"
COMMANDS = "!analyze -v; !verifier 3; lm m RAMBlock*; q"
FULL_DUMP_MIN_BYTES = 64 * 1024 * 1024


def pdb_dirs():
    dirs = [str(PDB_ROOT)]
    if PDB_ROOT.is_dir():
        dirs += sorted(str(p) for p in PDB_ROOT.iterdir()
                       if p.is_dir() and (p / "RAMBlockCache.pdb").is_file())
    return dirs


def find_dumps(explicit=None, all_dumps=False):
    if explicit:
        return [Path(explicit)]
    if all_dumps:
        return sorted(DUMP_COPY.glob("*.dmp"))
    if FULL_DUMP.is_file() and FULL_DUMP.stat().st_size >= FULL_DUMP_MIN_BYTES:
        return [FULL_DUMP]
    if MINIDUMP.is_dir():
        ds = sorted(MINIDUMP.glob("*.dmp"), key=lambda p: p.stat().st_mtime,
                    reverse=True)
        if ds:
            return [ds[0]]
    return []


def classify(r, text, cdb_exit=0):
    """纯解析/判据。抽成独立函数以便 --self-test 提供阳性对照文本。"""
    r["cdb_exit"] = cdb_exit
    r["cdb_lines"] = len(text.splitlines())

    m = re.search(r"^BUGCHECK_CODE:\s*([0-9a-fA-F]+)", text, re.M)
    r["bugcheck"] = m.group(1) if m else None
    m = re.search(r"^ExceptionAddress:\s*(\S+)\s*\(([^)]*)\)", text, re.M)
    r["exception_address"] = m.group(1) if m else None
    r["faulting_symbol"] = m.group(2) if m else None
    m = re.search(r"^FAILURE_BUCKET_ID:\s*(.+)$", text, re.M)
    r["failure_bucket"] = m.group(1).strip() if m else None

    triage = ("Kernel Generated Triage Dump" in text or
              "Mini Kernel Dump File: Only registers" in text)
    verifier_ok = "Unable to get verifier list" not in text
    our_module = bool(re.search(r"^\s*[0-9a-f`]+\s+[0-9a-f`]+\s+RAMBlockCache\s",
                                text, re.M))
    alloc_frames = [l.strip() for l in text.splitlines()
                    if "RAMBlockCache!" in l and
                    re.search(r"ExAllocate|Rb2Alloc|RbLog|Pool", l, re.I)]
    r.update({"triage": triage, "verifier_list_available": verifier_ok,
              "our_module_in_dump": our_module,
              "pool_alloc_frames": alloc_frames[:8],
              "full_dump_by_size": r["size_bytes"] >= FULL_DUMP_MIN_BYTES})

    reasons = []
    if triage:
        reasons.append("Kernel Generated Triage Dump：只含寄存器与栈，无完整内核内存"
                       "，Pool Tracking 分配栈不可恢复")
    if not r["full_dump_by_size"]:
        reasons.append("dump 小于 64 MiB，不是完整内核转储"
                       "（CrashDumpEnabled=2 的完整转储通常远大于此）")
    if not verifier_ok:
        reasons.append("!verifier 3 报 Unable to get verifier list："
                       "无法读取 Verifier Pool Tracking 分配调用栈")
    if not alloc_frames:
        reasons.append("未出现我方分配栈帧，不能把被腐坏块的"
                       "ExAllocatePool2 调用点点名")
    if not reasons and our_module:
        r["VERDICT"] = "POINTED"
        r["reasons"] = ["完整转储 + Verifier 分配栈可用；"
                        "以 pool_alloc_frames 为准精确点名"]
    else:
        r["VERDICT"] = "INSUFFICIENT"
        r["reasons"] = reasons
        r["next_step"] = ("保留现场 → Ctrl+Scroll Lock×2 抓完整内核转储"
                          "（CrashDumpEnabled=2 / AlwaysKeepMemoryDump=1 / "
                          "CrashOnCtrlScroll=1 已就绪）→ 重跑本脚本。"
                          "全程单机，无需双机 WinDbg。")
    return r


def analyze_one(dump, sym, text_override=None, size_override=None):
    r = {"dump": str(dump),
         "size_bytes": size_override if size_override is not None else
                       (dump.stat().st_size if dump.is_file() else 0),
         "utc": datetime.now(timezone.utc).isoformat()}
    if text_override is not None:
        return classify(r, text_override, 0)
    if not dump.is_file():
        r.update({"VERDICT": "INSUFFICIENT",
                  "reasons": ["dump file missing"]})
        return r
    if not CDB.is_file():
        r.update({"VERDICT": "INSUFFICIENT",
                  "reasons": ["cdb.exe not found at %s" % CDB]})
        return r

    cmd = [str(CDB), "-z", str(dump), "-y", sym, "-c", COMMANDS]
    p = subprocess.run(cmd, capture_output=True, timeout=180)
    text = p.stdout.decode("utf-8", "replace") + "\n" + \
           p.stderr.decode("utf-8", "replace")
    return classify(r, text, p.returncode)


SYNTHETIC_FULL = """BUGCHECK_CODE:  7e
ExceptionAddress: fffff800e1b99210 (nt!RtlpHpVsSlotCompactChunks+0x210)
FAILURE_BUCKET_ID:  AV_nt!RtlpHpVsSlotCompactChunks
start             end                 module name
fffff800`74b00000 fffff800`74b6f000   RAMBlockCache   (pdb symbols)
Verifier Pool Tracking allocation stacks:
  RAMBlockCache!Rb2Alloc+0x18
  RAMBlockCache!ExAllocatePool2+0x42
"""


def self_test():
    """阳性/阴性对照：证明拿到真正完整转储时脚本会判 POINTED。"""
    fake = Path("synthetic-full.dmp")
    sz = 200 * 1024 * 1024
    pos = analyze_one(fake, "", text_override=SYNTHETIC_FULL,
                      size_override=sz)
    no_ver = analyze_one(fake, "", text_override=SYNTHETIC_FULL +
                         "\nUnable to get verifier list\n", size_override=sz)
    triage = analyze_one(fake, "", text_override=SYNTHETIC_FULL +
                         "\nKernel Generated Triage Dump\n", size_override=sz)
    checks = {
        "full_dump_with_verifier_stack_is_POINTED": pos["VERDICT"] == "POINTED",
        "missing_verifier_list_is_INSUFFICIENT": no_ver["VERDICT"] == "INSUFFICIENT",
        "triage_marker_is_INSUFFICIENT": triage["VERDICT"] == "INSUFFICIENT",
        "positive_frames_reported": len(pos["pool_alloc_frames"]) >= 2,
    }
    print(json.dumps({"SELF_TEST": checks}, ensure_ascii=False))
    print("RESULT: %s" % ("PASS" if all(checks.values()) else "FAIL"))
    return 0 if all(checks.values()) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", nargs="?", default=None)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--out", default=None)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    sym = SYM_STORE + ";" + ";".join(pdb_dirs())
    dumps = find_dumps(args.dump, args.all)
    if not dumps:
        print("RESULT: INSUFFICIENT - no dump found")
        return 2
    results = [analyze_one(d, sym) for d in dumps]
    pointed = sum(1 for x in results if x["VERDICT"] == "POINTED")
    summary = {"analyzed": len(results), "pointed": pointed,
               "insufficient": len(results) - pointed,
               "symbol_pdb_dirs": len(pdb_dirs()) - 1,
               "results": results}
    out = Path(args.out) if args.out else OUT_DIR / "c6_dump_analysis.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(summary, ensure_ascii=False, indent=2),
                   encoding="utf-8")
    for x in results:
        print("%-34s %-8s bc=%-5s triage=%-5s verifier=%-5s %s" %
              (Path(x["dump"]).name, x["VERDICT"], x.get("bugcheck") or "-",
               x.get("triage"), x.get("verifier_list_available"),
               x.get("failure_bucket") or ""))
    print("SUMMARY: analyzed=%d pointed=%d insufficient=%d out=%s" %
          (len(results), pointed, len(results) - pointed, out))
    print("RESULT: %s" % ("POINTED" if pointed else "INSUFFICIENT"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
