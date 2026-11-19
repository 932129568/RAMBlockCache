// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { renderHook } from '@testing-library/react';
import type { RefObject } from 'react';
import { useLiquidWave } from './useLiquidWave';
import { computeAdaptiveGrid } from '../lib/liquidWaveCore';

let root: HTMLDivElement;
let ref: RefObject<HTMLElement | null>;
let rafQueue: Array<{ id: number; cb: FrameRequestCallback }>;
let rafSeq: number;
let putImageData: ReturnType<typeof vi.fn>;

const stubMatchMedia = (reducedMotion: boolean) => {
  vi.stubGlobal('matchMedia', (query: string) => ({
    matches: reducedMotion && query.includes('prefers-reduced-motion'),
    media: query,
    onchange: null,
    addListener: () => {},
    removeListener: () => {},
    addEventListener: () => {},
    removeEventListener: () => {},
    dispatchEvent: () => false,
  }));
};

/** 手动推进一帧；timestamps 由测试给，避免依赖真实时钟。 */
const pumpFrame = (t: number) => {
  const pending = rafQueue;
  rafQueue = [];
  for (const entry of pending) entry.cb(t);
};

const makeCard = (parent: HTMLElement): HTMLDivElement => {
  const card = document.createElement('div');
  card.className = 'rbc-card';
  card.getBoundingClientRect = () =>
    ({ left: 0, top: 0, width: 200, height: 100, right: 200, bottom: 100, x: 0, y: 0, toJSON: () => ({}) }) as DOMRect;
  parent.appendChild(card);
  return card;
};

const pointerMove = (target: HTMLElement, x: number, y: number) => {
  target.dispatchEvent(new MouseEvent('pointermove', { clientX: x, clientY: y, bubbles: true }));
};

beforeEach(() => {
  rafQueue = [];
  rafSeq = 1;
  stubMatchMedia(false);
  vi.stubGlobal('requestAnimationFrame', (cb: FrameRequestCallback) => {
    const id = rafSeq++;
    rafQueue.push({ id, cb });
    return id;
  });
  vi.stubGlobal('cancelAnimationFrame', (id: number) => {
    rafQueue = rafQueue.filter((e) => e.id !== id);
  });

  putImageData = vi.fn();
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(
    () =>
      ({
        createImageData: (w: number, h: number) =>
          ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h }) as unknown as ImageData,
        putImageData,
      }) as unknown as CanvasRenderingContext2D,
  );

  root = document.createElement('div');
  root.className = 'rbc-app-bg';
  document.body.appendChild(root);
  ref = { current: root };
});

