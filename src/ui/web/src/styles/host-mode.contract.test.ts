/// <reference types="node" />
// src/ui/web/src/styles/host-mode.contract.test.ts
//
// U3 CSS 契约门禁（V7 / V8 的静态侧）。
//
// 为什么要单独一个文件而不是塞进 liquid-glass.contract.test.ts：
// 那边钉的是 U1/U2 的 18+18+13 三族令牌数量与 url() 内容，断言方向是
// "这些必须存在"；这边钉的是 U3 新族的"零死链 + 零违禁属性 + 三个交互
// 变量必须有读取方"，断言方向是"这些必须成立"。混在一起会让
// "既有的 url(#rbc-liquid-refract) 必须存在" 与 "本区块严禁 url()"
// 两条规则互相打架 —— 因此新增区块用切片方式单独校验。

import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

const tokensCss = readFileSync(new URL('./tokens.css', import.meta.url), 'utf-8');
const glassCss = readFileSync(new URL('./liquid-glass.css', import.meta.url), 'utf-8');

const stripComments = (text: string) => text.replace(/\/\*[\s\S]*?\*\//g, '');

/** U3 追加块的起点：注释里的 U3 标题是唯一稳定锚。
 *  锚点落在注释内部，必须回溯到该注释的开头标记（lastIndexOf 注释起始符），
 *  否则切片从注释中间开始 —— stripComments 匹配不到未闭合的结束标记，
 *  注释正文会残留在被断言的文本里（正文里正好写着 display / contain:paint
 *  与 url()，会让"零违禁"断言自伤）。这是本文件第一版踩到的真坑。 */
const U3_ANCHOR = 'U3 分层液态玻璃 + 宿主真透色开闸';
const anchorIdx = glassCss.indexOf(U3_ANCHOR);
const u3Start = anchorIdx >= 0 ? glassCss.lastIndexOf('/*', anchorIdx) : -1;
const u3Block = u3Start >= 0 ? stripComments(glassCss.slice(u3Start)) : '';

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

describe('U3 分层玻璃 CSS 契约', () => {
  it('存在 U3 追加块（锚点缺失会让下面所有切片断言变成空跑）', () => {
    expect(u3Start).toBeGreaterThan(0);
    expect(u3Block.length).toBeGreaterThan(500);
  });

  it('10 个 --rbc-layer-* 令牌全部在 tokens.css 声明', () => {
    const clean = stripComments(tokensCss);
    for (const tok of LAYER_TOKENS) {
      expect(clean).toContain(`${tok}:`);
    }
  });

  it('10 个令牌零死链：每一个都在 liquid-glass.css 里被 var() 消费', () => {
    const clean = stripComments(glassCss);
    const dead = LAYER_TOKENS.filter((tok) => !clean.includes(`var(${tok})`));
    expect(dead).toEqual([]);
  });

  it('交互变量有读取方：hook 写入的 CSS 变量必须被 CSS 消费（U2 缺陷回归网）', () => {
    // hook 侧只写不读，CSS 侧只读不写。二者缺一，碰撞高光/偏转色散就是死层。
    // 注意 --rbc-rim-angle 的消费点在 tokens.css 的 --rbc-layer-bevel-gradient 里
    // （渐变角度取它），不是 glass.css —— 断言必须指向真正的消费方。
    for (const v of ['--rbc-impact-glow', '--rbc-impact-x', '--rbc-impact-y']) {
      expect(u3Block).toContain(`var(${v}`);
    }
    expect(stripComments(tokensCss)).toContain('var(--rbc-rim-angle');
  });

  it('顶层厚玻璃确实开了嵌套 backdrop-filter（不是 none）', () => {
    expect(u3Block).toMatch(/\.rbc-section\s+\.rbc-card:not\([^)]+\):not\([^)]+\)\s*\{[^}]*backdrop-filter:\s*\n?\s*blur\(/s);
    expect(u3Block).toContain('var(--rbc-layer-card-blur)');
  });

  it('倒角层特异性为 (0,4,1) 且 padding 提到 1.25px（压过既有 (0,3,1)）', () => {
    const m = u3Block.match(
      /\.rbc-section\s+\.rbc-card:not\(\.rbc-card-accent\):not\(\.rbc-card-danger\)::before\s*\{([^}]+)\}/,
    );
    expect(m).not.toBeNull();
    expect(m![1]).toContain('padding: 1.25px');
  });

  it('V8: U3 区块严禁 display / flex-direction / gap / contain: paint', () => {
    // 只切 U3 块：文件头注释里写着 display:flex 是在说明"没有覆写"，不是真声明。
    expect(u3Block).not.toMatch(/\bdisplay\s*:/);
    expect(u3Block).not.toMatch(/\bflex-direction\s*:/);
    expect(u3Block).not.toMatch(/\bgap\s*:/);
    expect(u3Block).not.toMatch(/\bcontain\s*:\s*paint/);
  });

  it('新增倒角层零 url()：既有 url(#rbc-liquid-refract) 已实测为死层，不得再叠 SVG 滤镜', () => {
    expect(u3Block).not.toContain('url(');
    // 但既有契约必须仍在（这条防的是"顺手删掉别人依赖的滤镜"）
    expect(glassCss).toContain('url(#rbc-liquid-refract)');
  });

  it('动效时长走令牌而非写死秒数（reduced-motion 必须能把它归零）', () => {
    expect(u3Block).toContain('var(--rbc-dur-normal)');
    // U3 块内的 transition 不许出现写死的 0.22s 这类秒数
    const transitions = u3Block.match(/transition:[\s\S]*?;/g) ?? [];
    expect(transitions.length).toBeGreaterThan(0);
    for (const t of transitions) {
      expect(t).not.toMatch(/\b0?\.[0-9]+\s*s\b/);
    }
  });

  it('宿主开闸只挂在 html.rbc-host-transparent 之下，不污染浏览器直开形态', () => {
    // body / .rbc-app-bg 置透明必须是 html.rbc-host-transparent 的后代选择器
    expect(u3Block).toMatch(/html\.rbc-host-transparent\s+body/);
    expect(u3Block).toMatch(/html\.rbc-host-transparent\s+\.rbc-app-bg/);
    // 反射式检查：不存在裸 body {} 或裸 .rbc-app-bg {} 的透明声明
    expect(u3Block).not.toMatch(/(^|\})\s*body\s*\{[^}]*transparent/m);
  });

  it('强制色模式下光学层退化为 none / Canvas（不留半透明死层）', () => {
    // 退化声明写在 tokens.css（令牌归零），消费规则在 glass.css（撤销 mask/背景）。
    const cleanTokens = stripComments(tokensCss);
    expect(u3Block).toContain('@media (forced-colors: active)');
    const forcedBlock = cleanTokens.slice(cleanTokens.indexOf('@media (forced-colors: active)', cleanTokens.indexOf('U3 分层液态玻璃令牌') >= 0 ? cleanTokens.indexOf('U3 分层液态玻璃令牌') : 0));
    expect(forcedBlock).toMatch(/--rbc-layer-card-blur:\s*0px/);
    expect(forcedBlock).toMatch(/--rbc-layer-host-scrim:\s*Canvas/);
  });

  /* ======================================================================
   * [284 2026-11-19] W3: html 元素本身必须被开闸
   *
   * 用户第 N 轮反馈"透色完全没生效"的真因，就藏在 index.html 的内联样式里：
   *   <style> html, body { background: #f3f5f9 } </style>
   * 上一版只重置了 body 与 .rbc-app-bg，漏了 html 元素本身。
   * 按 CSS 画布传播规则，body 透明时画布底色回退到 html 的背景色 ⇒
   * html 的 #f3f5f9 从透明 body 下面透出来，整窗仍是浅蓝白。
   *
   * 与此同时 DWM 侧的 extend / backdrop HRESULT 全是 0x0（成功）、
   * sourceBg=#00FFFFFF —— 也就是说"通道全通、但被自己家 html 挡住"。
   * 这条判据专门防这个复发：开闸选择器列表里必须有裸 html。
   * ====================================================================== */
  it('W3: 宿主开闸必须同时命中 html 元素本身（否则画布底色会从透明 body 下方透出）', () => {
    const clean = stripComments(glassCss);
    // 必须存在 "html.rbc-host-transparent," 或 "... ,html.rbc-host-transparent {" 这类
    // 把 html 自身列为选择器之一的规则块。
    expect(clean).toMatch(/html\.rbc-host-transparent\s*,/);
    // 反射式守卫：该规则块里必须声明 background transparent
    const block = clean.match(/html\.rbc-host-transparent\s*,[^{]*\{([^}]*)\}/s);
    expect(block).not.toBeNull();
    expect(block![1]).toMatch(/background(-color)?\s*:\s*transparent/);
  });

  it('W3b: index.html 的防闪内联底色仍然存在（不许为了透色把这个优化删掉）', () => {
    // 这条是反向守卫：修透色不能顺手删掉首帧防闪，否则重新引入"闪一下"。
    const indexHtml = readFileSync(new URL('../../index.html', import.meta.url), 'utf-8');
    expect(indexHtml).toMatch(/background:\s*#f3f5f9/);
  });
});