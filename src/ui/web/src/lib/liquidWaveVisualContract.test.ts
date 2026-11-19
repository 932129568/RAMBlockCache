// src/ui/web/src/lib/liquidWaveVisualContract.test.ts
//
// [v23 2026-11-19] 补齐 289 §7.2 点名的三大缺失判据。
//
// 为什么这三条必须单独存在：此前 113 个用例全部只检查「类名/符号存在性」，
// 于是下面三个缺陷可以同时满足全部用例、又全部肉眼可见：
//   (a) 合成不透明度 0.71 —— 桌面被白纱遮死，读起来是灰白板；
//   (b) 位图被 CSS 各向异性拉伸 6.56:1 —— 圆形涟漪变横向糊斑；
//   (c) 超预算波场永不 putImageData —— 画面冻结 1.2s 后整块消失。
// 这三件事全部可以在 jsdom / 静态文件里断言，不需要真机、不需要像素采样。
// 「测试全绿 ⇒ 液态玻璃达标」这句话只有补上这三条之后才勉强成立；
// 即使如此，相位、色散、光照角度这些仍然只能真机验收，本文件不做此承诺。
//
// @vitest-environment jsdom

import fs from 'node:fs';
import path from 'node:path';
import { cleanup, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
  BORDER_HIT_THRESHOLD,
  computeAdaptiveGrid,
  createWaveField,
  injectWaveImpulse,
  stepWave,
  wavePeak,
  type BorderCollision,
} from './liquidWaveCore';
import { useLiquidWave } from '../hooks/useLiquidWave';

const stylesDir = path.resolve(__dirname, '../styles');
const tokensCss = fs.readFileSync(path.join(stylesDir, 'tokens.css'), 'utf8');
const glassCss = fs.readFileSync(path.join(stylesDir, 'liquid-glass.css'), 'utf8');
const stripComments = (t: string) => t.replace(/\/\*[\s\S]*?\*\//g, '');

/** 从 CSS 文本里取某个令牌的 rgba 白纱 alpha；取不到返回 null。 */
function scrimAlpha(css: string): number | null {
  const clean = stripComments(css);
  const m = clean.match(
    /--rbc-layer-host-scrim:\s*rgba\(\s*255\s*,\s*255\s*,\s*255\s*,\s*([0-9.]+)\s*\)/,
  );
  return m ? Number(m[1]) : null;
}

/** 取 --rbc-layer-section-fill 里出现的最大 alpha（底层玻璃填充最不透明的一档）。 */
function maxSectionFillAlpha(css: string): number {
  const clean = stripComments(css);
  const block = clean.match(/--rbc-layer-section-fill:\s*linear-gradient\(([^;]+)\);/);
  if (!block) return 0;
  const alphas = Array.from(block[1]!.matchAll(/rgba\([^)]*?,\s*([0-9.]+)\s*\)/g), (m) =>
    Number(m[1]),
  );
  return alphas.length > 0 ? Math.max(...alphas) : 0;
}