afterEach(() => {
  root.remove();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe('useLiquidWave', () => {
  it('prefers-reduced-motion 下完全不挂载 canvas 与事件', () => {
    stubMatchMedia(true);
    makeCard(root);
    renderHook(() => useLiquidWave({ root: ref }));

    expect(root.querySelectorAll('canvas.rbc-card-wave')).toHaveLength(0);
    pointerMove(root.querySelector('.rbc-card') as HTMLElement, 10, 10);
    expect(rafQueue).toHaveLength(0);
  });

  it('每张卡片挂一张隐藏画布，指针移动后才唤醒', () => {
    makeCard(root);
    makeCard(root);
    renderHook(() => useLiquidWave({ root: ref }));

    const canvases = root.querySelectorAll<HTMLCanvasElement>('canvas.rbc-card-wave');
    expect(canvases).toHaveLength(2);
    for (const c of Array.from(canvases)) {
      expect(c.style.opacity).toBe('0');
      expect(c.style.mixBlendMode).toBe('screen');
      expect(c.style.pointerEvents).toBe('none');
      // [v23] 位图尺寸不再固定 160x90，而是按宿主 200x100 的宽高比分配。
      // 判据从"等于某个魔数"改成"等于 computeAdaptiveGrid 的输出"：
      // 真正要钉的是"位图宽高比跟随宿主"，不是那对数字。
      const expected = computeAdaptiveGrid(200, 100);
      expect(c.width).toBe(expected.cols);
      expect(c.height).toBe(expected.rows);
      // 反例守卫：若有人把自适应改成"恒等于 160x90 再 CSS 拉伸"，
      // 200x100 宿主上拉伸比会是 1.25:1，而自适应给出 ~1.0 —— 这里必须看到差异。
      const stretch =
        Math.max(200 / c.width, 100 / c.height) / Math.min(200 / c.width, 100 / c.height);
      expect(stretch).toBeLessThan(1.05);
    }

    pointerMove(root.querySelector('.rbc-card') as HTMLElement, 100, 50);
    expect(rafQueue.length).toBeGreaterThan(0);

    pumpFrame(16);
    const first = root.querySelector<HTMLCanvasElement>('canvas.rbc-card-wave');
    expect(first?.style.opacity).toBe('1');
    expect(putImageData).toHaveBeenCalled();
  });

  it('波场真归静后才休眠，且不会在还有残留时提前强杀（旧实现 1.2s 硬切回归网）', () => {
    makeCard(root);
    renderHook(() => useLiquidWave({ root: ref }));
    const card = root.querySelector('.rbc-card') as HTMLElement;

    // [可测性] 两个时钟必须一起推进：事件时间取 performance.now()，
    // 帧时间取 rAF 参数。只推 rAF 参数的话空闲计时永远停在 0，
    // 「不许提前强杀」这条判据就是空跑（第一版正是这样写的，自测没发现）。
    let fakeNow = 1000;
    vi.spyOn(performance, 'now').mockImplementation(() => fakeNow);

    // 用真实的高速滑动起波（单次 pointermove 首帧速度=0，只注入最低幅度
    // 0.135，那种弱激励 ~50 帧就自然消失，测不到"提前强杀"这条缺陷）。
    // 200x100 卡片上 10px -> 190px / 16ms ⇒ ~11250 px/s ⇒ 幅度打满。
    card.dispatchEvent(new MouseEvent('pointermove', { clientX: 10, clientY: 50, bubbles: true }));
    fakeNow = 1016;
    card.dispatchEvent(new MouseEvent('pointermove', { clientX: 190, clientY: 50, bubbles: true }));

    const canvas = root.querySelector<HTMLCanvasElement>('canvas.rbc-card-wave');
    expect(canvas?.style.opacity).toBe('1');

    let t = 1016;
    const advance = (frames: number) => {
      for (let i = 0; i < frames; i++) {
        t += 16;
        fakeNow = t;
        pumpFrame(t);
      }
    };

    // 第 1 段（缺陷回归网）：此刻离最后一次输入已 1.2~2.2 s，
    // 高速滑动注入的波仍明显可见。旧实现（IDLE_SLEEP_MS=1200 单条件）
    // 会在这一区间把画布清掉 —— 那就是「卡住然后突然整块消失」。
    advance(110); // 累计 1.76 s
    expect(canvas?.style.opacity).toBe('1');
    expect(rafQueue.length).toBeGreaterThan(0);

    // 第 2 段：继续推进到 ~3 s，笔一笔也画不出来了 ⇒ 必须入睡、rAF 归零。
    advance(120);
    expect(canvas?.style.opacity).toBe('0');
    expect(rafQueue).toHaveLength(0);
    expect(window.__RBC_WAVE_PERF__?.awakeCount).toBe(0);
  });

  it('页面新插入的卡片会被重新扫描（lazy route 之后仍有波纹）', async () => {
    renderHook(() => useLiquidWave({ root: ref }));
    expect(root.querySelectorAll('canvas.rbc-card-wave')).toHaveLength(0);

    makeCard(root);
    await new Promise((r) => setTimeout(r, 0));

    expect(root.querySelectorAll('canvas.rbc-card-wave')).toHaveLength(1);
  });

  it('卸载时移除画布、还原临时 position，并停止响应指针', () => {
    vi.spyOn(window, 'getComputedStyle').mockReturnValue({ position: 'static' } as CSSStyleDeclaration);
    makeCard(root);
    const card = root.querySelector('.rbc-card') as HTMLElement;
    const { unmount } = renderHook(() => useLiquidWave({ root: ref }));

    expect(card.querySelector('canvas.rbc-card-wave')).not.toBeNull();
    expect(card.style.position).toBe('relative');

    unmount();

    expect(root.querySelectorAll('canvas.rbc-card-wave')).toHaveLength(0);
    expect(card.style.position).toBe('');
    pointerMove(card, 10, 10);
    expect(rafQueue).toHaveLength(0);
  });
});
