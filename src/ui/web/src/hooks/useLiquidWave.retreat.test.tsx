// @vitest-environment jsdom
// src/ui/web/src/hooks/useLiquidWave.retreat.test.ts
// [299 2026-11-19]
//
// 判据：原生 D3D11+DComp 宿主开闸后，CPU 水波必须**真的**退让。
//
// 为什么要专门为这一条写测试
// --------------------------
// 295 Wave 1 的教训就是「单测全绿、运行期零生效」：useNativeGlassSync.ts 与它的
// 14 条单测都已落盘，但 App.tsx 一行都没调用，只有它自己的 .test.ts 在 import 它。
// 那种状态下 156 条测试照样全绿 —— 因为**没有任何一条测试看 App.tsx**（实测：
// `grep -rn "app/App" src --include=*.test.tsx` 零命中）。
//
// 所以本轮补两层判据，缺一不可：
//   A) 行为层：退让逻辑本身成立（本文件）。
//   B) 接线层：App.tsx 真的调用了它（appWiring.contract.test.ts）。
// 只做 A 会重蹈覆辙 —— 逻辑对、但没人调用。
//
// 断言全部落在可观测副作用上（canvas 是否存在、rAF 是否被排），不做像素采样。

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { cleanup, render } from '@testing-library/react';
import React, { useRef } from 'react';
import { useLiquidWave } from './useLiquidWave';

let rafQueue: Array<{ id: number; cb: FrameRequestCallback }>;
let rafSeq: number;

const stubMatchMedia = () => {
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
};

const pumpFrame = (t: number) => {
  const pending = rafQueue;
  rafQueue = [];
  for (const entry of pending) entry.cb(t);
};

const Harness: React.FC = () => {
  const rootRef = useRef<HTMLDivElement | null>(null);
  const setRoot = (el: HTMLDivElement | null) => {
    // useRef 的 .current 是 mutable 的（只有 RefObject 视图才是 readonly），
    // 这里刻意用 useRef<HTMLDivElement | null> 以便直接赋值。
    (rootRef as { current: HTMLDivElement | null }).current = el;
    if (el) mockBox(el, 800, 600);
  };
  useLiquidWave({ root: rootRef });
  return (
    <div ref={setRoot}>
      <section
        className="rbc-section"
        data-testid="sec"
        // jsdom 的 getBoundingClientRect 恒为 0×0，而 onPointerMove 在
        // rect.width <= 0 时直接 return（不注波、不 ensureLoop）—— 不补这个
        // 矩形，本测试就会变成「恒不排 rAF」的空跑。
        ref={(el) => {
          if (el) mockBox(el);
        }}
      >
        <div
          className="rbc-card"
          data-testid="cardA"
          ref={(el) => {
            if (el) mockBox(el);
          }}
        />
      </section>
    </div>
  );
};

function mockBox(el: HTMLElement, w = 400, h = 200) {
  el.getBoundingClientRect = () =>
    ({
      left: 0,
      top: 0,
      right: w,
      bottom: h,
      width: w,
      height: h,
      x: 0,
      y: 0,
      toJSON: () => ({}),
    }) as DOMRect;
}

const canvases = () => document.querySelectorAll('canvas.rbc-card-wave').length;

const fire = (target: HTMLElement, x: number, y: number) => {
  target.dispatchEvent(
    new MouseEvent('pointermove', { clientX: x, clientY: y, bubbles: true, cancelable: true }),
  );
};

beforeEach(() => {
  rafQueue = [];
  rafSeq = 1;
  document.documentElement.classList.remove('rbc-host-dcomp');
  stubMatchMedia();
  vi.stubGlobal('requestAnimationFrame', (cb: FrameRequestCallback) => {
    const id = rafSeq++;
    rafQueue.push({ id, cb });
    return id;
  });
  vi.stubGlobal('cancelAnimationFrame', (id: number) => {
    rafQueue = rafQueue.filter((e) => e.id !== id);
  });
  vi.spyOn(performance, 'now').mockImplementation(() => 1000);
  // jsdom 没有 canvas 2D 实现，getContext('2d') 返回 null 会让 attachSurface 提前
  // return（那样这条测试就变成空跑）。必须补最小可用的 stub ——
  // 与 useLiquidWave.coupling.test.tsx 的做法一致。
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(
    () =>
      ({
        createImageData: (w: number, h: number) =>
          ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h }) as unknown as ImageData,
        putImageData: vi.fn(),
      }) as unknown as CanvasRenderingContext2D,
  );
});

afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  document.documentElement.classList.remove('rbc-host-dcomp');
});