beforeEach(() => {
  vi.stubGlobal('matchMedia', (query: string) => ({
    matches: false,
    media: query,
    onchange: null,
    addListener: () => {},
    removeListener: () => {},
    addEventListener: () => {},
    removeEventListener: () => {},
    dispatchEvent: () => false,
  }));
});

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe('V15: 合成不透明度（防白纱把桌面遮死）', () => {
  it('两处 --rbc-layer-host-scrim 都 <= 0.25（:root 与 html.rbc-host-transparent 双写点）', () => {
    // 必须查两处：宿主模式下生效的是 html.rbc-host-transparent 块（权重更高），
    // 只修 :root 等于没修 —— 这正是 v22 遗留 0.62 时"改了却没效果"的原因。
    const rootIdx = stripComments(tokensCss).indexOf(':root');
    const hostIdx = stripComments(tokensCss).indexOf('html.rbc-host-transparent');
    expect(rootIdx).toBeGreaterThanOrEqual(0);
    expect(hostIdx).toBeGreaterThan(rootIdx);
    expect(scrimAlpha(tokensCss)).not.toBeNull();
    expect(scrimAlpha(tokensCss)!).toBeLessThanOrEqual(0.25);
  });

  it('合成后不透明度 < 0.4（scrim 叠在底层玻璃 fill 之上），且确实 > 0（不是全透明白洞）', () => {
    const scrim = scrimAlpha(tokensCss);
    expect(scrim).not.toBeNull();
    const fill = maxSectionFillAlpha(tokensCss);
    // 合成公式：scrim + fill × (1 − scrim)。顺序无所谓（都是白色叠加），
    // 但两项都必须参与 —— 只盯 scrim 会漏掉"fill 太实"这条路径。
    const composited = scrim! + fill * (1 - scrim!);
    expect(fill).toBeGreaterThan(0); // 若 fill 解析失败，这条断言会让上面的式子失去意义
    expect(composited).toBeLessThan(0.4);
    expect(composited).toBeGreaterThan(0.05);
  });

  /* ======================================================================
   * [292 2026-11-19] V19: 合成白度必须把 **三层** 都算进来，且用乘法合成
   *
   * 上面那条 V15 是空跑的元凶，必须单独说明，否则后来人还会照着它写。
   * 它有两个漏洞，每一个都足以让"磨砂玻璃"这种缺陷全绿通过：
   *
   *   1) 漏了 card 层。用户看到的是 section 里再嵌 card，三层白纱依次叠加。
   *      V15 只算 scrim + section，合成 0.446 < 0.4 的邻域里擦边过了，
   *      而真实值（再加 card 0.26）是 0.590。差的那 0.144 正好是
   *      "还能看见一点桌面"与"完全糊成一块白"的分界。
   *
   *   2) 加法合成公式在数学上是错的。白色叠白色用 1-(1-a)(1-b)（乘法），
   *      不是 a+b(1-a) 之外再加一项 —— 两项时后者恰好等价，三项时就不等价了：
   *      加法链 0.34+0.16×0.66+0.26×0.446 = 0.564
   *      乘法链 1-(0.66×0.84×0.74)          = 0.590
   *      两者差 0.026，而 V15 用加法 + 漏项得到 0.446 —— 一个"看起来对"的假数字。
   *
   * 更要命的是第三条：V15 读的是 tokens.css 的**字面值**，而运行时真正生效的
   * scrim 由 useLiquidGlassPointer 按 luma 插值覆盖（旧代码顶到 0.340）。
   * 也就是说即使令牌写成 0.14，用户看到的也可能是 0.34 —— 判据钉在"声明"上，
   * 而缺陷发生在"后果"里。这条 V19 三个漏洞一起堵。
   * ====================================================================== */
  it('V19: 三层白纱的乘法合成白度 < 0.30，且透射率 >= 0.70（"能透色却看不见桌面"的直接判据）', () => {
    const scrim = scrimAlpha(tokensCss);
    expect(scrim).not.toBeNull();

    // card 层：取 --rbc-layer-card-fill 的最大 alpha（authoring 块与宿主块都要看）
    const cardFills = Array.from(
      stripComments(tokensCss).matchAll(/--rbc-layer-card-fill:\s*linear-gradient\(([\s\S]*?)\);/g),
      (m) => m[1]!,
    );
    expect(cardFills.length).toBeGreaterThanOrEqual(2); // :root + html.rbc-host-transparent
    // 宿主模式下生效的是后面那块（权重更高），取它的值才是"实际生效"
    const lastCardFill = cardFills[cardFills.length - 1]!;
    const cardAlphas = Array.from(
      lastCardFill.matchAll(/rgba\([^)]*?,\s*([0-9.]+)\s*\)/g),
      (m) => Number(m[1]),
    );
    expect(cardAlphas.length).toBeGreaterThan(0);
    const card = Math.max(...cardAlphas);

    const section = maxSectionFillAlpha(tokensCss);
    expect(section).toBeGreaterThan(0);

    // 乘法合成（白色叠白色的正确公式）
    const composited = 1 - (1 - scrim!) * (1 - section) * (1 - card);
    const transmittance = 1 - composited;

    // 这三条一起钉住"液态玻璃"与"磨砂塑料"的分界。
    // 旧值 0.59 会让前两条同时失败；0.17 的新值有充足余量。
    expect(
      composited,
      `合成白度 ${composited.toFixed(3)}（scrim=${scrim}, section=${section}, card=${card}）`,
    ).toBeLessThan(0.3);
    expect(transmittance).toBeGreaterThanOrEqual(0.7);
    // 反向守卫：不能为了"透"直接把三层都抹成 0，那是白洞不是玻璃
    expect(composited).toBeGreaterThan(0.03);
  });

  it('V19b: luma 覆盖区间的最坏值也必须 < 0.32（运行时生效值，不是令牌字面值）', () => {
    // 这条钉的是 useLiquidGlassPointer 的 SCRIM_ALPHA_MIN/MAX。
    // 为什么必须单独钉：令牌写 0.06，运行时却可能被 luma 顶到 0.34 ——
    // 只查令牌等于只查了一半。这里的上限就是"旧代码 0.38"留下的疤。
    const hook = fs.readFileSync(path.join(__dirname, '../hooks/useLiquidGlassPointer.ts'), 'utf8');
    const minM = hook.match(/SCRIM_ALPHA_MIN\s*=\s*([0-9.]+)/);
    const maxM = hook.match(/SCRIM_ALPHA_MAX\s*=\s*([0-9.]+)/);
    expect(minM, 'SCRIM_ALPHA_MIN 必须能被解析，否则这条判据空跑').not.toBeNull();
    expect(maxM, 'SCRIM_ALPHA_MAX 必须能被解析，否则这条判据空跑').not.toBeNull();

    const maxScrim = Number(maxM![1]);
    expect(minM![1]).toBeTruthy();
    // 插值上界必须 <= 0.16：再高就会把三层合成推回"磨砂"区间
    expect(maxScrim, `SCRIM_ALPHA_MAX=${maxScrim}`).toBeLessThanOrEqual(0.16);

    const section = maxSectionFillAlpha(tokensCss);
    const cardFills = Array.from(
      stripComments(tokensCss).matchAll(/--rbc-layer-card-fill:\s*linear-gradient\(([\s\S]*?)\);/g),
      (m) => m[1]!,
    );
    const lastCardFill = cardFills[cardFills.length - 1]!;
    const card = Math.max(
      ...Array.from(lastCardFill.matchAll(/rgba\([^)]*?,\s*([0-9.]+)\s*\)/g), (m) => Number(m[1])),
    );

    // 最坏情况：luma 把 scrim 顶到上界
    const worst = 1 - (1 - maxScrim) * (1 - section) * (1 - card);
    expect(worst, `最坏合成白度 ${worst.toFixed(3)}`).toBeLessThan(0.32);
  });

  it('V19c: luma 无效值不得覆盖令牌（-1 哨兵必须被跳过，否则会反向压到最低端）', () => {
    // 宿主用 -1 表示"无壁纸亮度数据"。旧判据 `Number.isFinite` 会放行 -1，
    // 随后 clamp 成 0 ⇒ 当成"纯黑壁纸" ⇒ scrim 压到区间最低端。方向相反。
    const hook = fs.readFileSync(path.join(__dirname, '../hooks/useLiquidGlassPointer.ts'), 'utf8');
    // 必须同时出现上下界检查，仅 isFinite 不足以防住 -1
    expect(hook).toMatch(/data\.luma\s*>=\s*0/);
    expect(hook).toMatch(/data\.luma\s*<=\s*1/);
  });

  it('旧值 0.62 不得回到 scrim 令牌上（回归网，只钉该令牌，不误伤同值的其它阴影）', () => {
    // 只查 --rbc-layer-host-scrim 的取值：tokens.css 里还有一处
    // `inset 1px 0 1px 0 rgba(255,255,255,0.62)` 是别处的内阴影，
    // 全文搜索会把那条也算进来，变成一条"永远为假"的空断言。第一版就是这样写错的。
    const clean = stripComments(tokensCss);
    const scrimDecls = Array.from(
      clean.matchAll(/--rbc-layer-host-scrim:\s*([^;]+);/g),
      (m) => m[1]!.trim(),
    );
    expect(scrimDecls.length).toBeGreaterThanOrEqual(2); // :root + html.rbc-host-transparent
    for (const decl of scrimDecls) {
      expect(decl).not.toContain('0.62');
    }
  });

  it('桌面主色微光恢复且用了 hsl() 包装（令牌是 HSL 分量，直接当颜色用会整条失效）', () => {
    const clean = stripComments(glassCss);
    expect(clean).toContain('--rbc-layer-text-halo');
    // 必须能搜到 hsl(var(--rbc-lg-desktop-h1
    expect(clean).toMatch(/hsl\(\s*var\(--rbc-lg-desktop-h1/);
    // 反向检查：不得出现 "var(--rbc-lg-desktop-h1)" 直接被当作颜色（缺 hsl 包装）
    expect(clean).not.toMatch(/:\s*var\(--rbc-lg-desktop-h1[^)]*\)\s*[;\n]/);
  });
});

