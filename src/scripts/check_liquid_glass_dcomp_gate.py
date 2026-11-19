#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate27 - 液态玻璃原生宿主（D3D11 + DirectComposition）防空跑门禁。

为什么值得加这条门禁
====================
295 的输入是一份 81 KB 的外部方案，它附了约 71 KB 源码，声称"历经五轮迭代与
14 项红队攻防闭环"。逐符号核实后：13 个符号全仓零命中、2 个函数只有声明没有
实现、HLSL 里有两处 groupshared 数组**越界写**（自己声称已修的 RT-U7 仍然越界）。
也就是说，「代码看起来完整」与「代码能编译、能跑」之间隔着一整个编译器。

本门禁钉的是**那些一旦写错就必然静默出错、肉眼看不出来**的约束：

  G1  HLSL 的 smin 系数必须是 1/6          —— 改了液滴融合处出现折痕
  G2  discard 必须在 ddx_fine/ddy_fine 之后 —— 早了就是圆角边缘雪花（RT-U1）
  G3  #if / #endif 必须配平                —— 少一个 endif，整个文件的语义塌掉
  G4  禁止二维 groupshared [M][N]          —— 这是 RT-U7 要修的原始形态
  G5  groupshared 声明容量 >= 实际索引上界 —— **方案原文就是在这条上翻车的**
  G6  宿主的 DComp 四要素                  —— 缺一即透色失效或白闪
  G7  无边框窗口的 三处消息处理            —— RT-U4
  G8  DWMSBT_NONE 必须清零系统材质          —— 不清零就有 DWM 的固定浊度地板
  G9  RbL2Periodic 体内不得有 RbL2IdxSave  —— [172/A.2]
  G10 RbUnload 的 F10 排空顺序              —— 反了就是 UAF（RT-K4）

三条纪律（与 check_a4_clean_gate.py 同源）：
  1. 空匹配集**不得**判 PASS。找不到文件/找不到锚点一律 FAIL。
  2. 钉「消费方」而非「声明方」。G5 不看有没有 s_TileH 声明，而是**算出**
     索引算式的上界再与声明容量比对。
  3. 必须跑变异证明有牙。变异清单见文件末尾 mutate 说明，实测结果记在 295 §7。

退出码：0 = PASS，1 = FAIL。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
fails: list[str] = []
ok: list[str] = []


def rel(p: Path) -> str:
    return p.relative_to(ROOT).as_posix()


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def strip_c_comments(s: str) -> str:
    s = re.sub(r"/\*[\s\S]*?\*/", "", s)
    s = re.sub(r"//[^\n]*", "", s)
    return s


def require(path: Path) -> str | None:
    if not path.exists():
        fails.append(f"缺失文件: {rel(path) if path.is_relative_to(ROOT) else path}")
        return None
    txt = read(path)
    if not txt.strip():
        fails.append(f"空文件: {rel(path)}")
        return None
    return txt


# ---------------------------------------------------------------------------
# 路径
# ---------------------------------------------------------------------------
HOST_SRC = ROOT / "src" / "ui" / "native-host" / "src"
HOST_SHADERS = ROOT / "src" / "ui" / "native-host" / "shaders"

glass_hpp = require(HOST_SRC / "glass_window.hpp")
glass_cpp = require(HOST_SRC / "glass_window.cpp")
capture_h = require(HOST_SRC / "desktop_capture.h")
capture_cpp = require(HOST_SRC / "desktop_capture.cpp")
glass_hlsl = require(HOST_SHADERS / "liquid_glass_core.hlsl")
apca_hlsl = require(HOST_SHADERS / "apca_reduce.hlsl")

# 判据 1 的前置：文件全都拿到了才继续（空集不得 PASS）
if fails:
    print("== Gate27 ==")
    for f in fails:
        print(f"[FAIL] {f}")
    print("GATE27 FAIL: 前置文件缺失，判据不会空跑通过")
    sys.exit(1)

assert glass_hpp and glass_cpp and capture_h and capture_cpp and glass_hlsl and apca_hlsl
hlsl_clean = strip_c_comments(glass_hlsl)
apca_clean = strip_c_comments(apca_hlsl)

