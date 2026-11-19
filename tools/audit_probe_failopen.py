#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""audit_probe_failopen.py — 探针判据 "fail-open" 静态审计

为什么需要这个工具：
  本项目被"探针假绿"坑过不止一次。最典型的一次是 `d4_verify_cache_takeover.py`
  把 `identical=false`（写后读回内容与写入不符 = 静默数据错误）只写进 note，
  判据列仍然是 PASS —— 于是 P0 级陈旧读缺陷被一句备注掩盖了很多轮。
  纪律是：**判据 FAIL 必须真的 FAIL；异常、缺失数据、状态非 0 都不得默认通过。**
  本工具用 AST 静态扫描探针脚本，把这类"看着是判据、实际永远通过"的写法揪出来，
  从而能在 CI 里拦住下一个假绿探针。

扫描范围（默认相对仓库根）：
  tests/e2e/*.py
  tools/probe_*.py

检出模式：
  FO1  吞异常         except/except Exception 分支里只有 pass/continue/print，
                      且没有 FAIL/fail/exit(1)/sys.exit 之类判负动作
  FO2  异常即通过     try 里做断言性质的操作，except 后把结果变量赋成通过值
                      （ok = True / result = "PASS"）
  FO3  缺失即通过     函数里 `if not X:` / `if X is None:` 缺失判断分支直接
                      return True（启发式，输出附上下文供人工判断）
  FO4  降级为备注     同一条判据字符串里 PASS 与 note/skip/warn 共存；
                      或一行 row(...) 里 status=PASS 而 note 写的是 mismatch/error
                      （这正是 d4 当年的形态）
  FO5  未断状态       RPC/HTTP 调用后没有校验返回的 status 字段就直接用数据

用法：
  python -B tools/audit_probe_failopen.py
  python -B tools/audit_probe_failopen.py --json
  python -B tools/audit_probe_failopen.py --self-test
  python -B tools/audit_probe_failopen.py --ignore-cleanup   # 隐藏清理型 FO1（见下）
  python -B tools/audit_probe_failopen.py --root <仓库根> [--json <out.json>]

说明（口径）：
  * FO5 只报"根因函数"一次（真正取回 HTTP 响应体的那个 helper），不逐个调用点重复报；
    detail 里给出该文件继承此缺陷的调用点数量。
  * FO1 默认包含"try 段只是删文件/关句柄"的清理型吞异常（tag=cleanup）。这类在语义上
    不影响判据，但会持续压红 CI；需要可用门禁时加 --ignore-cleanup 把它滤掉。
  * FO3/FO4 是启发式，只做提示、不参与退出码。

退出码：
  0  无 FO1/FO2/FO5 命中（FO3/FO4 是启发式，不单独判失败）
  1  命中任一 FO1/FO2/FO5 —— 可直接接入 CI
  2  有文件无法解析（SyntaxError）/无法读取 —— 本工具自己也不 fail-open
  3  --self-test 未全部命中（工具失去检测能力）
"""
import argparse
import ast
import glob
import json
import os
import re
import sys

# --------------------------------------------------------------------------- 常量

RULE_TITLES = {
    "FO1": "吞异常（异常分支静默）",
    "FO2": "异常即通过（except 里赋通过值）",
    "FO3": "缺失即通过（缺失分支 return True）",
    "FO4": "不一致降级为备注（PASS + note/skip/warn）",
    "FO5": "未断 RPC status 就使用数据",
}
# 命中这些编号即判 CI 失败
HARD_RULES = ("FO1", "FO2", "FO5")

PASS_LITERALS = {"PASS", "PASSED", "OK", "TRUE", "SUCCESS", "GOOD", "YES"}
DEGRADE_WORDS = re.compile(r"note|skip|warn", re.I)          # FO4 字面量判据（按规格）
STRONG_DEGRADE = re.compile(                                  # FO4 row 调用变体
    r"mismatch|differ|not[\s_-]?identical|不一致|降级|error|fail|缺失", re.I)
FAIL_ACTION = re.compile(r"FAIL|fail|exit\(1\)|sys\.exit")     # FO1 排除项
STATUS_GUARD_OPS = (ast.Eq, ast.NotEq, ast.Lt, ast.Gt, ast.LtE, ast.GtE, ast.Is, ast.IsNot)

RPC_CALLEES = {"rpc", "try_rpc", "rpc_call", "call_rpc", "http_json", "http_post",
               "http_get", "post_json", "get_json", "api_call", "fetch_json"}
TRANSPORT_CALLEES = {"urlopen"}
ROWWISE_CALLEES = {"row", "add_row", "record", "emit", "report", "judge", "expect"}
CLEANUP_CALLEES = {"unlink", "remove", "rmdir", "rmtree", "close", "closehandle",
                   "kill", "terminate", "cleanup", "clean", "flush", "fsync"}

MAX_SNIPPET = 160


# --------------------------------------------------------------------------- 辅助

def callee_name(call):
    """Call 节点的被调名（取最后一段，`a.b.c()` → 'c'）。"""
    f = call.func
    if isinstance(f, ast.Name):
        return f.id
    if isinstance(f, ast.Attribute):
        return f.attr
    return ""


def snippet(lines, lineno):
    """单行代码片段，去前导空白。"""
    if 1 <= lineno <= len(lines):
        s = lines[lineno - 1].strip()
        return s[:MAX_SNIPPET]
    return ""


def is_pass_const(node):
    """`True` 或 'PASS'/'OK' 这类"通过值"常量。"""
    if isinstance(node, ast.Constant):
        v = node.value
        if v is True:
            return True
        if isinstance(v, str) and v.strip().upper() in PASS_LITERALS:
            return True
    return False


def is_silent_stmt(st):
    """吞异常判定用的"啥也没干"语句：pass/continue/break/裸字符串/print 类调用。"""
    if isinstance(st, (ast.Pass, ast.Continue, ast.Break)):
        return True
    if isinstance(st, ast.Expr):
        v = st.value
        if isinstance(v, ast.Constant) and isinstance(v.value, str):
            return True
        if isinstance(v, ast.Call):
            n = callee_name(v).lower()
            return n in {"print", "log", "debug", "info", "warning", "warn", "write", "echo"}
    return False


def walk_no_nested(node):
    """遍历但不下钻到嵌套函数/类/lambda（避免把内层 return 算到外层头上）。"""
    stack = [node]
    while stack:
        cur = stack.pop()
        yield cur
        for ch in ast.iter_child_nodes(cur):
            if isinstance(ch, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Lambda)):
                continue
            stack.append(ch)


def try_nodes(tree):
    kinds = [ast.Try]
    if hasattr(ast, "TryStar"):
        kinds.append(ast.TryStar)
    return [n for n in ast.walk(tree) if isinstance(n, tuple(kinds))]


def functions(tree):
    return [n for n in ast.walk(tree)
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]


def seg(src, node):
    try:
        return ast.get_source_segment(src, node) or ""
    except Exception:
        return ""


# --------------------------------------------------------------------------- 检测

def _status_var_names(fn, src):
    """函数内持有 status 的变量名：`st = d.get("status")` → {'st'}。"""
    names = set()
    for n in ast.walk(fn):
        if isinstance(n, ast.Assign) and "status" in seg(src, n).lower():
            for t in n.targets:
                for x in ast.walk(t):
                    if isinstance(x, ast.Name):
                        names.add(x.id)
    return names


def _is_transport_call(node):
    """直接跟 HTTP 打交道的调用：urlopen / requests.* / http.client。"""
    if not isinstance(node, ast.Call):
        return False
    return callee_name(node).lower() in TRANSPORT_CALLEES


def _count_callsites(tree, name):
    n = 0
    for x in ast.walk(tree):
        if isinstance(x, ast.Call) and callee_name(x) == name:
            n += 1
    return n


def _data_access_style(tree, name):
    """调用点怎么取 body：'get'=静默退化，'subscript'=多半 KeyError，''=看不出来。"""
    sub = get = False
    for n in ast.walk(tree):
        if isinstance(n, ast.Subscript) and isinstance(n.value, ast.Call) \
                and callee_name(n.value) == name:
            sl = n.slice
            if isinstance(sl, ast.Constant) and sl.value == "data":
                sub = True
        if isinstance(n, ast.Call) and callee_name(n) == "get" \
                and isinstance(n.func, ast.Attribute) \
                and isinstance(n.func.value, ast.Call) \
                and callee_name(n.func.value) == name \
                and n.args and isinstance(n.args[0], ast.Constant) \
                and n.args[0].value == "data":
            get = True
    return "get" if get else ("subscript" if sub else "")


def func_checks_status(fn, src):
    """该函数是否校验了 status 字段（`st != 0` / `d.get("status") == 0` 都算）。"""
    svars = _status_var_names(fn, src)
    for n in ast.walk(fn):
        if isinstance(n, ast.Compare) and isinstance(n.ops[0], STATUS_GUARD_OPS):
            s = seg(src, n)
            low = s.lower()
            if "status" in low:
                return True
            if any(re.search(r"\b%s\b" % re.escape(v), low) for v in svars):
                return True
    return False


def audit_tree(tree, src, relpath):
    """对单棵 AST 跑全部规则，返回 findings 列表。"""
    lines = src.splitlines()
    out = []
    seen = set()

    def add(rule, line, code, detail, variant=""):
        key = (rule, line)
        if key in seen:
            return
        seen.add(key)
        out.append({"rule": rule, "file": relpath, "line": line,
                    "code": code or snippet(lines, line), "detail": detail,
                    "variant": variant})

    local_funcs = {}
    for f in functions(tree):
        local_funcs[f.name] = f

    # ---- FO1: except 分支静默吞掉 -------------------------------------------
    for t in try_nodes(tree):
        try_src = " ".join(seg(src, s) for s in t.body).lower()
        cleanup = any(w in try_src for w in
                      ("unlink", "rmtree", "rmdir", "close", "remove", "cleanup"))
        for h in t.handlers:
            if any(isinstance(s, ast.Raise) for s in h.body):
                continue                                  # 重抛，不算吞
            if not h.body or not all(is_silent_stmt(s) for s in h.body):
                continue
            hsrc = " ".join(seg(src, s) for s in h.body)
            if FAIL_ACTION.search(hsrc):
                continue                                  # 有判负动作
            etype = "bare except" if h.type is None else seg(src, h.type)
            variant = "cleanup" if cleanup else "normal"
            detail = "%s 分支只做了 [%s]，没有 FAIL/exit；try 段内容%s" % (
                etype, hsrc.strip().replace("\n", " ")[:60],
                "疑似清理代码" if cleanup else "非清理代码")
            add("FO1", h.lineno, snippet(lines, h.lineno),
                detail + " → 该异常路径判据默认通过", variant)

    # ---- FO2: except 里把结果赋成通过值 -------------------------------------
    for t in try_nodes(tree):
        try_body_names = set()
        assertion_evidence = False
        for s in t.body:
            for n in ast.walk(s):
                if isinstance(n, ast.Assign):
                    for tg in n.targets:
                        for x in ast.walk(tg):
                            if isinstance(x, ast.Name):
                                try_body_names.add(x.id)
                if isinstance(n, (ast.Assert, ast.Compare)):
                    assertion_evidence = True
                elif isinstance(n, ast.Call) and re.search(
                        r"assert|check|verify|validate|probe|expect|should",
                        callee_name(n), re.I):
                    assertion_evidence = True
        for h in t.handlers:
            for s in h.body:
                for n in ast.walk(s):
                    targets, value = [], None
                    if isinstance(n, ast.Assign):
                        targets, value = n.targets, n.value
                    elif isinstance(n, ast.AnnAssign) and n.value is not None:
                        targets, value = [n.target], n.value
                    if value is None or not is_pass_const(value):
                        continue
                    names = set()
                    for tg in targets:
                        for x in ast.walk(tg):
                            if isinstance(x, ast.Name):
                                names.add(x.id)
                    if not (names & try_body_names or assertion_evidence):
                        continue
                    add("FO2", n.lineno, snippet(lines, n.lineno),
                        "except 分支把 %s 赋成通过值 %r；try 段%s → 异常被当成通过" % (
                            "/".join(sorted(names)) or "?",
                            seg(src, value),
                            "有断言/比较" if assertion_evidence else "给同一变量赋过值"),
                        "assign")

    # ---- FO3: 缺失判断分支 return True --------------------------------------
    for fn in functions(tree):
        for n in ast.walk(fn):
            if not isinstance(n, ast.If):
                continue
            test = n.test
            missing = False
            if isinstance(test, ast.UnaryOp) and isinstance(test.op, ast.Not):
                missing = True
            elif isinstance(test, ast.Compare):
                if any(isinstance(op, (ast.Is, ast.IsNot)) for op in test.ops) and \
                        any(isinstance(c, ast.Constant) and c.value is None
                            for c in test.comparators):
                    missing = True
            if not missing:
                continue
            ret = None
            for st in n.body:
                for x in walk_no_nested(st):
                    if isinstance(x, ast.Return) and is_pass_const(x.value):
                        ret = x
                        break
                if ret is not None:
                    break
            if ret is None:
                continue
            add("FO3", n.lineno, snippet(lines, n.lineno),
                "%s() 中 `%s` 命中后第 %d 行直接 return %r → 数据缺失/为空被判通过；"
                "需人工确认这是「空集为真」的合法语义还是 fail-open" % (
                    fn.name, seg(src, test).strip(), ret.lineno, seg(src, ret.value)),
                "guard-return-True")

    # ---- FO4: PASS 与降级措辞共存 -------------------------------------------
    for n in ast.walk(tree):
        if isinstance(n, ast.Constant) and isinstance(n.value, str) and len(n.value) < 300:
            s = n.value
            if "pass" in s.lower() and DEGRADE_WORDS.search(s):
                add("FO4", n.lineno, snippet(lines, n.lineno),
                    "同一判据字符串内 PASS 与 note/skip/warn 共存：%r" % s[:90], "literal")
        elif isinstance(n, ast.Call) and callee_name(n).lower() in ROWWISE_CALLEES:
            args = list(n.args) + [k.value for k in n.keywords]
            has_pass = any(is_pass_const(a) for a in args)
            bad_note = [a.value for a in args
                        if isinstance(a, ast.Constant) and isinstance(a.value, str)
                        and STRONG_DEGRADE.search(a.value)]
            if has_pass and bad_note:
                add("FO4", n.lineno, snippet(lines, n.lineno),
                    "%s(...) 里状态为 PASS，而备注写的是「%s」→ 失败迹象被降级成备注" % (
                        callee_name(n), bad_note[0][:80]), "row-call")

    # ---- FO5: 未校验 status 就使用数据 --------------------------------------
    # 只报"根因"：真正取回响应体的那个 helper（每个函数报一次），
    # 不再对每个调用点重复报同一件事（否则一处 helper 会放大成几十条噪音）。
    transport_funcs = {}
    for name, fn in local_funcs.items():
        if any(_is_transport_call(n) for n in ast.walk(fn)):
            transport_funcs[name] = fn
    for name, fn in sorted(transport_funcs.items()):
        if func_checks_status(fn, src):
            continue
        line = None
        for n in ast.walk(fn):
            if _is_transport_call(n):
                line = n.lineno
                break
        sites = _count_callsites(tree, name)
        fs = seg(src, fn)
        style = _data_access_style(tree, name)
        if not style and re.search(r"\[\s*[\"']data[\"']\s*\]", fs):
            style = "subscript"     # helper 自己就地按 ["data"] 取 body
        silent = style == "get" or bool(
            re.search(r"\.get\(\s*[\"']data[\"']\s*\)", fs)) or "or {}" in fs
        if "/rb/rpc" not in fs:
            hint = ""          # 通用传输层 / 非 rpc 端点，不作额外断言
        elif silent:
            hint = ("；静默形态：status!=0 时取不到 data 会退化成 {} / None，"
                    "判据据此仍可判通过")
        elif style == "subscript":
            hint = ("；调用点按 [\"data\"] 取用，status!=0 时多半 KeyError 崩溃"
                    "（比静默通过好，但仍未断状态）")
        else:
            hint = ("；调用点直接用整个报文，status!=0 时具体拿到什么需人工确认"
                    "（index.ts 的错误响应形状未核实）")

        add("FO5", line or fn.lineno, snippet(lines, line or fn.lineno),
            "%s() 取回 HTTP 响应后未校验 status 就直接返回/使用数据"
            "（函数内无任何 status 比较；本文件 %d 处调用点继承此 fail-open）%s"
            % (name, sites, hint),
            "helper-no-status-check")

    # 响应体在调用点就地消费（没有本地 helper 兜着）的情况
    for fn in functions(tree):
        if fn.name in transport_funcs or func_checks_status(fn, src):
            continue
        for st in walk_no_nested(fn):
            if not isinstance(st, ast.stmt):
                continue
            callees = [callee_name(n) for n in ast.walk(st)
                       if isinstance(n, ast.Call) and callee_name(n).lower() in RPC_CALLEES]
            callees = [c for c in callees if c not in local_funcs]
            if not callees:
                continue
            add("FO5", st.lineno, snippet(lines, st.lineno),
                "%s() 里调用外部 %s() 取回响应后未校验 status 就直接使用数据"
                % (fn.name, "/".join(sorted(set(callees)))),
                "inline-no-status-check")

    out.sort(key=lambda f: (f["rule"], f["file"], f["line"]))
    return out


# ------------------------------------------------------------------- 自助测试

SELF_TEST_CASES = [
    ("FO1", """\
def probe():
    try:
        do_check()
    except Exception:
        pass
"""),
    ("FO2", """\
def probe():
    ok = False
    try:
        ok = check_identity(a, b)
    except Exception:
        ok = True
    return ok
"""),
    ("FO3", """\
def probe(v):
    if v is None:
        return True
    return v == 1
"""),
    ("FO4", """\
def probe(mismatch):
    return "PASS - note: readback mismatch downgraded"
"""),
    ("FO5", """\
import json
import urllib.request


def rpc(m):
    req = urllib.request.Request("http://127.0.0.1:5199/rb/rpc", data=b"{}")
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode()).get("data")
"""),
]


def self_test(out=sys.stdout):
    """5 段最小代码分别命中 FO1–FO5；任一未命中也返回失败。"""
    print("== self-test: 5 段最小代码应分别命中 FO1–FO5 ==", file=out)
    ok_all = True
    for i, (want, code) in enumerate(SELF_TEST_CASES, 1):
        try:
            tree = ast.parse(code)
        except SyntaxError as e:
            print("[FAIL] case %d (%s) 自带代码无法解析: %s" % (i, want, e), file=out)
            ok_all = False
            continue
        found = audit_tree(tree, code, "<selftest-%d>" % i)
        got = {f["rule"] for f in found}
        hit = want in got
        print("[%s] case %d 期望 %s → 命中 %s%s" % (
            "PASS" if hit else "FAIL", i, want,
            ",".join(sorted(got)) or "(无)",
            "" if hit else "   <-- 该规则失去检测能力"), file=out)
        if not hit:
            ok_all = False
        else:
            for f in found:
                if f["rule"] == want:
                    print("        %s  %s" % (f["line"], f["code"]), file=out)
                    break
    # 反向验证：一段干净的代码不应产生任何命中
    clean = """\
def probe(a, b):
    if a != b:
        print("FAIL: mismatch")
        return False
    return True
"""
    found = audit_tree(ast.parse(clean), clean, "<selftest-clean>")
    if found:
        ok_all = False
        print("[FAIL] 干净样例被误报 %d 条: %s" % (
            len(found), ", ".join("%s@%d" % (f["rule"], f["line"]) for f in found)),
            file=out)
    else:
        print("[PASS] 干净样例 0 误报", file=out)
    print("self-test: %s (5/5 %s)" % ("OK" if ok_all else "FAILED",
                                     "命中" if ok_all else "未全命中"), file=out)
    return ok_all


# ------------------------------------------------------------------- 主流程

def collect_files(root):
    pats = [os.path.join(root, "tests", "e2e", "*.py"),
            os.path.join(root, "tools", "probe_*.py")]
    files = []
    for p in pats:
        files.extend(sorted(glob.glob(p)))
    # 去重 + 稳定排序
    return sorted(set(os.path.abspath(f) for f in files))


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="探针判据 fail-open 静态审计（AST，标准库）")
    ap.add_argument("paths", nargs="*", help="额外要扫描的文件（默认扫 tests/e2e + tools/probe_*）")
    ap.add_argument("--root", default=None,
                    help="仓库根（默认为本脚本所在目录的上一级）")
    ap.add_argument("--json", nargs="?", const="-", default=None, metavar="PATH",
                    help="输出 JSON；不给路径或给 '-' 时打到 stdout")
    ap.add_argument("--self-test", action="store_true",
                    help="内建 5 段最小代码，验证每条规则都有牙")
    ap.add_argument("--ignore-cleanup", action="store_true",
                    help="不报 FO1 中 try 段只是删文件/关句柄的清理型吞异常"
                         "（默认仍报，按规格口径）")
    args = ap.parse_args(argv)

    if args.self_test:
        return 0 if self_test() else 3

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = collect_files(root)
    for p in args.paths:
        files.append(os.path.abspath(p))
    files = sorted(set(files))

    findings, errors = [], []
    scanned = []
    for path in files:
        rel = os.path.relpath(path, root).replace(os.sep, "/")
        try:
            with open(path, "r", encoding="utf-8") as f:
                src = f.read()
        except OSError as e:
            errors.append({"file": rel, "error": "%s: %s" % (type(e).__name__, e)})
            continue
        except UnicodeDecodeError as e:
            errors.append({"file": rel, "error": "UnicodeDecodeError: %s" % e})
            continue
        try:
            tree = ast.parse(src, filename=path)
        except SyntaxError as e:
            # 工具自己也不许 fail-open：解析不了就报错，绝不静默跳过
            errors.append({"file": rel, "line": e.lineno,
                           "error": "SyntaxError: %s" % e.msg})
            continue
        scanned.append(rel)
        findings.extend(audit_tree(tree, src, rel))

    counts = {r: 0 for r in RULE_TITLES}
    suppressed = 0
    if args.ignore_cleanup:
        kept = []
        for f in findings:
            if f["rule"] == "FO1" and f.get("variant") == "cleanup":
                suppressed += 1
                continue
            kept.append(f)
        findings = kept
    for f in findings:
        counts[f["rule"]] = counts.get(f["rule"], 0) + 1
    hard = [f for f in findings if f["rule"] in HARD_RULES]
    if errors:
        code = 2
    elif hard:
        code = 1
    else:
        code = 0

    if args.json:
        payload = {"root": root, "scanned": scanned, "file_count": len(scanned),
                   "findings": findings, "counts": counts,
                   "rules": RULE_TITLES, "hard_rules": list(HARD_RULES),
                   "errors": errors, "exit_code": code}
        text = json.dumps(payload, ensure_ascii=False, indent=2)
        if args.json == "-":
            print(text)
        else:
            with open(args.json, "w", encoding="utf-8") as f:
                f.write(text + "\n")
            print("INFO json=%s" % args.json)
        return code

    # ---- 人类可读摘要
    print("== 探针判据 fail-open 静态审计 ==")
    print("root          : %s" % root)
    print("扫描文件      : %d" % len(scanned))
    for rel in scanned:
        print("                %s" % rel)
    print("发现总数      : %d" % len(findings))
    print("按编号分布    : " + "  ".join(
        "%s=%d" % (r, counts[r]) for r in sorted(counts)))
    for r in sorted(RULE_TITLES):
        print("  %s %-34s %d%s" % (r, RULE_TITLES[r], counts[r],
                                   "  [判 CI 失败]" if r in HARD_RULES else "  [启发式]"))
    print("")
    for r in sorted(RULE_TITLES):
        group = [f for f in findings if f["rule"] == r]
        if not group:
            print("-- %s %s : 无命中" % (r, RULE_TITLES[r]))
            continue
        print("-- %s %s : %d 条" % (r, RULE_TITLES[r], len(group)))
        for f in group:
            print("   %s:%d  %s" % (f["file"], f["line"], f["code"]))
            print("       %s%s" % (f["detail"],
                                   "  [tag=%s]" % f["variant"] if f["variant"] else ""))
    if errors:
        print("")
        print("== 解析/读取失败（exit 2，不静默跳过）==")
        for e in errors:
            print("   %s:%s  %s" % (e["file"], e.get("line", "?"), e["error"]))
    if suppressed:
        print("（--ignore-cleanup：已隐藏 %d 条清理型 FO1）" % suppressed)
    print("")
    print("EXIT %d  (硬失败规则 %s 命中 %d 条；FO3/FO4 仅供人工复核)"
          % (code, "/".join(HARD_RULES), len(hard)))
    return code


if __name__ == "__main__":
    sys.exit(main())
