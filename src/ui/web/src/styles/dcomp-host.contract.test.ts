/// <reference types="node" />
// src/ui/web/src/styles/dcomp-host.contract.test.ts
// [295 Wave 1 2026-11-19]
//
// DComp 原生宿主共存块的 CSS 契约。
//
// 为什么单独立一个文件（而不是塞进 host-mode.contract.test.ts）：
//   · host-mode 用 U3 锚点**切片**，判据只覆盖 U3 那一段。DComp 块排在 U3 之后，
//     在它的切片里根本不可见 ⇒ 那块 CSS 在既有判据下完全无覆盖。
//   · host-mode 钉的是「10 个 --rbc-layer-* 令牌必须存在且被消费」，
//     这边钉的是「DComp 块必须让路、且不得引入零消费令牌」——
//     两条断言方向相反（前者要求保留，后者要求退场），混在一处会互相打架。
//
// 锚点必须回溯到注释起始符：切片若从注释正文中间开始，stripComments 匹配不到
// 未闭合的结束标记，注释里写的 `background: transparent` 会冒充真实声明 ——
// 这是 host-mode.contract.test.ts 第一版踩到的真坑，此处照抄其教训。

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

// node 环境下 import.meta.url 是 file://，可直接用；但 Windows + 中文路径下
// 必须经 fileURLToPath 才能被 fs 接受（与 vite.config.ts 的既有修复同口径）。
const glassCss = readFileSync(fileURLToPath(new URL('./liquid-glass.css', import.meta.url)), 'utf-8');
const tokensCss = readFileSync(fileURLToPath(new URL('./tokens.css', import.meta.url)), 'utf-8');

const stripComments = (s: string): string => s.replace(/\/\*[\s\S]*?\*\//g, '');

const ANCHOR = '[295 2026-11-19] DComp 原生宿主共存态';
const anchorIdx = glassCss.indexOf(ANCHOR);
const blockStart = anchorIdx >= 0 ? glassCss.lastIndexOf('/*', anchorIdx) : -1;
const dcompBlock = blockStart >= 0 ? stripComments(glassCss.slice(blockStart)) : '';

const LAYER_TOKENS = [
  '--rbc-layer-section-fill',
  '--rbc-layer-card-fill',
  '--rbc-layer-card-blur',
  '--rbc-layer-card-saturate',
  '--rbc-layer-card-brightness',
  '--rbc-layer-host-scrim',
  '--rbc-layer-section-rim',
  '--rbc-layer-card-rim',
  '--rbc-layer-card-shadow',
  '--rbc-layer-bevel-gradient',
];

describe('295 DComp 宿主共存 CSS 契约', () => {
  it('DComp 块存在（锚点缺失会让下面所有切片断言变成空跑）', () => {
    expect(blockStart).toBeGreaterThan(0);
    expect(dcompBlock.length).toBeGreaterThan(600);
  });

  it('自绘玻璃全部退场：section/card 的 background 与 backdrop-filter 都被 !important 压掉', () => {
    // 缺任一条，真桌面就被页面自绘的 scrim/模糊重新盖住 —— 玻璃退回塑料（293 的结论）。
    expect(dcompBlock).toMatch(
      /html\.rbc-host-dcomp\s+\.rbc-section,\s*html\.rbc-host-dcomp\s+\.rbc-card\s*\{[^}]*background:\s*transparent\s*!important/s,
    );
    expect(dcompBlock).toMatch(
      /html\.rbc-host-dcomp\s+\.rbc-section,\s*html\.rbc-host-dcomp\s+\.rbc-card\s*\{[^}]*backdrop-filter:\s*none\s*!important/s,
    );
  });

  it('假边缘层退场（::before / ::after 的 opacity:0），避免与 HLSL rimStroke 叠成双描边', () => {
    expect(dcompBlock).toMatch(/html\.rbc-host-dcomp[^{]*::before[^{]*\{[^}]*opacity:\s*0\s*!important/s);
  });

  it('块内每个 var() 引用的令牌都真实存在（零死引用），且确实抽到了东西', () => {
    // 「引用未定义变量时整条属性失效、不报错」是本项目踩过的真坑。
    const cleanTokens = stripComments(tokensCss);
    const used = new Set<string>();
    for (const m of dcompBlock.matchAll(/var\(\s*(--[a-z0-9-]+)/gi)) used.add(m[1] ?? '');
    used.delete('');

    const selfDefined = new Set<string>();
    for (const m of dcompBlock.matchAll(/^\s*(--[a-z0-9-]+)\s*:/gim)) selfDefined.add(m[1] ?? '');

    const dead = [...used].filter((t) => t !== '' && !selfDefined.has(t) && !cleanTokens.includes(`${t}:`));
    expect(dead).toEqual([]);
    // 反向自检：空集合会让上面那条空跑。
    expect(used.size).toBeGreaterThan(0);
  });

  it('不引入 --rbc-text-muted（方案原文的死令牌，全仓零消费）', () => {
    expect(dcompBlock).not.toContain('--rbc-text-muted');
    expect(stripComments(tokensCss)).not.toContain('--rbc-text-muted:');
  });

  it('极性翻字只覆盖真实存在的两个令牌', () => {
    for (const polarity of ['light-text', 'dark-text']) {
      const re = new RegExp(
        `html\\.rbc-host-dcomp\\[data-rbc-apca-polarity='${polarity}'\\]\\s*\\{([^}]*)\\}`,
        's',
      );
      const m = re.exec(dcompBlock);
      expect(m, `缺 ${polarity} 极性块`).not.toBeNull();
      if (!m) continue;
      const body = m[1] ?? '';
      expect(body).toContain('--rbc-text-primary:');
      expect(body).toContain('--rbc-text-secondary:');
    }
  });

  it('既有 U3 块未被破坏：10 个 --rbc-layer-* 令牌仍全部被 var() 消费（295 只允许追加）', () => {
    const clean = stripComments(glassCss);
    const dead = LAYER_TOKENS.filter((t) => !clean.includes(`var(${t})`));
    expect(dead).toEqual([]);
  });

  it('DComp 块排在 U3 块之后（追加而非替换的位置约束）', () => {
    const u3 = glassCss.indexOf('U3 分层液态玻璃 + 宿主真透色开闸');
    expect(u3).toBeGreaterThan(0);
    expect(blockStart).toBeGreaterThan(u3);
  });
});