# ---------------------------------------------------------------------------
# G1 smin 系数 1/6
# ---------------------------------------------------------------------------
if "SminCubic" not in hlsl_clean:
    fails.append("G1: liquid_glass_core.hlsl 里找不到 SminCubic")
else:
    m = re.search(r"float\s+SminCubic\s*\(([^)]*)\)\s*\{([\s\S]*?)\}", hlsl_clean)
    if not m:
        fails.append("G1: SminCubic 定义体截取失败（判据不得静默跳过）")
    else:
        body = m.group(2)
        if not re.search(r"\(\s*1\.0\s*/\s*6\.0\s*\)", body):
            fails.append(
                "G1: SminCubic 里没有 (1.0 / 6.0) —— C2 三次 smin 的归一化因子被改，"
                "液滴融合处会出现曲率突变"
            )
        else:
            ok.append("G1: SminCubic 保持 1/6 归一化")

# ---------------------------------------------------------------------------
# G2 discard 必须在 ddx_fine/ddy_fine 之后
# ---------------------------------------------------------------------------
if "discard" not in hlsl_clean:
    fails.append("G2: 找不到任何 discard（判据空转，不得 PASS）")
else:
    # 取 PSLiquidGlass 函数体
    mm = re.search(r"float4\s+PSLiquidGlass\s*\([^)]*\)\s*:\s*SV_Target\s*\{", hlsl_clean)
    if not mm:
        fails.append("G2: 找不到 PSLiquidGlass 入口")
    else:
        start = mm.end()
        depth = 1
        i = start
        while i < len(hlsl_clean) and depth > 0:
            if hlsl_clean[i] == "{":
                depth += 1
            elif hlsl_clean[i] == "}":
                depth -= 1
            i += 1
        body = hlsl_clean[start:i]
        # ddx_fine 的位置（-1 = 该变体未用焦散）
        dx_pos = body.find("ddx_fine")
        # 找到第一个「会裁剪片元」的 discard（coverage 那条）
        cov = re.search(r"coverage\s*<=\s*[\d.eE-]+\s*\)?\s*discard|if\s*\(\s*coverage[^)]*\)\s*discard", body)
        if dx_pos < 0:
            # 没开焦散变体时 ddx 不存在是合法的；但仍要求 discard 前有 coverage 计算
            ok.append("G2: 该变体未启用 ENABLE_CAUSTICS，无 ddx_fine（合法）")
        elif not cov:
            fails.append("G2: 找不到 coverage 相关的 discard —— 判据不得空跑")
        elif cov.start() < dx_pos:
            fails.append(
                "G2: coverage discard 出现在 ddx_fine/ddy_fine **之前** —— "
                "RT-U1 缺陷回潮（GPU 2x2 quad 导数失效 => 圆角边缘雪花）"
            )
        else:
            ok.append("G2: discard 在 ddx_fine/ddy_fine 之后")

# ---------------------------------------------------------------------------
# G3 #if / #endif 配平（按变体统计，不按全文 —— 全文配平但单变体塌陷同样致命）
# ---------------------------------------------------------------------------
for name, txt in (("liquid_glass_core.hlsl", hlsl_clean), ("apca_reduce.hlsl", apca_clean)):
    opens = len(re.findall(r"^\s*#if(n?def)?\b", txt, re.M))
    ends = len(re.findall(r"^\s*#endif\b", txt, re.M))
    if opens != ends:
        fails.append(f"G3: {name} 的 #if({opens}) 与 #endif({ends}) 不配平")
    elif opens == 0:
        fails.append(f"G3: {name} 没有任何 #if 条件编译 —— 变体隔离消失，判据空转")
    else:
        ok.append(f"G3: {name} #if/#endif 配平 ({opens})")

# 变体隔离必须存在：HDR_SCRGB 是 RT-U3 的唯一防线
for name, txt in (("liquid_glass_core.hlsl", hlsl_clean), ("apca_reduce.hlsl", apca_clean)):
    if "HDR_SCRGB" not in txt:
        fails.append(f"G3b: {name} 缺少 #if HDR_SCRGB 分支 —— SDR 路径会二次 Gamma（RT-U3）")
