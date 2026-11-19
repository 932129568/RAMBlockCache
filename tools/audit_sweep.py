#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""audit_sweep.py -- reproducible defect sweep for the RAMBlockCache tree.

Why this file exists
--------------------
The project has 74k lines of first-party source across four languages
(C, C#, TypeScript, Python).  A "30 round" review cannot be honest if it is
narrated from memory; every round has to be re-runnable so that a later
reader can reproduce the finding or prove it was fixed.

This tool implements 30 defect classes (C01..C08, T01..T08, S01..S08,
P01..P06).  Each class is a *high precision* heuristic, not a compiler:
every hit still needs a human to confirm or dismiss it.  The tool therefore
prints the source line so that triage needs no second lookup.

Usage
-----
    python tools/audit_sweep.py --list
    python tools/audit_sweep.py --round C01
    python tools/audit_sweep.py --all
    python tools/audit_sweep.py --all --json out.json
    python tools/audit_sweep.py --all --fail-on-high   # exit 1 if HIGH left

Exit codes: 0 clean/swept, 1 --fail-on-high and a HIGH hit remains,
            2 usage error.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

EXCLUDE_DIRS = {
    "node_modules", "obj", "bin", "dist", "build", "__pycache__",
    "x64", ".git", ".vs", "archive", "packages",
}
# Generated / third-party sources are never ours to fix.
EXCLUDE_NAMES = {
    "rb-config.generated.ts",
    "AssemblyInfo.cs",
}

GROUPS = {
    "c":  [ROOT / "src" / "driver", ROOT / "src" / "core", ROOT / "src" / "ctl"],
    "cs": [ROOT / "src" / "ui" / "controller"],
    "ts": [ROOT / "src" / "ui" / "web" / "src"],
    "py": [ROOT / "src" / "scripts", ROOT / "tools"],
}
SUFFIX = {"c": {".c", ".h"}, "cs": {".cs"}, "ts": {".ts", ".tsx"}, "py": {".py"}}


def iter_files(group: str):
    for base in GROUPS[group]:
        if not base.exists():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file() or p.suffix not in SUFFIX[group]:
                continue
            if p.name in EXCLUDE_NAMES:
                continue
            if any(part in EXCLUDE_DIRS for part in p.parts):
                continue
            yield p


def read(path: Path) -> list[str]:
    try:
        return path.read_text(encoding="utf-8-sig", errors="replace").splitlines()
    except OSError:
        return []


def rel(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


class Hit:
    __slots__ = ("cls", "path", "line", "text", "sev", "note")

    def __init__(self, cls, path, line, text, sev, note=""):
        self.cls, self.path, self.line = cls, path, line
        self.text, self.sev, self.note = text.strip(), sev, note

    def as_dict(self):
        return {"class": self.cls, "path": self.path, "line": self.line,
                "sev": self.sev, "text": self.text, "note": self.note}

    def __str__(self):
        return "%-4s %-5s %s:%d  %s%s" % (
            self.cls, self.sev, self.path, self.line, self.text[:150],
            ("   << " + self.note) if self.note else "")


RESULTS: list[Hit] = []


def emit(cls, path, i, lines, sev, note=""):
    RESULTS.append(Hit(cls, rel(path), i + 1, lines[i], sev, note))


# ---------------------------------------------------------------- helpers
def _blank_comments(lines):
    """Yield (index, line) with // and /* */ comment bodies blanked.

    Only used for structural checks; string literals are left intact, which is
    intentional -- a format string inside a literal is exactly what we want.
    """
    in_block = False
    for i, ln in enumerate(lines):
        out = []
        j = 0
        while j < len(ln):
            if in_block:
                k = ln.find("*/", j)
                if k < 0:
                    j = len(ln)
                    break
                in_block = False
                j = k + 2
                continue
            if ln.startswith("/*", j):
                in_block = True
                j += 2
                continue
            if ln.startswith("//", j):
                break
            out.append(ln[j])
            j += 1
        yield i, "".join(out)


# ================================================================ C classes
RE_ALLOC = re.compile(
    r"(?:=\s*)?(?:\w+\s*=\s*)?(?:malloc|calloc|realloc|ExAllocatePool\w*|"
    r"ExAllocatePoolWithTag|HeapAlloc|Rb2Alloc|RbAlloc|new)\s*\(")
RE_ALLOC_DECL = re.compile(
    r"^\s*(?:[A-Za-z_][\w\s\*]*?)\s*\*?\s*(\w+)\s*=\s*(?:\(\s*[\w\s\*]+\)\s*)?"
    r"(?:malloc|calloc|ExAllocatePool\w*|HeapAlloc|Rb2Alloc|RbAlloc)\s*\(")
RE_NULLTEST = re.compile(r"\b(if\s*\(\s*!?\s*[A-Za-z_]\w*\s*(?:==|!=)?\s*(?:NULL|nullptr|0)?\s*\))")
RE_ALLOC_CHECK = re.compile(r"(!\s*[A-Za-z_]\w*|\b[A-Za-z_]\w*\s*==\s*NULL|\bNULL\s*==\s*[A-Za-z_]\w*|"
                            r"\bNT_SUCCESS\s*\(|\b[A-Za-z_]\w*\s*==\s*nullptr|"
                            # [precision 2026-11-13] `if (ptr)` 与 `ptr ? ... : ...`
                            # 同样是判空，漏掉它们会把两种最常见的 C 写法误报成 HIGH。
                            r"\bif\s*\(\s*[A-Za-z_]\w*\s*\)|\b[A-Za-z_]\w*\s*\?)")


def c01_unchecked_alloc():
    """Allocate into a local, then dereference it with no NULL test nearby."""
    for p in iter_files("c"):
        lines = read(p)
        for i, ln in enumerate(lines):
            m = RE_ALLOC_DECL.search(ln)
            if not m:
                continue
            var = m.group(1)
            window = "\n".join(lines[i:min(len(lines), i + 6)])
            if RE_ALLOC_CHECK.search(window):
                continue
            # Dereference or pass-to-function later in the same function?
            tail = "\n".join(lines[i + 1:i + 40])
            if re.search(r"\b" + re.escape(var) + r"\s*(?:->|\[)", tail) or \
               re.search(r"\*\s*" + re.escape(var) + r"\b", tail):
                emit("C01", p, i, lines, "HIGH",
                     "alloc result %r dereferenced with no NULL test" % var)


def c02_unsafe_string():
    """strcpy/strcat/sprintf/gets -- unbounded writes.

    A preceding `n >= MAX_PATH - K` style guard proves the remaining capacity,
    so those call sites drop to LOW instead of HIGH (they are still fragile,
    but they are not the defect this class is hunting).
    """
    rx = re.compile(r"\b(strcpy|strcat|sprintf|gets|wcscpy|wcscat|wsprintf)\s*\(")
    guard = re.compile(r"MAX_PATH\s*-\s*\d+|\bsizeof\s*\(\s*\w+\s*\)\s*-|remaining|capacity", re.I)
    for p in iter_files("c"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if rx.search(ln):
                kind = re.search(r"\b(strcpy|strcat|sprintf|gets|wcscpy|wcscat|wsprintf)\b", ln).group(1)
                near = "\n".join(lines[max(0, i - 8):i + 1])
                bounded = bool(guard.search(near))
                sev = "LOW" if ("wsprintf" in kind or bounded) else "HIGH"
                emit("C02", p, i, lines, sev,
                     "%s has no bound%s" % (kind, " (nearby capacity guard)" if bounded else ""))


def c03_assign_in_if():
    """if (x = y) -- assignment where a comparison was meant."""
    rx = re.compile(r"\bif\s*\(\s*[A-Za-z_]\w*(?:->|\.)?[A-Za-z_]*\s*=\s*[^=]")
    for p in iter_files("c"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if rx.search(ln):
                emit("C03", p, i, lines, "HIGH", "assignment inside if condition")


def c04_switch_fallthrough():
    """case body ending without break/return/goto and without a fallthrough comment."""
    for p in iter_files("c"):
        lines = read(p)
        n = len(lines)
        for i, ln in enumerate(lines):
            if not re.match(r"\s*case\s+[^:]+:\s*$", ln) and \
               not re.match(r"\s*default\s*:\s*$", ln):
                continue
            # find next case/default at same indent
            j = i + 1
            body = []
            while j < n:
                if re.match(r"\s*(case\s+[^:]+:|default\s*:|\})", lines[j]):
                    break
                body.append(lines[j])
                j += 1
            if not body:
                continue            # empty case, intentional stack
            tail = [b for b in body if b.strip() and not b.strip().startswith(("/*", "//", "*", "}"))]
            if not tail:
                continue
            last = tail[-1].rstrip()
            if last.endswith(("break;", "return;", "goto", "continue;", "}", "return;")):
                continue
            blob = "\n".join(body)
            if "fall through" in blob.lower() or "fallthrough" in blob.lower():
                continue
            emit("C04", p, j - 1, lines, "MED", "case body may fall through")


def c05_size_mult_overflow():
    """size = a * b * c used as an allocation/copy length with no overflow guard."""
    rx = re.compile(r"(?:len|size|bytes|total|cb|sz)\w*\s*=\s*[^;\n]*\*[^;\n]*\*")
    for p in iter_files("c"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if rx.search(ln, re.I):
                window = "\n".join(lines[max(0, i - 3):i + 4])
                if re.search(r"MAXULONG|SIZE_T_MAX|_UI64_MAX|overflow|RtlSizeTMult|"
                             r"RtlULongLongMult|>=\s*\(?.*\)?\s*/\s*", window, re.I):
                    continue
                emit("C05", p, i, lines, "MED", "multiplication into a size, no overflow guard")


def c06_ulong_truncation():
    """(ULONG) cast of a 64-bit expression feeding a length/offset."""
    rx = re.compile(r"\(\s*(?:ULONG|UINT32|DWORD|int)\s*\)\s*\(?\s*[\w\.\]\)\->]*\s*[-+*]")
    for p in iter_files("c"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if rx.search(ln):
                emit("C06", p, i, lines, "LOW", "narrowing cast of an arithmetic expression")


def c07_ioctl_input_len():
    """IOCTL handler that reads the input buffer without a length compare."""
    rx = re.compile(r"InputBufferLength\s*(?:<\s*|==\s*0|\))")
    for p in iter_files("c"):
        lines = read(p)
        blob = "\n".join(lines)
        for i, ln in enumerate(lines):
            if "InputBufferLength" in ln and re.search(r"sizeof\s*\(", ln) is None and \
               not rx.search(ln):
                # only flag uses that look like a read gate
                if re.search(r"InputBufferLength\s*[<>=!]", ln):
                    continue
                emit("C07", p, i, lines, "LOW",
                     "InputBufferLength used without a sizeof() bound in this expression")


def c08_free_then_use():
    """free/ExFreePool of a variable, then that variable referenced again.

    Precision note: the first version of this check produced 9/9 false
    positives, all of the shape

        if (!x) { Rb2Free(x); return; }      <- free, then RETURN
        use(x);                              <- never reached

    so a `return`/`break`/`goto` between the free and the use now clears
    the hit.  That is not a proof (a conditional return can still leave a
    live path), but it removes the entire class of "guard clause" noise
    while keeping the real use-after-free shape.
    """
    rx = re.compile(r"\b(?:free|ExFreePool\w*|HeapFree|Rb2Free|RbFree)\s*\(\s*([A-Za-z_]\w*)\s*\)")
    for p in iter_files("c"):
        lines = read(p)
        n = len(lines)
        for i, ln in enumerate(lines):
            m = rx.search(ln)
            if not m:
                continue
            var = m.group(1)
            if var == "0":
                continue
            # 释放所在行本身就可能带 return（单行守卫 `if (!p) { free(p); return; }`）
            exited = bool(re.search(r"\b(return|break|continue|goto)\b", ln))
            redeclared = False
            for j in range(i + 1, min(n, i + 25)):
                nxt = lines[j]
                if re.search(r"\b(return|break|continue|goto)\b", nxt):
                    exited = True
                # 同名变量在新作用域重新声明 ⇒ 后面的 var 已不是被释放的那个
                if re.search(r"\b[A-Za-z_][\w\s\*]*\b" + re.escape(var) + r"\s*=", nxt):
                    redeclared = True
                if re.match(r"\s*case\b|\s*default\b|\}\s*$", nxt):
                    break
                if re.search(r"\b" + re.escape(var) + r"\s*(?:->|\[|\))", nxt) and \
                   not re.search(r"(?:free|ExFreePool|HeapFree|Rb2Free|RbFree)\s*\(", nxt):
                    if not exited and not redeclared:
                        emit("C08", p, j, lines, "HIGH",
                             "%r freed at line %d then used here" % (var, i + 1))
                    break


# =============================================================== TS classes
def t01_json_parse_unguarded():
    for p in iter_files("ts"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if "JSON.parse(" not in ln:
                continue
            window = "\n".join(lines[max(0, i - 8):i + 1])
            if "try" in window or "catch" in window or ".catch(" in window:
                continue
            emit("T01", p, i, lines, "MED", "JSON.parse with no enclosing try/catch")


def t02_effect_timer_leak():
    for p in iter_files("ts"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if not re.search(r"(setInterval|addEventListener)\s*\(", ln):
                continue
            tail = "\n".join(lines[i:min(len(lines), i + 40)])
            if re.search(r"clearInterval|removeEventListener", tail):
                continue
            emit("T02", p, i, lines, "MED",
                 "timer/listener created with no clear/remove in the same block")


def t03_fetch_no_catch():
    for p in iter_files("ts"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if not re.search(r"\bfetch\s*\(", ln):
                continue
            window = "\n".join(lines[max(0, i - 6):min(len(lines), i + 3)])
            if re.search(r"catch|\.catch\(", window):
                continue
            emit("T03", p, i, lines, "MED", "fetch() without error handling nearby")


def t04_any_cast():
    for p in iter_files("ts"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"\bas\s+any\b", ln):
                emit("T04", p, i, lines, "LOW", "explicit any cast erases the contract")


def t05_nonnull_assert():
    for p in iter_files("ts"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"\w!\.", ln) or re.search(r"\w!\[", ln):
                if "!==" in ln or "===" in ln:
                    continue
                emit("T05", p, i, lines, "LOW", "non-null assertion on a possibly-absent value")


def t06_number_nan():
    for p in iter_files("ts"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"(parseInt|parseFloat|Number)\s*\(", ln):
                window = "\n".join(lines[i:min(len(lines), i + 6)])
                if re.search(r"isNaN|Number\.isFinite|isFinite", window):
                    continue
                emit("T06", p, i, lines, "LOW", "numeric parse with no NaN guard")


def t07_loose_equality():
    for p in iter_files("ts"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"[^=!<>]==[^=]|!=[^=]", ln) and "===" not in ln and "!==" not in ln:
                if re.match(r"\s*(//|\*)", ln):
                    continue
                emit("T07", p, i, lines, "LOW", "loose equality")


def t08_storage_json():
    for p in iter_files("ts"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"(localStorage|sessionStorage)\.(getItem)", ln):
                window = "\n".join(lines[i:min(len(lines), i + 6)])
                if "JSON.parse" in window and "try" not in window:
                    emit("T08", p, i, lines, "MED",
                         "storage read parsed without a try/catch")


# =============================================================== C# classes
def s01_async_void():
    for p in iter_files("cs"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"\basync\s+void\b", ln):
                if re.search(r"(Click|Loaded|Closing|Closed|Shutdown|Startup|Tick)\w*\s*\(", ln):
                    continue
                emit("S01", p, i, lines, "MED",
                     "async void outside an event handler: exceptions escape")


def s02_empty_catch():
    for p in iter_files("cs"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"catch\s*(\([^)]*\))?\s*\{\s*\}", ln):
                emit("S02", p, i, lines, "MED", "empty catch swallows the failure")
                continue
            if re.search(r"catch\s*(\([^)]*\))?\s*$", ln):
                # look at next 3 lines for a closing brace with no body
                body = [x.strip() for x in lines[i + 1:i + 4]]
                if body and body[0] == "}":
                    emit("S02", p, i, lines, "MED", "empty catch block")


def s03_blocking_on_task():
    for p in iter_files("cs"):
        lines = read(p)
        for i, ln in enumerate(lines):
            # [precision 2026-11-13] 只认「小写标识符.Result」：首批 4 个命中
            # 全是 `VolumeGuard.Result` 这类嵌套**类型名**，不是 Task.Result。
            if re.search(r"\b[a-z_][A-Za-z0-9_]*\.Result\b|\.Wait\(\)|\.GetAwaiter\(\)\.GetResult\(\)", ln):
                if re.search(r"Func<[^>]*Result>|Task<[^>]*Result>", ln):
                    continue
                emit("S03", p, i, lines, "HIGH", "blocking wait on a Task in a UI/async path")


def s04_parse_without_tryparse():
    for p in iter_files("cs"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"\b(int|long|double|float|uint|decimal|byte|short)\.Parse\s*\(", ln):
                emit("S04", p, i, lines, "MED", "Parse throws on malformed input; prefer TryParse")


def s05_datetime_now():
    for p in iter_files("cs"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if "DateTime.Now" in ln:
                emit("S05", p, i, lines, "LOW",
                     "DateTime.Now in a value that may be persisted/compared across TZ")


def s06_event_leak():
    for p in iter_files("cs"):
        lines = read(p)
        for i, ln in enumerate(lines):
            m = re.search(r"(\w+)\.(\w+)\s*\+=\s*", ln)
            if not m:
                continue
            tail = "\n".join(lines[i:min(len(lines), i + 120)])
            if re.search(r"\.(Unsubscribe|Detach)\w*\s*\(|%s\.%s\s*-=" % (m.group(1), m.group(2)), tail):
                continue
            emit("S06", p, i, lines, "LOW",
                 "handler subscribed to %s.%s with no matching -=" % (m.group(1), m.group(2)))


def s07_disposable():
    for p in iter_files("cs"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"new\s+(FileStream|StreamReader|StreamWriter|Process|HttpClient|"
                         r"RegistryKey|PerformanceCounter)\s*\(", ln):
                window = "\n".join(lines[max(0, i - 3):min(len(lines), i + 2)])
                if "using" in window or "using var" in ln:
                    continue
                emit("S07", p, i, lines, "MED", "IDisposable created without using/Dispose")


def s08_swallow_task():
    for p in iter_files("cs"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"Task\.Run\s*\(", ln) and "await" not in ln:
                window = "\n".join(lines[i:min(len(lines), i + 4)])
                if ".ContinueWith" in window or "await" in window:
                    continue
                emit("S08", p, i, lines, "LOW",
                     "fire-and-forget Task.Run: exceptions and ordering are lost")


# =============================================================== PY classes
def p01_bare_except():
    for p in iter_files("py"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.match(r"\s*except\s*:\s*$", ln) or re.match(r"\s*except\s*:\s*\w", ln):
                emit("P01", p, i, lines, "MED", "bare except catches everything")


def p02_except_pass():
    for p in iter_files("py"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.match(r"\s*except\b", ln):
                body = lines[i + 1:i + 3]
                if body and re.match(r"\s*(pass|continue)\s*$", body[0]):
                    emit("P02", p, i, lines, "MED", "exception silently discarded")


def p03_open_no_with():
    for p in iter_files("py"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"(?<!with )\bopen\s*\(", ln) and "with " not in ln:
                if re.search(r"= *open\s*\(", ln):
                    emit("P03", p, i, lines, "LOW", "open() without a with-block")


def p04_shell_true():
    for p in iter_files("py"):
        # 本工具自身包含 "shell=True" 字面量（规则文本），不自扫
        if p.name == "audit_sweep.py":
            continue
        lines = read(p)
        for i, ln in enumerate(lines):
            if "shell=True" in ln:
                emit("P04", p, i, lines, "HIGH", "shell=True with interpolated arguments")


def p05_mutable_default():
    for p in iter_files("py"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.search(r"def\s+\w+\s*\([^)]*(=\s*(\[\]|\{\}))", ln):
                emit("P05", p, i, lines, "MED", "mutable default argument")


def p06_assert_guard():
    for p in iter_files("py"):
        lines = read(p)
        for i, ln in enumerate(lines):
            if re.match(r"\s*assert\b", ln):
                emit("P06", p, i, lines, "LOW",
                     "assert as a runtime guard disappears under python -O")


CHECKS = {
    "C01": c01_unchecked_alloc, "C02": c02_unsafe_string, "C03": c03_assign_in_if,
    "C04": c04_switch_fallthrough, "C05": c05_size_mult_overflow,
    "C06": c06_ulong_truncation, "C07": c07_ioctl_input_len, "C08": c08_free_then_use,
    "T01": t01_json_parse_unguarded, "T02": t02_effect_timer_leak,
    "T03": t03_fetch_no_catch, "T04": t04_any_cast, "T05": t05_nonnull_assert,
    "T06": t06_number_nan, "T07": t07_loose_equality, "T08": t08_storage_json,
    "S01": s01_async_void, "S02": s02_empty_catch, "S03": s03_blocking_on_task,
    "S04": s04_parse_without_tryparse, "S05": s05_datetime_now,
    "S06": s06_event_leak, "S07": s07_disposable, "S08": s08_swallow_task,
    "P01": p01_bare_except, "P02": p02_except_pass, "P03": p03_open_no_with,
    "P04": p04_shell_true, "P05": p05_mutable_default, "P06": p06_assert_guard,
}

TITLES = {
    "C01": "C: allocation result dereferenced without NULL test",
    "C02": "C: unbounded string function",
    "C03": "C: assignment inside if condition",
    "C04": "C: switch case falls through without break",
    "C05": "C: size multiplication without overflow guard",
    "C06": "C: narrowing cast of arithmetic expression",
    "C07": "C: IOCTL InputBufferLength without sizeof bound",
    "C08": "C: use after free",
    "T01": "TS: JSON.parse without try/catch",
    "T02": "TS: timer/listener without cleanup",
    "T03": "TS: fetch without error handling",
    "T04": "TS: `as any`",
    "T05": "TS: non-null assertion",
    "T06": "TS: numeric parse without NaN guard",
    "T07": "TS: loose equality",
    "T08": "TS: storage JSON parsed unguarded",
    "S01": "C#: async void outside event handler",
    "S02": "C#: empty catch",
    "S03": "C#: blocking wait on Task",
    "S04": "C#: Parse instead of TryParse",
    "S05": "C#: DateTime.Now in a comparable value",
    "S06": "C#: event subscription without unsubscribe",
    "S07": "C#: IDisposable not disposed",
    "S08": "C#: fire-and-forget Task.Run",
    "P01": "PY: bare except",
    "P02": "PY: exception silently discarded",
    "P03": "PY: open() without with",
    "P04": "PY: shell=True",
    "P05": "PY: mutable default argument",
    "P06": "PY: assert as runtime guard",
}


def main(argv=None):
    ap = argparse.ArgumentParser(description="30-class defect sweep")
    ap.add_argument("--list", action="store_true", help="list the 30 classes")
    ap.add_argument("--round", action="append", default=[],
                    help="run one class (repeatable)")
    ap.add_argument("--all", action="store_true", help="run every class")
    ap.add_argument("--json", default=None, help="write results as JSON")
    ap.add_argument("--fail-on-high", action="store_true",
                    help="exit 1 when a HIGH severity hit remains")
    ap.add_argument("--quiet", action="store_true", help="only print the summary")
    args = ap.parse_args(argv)

    if args.list or (not args.round and not args.all):
        for k in CHECKS:
            print("%-4s %s" % (k, TITLES[k]))
        return 0

    sel = list(CHECKS) if args.all else [r.upper() for r in args.round]
    for r in sel:
        if r not in CHECKS:
            print("unknown class %r (use --list)" % r, file=sys.stderr)
            return 2

    for r in sel:
        n_before = len(RESULTS)
        CHECKS[r]()
        if not args.quiet:
            print("\n=== %s  %s  (%d hit%s) ===" % (
                r, TITLES[r], len(RESULTS) - n_before,
                "" if len(RESULTS) - n_before == 1 else "s"))
            for h in RESULTS[n_before:]:
                print("  " + str(h))

    by_cls: dict[str, int] = {}
    by_sev: dict[str, int] = {}
    for h in RESULTS:
        by_cls[h.cls] = by_cls.get(h.cls, 0) + 1
        by_sev[h.sev] = by_sev.get(h.sev, 0) + 1

    print("\n" + "=" * 62)
    print("SUMMARY  classes=%d  hits=%d  %s" % (
        len(sel), len(RESULTS),
        "  ".join("%s=%d" % (k, by_sev[k]) for k in ("HIGH", "MED", "LOW") if k in by_sev)))
    for k in sel:
        print("  %-4s %3d  %s" % (k, by_cls.get(k, 0), TITLES[k]))

    if args.json:
        Path(args.json).write_text(
            json.dumps([h.as_dict() for h in RESULTS], ensure_ascii=False, indent=2),
            encoding="utf-8")
        print("json -> %s" % args.json)

    if args.fail_on_high and by_sev.get("HIGH"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