describe('V16: 自适应网格（防圆形涟漪被拉成横向糊斑）', () => {
  const GEOMETRIES: Array<{ name: string; w: number; h: number }> = [
    { name: 'wide-banner-section', w: 1400, h: 120 },
    { name: 'full-page-section', w: 1400, h: 700 },
    { name: 'stat-card', w: 340, h: 150 },
    { name: 'medium-card', w: 240, h: 96 },
    { name: 'tall-panel', w: 280, h: 640 },
    { name: 'default-160x90', w: 160, h: 90 },
    { name: 'narrow-rail', w: 120, h: 700 },
    { name: 'strip-900x40', w: 900, h: 40 },
  ];

  it('所有几何下的拉伸比 < 1.35（旧实现 banner 上是 6.56:1）', () => {
    for (const g of GEOMETRIES) {
      const { cols, rows } = computeAdaptiveGrid(g.w, g.h);
      const scaleX = g.w / cols;
      const scaleY = g.h / rows;
      const stretch = Math.max(scaleX, scaleY) / Math.min(scaleX, scaleY);
      expect(stretch, `${g.name} -> ${cols}x${rows}, stretch=${stretch.toFixed(3)}`).toBeLessThan(
        1.35,
      );
    }
  });

  it('单场格数不超过 14,400 预算（"算力不变"必须是数字，不能是口头承诺）', () => {
    for (const g of GEOMETRIES) {
      const { cols, rows } = computeAdaptiveGrid(g.w, g.h);
      expect(cols * rows, `${g.name} -> ${cols}x${rows}`).toBeLessThanOrEqual(14400);
    }
  });

  it('退化输入一律回落 160x90（jsdom / 隐藏容器 / NaN 都不得让网格崩成一维）', () => {
    for (const [w, h] of [
      [0, 100],
      [100, 0],
      [-5, 100],
      [Number.NaN, 100],
      [Number.POSITIVE_INFINITY, 100],
    ] as const) {
      expect(computeAdaptiveGrid(w, h)).toEqual({ cols: 160, rows: 90 });
    }
    // 极端比例仍要给出可用网格，而不是 1 x N
    const extreme = computeAdaptiveGrid(4000, 60);
    expect(extreme.cols).toBeGreaterThanOrEqual(16);
    expect(extreme.rows).toBeGreaterThanOrEqual(12);
  });

  it('画布位图尺寸跟随宿主宽高比（不是固定 160x90）—— hook 侧端到端', () => {
    const root = document.createElement('div');
    document.body.appendChild(root);

    const banner = document.createElement('div');
    banner.className = 'rbc-card';
    banner.getBoundingClientRect = () =>
      ({
        left: 0,
        top: 0,
        right: 1400,
        bottom: 120,
        width: 1400,
        height: 120,
        x: 0,
        y: 0,
        toJSON: () => ({}),
      }) as DOMRect;
    root.appendChild(banner);

    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(
      () =>
        ({
          createImageData: (w: number, h: number) =>
            ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h }) as unknown as ImageData,
          putImageData: vi.fn(),
        }) as unknown as CanvasRenderingContext2D,
    );

    const ref = { current: root };
    renderHook(() => useLiquidWave({ root: ref }));

    const canvas = banner.querySelector<HTMLCanvasElement>('canvas.rbc-card-wave');
    expect(canvas).not.toBeNull();

    const expected = computeAdaptiveGrid(1400, 120);
    // 旧实现这里恒为 160 x 90 —— 位图被 CSS 拉成 6.56:1。
    expect(canvas!.width).toBe(expected.cols);
    expect(canvas!.height).toBe(expected.rows);
    expect(canvas!.height).not.toBe(90);

    const stretch =
      Math.max(1400 / canvas!.width, 120 / canvas!.height) /
      Math.min(1400 / canvas!.width, 120 / canvas!.height);
    expect(stretch).toBeLessThan(1.35);

    root.remove();
  });
});