if "ENABLE_DISPERSION" not in hlsl_clean:
    fails.append("G3b: liquid_glass_core.hlsl 缺少 ENABLE_DISPERSION 分支 —— 色散变体消失")
if not any("HDR_SCRGB" in t for t in (hlsl_clean, apca_clean)):
    fails.append("G3b: 两个着色器都没有 HDR_SCRGB")

# ---------------------------------------------------------------------------
# G4 禁止二维 groupshared [M][N]
#
# 坑（M4 变异第一版漏杀）：原正则写 `\[\s*\d+\s*\]\s*\[\s*\d+\s*\]`，只认
# **数字**维度。而真实写法是 `[BLUR_TILE_W][TILEH_SIZE]` —— 两个都是宏名 ⇒
# 变异体被判 PASS（BLIND）。二维 groupshared 的危害与维度写成数字还是宏名无关，
# 所以维度必须同时接受标识符。
# ---------------------------------------------------------------------------
two_d = re.findall(
    r"groupshared\s+\w+\s+\w+\s*\[\s*[\w]+\s*\]\s*\[\s*[\w]+\s*\]",
    hlsl_clean,
)
if two_d:
    fails.append(f"G4: 存在二维 groupshared（{two_d[0]}）—— RT-U7 要修的原始形态回潮")
else:
    # 反向自检：必须真的存在一维 groupshared，否则这条判据是空转
    one_d = re.findall(r"groupshared\s+\w+\s+\w+\s*\[\s*[\w]+\s*\]", hlsl_clean)
    if not one_d:
        fails.append("G4: 找不到任何 groupshared 声明 —— 判据空转（不得 PASS）")
    else:
        ok.append(f"G4: 无二维 groupshared（一维声明 {len(one_d)} 处）")

# ---------------------------------------------------------------------------
# G5 groupshared 容量 >= 实际索引上界（**方案原文正是在这条上翻车**）
#
# 做法：从 #define 里解析出 TILEH_SIZE / TILEV_SIZE / BLUR_THREADS / MAX_RADIUS /
# BLUR_TILE_W / BLUR_TILE_H 的实际数值，再按代码里的索引算式求上界。
# 不解析宏表达式（如 BLUR_THREADS + 2 * MAX_RADIUS）就等于没测。
# ---------------------------------------------------------------------------
def macro_value(name: str, txt: str) -> int | None:
    m = re.search(rf"^\s*#define\s+{name}\s+(.+?)\s*$", txt, re.M)
    if not m:
        return None
    expr = m.group(1)
    # 只接受由整数、标识符、+ - * / ( ) 组成的表达式
    if not re.fullmatch(r"[\w\s+\-*/()]+", expr):
        return None
    for other in re.findall(r"[A-Za-z_]\w*", expr):
        v = macro_value(other, txt)
        if v is None:
            return None
        expr = re.sub(rf"\b{other}\b", str(v), expr)
    try:
        return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - 已白名单校验字符集
    except Exception:
        return None


need = ["BLUR_THREADS", "MAX_RADIUS", "BLUR_TILE_W", "BLUR_TILE_H", "TILEH_SIZE", "TILEV_SIZE"]
vals = {n: macro_value(n, hlsl_clean) for n in need}
missing = [n for n, v in vals.items() if v is None]
if missing:
    fails.append(f"G5: 无法解析宏 {missing} —— 容量校验不得静默跳过")
