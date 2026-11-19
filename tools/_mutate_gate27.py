#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate27 变异测试：证明判据有牙，且证明「抓到的是该抓的那条」。

为什么不写成 .ps1 字符串替换
============================
第一版（pwsh 版）跑出 kill=7 blind=1 skip=2，看着挺好，逐条核对 FAIL 文案后发现
**至少两条是假阳性**：

  · M2（discard 移到 ddx 之前）—— 插入点正好落在注释行里，等于把 discard 删了。
    门禁抓到的是「找不到 coverage 的 discard」，不是「顺序颠倒」。
  · M10（排空移到 destroy 之后）—— 整块被删而不是被搬。
    门禁抓到的是「没有 PersistInFlight 排空」，不是「顺序反了」。
  · M3（删 HDR_SCRGB）—— 只删了 132 行那处，226 行还有一处，
    G3b 只因「还在」而通过另一条分支；这不叫「判据有牙」。

「变异后 FAIL」不等于「该判据有牙」—— 中间隔着「FAIL 是不是这条判据给的」。
所以本脚本对每个变异声明 expect=该判据 FAIL 文案的**必须片段**，
跑完逐条比对：FAIL 里必须出现 expect，否则记 FAKED（假阳性），与 BLIND 同样算失败。

纪律
----
1. 变异只改副本，改完必定还原（finally），字节级还原保留 BOM/CRLF
2. 每个变异必须让门禁 exit != 0，**且** FAIL 文案必须命中 expect
3. 还原后必须复跑回 PASS，否则判 BROKEN（是脚本坏了，不是门禁坏了）
4. 锚点未命中 ⇒ SKIP，不计 KILL；KILL=0 不得判 PASS

用法：python tools/_mutate_gate27.py
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / "src" / "scripts" / "check_liquid_glass_dcomp_gate.py"

HLSL = ROOT / "src" / "ui" / "native-host" / "shaders" / "liquid_glass_core.hlsl"
APCA = ROOT / "src" / "ui" / "native-host" / "shaders" / "apca_reduce.hlsl"
CPP = ROOT / "src" / "ui" / "native-host" / "src" / "glass_window.cpp"
L2C = ROOT / "src" / "driver" / "rbL2.c"
DRV = ROOT / "src" / "driver" / "rbdriver.c"


