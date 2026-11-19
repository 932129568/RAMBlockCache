// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { cleanup, renderHook } from '@testing-library/react';
import type { RefObject } from 'react';
import { useLiquidGlassPointer } from './useLiquidGlassPointer';

let root: HTMLDivElement;
let card: HTMLDivElement;
let ref: RefObject<HTMLElement | null>;

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
  // Run the rAF callback synchronously so the CSS custom properties are
  // observable immediately after dispatching a pointermove event.
  vi.stubGlobal('requestAnimationFrame', (cb: FrameRequestCallback) => {
    cb(0);
    return 1;
  });
  vi.stubGlobal('cancelAnimationFrame', () => {});

  root = document.createElement('div');
  card = document.createElement('div');
  card.className = 'rbc-section';
  root.appendChild(card);
  document.body.appendChild(root);
  ref = { current: root };
});

afterEach(() => {
  document.body.innerHTML = '';
  // The hook writes on documentElement, which survives body.innerHTML = ''.
  document.documentElement.style.removeProperty('--rbc-lg-desktop-h1');
  document.documentElement.style.removeProperty('--rbc-lg-desktop-h2');
  document.documentElement.classList.remove('rbc-host-transparent');
  delete (window as unknown as { chrome?: unknown }).chrome;
  vi.unstubAllGlobals();
});