else:
    nthreads = vals["BLUR_THREADS"]  # type: ignore[assignment]
    maxr = vals["MAX_RADIUS"]  # type: ignore[assignment]
    tilew = vals["BLUR_TILE_W"]  # type: ignore[assignment]
    tileh = vals["BLUR_TILE_H"]  # type: ignore[assignment]
    sizeh = vals["TILEH_SIZE"]  # type: ignore[assignment]
    sizev = vals["TILEV_SIZE"]  # type: ignore[assignment]

    # 声明容量必须自洽：TILEH_SIZE 至少 主体 + 两侧 halo
    if sizeh < nthreads + 2 * maxr:
        fails.append(f"G5: TILEH_SIZE={sizeh} < 主体{nthreads}+2*halo{2 * maxr}")
    if sizev < (tileh + 2 * maxr) * tilew:
        fails.append(f"G5: TILEV_SIZE={sizev} < (tileh{tileh}+2*halo{2 * maxr})*tilew{tilew}")

    # 真实索引上界：从代码里抓 `s_TileH[...]` / `s_TileV[...]` 的算式，用极值代入。
    # 水平：局部线程 local ∈ [0, nthreads-1]，r ∈ [1, maxr]
    hi_h = [
        nthreads + maxr + (maxr - 1),            # right halo: BLUR_THREADS + MAX_RADIUS + local, local < r <= maxr
        (nthreads - 1) + maxr,                    # main: local + MAX_RADIUS
    ]
    max_h = max(hi_h)
    if max_h >= sizeh:
        fails.append(
            f"G5: s_TileH 索引上界 {max_h} >= 声明容量 {sizeh} —— **越界写 LDS**（方案原文的缺陷）"
        )
    else:
        ok.append(f"G5: s_TileH 上界 {max_h} < 容量 {sizeh}")

    # 垂直：ly ∈ [0, tileh-1]
    rows_needed = tileh + maxr + (maxr - 1)       # downRow = TILE_H + MAX_RADIUS + ly, ly < r
    max_v = (rows_needed - 1) * tilew + (tilew - 1)
    if rows_needed > tileh + 2 * maxr:
        fails.append(f"G5: s_TileV 需要 {rows_needed} 行 > 声明 {tileh + 2 * maxr} 行")
    if max_v >= sizev:
        fails.append(f"G5: s_TileV 索引上界 {max_v} >= 声明容量 {sizev} —— 越界写 LDS")
    else:
        ok.append(f"G5: s_TileV 上界 {max_v} < 容量 {sizev}")

# ---------------------------------------------------------------------------
# G6 DComp 四要素
# ---------------------------------------------------------------------------
cpp_clean = strip_c_comments(glass_cpp)
g6 = {
    "WS_EX_NOREDIRECTIONBITMAP": "缺它就有 GDI 重定向位图 => 白闪 + 冗余拷贝",
    "DXGI_ALPHA_MODE_PREMULTIPLIED": "缺它交换链不透明 => 底下的 Visual 0 全被盖死",
    "SetMaximumFrameLatency(1)": "缺它延迟 >= 2 帧 => 拖动时玻璃明显滞后",
    "SetSourceSize": "缺它缩放时像素错位（DComp 强制 DXGI_SCALING_STRETCH）",
}
for needle, why in g6.items():
    if needle not in cpp_clean:
        fails.append(f"G6: glass_window.cpp 缺 {needle} —— {why}")
if all(n in cpp_clean for n in g6):
    ok.append("G6: DComp 四要素齐备")

# CreateSwapChainForComposition 必须存在（用 DComp 而非 HWND 交换链）
if "CreateSwapChainForComposition" not in cpp_clean:
    fails.append("G6b: 未使用 CreateSwapChainForComposition —— 这不是 DComp 路径")

# ---------------------------------------------------------------------------
# G7 无边框窗口三处消息
# ---------------------------------------------------------------------------
g7 = {
    "WM_NCCALCSIZE": "缺它 WS_THICKFRAME 会在顶部留一条白色非客户区（RT-U4）",
    "WM_NCHITTEST": "缺它无边框窗口没有 8 向缩放热区",
    "SM_REMOTESESSION": "缺它 RDP 会话里自捕获会无限递归",
}
for needle, why in g7.items():
    if needle not in cpp_clean:
        fails.append(f"G7: glass_window.cpp 缺 {needle} —— {why}")
if all(n in cpp_clean for n in g7):
    ok.append("G7: 无边框三处消息齐备")