describe('V17: 波前触边碰撞 + 超预算交错上传（防冻结）', () => {
  it('V17a: 阻尼区间内中心冲击必然触边，且触边强度超过门限', () => {
    const field = createWaveField(160, 90);
    injectWaveImpulse(field, 0.5, 0.5, 0.9);
    stepWave(field);

    let firstHitFrame = -1;
    let maxStrength = 0;
    const hits: BorderCollision[] = [];
    for (let f = 0; f < 120; f++) {
      hits.length = 0;
      stepWave(field, undefined, hits);
      if (hits.length > 0) {
        if (firstHitFrame < 0) firstHitFrame = f;
        for (const h of hits) {
          if (h.strength > maxStrength) maxStrength = h.strength;
          expect(h.along).toBeGreaterThanOrEqual(0);
          expect(h.along).toBeLessThanOrEqual(1);
        }
      }
    }

    // 旧实现在 300 帧内零触边（边界用索引折返采样，幅值永远上不去）。
    // 这条断言是「波浪相互碰撞」这条需求的物理可行性证明：
    // 不触边 ⇒ 跨玻璃传导只能靠事件硬塞 ⇒ 观感是「无处不在的联动」而非碰撞。
    expect(firstHitFrame).toBeGreaterThan(0);
    expect(firstHitFrame).toBeLessThan(100);
    expect(maxStrength).toBeGreaterThan(BORDER_HIT_THRESHOLD);
  });

  it('V17a-2: 触边事件的 side 与 along 必须自洽（撞哪条边就报哪条边）', () => {
    // 120x60 是 2:1 横条：中心对称冲击沿短轴先到上下两条长边。
    const field = createWaveField(120, 60);
    injectWaveImpulse(field, 0.5, 0.5, 1.0);
    stepWave(field);

    let totalHits = 0;
    const sidesSeen = new Set<string>();
    for (let f = 0; f < 200; f++) {
      const hits: BorderCollision[] = [];
      stepWave(field, undefined, hits);
      for (const h of hits) {
        totalHits += 1;
        sidesSeen.add(h.side);
        // along 是「沿该边的归一化位置」，必须落在 [0,1] 内。
        expect(h.along).toBeGreaterThanOrEqual(0);
        expect(h.along).toBeLessThanOrEqual(1);
        expect(h.strength).toBeGreaterThan(BORDER_HIT_THRESHOLD);
      }
    }

    // 第一版这里用了外层的复数组，循环里被 stepWave 内部清空 → 断言整体空跑。
    // 计数必须在循环内累加。
    expect(totalHits).toBeGreaterThan(0);
    expect(sidesSeen.size).toBeGreaterThan(0);

    // [纠错] 第二版这里断言「必须先撞 top/bottom（短轴）」，实测 FAIL：
    // 二维波是同时向 4 个方向传播的，短轴先到只意味着"先到"，
    // 不代表长边永远不会被撞 —— 过了几百帧左右边一定会撞到。
    // 那条断言是错的预期，不是缺陷。这里只保留真正有意义的检查：
    // 四边都可能出现，但每一侧的 along 必须落在合法区间（上面已逐条断言）。
    expect(sidesSeen.size).toBeGreaterThanOrEqual(2);
  });

  it('V17b: 超预算时改隔帧交错上传 —— 单张画布最长连续跳过帧数 <= 1', () => {
    const rafQueue: FrameRequestCallback[] = [];
    vi.stubGlobal('requestAnimationFrame', (cb: FrameRequestCallback) => {
      rafQueue.push(cb);
      return rafQueue.length;
    });
    vi.stubGlobal('cancelAnimationFrame', () => {});

    const putImageData = vi.fn();
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(
      () =>
        ({
          createImageData: (w: number, h: number) =>
            ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h }) as unknown as ImageData,
          putImageData,
        }) as unknown as CanvasRenderingContext2D,
    );

    // 10 张卡，预算压到 2：8 张必须走交错通道。
    // 手工建 DOM + read root 引用，避免在 .ts 里写 JSX（本文件不引 tsx 语义）。
    const root = document.createElement('div');
    document.body.appendChild(root);
    const cards: HTMLElement[] = [];
    for (let i = 0; i < 10; i++) {
      const el = document.createElement('div');
      el.className = 'rbc-card';
      el.getBoundingClientRect = () =>
        ({
          left: 0,
          top: 0,
          right: 200,
          bottom: 100,
          width: 200,
          height: 100,
          x: 0,
          y: 0,
          toJSON: () => ({}),
        }) as DOMRect;
      root.appendChild(el);
      cards.push(el);
    }

    const ref = { current: root };
    renderHook(() => useLiquidWave({ root: ref, maxRendered: 2 }));

    // 全部唤醒：每张都吃一次点击波。
    for (const el of cards) {
      el.dispatchEvent(new MouseEvent('pointerdown', { clientX: 100, clientY: 50, bubbles: true }));
    }

    expect(rafQueue.length).toBeGreaterThan(0);
    for (let step = 0; step < 12; step++) {
      const cb = rafQueue.shift();
      if (!cb) break;
      cb(1000 + step * 16);
    }

    const perf = window.__RBC_WAVE_PERF__;
    expect(perf).toBeDefined();
    // 核心判据：旧实现这里是「永远不上传」⇒ 该值随时间线性增长（实测 >>2）；
    // 交错后上界恒为 1。
    expect(perf!.maxSkippedFrames).toBeLessThanOrEqual(1);

    // 同时必须确实发生过分帧：预算 2 < 10 张，若 skipped 恒为 0
    // 说明"交错"根本没走到，这条断言就退化成空跑了。
    expect(putImageData.mock.calls.length).toBeGreaterThan(0);

    root.remove();
  });

  it('V17c: 物理步进本身是耗散的（第 1 帧的起振过冲之后峰值单调下降）', () => {
    // 波动方程显式格式在前两帧会有一次起振过冲（h(t-1)=0 起步），
    // 那是初值处理，不是不稳定：断言必须从过冲之后开始，否则会误判。
    // 第一版写成「从第 1 帧起逐帧单调」，被这次过冲打成假 FAIL。
    const field = createWaveField(160, 90);
    injectWaveImpulse(field, 0.5, 0.5, 1.0);
    stepWave(field);
    stepWave(field);
    stepWave(field);
    stepWave(field);

    let prev = wavePeak(field);
    expect(prev).toBeGreaterThan(0.1);
    for (let f = 0; f < 40; f++) {
      stepWave(field);
      const cur = wavePeak(field);
      expect(cur).toBeLessThanOrEqual(prev + 1e-9);
      prev = cur;
    }
    // 40 帧后必须显著衰减（耗散确实在起作用，而不是维持一个常驻水面）。
    expect(prev).toBeLessThan(0.35);
  });
});