describe('useLiquidGlassPointer', () => {
  it('writes pointer coordinates onto the hovered .rbc-section', () => {
    renderHook(() => useLiquidGlassPointer(ref));
    card.dispatchEvent(
      new MouseEvent('pointermove', { bubbles: true, clientX: 12, clientY: 34 }),
    );
    expect(card.style.getPropertyValue('--rbc-lg-ptr-x')).toBe('12px');
    expect(card.style.getPropertyValue('--rbc-lg-ptr-y')).toBe('34px');
  });

  it('ignores pointer moves when reduced motion is preferred', () => {
    vi.stubGlobal('matchMedia', (query: string) => ({
      matches: true,
      media: query,
      onchange: null,
      addListener: () => {},
      removeListener: () => {},
      addEventListener: () => {},
      removeEventListener: () => {},
      dispatchEvent: () => false,
    }));
    renderHook(() => useLiquidGlassPointer(ref));
    card.dispatchEvent(
      new MouseEvent('pointermove', { bubbles: true, clientX: 12, clientY: 34 }),
    );
    expect(card.style.getPropertyValue('--rbc-lg-ptr-x')).toBe('');
  });

  it('applies desktop accent values pushed by the WPF WebView2 host', () => {
    const webview = new EventTarget();
    Object.defineProperty(window, 'chrome', {
      value: { webview },
      configurable: true,
      writable: true,
    });
    renderHook(() => useLiquidGlassPointer(ref));
    webview.dispatchEvent(
      new MessageEvent('message', {
        data: { type: 'rbc-desktop-accent', h1: '206 88% 54%', h2: '262 60% 62%' },
      }),
    );
    expect(document.documentElement.style.getPropertyValue('--rbc-lg-desktop-h1')).toBe('206 88% 54%');
    expect(document.documentElement.style.getPropertyValue('--rbc-lg-desktop-h2')).toBe('262 60% 62%');
  });

  // [U3] 宿主真透色开闸：只有收到桌面色消息才加类，浏览器直开时类必须不存在，
  // 否则网页端会退化成"透明底 + 桌面不存在"的白洞。
  it('opens host-transparent only after a desktop accent message', () => {
    const webview = new EventTarget();
    Object.defineProperty(window, 'chrome', {
      value: { webview },
      configurable: true,
      writable: true,
    });
    renderHook(() => useLiquidGlassPointer(ref));
    expect(document.documentElement.classList.contains('rbc-host-transparent')).toBe(false);

    webview.dispatchEvent(
      new MessageEvent('message', { data: { type: 'rbc-desktop-accent', h1: '206 88% 54%' } }),
    );
    expect(document.documentElement.classList.contains('rbc-host-transparent')).toBe(true);
  });

  it('does not open host-transparent without a WebView2 host (browser fallback)', () => {
    renderHook(() => useLiquidGlassPointer(ref));
    window.dispatchEvent(
      new MessageEvent('message', { data: { type: 'rbc-desktop-accent', h1: '206 88% 54%' } }),
    );
    expect(document.documentElement.classList.contains('rbc-host-transparent')).toBe(false);
  });

  it('ignores unrelated WebView2 messages', () => {
    const webview = new EventTarget();
    Object.defineProperty(window, 'chrome', {
      value: { webview },
      configurable: true,
      writable: true,
    });
    renderHook(() => useLiquidGlassPointer(ref));
    webview.dispatchEvent(
      new MessageEvent('message', { data: { type: 'other', h1: '0 0% 0%' } }),
    );
    expect(document.documentElement.style.getPropertyValue('--rbc-lg-desktop-h1')).toBe('');
  });

  it('detaches listeners on unmount', () => {
    const { unmount } = renderHook(() => useLiquidGlassPointer(ref));
    unmount();
    card.dispatchEvent(
      new MouseEvent('pointermove', { bubbles: true, clientX: 7, clientY: 9 }),
    );
    expect(card.style.getPropertyValue('--rbc-lg-ptr-x')).toBe('');
  });

  /* ======================================================================
   * [284 2026-11-19] W2: 修复「永远蓝白色底板」的竞态判据
   *
   * 真因：WPF 在 NavigationCompleted 立刻 PostWebMessageAsJson，而
   * chrome.webview.addEventListener('message') 是在本 hook 的 effect 里注册的。
   * 那一刻还没注册 ⇒ 消息被丢弃 ⇒ rbc-host-transparent 永不加 ⇒ 页面停在
   * 不透明的 #f3f5f9 上。用户看到的是"不能透色、还是蓝白色底板"。
   *
   * 修法不是猜延时，而是把消息变成可重放：前端一挂载就上报 rbc-web-ready，
   * 宿主收到后补发同一份 payload。下面两条判据分别钉住"上报"与"重放后生效"。
   * ====================================================================== */

  it('W2a: 宿主存在时挂载完成必须上报 rbc-web-ready（否则宿主不知道该补发）', () => {
    const posted: unknown[] = [];
    const webview = new EventTarget() as EventTarget & {
      postMessage?: (m: unknown) => void;
    };
    webview.postMessage = (m: unknown) => posted.push(m);
    Object.defineProperty(window, 'chrome', {
      value: { webview },
      configurable: true,
      writable: true,
    });

    renderHook(() => useLiquidGlassPointer(ref));

    expect(posted).toEqual([{ type: 'rbc-web-ready' }]);
  });

  it('W2b: 浏览器直开（无 chrome.webview）时不得上报任何消息', () => {
    // 这条防的是"在网页形态下也去发 rbc-web-ready"——那会让桥接/iframe 收到
    // 一个没人处理的控制消息，属于无意义副作用。
    const posted: unknown[] = [];
    const webview = new EventTarget() as EventTarget & {
      postMessage?: (m: unknown) => void;
    };
    webview.postMessage = (m: unknown) => posted.push(m);
    // 不挂 window.chrome

    renderHook(() => useLiquidGlassPointer(ref));

    expect(posted).toEqual([]);
  });

  it('W2c: 宿主补发的消息必须真的把透色类挂上（重放路径端到端）', () => {
    const webview = new EventTarget() as EventTarget & {
      postMessage?: (m: unknown) => void;
    };
    webview.postMessage = () => {};
    Object.defineProperty(window, 'chrome', {
      value: { webview },
      configurable: true,
      writable: true,
    });

    renderHook(() => useLiquidGlassPointer(ref));
    // 模拟宿主收到 rbc-web-ready 之后的补发
    webview.dispatchEvent(
      new MessageEvent('message', {
        data: { type: 'rbc-desktop-accent', h1: '196 72% 46%', h2: '262 60% 62%', luma: 0.2 },
      }),
    );

    expect(document.documentElement.classList.contains('rbc-host-transparent')).toBe(true);
    expect(document.documentElement.style.getPropertyValue('--rbc-lg-desktop-h1')).toBe('196 72% 46%');
    /* [292 2026-11-19] 判据重写：旧断言是 `0\.1[0-9]+`（配 0.10..0.38 的旧区间），
     * 它有两个问题：
     *   1) 只钉"落在一个数字区间里"，不钉**插值方向**。方向反了（越亮越盖）
     *      仍然满足 0.1x，所以它抓不到 292 修的那个缺陷。
     *   2) 区间数字写死在正则里，端点一改就得跟着改，改的时候很容易顺手放宽。
     *
     * 现在按公式钉死：alpha = MAX - luma × (MAX - MIN)，端点 0.04 / 0.14。
     * luma=0.2 ⇒ 0.14 - 0.2 × 0.10 = 0.12。
     * 若有人把方向改回去 ⇒ 0.04 + 0.02 = 0.06，本断言立即失败。
     * 若有人把端点改回 0.38 ⇒ 0.38 - 0.2 × 0.28 = 0.324，本断言立即失败。 */
    const scrim = document.documentElement.style.getPropertyValue('--rbc-layer-host-scrim');
    expect(scrim).toBe('rgba(255, 255, 255, 0.120)');
  });

  it('[292] luma 越低白纱越厚（方向守卫：深色正文需要"暗壁纸多盖"）', () => {
    // 独立的方向守卫，不依赖具体端点数：只比较两个 luma 的相对大小。
    // 这样即使将来调整端点，方向错了仍然会被抓住。
    const readScrimAt = (luma: number): number => {
      cleanup();
      // 每个样本都要重置内联覆盖，否则上一个样本的值会残留
      document.documentElement.style.removeProperty('--rbc-layer-host-scrim');
      document.documentElement.removeAttribute('data-rbc-transparent-ack');
      const webview = new EventTarget();
      Object.defineProperty(window, 'chrome', {
        value: { webview },
        configurable: true,
        writable: true,
      });
      const localRef = { current: document.createElement('div') };
      document.body.appendChild(localRef.current);
      renderHook(() => useLiquidGlassPointer(localRef));
      webview.dispatchEvent(
        new MessageEvent('message', {
          data: { type: 'rbc-desktop-accent', h1: '196 72% 46%', luma },
        }),
      );
      const v = document.documentElement.style.getPropertyValue('--rbc-layer-host-scrim');
      const m = v.match(/([0-9.]+)\s*\)/);
      return m ? Number(m[1]) : Number.NaN;
    };

    const darkWallpaper = readScrimAt(0.1); // 暗壁纸
    const brightWallpaper = readScrimAt(0.9); // 亮壁纸

    expect(Number.isNaN(darkWallpaper)).toBe(false);
    expect(Number.isNaN(brightWallpaper)).toBe(false);
    // 暗壁纸必须盖得更厚（深色正文压在暗底上会糊）
    expect(darkWallpaper).toBeGreaterThan(brightWallpaper);
  });

  it('[292] luma = -1 哨兵必须被跳过，不得覆盖令牌（"无数据"不是"纯黑"）', () => {
    cleanup();
    document.documentElement.style.removeProperty('--rbc-layer-host-scrim');
    document.documentElement.removeAttribute('data-rbc-transparent-ack');
    const webview = new EventTarget();
    Object.defineProperty(window, 'chrome', {
      value: { webview },
      configurable: true,
      writable: true,
    });
    const localRef = { current: document.createElement('div') };
    document.body.appendChild(localRef.current);
    renderHook(() => useLiquidGlassPointer(localRef));
    webview.dispatchEvent(
      new MessageEvent('message', {
        data: { type: 'rbc-desktop-accent', h1: '196 72% 46%', luma: -1 },
      }),
    );
    // 未覆盖 ⇒ 内联样式里没有这个属性，CSS 令牌默认值（0.06）生效
    const v = document.documentElement.style.getPropertyValue('--rbc-layer-host-scrim');
    expect(v).toBe('');
  });
});