def run_gate() -> tuple[int, str]:
    p = subprocess.run([sys.executable, str(GATE)], capture_output=True, text=True, encoding="utf-8", errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def decode(b: bytes) -> tuple[str, bool]:
    """返回 (文本, 是否有 BOM)。读的时候按 BOM 判定，写回原样。"""
    if b[:3] == b"\xef\xbb\xbf":
        return b[3:].decode("utf-8", errors="replace"), True
    return b.decode("utf-8", errors="replace"), False


def encode(txt: str, bom: bool) -> bytes:
    raw = txt.encode("utf-8")
    return (b"\xef\xbb\xbf" + raw) if bom else raw


# ---------------------------------------------------------------------------
# 变异清单。每个变异 = (编号, 描述, 目标文件, 变异函数, expect)
#   expect：门禁 FAIL 文案里**必须**出现的片段。空串表示只要求 exit != 0。
# ---------------------------------------------------------------------------
def m1_smin(txt: str) -> str | None:
    """G1：把 1/6 改成 1/7。只改一处（全文件仅一处）。"""
    if txt.count("1.0 / 6.0") != 1:
        return None
    return txt.replace("1.0 / 6.0", "1.0 / 7.0")


def m2_discard_before_ddx(txt: str) -> str | None:
    """G2：把 coverage discard **整句搬**到 ddx_fine 之前。

    第一版把插入点放在 `ddx_fine` 的**子串位置**上，而那位置在注释里
    （`// [RT-U1] 焦散必须在 discard **之前**算完：ddx_fine/ddy_fine 依赖…`），
    于是 discard 被插进注释 = discard 消失，门禁抓到的是「找不到」而非「顺序」。
    改为：定位真正含 ddx_fine 的**语句行**（不含 // 前缀），插到它所在行之前。
    """
    m = re.search(r"^[^\n/]*\bddx_fine\s*\(", txt, re.M)
    if not m:
        return None
    stmt = "    if (coverage <= 1e-4) discard;   /* MUTANT: 提前到 ddx 之前 */\n    "
    txt2 = txt[: m.start()] + stmt + txt[m.start() :]
    # 去掉原来那行（第一次出现、且是真正执行的那条）
    orig = "    if (coverage <= 1e-4) discard;\n"
    if orig not in txt2:
        return None
    # 删除的是「后一次」出现，即原位置
    i = txt2.find(orig)          # 这是刚插进去的那条（带注释），不会被匹配
    i = txt2.find(orig, i)
    if i < 0:
        return None
    return txt2[:i] + txt2[i + len(orig) :]


def m3_hdr_all(txt: str) -> str | None:
    """G3b：把**所有** `#if HDR_SCRGB` 分支删掉（第一版只删一处，226 行还有）。

    注意：删掉的必须是宏名本身，让 G3b 的 `"HDR_SCRGB" not in txt` 命中。
    全部改成 `#if 0` 仍保留宏名 ⇒ 不会触发 G3b。故这里连同注释里的出现也一并抹掉。
    """
    if "#if HDR_SCRGB" not in txt:
        return None
    txt2 = txt.replace("#if HDR_SCRGB", "#if 0")
    # 注释与宏名一并抹掉，模拟「RT-U3 防线整个失守」
    txt2 = txt2.replace("HDR_SCRGB", "HDR_X")
    return txt2


def m4_two_d_groupshared(txt: str) -> str | None:
    """G4：一维 groupshared 改成二维 [M][N]。"""
    m = re.search(r"(groupshared\s+float4\s+)(s_TileH)\s*\[\s*([^\]]+?)\s*\]", txt)
    if not m:
        return None
    repl = f"{m.group(1)}{m.group(2)}[BLUR_TILE_W][{m.group(3)}]"
    return txt[: m.start()] + repl + txt[m.end() :]


def m5_tileh_shrink(txt: str) -> str | None:
    """G5：把 TILEH_SIZE 缩到 BLUR_THREADS（无 halo），必然越界。

    第一版把 TILEH_SIZE 改成 `(BLUR_THREADS + 2*MAX_RADIUS)` —— **那正是源码原值**，
    变异等于没变异（所以第一版记成 BLIND，其实是无效变异，不是判据空跑）。
    真正能越界的改法是把 halo 拿掉，索引上界 263 > 容量 256。
    """
    m = re.search(r"^#define\s+TILEH_SIZE\s+.*$", txt, re.M)
    if not m:
        return None
    return txt[: m.start()] + "#define TILEH_SIZE   (BLUR_THREADS)" + txt[m.end() :]


def m6_latency(txt: str) -> str | None:
    if txt.count("SetMaximumFrameLatency(1)") != 1:
        return None
    return txt.replace("SetMaximumFrameLatency(1)", "SetMaximumFrameLatency(3)")


def m7_rdp(txt: str) -> str | None:
    if txt.count("SM_REMOTESESSION") < 1:
        return None
    return txt.replace("SM_REMOTESESSION", "SM_CMONITORS")


def m8_backdrop(txt: str) -> str | None:
    if "const int backdropNone = 1;" not in txt:
        return None
    return txt.replace("const int backdropNone = 1;", "const int backdropNone = 3;")


def m9_l2idx_in_periodic(txt: str) -> str | None:
    """G9：在 RbL2Periodic 函数体开头插一条 RbL2IdxSave。

    第一版正则 `RbL2Periodic\\s*\\([^)]*\\)\\s*\\{` 要求右括号后**紧跟** `{`，
    而真实源码是 `VOID RbL2Periodic(VOID)                            /* Rb2Tick 每 60s */`
    —— 中间隔着尾注释，故锚点未命中（SKIP）。
    第二版改成 `[^\\n{]*\\{` 仍不命中：因为注释与 `{` 各占一行，`[^\\n]` 跨不过换行。
    这里显式允许「右括号 到 { 之间」出现任意字符（含换行与注释）。
    """
    m = re.search(r"VOID\s+RbL2Periodic\s*\(\s*VOID\s*\)(?:[^{;]|\n)*\{", txt)
    if not m:
        return None
    ins = "\n    RbL2IdxSaveGuarded(); /* MUTANT: 违反 [172/A.2] */\n"
    return txt[: m.end()] + ins + txt[m.end() :]


def m10_drain_after_destroy(txt: str) -> str | None:
    """G10：把 F10 排空块**整段搬到** rb_engine_destroy 之后（顺序颠倒）。

    第一版用 `IndexOf("\\r\\n    }\\r\\n")` 找闭合，把块截得太短、结果连排空一起丢了。
    第二版用 `rfind("{")` 往前找外层块，却抓到的是**上一条** `if (g_Rb.ControlDevice) {`
    的花括号（F10 块自己的 `{` 在注释之后更近，rfind 选错了），于是搬走的是控制设备块，
    门禁报的是「注销在排空之后」—— FAKED（红得对、红错地方）。

    正确做法：从 F10 注释标记往后找**第一条** `{`（那就是排空块自己的），
    再花括号配平到闭合。
    """
    start_mark = "/* [RT-K4 / F10 2026-11-19]"
    s = txt.find(start_mark)
    if s < 0:
        return None
    ob = txt.find("{", s)              # 往后找：这是 F10 块自己的外层 `{`
    if ob < 0:
        return None
    depth = 0
    i = ob
    while i < len(txt):
        if txt[i] == "{":
            depth += 1
        elif txt[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    if depth != 0:
        return None
    end = i + 1
    block = txt[ob:end]
    # 块里必须真有排空判据，否则搬了个空壳（变异无效）
    if "PersistInFlight" not in block:
        return None

    target = "if (g_Rb.Engine) { rb_engine_destroy(g_Rb.Engine); g_Rb.Engine = NULL; }"
    tp = txt.find(target)
    if tp < 0 or tp < end:
        return None
    txt2 = txt[:ob] + txt[end:]
    tp2 = txt2.find(target)
    tle2 = txt2.find("\n", tp2)
    return txt2[: tle2 + 1] + "\n" + block + "\n" + txt2[tle2 + 1 :]


MUTATIONS = [
    ("M1", "G1: SminCubic 1/6 -> 1/7（液滴融合折痕）", HLSL, m1_smin, "G1:"),
    ("M2", "G2: coverage discard 移到 ddx_fine 之前（圆角雪花）", HLSL, m2_discard_before_ddx, "G2: coverage discard"),
    ("M3", "G3b: HDR_SCRGB 防线整体失守（SDR 二次 Gamma）", HLSL, m3_hdr_all, "G3b: liquid_glass_core"),
    ("M4", "G4: 二维 groupshared 回潮 [M][N]", HLSL, m4_two_d_groupshared, "G4: 存在二维 groupshared"),
    ("M5", "G5: TILEH_SIZE 去掉 halo（越界写 LDS）", HLSL, m5_tileh_shrink, "G5: s_TileH"),
    ("M6", "G6: SetMaximumFrameLatency(1)->(3)（拖动滞后）", CPP, m6_latency, "G6: glass_window.cpp 缺 SetMaximumFrameLatency"),
    ("M7", "G7: 删 SM_REMOTESESSION（RDP 无限递归）", CPP, m7_rdp, "G7: glass_window.cpp 缺 SM_REMOTESESSION"),
    ("M8", "G8: backdropNone 1 -> 3（Acrylic 浊度地板）", CPP, m8_backdrop, "G8: 找不到"),
    ("M9", "G9: RbL2Periodic 体内插 RbL2IdxSave（索引撕裂）", L2C, m9_l2idx_in_periodic, "G9: RbL2Periodic 体内出现"),
    ("M10", "G10: 排空搬到 rb_engine_destroy 之后（UAF）", DRV, m10_drain_after_destroy, "排空在 rb_engine_destroy"),
]


def main() -> int:
    code, out = run_gate()
    if code != 0:
        print("[ABORT] 基线不是 PASS，变异测试无意义")
        print(out)
        return 2
    print("[BASE] PASS (exit=0)\n")

    kills = blinds = skips = faked = 0
    detail: list[str] = []

    for name, desc, path, fn, expect in MUTATIONS:
        orig = path.read_bytes()          # 字节级备份
        try:
            txt, has_bom = decode(orig)
            new = fn(txt)
            if new is None or new == txt:
                print(f"[SKIP] {name}: 锚点未命中或变异无效（{desc}）")
                skips += 1
                detail.append(f"{name}=SKIP")
                continue
            path.write_bytes(encode(new, has_bom))

            code2, out2 = run_gate()
            fails = [ln for ln in out2.splitlines() if ln.startswith("[FAIL]")]
            if code2 == 0:
                print(f"[BLIND] {name}: {desc} —— 变异后仍 PASS，判据空跑")
                blinds += 1
                detail.append(f"{name}=BLIND")
            elif expect and not any(expect in ln for ln in fails):
                # 门禁确实红了，但不是这条判据红的 —— 假阳性，同样不算有牙
                print(f"[FAKED] {name}: {desc}")
                print(f"        expect '{expect}' 未命中，实际 FAIL 为：")
                for ln in fails:
                    print(f"          {ln}")
                faked += 1
                detail.append(f"{name}=FAKED")
            else:
                print(f"[KILL] {name}: {desc}")
                print(f"       -> {next(ln for ln in fails if expect in ln)}")
                kills += 1
                detail.append(f"{name}=KILL")
        finally:
            path.write_bytes(orig)        # 无条件还原

    code3, out3 = run_gate()
    print()
    if code3 != 0:
        print("[BROKEN] 还原后门禁不是 PASS —— 变异脚本没还原干净")
        print(out3)
        return 3
    print("[RESTORE] PASS (exit=0)\n")

    print(f"SUMMARY kill={kills} blind={blinds} faked={faked} skip={skips} total={len(MUTATIONS)}")
    print("DETAIL  " + " ".join(detail))

    if blinds or faked:
        return 1                        # 有判据空跑或假阳性 ⇒ 不放过
    if kills == 0:
        return 4                        # 一个都没杀掉 ⇒ 判据完全没牙
    if skips:
        return 5                        # 锚点漂移，需先修脚本
    return 0


if __name__ == "__main__":
    sys.exit(main())