describe('useLiquidWave 在原生 DComp 宿主下退让', () => {
  it('R0 对照组：没有 rbc-host-dcomp 时正常挂 canvas（证明判据不是空跑）', () => {
    const { getByTestId } = render(<Harness />);
    expect(canvases()).toBe(2); // section + card
    fire(getByTestId('sec'), 10, 10);
    expect(rafQueue.length).toBeGreaterThan(0);
  });

  it('R1 effect 入口就已是原生宿主 => 一个 canvas 都不挂、不排 rAF', () => {
    document.documentElement.classList.add('rbc-host-dcomp');
    const { getByTestId } = render(<Harness />);
    expect(canvases()).toBe(0);
    fire(getByTestId('sec'), 10, 10);
    expect(rafQueue.length).toBe(0);
  });

  it('R2 运行中途才加 rbc-host-dcomp（真实时序）=> 立即拆 canvas 并归零遥测', async () => {
    const { getByTestId } = render(<Harness />);
    expect(canvases()).toBe(2);
    fire(getByTestId('sec'), 10, 10);
    expect(rafQueue.length).toBeGreaterThan(0);

    // 宿主握手完成 —— 类被异步加上；MutationObserver 是微任务，等一拍
    await vi.waitFor(() => {
      document.documentElement.classList.add('rbc-host-dcomp');
      expect(canvases()).toBe(0);
    });

    // 清扫后 rAF 队列必须清空，遥测归零（否则残留 awakeCount > 0 是假读数）
    expect(rafQueue.length).toBe(0);
    expect(window.__RBC_WAVE_PERF__?.awakeCount ?? 0).toBe(0);

    // 再划过也不得重新挂 canvas / 重新起 loop
    fire(getByTestId('sec'), 20, 20);
    expect(canvases()).toBe(0);
    expect(rafQueue.length).toBe(0);
  });

  it('R3 撤掉类后仍可恢复（退让不是单向自毁）', async () => {
    document.documentElement.classList.add('rbc-host-dcomp');
    const { getByTestId } = render(<Harness />);
    expect(canvases()).toBe(0);

    await vi.waitFor(() => {
      document.documentElement.classList.remove('rbc-host-dcomp');
      // 撤类后下一次 pointermove 应当重新建 canvas
      fire(getByTestId('sec'), 30, 30);
      expect(canvases()).toBeGreaterThan(0);
    });
    expect(rafQueue.length).toBeGreaterThan(0);
  });

  /**
   * R4 tick 体内的退让守卫（M3 变异抓出的缺口）
   *
   * R1/R2 只能证明「canvas 被拆了 / 入口守卫生效」，**证明不了 tick 内部**。
   * 实测：把 tick 里那段 `if (isNativeDcompHost()) { ... return; }` 改成
   * `if (false)` 之后，R1-R3 仍然 4/4 全绿 —— 因为拆完 canvas 之后根本没有
   * surface 可供 tick 处理，tick 早就因 awake.length === 0 自行返回了。
   *
   * 所以必须构造一个「tick 已经在飞、surface 还在」的窗口，才能在开闸后
   * 观察 tick 是否真的停下。造法：不靠 class 触发拆解，直接让 tick 跑到一半
   * 时开闸 —— 用 pumpFrame 手动驱动帧。
   */
  it('R4 tick 正在跑时开闸 => 该帧起就不再 stepWave（M3 变异必须被这里抓住）', () => {
    const { getByTestId } = render(<Harness />);
    const sec = getByTestId('sec');
    expect(canvases()).toBe(2);

    // 起波并让 loop 挂上
    fire(sec, 10, 10);
    expect(rafQueue.length).toBeGreaterThan(0);

    // 手动驱动一帧：此时 tick 正常跑（对照）
    pumpFrame(1000);
    // 帧内会继续排下一帧（仍有 awake）
    expect(rafQueue.length).toBeGreaterThan(0);

    // 现在开闸。注意**不**去触发 MutationObserver 的拆解路径 —— 我们要测的是
    // tick 自己会不会停。把观察器挪开的办法是直接改类并同步 pumpFrame，
    // 而不是 await（await 会让微任务先跑、canvas 先被拆掉，就测不到 tick 了）。
    document.documentElement.classList.add('rbc-host-dcomp');
    const before = rafQueue.length;
    pumpFrame(1016);

    // tick 内守卫命中 => 不再排下一帧（rafId = null），队列必须清空
    expect(rafQueue.length, 'tick 开闸后不得再排帧').toBe(0);
    expect(before).toBeGreaterThan(0);
  });
});