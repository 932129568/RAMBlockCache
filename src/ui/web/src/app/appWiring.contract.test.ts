// @vitest-environment node
// src/ui/web/src/app/appWiring.contract.test.ts
// [299 2026-11-19]
//
// 判据：App.tsx 必须**真的**把 useNativeGlassSync 接上，而不只是文件存在。
//
// 这条判据存在的唯一理由，是 295 那一轮的实况：
//   useNativeGlassSync.ts            9,670 B   ✅ 落盘
//   useNativeGlassSync.test.ts       14 条     ✅ 全绿
//   App.tsx                          一行都没 import ❌
//   => 156 条测试全绿，运行期零生效。
//
// 也就是说「单测全绿」这件事在这里**零证据力** —— 因为没有任何测试看 App.tsx。
// 补的方法是把它变成一条可执行断言，而不是再写一遍注释。
//
// 用 node 环境读文件（jsdom 下 import.meta.url 不是 file://，readFileSync 会抛）。
// 与 dcomp-host.contract.test.ts 的做法一致。
//
// 三类判据，缺一不可：
//   W1 useNativeGlassSync 被 import 且被调用（不只是 import）
//   W2 调用点在 useLiquidWave 之前（否则首帧会白跑一轮 CPU 水波）
//   W3 退让侧确实读了 rbc-host-dcomp 这个类（两端用同一个约定）

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const webRoot = resolve(here, '../..');

const readRel = (p: string): string => readFileSync(resolve(webRoot, p), 'utf8');

describe('App.tsx 接线契约（W1-W3）', () => {
  const appSrc = readRel('src/app/App.tsx');

  it('W1 useNativeGlassSync 既被 import 也被调用（杜绝空转孤岛）', () => {
    // import 形态
    expect(appSrc).toMatch(/import\s*\{[^}]*useNativeGlassSync[^}]*\}\s*from\s*['"][^'"]*useNativeGlassSync['"]/);
    // 调用形态：独立一行 useNativeGlassSync(rootRef)，不是被注释掉
    const callLines = appSrc
      .split('\n')
      .filter((l) => /useNativeGlassSync\s*\(\s*rootRef\s*\)/.test(l) && !/^\s*(\/\/|\*|\/\*)/.test(l));
    expect(callLines.length, 'useNativeGlassSync(rootRef) 必须有一处未被注释的调用').toBeGreaterThan(0);
  });

  it('W2 调用必须早于 useLiquidWave（否则首帧双层波纹）', () => {
    const lines = appSrc.split('\n');
    const idxNative = lines.findIndex(
      (l) => /useNativeGlassSync\s*\(\s*rootRef\s*\)/.test(l) && !/^\s*(\/\/|\*|\/\*)/.test(l),
    );
    const idxWave = lines.findIndex(
      (l) => /useLiquidWave\s*\(\s*\{/.test(l) && !/^\s*(\/\/|\*|\/\*)/.test(l),
    );
    expect(idxNative).toBeGreaterThan(-1);
    expect(idxWave).toBeGreaterThan(-1);
    // 期望类先加、退让守卫先能读到 —— 所以 native 必须在 wave 之前
    expect(idxNative).toBeLessThan(idxWave);
  });

  it('W3 两端共用 rbc-host-dcomp 这一个能力位（不各自造标志）', () => {
    const syncSrc = readRel('src/hooks/useNativeGlassSync.ts');
    const waveSrc = readRel('src/hooks/useLiquidWave.ts');

    // 发送侧：useNativeGlassSync 收到的 caps 里 engine 必须是 d3d11-dcomp
    expect(syncSrc).toMatch(/['"]d3d11-dcomp['"]/);
    // 收尾侧：它把类加到 <html>
    expect(syncSrc).toMatch(/rbc-host-dcomp/);
    // 消费侧：useLiquidWave 读的必须是同一个类名
    expect(waveSrc).toMatch(/rbc-host-dcomp/);
    // 反向：useLiquidWave 不得自己另立一个全局开关（否则两份状态会不同步）
    expect(waveSrc).not.toMatch(/__RBC_NATIVE_ACTIVE__|window\.rbcNativeActive/);
  });
});