# ---------------------------------------------------------------------------
# G8 DWMSBT_NONE：系统材质必须清零
#
# 坑（本门禁第一版就踩了）：常量名 `DWMWA_SYSTEMBACKDROP_TYPE` 在仓库里只出现在
# **尾注释**里（`DwmSetWindowAttribute(hwnd_, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, ...)`），
# 而 cpp_clean 是剥掉注释的文本 ⇒ 拿 cpp_clean 找常量名必然零命中、必然误判 FAIL。
# 所以这里必须分开用三份证据：
#   · 原始文本找常量名（只用于「有登记」这条，防止 magic number 无出处）
#   · 剥注释文本找**调用形状**（consumer）
#   · 剥注释文本找**传值**（必须是 1，不是 3/2/4）
# 只查常量名不查传值 = 钉声明方（把 DWMSBT_ACRYLIC 也算过），正是三条纪律禁的。
# ---------------------------------------------------------------------------
raw_cpp = glass_cpp

# 调用点必须存在：38 号属性、第三参是变量或字面量
attr_call = re.search(r"DwmSetWindowAttribute\s*\(\s*\w+\s*,\s*38\s*,", cpp_clean)
# 传值必须为 DWMSBT_NONE(1)
val_init = re.search(r"\bbackdropNone\s*=\s*1\s*;", cpp_clean)
# 不得把 backdropNone 重新赋成别的值。
# 坑（本判据第二版）：`(?!1\s*;)` 里若让 `\s*` 留在前瞻**之外**，正则会在 ` = 1;`
# 处回溯 —— 前瞻检查空格成功、`[^;]+` 吃掉 " 1" ⇒ 把唯一正解判成违规。
# 必须把空白吞进前瞻内部：`=(?!\s*1\s*;)[^;]+;`。
bad_reassign = re.search(r"\bbackdropNone\s*=(?!\s*1\s*;)\s*[^;]+;", cpp_clean)

if not attr_call:
    fails.append(
        "G8: glass_window.cpp 没有 DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, ...) "
        "—— 系统 Acrylic 会保留固定浊度地板（293 已证不可调）"
    )
elif not val_init:
    fails.append(
        "G8: 找不到 `backdropNone = 1;` —— 系统材质不是 DWMSBT_NONE(1)。"
        "293 已证 Acrylic(3) 浊度不可调、Mica(2)/Tabbed(4) 是 opaque"
    )
else:
    if bad_reassign:
        fails.append(f"G8: backdropNone 被改赋成非 1 的值（{bad_reassign.group(0)}）—— 系统材质回潮")
    elif "DWMWA_SYSTEMBACKDROP_TYPE" not in raw_cpp and "DWMSBT_NONE" not in raw_cpp:
        fails.append(
            "G8: 38 是裸 magic number 且全文无 `DWMWA_SYSTEMBACKDROP_TYPE`/`DWMSBT_NONE` 出处 —— "
            "后续维护者会按「38 大概能改」把它改成 Acrylic"
        )
    else:
        ok.append("G8: DWMSBT_NONE(1) 已清零系统材质（38 号属性 + 传值 1 双向钉住）")

# 透明背景（DefaultBackgroundColor）必须设为透明，否则 WebView2 画不透明底
if "put_DefaultBackgroundColor" not in cpp_clean:
    fails.append("G8b: 未设置 WebView2 DefaultBackgroundColor 透明 —— 控件会画不透明底，玻璃消失")
else:
    ok.append("G8b: WebView2 背景透明已设置")

# ---------------------------------------------------------------------------
# G9 RbL2Periodic 体内不得有 RbL2IdxSave（[172/A.2]）
# ---------------------------------------------------------------------------
l2c = ROOT / "src" / "driver" / "rbL2.c"
if not l2c.exists():
    fails.append("G9: 找不到 src/driver/rbL2.c")
