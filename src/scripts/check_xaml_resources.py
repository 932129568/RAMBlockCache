#!/usr/bin/env python3
"""check_xaml_resources.py — XAML StaticResource 引用完整性门禁（六件套之一）。

背景：V5~V7 连续三版因 RbcRadiusLg（FIX B26 当死令牌删除）被 DialogHost.xaml
引用却未随消费方加回，导致 App.DoStartup 加载 BAML 时 XamlParseException
fail-fast（软件打不开）。编译期不报（StaticResource 是运行时解析），
沙箱禁起 GUI 使冒烟也抓不到——本脚本静态差集兜底。

规则：
  1. 所有 {StaticResource Key} 引用必须在某 .xaml 有 x:Key="Key" 定义
     （App.xaml/Dark.xaml/控件 XAML 均可；键可在不同文件定义与引用）。
  2. 死令牌检查（反向）：定义但零引用的 x:Key 记 WARN 不 FAIL
     （FIX B26 纪律：死令牌应删；新增定义必须随消费方）。
退出码：0=通过（可带 WARN）；1=有缺失引用（阻断）。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / "src" / "ui" / "controller" / "RAMBlockCache.Controller"

REF_RE = re.compile(r"\{StaticResource\s+([A-Za-z0-9_.:]+)\}")
DEF_RE = re.compile(r'x:Key="([A-Za-z0-9_.:]+)"')


def main() -> int:
    if not ROOT.is_dir():
        print(f"FAIL: controller dir not found: {ROOT}")
        return 1
    xamls = [p for p in ROOT.rglob("*.xaml")
             if "obj" not in p.parts and "bin" not in p.parts]

    refs: dict[str, set[str]] = {}   # key -> 引用文件集
    defs: dict[str, str] = {}        # key -> 定义文件
    for p in xamls:
        text = p.read_text(encoding="utf-8-sig")
        for m in REF_RE.finditer(text):
            refs.setdefault(m.group(1), set()).add(p.name)
        for m in DEF_RE.finditer(text):
            # 同名键多处定义：保留首个（WPF 取最近作用域；此处仅查存在性）
            defs.setdefault(m.group(1), p.name)

    missing = {k: v for k, v in refs.items() if k not in defs}
    dead = {k: v for k, v in defs.items() if k not in refs}

    print(f"xaml files: {len(xamls)}  refs: {len(refs)}  defs: {len(defs)}")
    if missing:
        print("FAIL: StaticResource 引用缺失定义（运行时 XamlParseException 级）：")
        for k, users in sorted(missing.items()):
            print(f"  {k}  <- 引用方: {', '.join(sorted(users))}")
        return 1
    if dead:
        print("WARN: 死令牌（零引用定义；FIX B26 纪律应删，或随消费方加回）：")
        for k, f in sorted(dead.items()):
            print(f"  {k}  (定义于 {f})")
    print("PASS: 全部 StaticResource 引用有定义")
    return 0


if __name__ == "__main__":
    sys.exit(main())