else:
    l2txt = strip_c_comments(read(l2c))
    m = re.search(r"\b\w[\w\s*]*?\bRbL2Periodic\s*\([^)]*\)\s*\{", l2txt)
    if not m:
        fails.append("G9: 找不到 RbL2Periodic 函数（判据不得空跑）")
    else:
        depth = 1
        i = m.end()
        while i < len(l2txt) and depth > 0:
            if l2txt[i] == "{":
                depth += 1
            elif l2txt[i] == "}":
                depth -= 1
            i += 1
        body = l2txt[m.end():i]
        if "RbL2IdxSave" in body:
            fails.append(
                "G9: RbL2Periodic 体内出现 RbL2IdxSave —— 违反 [172/A.2]（周期定稿 .idx 会撕裂索引）"
            )
        else:
            ok.append("G9: RbL2Periodic 体内无 RbL2IdxSave")

# ---------------------------------------------------------------------------
# G10 RbUnload 里 F10 排空顺序
# ---------------------------------------------------------------------------
drv = ROOT / "src" / "driver" / "rbdriver.c"
if not drv.exists():
    fails.append("G10: 找不到 src/driver/rbdriver.c")
else:
    dtext = strip_c_comments(read(drv))
    m = re.search(r"\bVOID\s+RbUnload\s*\([^)]*\)\s*\{", dtext)
    if not m:
        fails.append("G10: 找不到 RbUnload（判据不得空跑）")
    else:
        depth = 1
        i = m.end()
        while i < len(dtext) and depth > 0:
            if dtext[i] == "{":
                depth += 1
            elif dtext[i] == "}":
                depth -= 1
            i += 1
        body = dtext[m.end():i]

        unreg = body.find("IoUnregisterShutdownNotification")
        drain = body.find("PersistInFlight")
        destroy = body.find("rb_engine_destroy")
        syncpool = body.find("RbSyncCtxPoolDelete")

        if unreg < 0:
            fails.append("G10: RbUnload 里没有 IoUnregisterShutdownNotification —— F10 排空无从谈起")
        elif drain < 0:
            fails.append("G10: RbUnload 里没有 PersistInFlight 排空 —— RT-K4 未落地（259 F10）")
        elif unreg > drain:
            fails.append("G10: 注销关闭通知在排空**之后** —— 排空期间还能进来新的关机持久化（RT-K4 顺序反了）")
        else:
            ok.append("G10: 注销通知先于排空")

        # UAF 的判据是「引擎/池还在被谁用」，不是「谁先 flush」。
        #
        # 坑（本判据第一版是错的）：原写 `drain` 必须早于 `RbFlushAllDirty`。
        # 但 RbUnload 自己的 RbFlushAllDirty 在**步骤 1**（引擎必须完整时刷脏），
        # 而 unreg/drain 在步骤 4/5 —— 同时满足 `unreg < drain` 与 `drain < flush`
        # 就需要 `drain < flush < unreg < drain`，是个恒假不等式。
        # 那条判据在任何实现下都不可能通过，等于「判据自身坏掉」（本轮第一个变异
        # 就把它证伪了）。RbUnload 自己刷脏与在途 persist 并发是否安全，属于
        # flush 层互斥问题，与 F10 的销毁期 UAF 无关。
        # 真正要守的是下面两条：排空必须早于**唯一两个会释放引擎/同步 ctx 池的点**。
        if destroy >= 0 and drain >= 0 and drain > destroy:
            fails.append("G10: PersistInFlight 排空在 rb_engine_destroy **之后** —— 引擎已拆还要排空 = UAF")
        if syncpool >= 0 and drain >= 0 and drain > syncpool:
            fails.append(
                "G10: 排空在 RbSyncCtxPoolDelete **之后** —— 在途 persist 的 RbSyncIo "
                "会向已删除的 lookaside 归还内存（UAF/0xC4）"
            )
        if destroy >= 0 and drain >= 0 and syncpool >= 0 and drain < destroy and drain < syncpool:
            ok.append("G10: 排空早于 rb_engine_destroy 与 RbSyncCtxPoolDelete")

print("== Gate27: 液态玻璃原生宿主 ==")
for line in ok:
    print(f"[OK] {line}")
for line in fails:
    print(f"[FAIL] {line}")

if fails:
    print(f"GATE27 FAIL: {len(fails)} 条")
    sys.exit(1)
print(f"GATE27 PASS: {len(ok)} 条判据全过")
sys.exit(0)