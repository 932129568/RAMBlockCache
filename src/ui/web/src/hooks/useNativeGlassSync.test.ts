// @vitest-environment jsdom
// src/ui/web/src/hooks/useNativeGlassSync.test.ts
// [295 Wave 1 2026-11-19]
//
// hook 行为契约。**只测行为，不读文件** —— jsdom 环境下 import.meta.url 不是
// file:// 协议，`readFileSync(new URL(...))` 必抛 "The URL must be of scheme file"。
// CSS 侧的契约放在 src/styles/dcomp-host.contract.test.ts（node 环境）。
// 这是本轮实测踩到的真错，不是风格选择。
//
// 三类判据：
//   1) 没有 chrome.webview 时完全 inert（WPF 宿主路径不受影响）
//   2) 收到非 d3d11-dcomp 的 caps 时不开闸（双宿主共存期的隔离）
//   3) 收到 d3d11-dcomp caps 后才开闸、上报布局、接收极性

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { renderHook } from '@testing-library/react';
import { createRef, type RefObject } from 'react';
import { useNativeGlassSync } from './useNativeGlassSync';

type FakeHost = {
  postMessage: ReturnType<typeof vi.fn>;
  addEventListener: ReturnType<typeof vi.fn>;
  removeEventListener: ReturnType<typeof vi.fn>;
  emit: (type: string, payload: unknown) => void;
  posted: () => { type?: string }[];
};

function installHost(): FakeHost {
  const listeners = new Map<string, ((ev: Event) => void)[]>();
  const posted: { type?: string }[] = [];
  const host: FakeHost = {
    postMessage: vi.fn((msg: unknown) => {
      posted.push(msg as { type?: string });
    }),
    addEventListener: vi.fn((type: string, fn: (ev: Event) => void) => {
      const arr = listeners.get(type) ?? [];
      arr.push(fn);
      listeners.set(type, arr);
    }),
    removeEventListener: vi.fn(),
    emit: (type, payload) => {
      for (const fn of listeners.get(type) ?? []) {
        fn({ data: payload } as MessageEvent);
      }
    },
    posted: () => posted,
  };
  // jsdom 里 window.chrome 不是普通可写属性：直接 `window.chrome = ...` 会被
  // 静默忽略（不抛错，但 hook 读到的仍是 undefined）—— 5 个用例全红才暴露出来。
  // 与 useLiquidGlassPointer.test.ts:78 的既有写法保持一致：defineProperty + configurable。
  Object.defineProperty(window, 'chrome', {
    value: { webview: host },
    configurable: true,
    writable: true,
  });
  return host;
}

function unmountHost(): void {
  delete (window as unknown as { chrome?: unknown }).chrome;
}

function mountWith(root: HTMLElement) {
  const ref = createRef<HTMLElement>() as RefObject<HTMLElement | null>;
  (ref as { current: HTMLElement | null }).current = root;
  return renderHook(() => useNativeGlassSync(ref));
}

// jsdom 不实现 ResizeObserver（MutationObserver 有，ResizeObserver 没有）。
// 不 stub 的话 hook 挂载时就抛 ReferenceError —— 实测 5 个用例全红。
class ResizeObserverStub {
  observe(): void {}
  unobserve(): void {}
  disconnect(): void {}
}

describe('useNativeGlassSync 行为', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    (globalThis as unknown as { ResizeObserver: unknown }).ResizeObserver = ResizeObserverStub;
    document.body.innerHTML = '';
  });

  afterEach(() => {
    vi.useRealTimers();
    unmountHost();
    document.body.innerHTML = '';
    delete document.documentElement.dataset.rbcApcaPolarity;
    delete document.documentElement.dataset.rbcGlassTier;
    document.documentElement.classList.remove('rbc-host-transparent', 'rbc-host-dcomp');
  });

  it('没有 chrome.webview 时完全 inert（WPF 宿主路径不受影响）', () => {
    const root = document.createElement('div');
    document.body.appendChild(root);
    expect(() => mountWith(root)).not.toThrow();
    expect(document.documentElement.classList.contains('rbc-host-dcomp')).toBe(false);
  });

  it('收到非 d3d11-dcomp 的 caps 时不开闸（WPF 宿主不得被误开）', () => {
    const root = document.createElement('div');
    document.body.appendChild(root);
    const host = installHost();
    mountWith(root);

    host.emit('message', { type: 'rbc-dcomp-caps', engine: 'wpf-dwm' });

    expect(document.documentElement.classList.contains('rbc-host-dcomp')).toBe(false);
    expect(document.documentElement.classList.contains('rbc-host-transparent')).toBe(false);
  });

  it('挂载即发握手（宿主据此知道页面支持 dcomp 玻璃）', () => {
    const root = document.createElement('div');
    document.body.appendChild(root);
    const host = installHost();
    mountWith(root);

    expect(host.postMessage).toHaveBeenCalledWith(
      expect.objectContaining({ type: 'rbc-web-ready', supportsDcompGlass: true }),
    );
  });

  it('收到 d3d11-dcomp caps 后开闸、上报布局、并接收 APCA 极性', async () => {
    const root = document.createElement('div');
    document.body.appendChild(root);
    const nav = document.createElement('div');
    nav.setAttribute('data-rbc-glass', 'nav');
    nav.style.borderTopLeftRadius = '14px';
    root.appendChild(nav);

    // jsdom 不做布局：getBoundingClientRect 全返回 0，会被 hook 的
    // `rect.width < 8` 过滤掉 ⇒ panels 永远是空数组、断言假红。
    // 显式给一个真实尺寸，模拟"导航胶囊在视口内"。
    vi.spyOn(nav, 'getBoundingClientRect').mockReturnValue({
      x: 24, y: 16, left: 24, top: 16, right: 824, bottom: 76,
      width: 800, height: 60, toJSON: () => ({}),
    } as DOMRect);

    const host = installHost();
    mountWith(root);

    host.emit('message', { type: 'rbc-dcomp-caps', engine: 'd3d11-dcomp', qualityTier: 'premium' });
    expect(document.documentElement.classList.contains('rbc-host-dcomp')).toBe(true);
    expect(document.documentElement.classList.contains('rbc-host-transparent')).toBe(true);
    expect(document.documentElement.dataset.rbcGlassTier).toBe('premium');

    // 让 rAF 里的布局上报落地
    vi.advanceTimersByTime(32);
    await Promise.resolve();
    await Promise.resolve();

    const layoutMsg = host.posted().find((m) => m.type === 'rbc-glass-layout') as
      | { panels?: { tier?: number; profile?: number }[] }
      | undefined;
    expect(layoutMsg, '未上报 rbc-glass-layout').toBeDefined();
    const panels = layoutMsg?.panels ?? [];
    expect(panels.length).toBeGreaterThan(0);
    // nav 玻璃必须带 tier=1 / profile=1（Lip 剖面）：这是宿主选光学参数的唯一依据
    expect(panels[0]?.tier).toBe(1);
    expect(panels[0]?.profile).toBe(1);

    host.emit('message', { type: 'rbc-apca-polarity', polarity: 'light-text' });
    expect(document.documentElement.dataset.rbcApcaPolarity).toBe('light-text');
  });

  it('未开闸前不因 pointermove 发消息（避免 WPF 路径上的 IPC 噪声）', () => {
    const root = document.createElement('div');
    document.body.appendChild(root);
    const host = installHost();
    mountWith(root);
    host.postMessage.mockClear();

    root.dispatchEvent(new PointerEvent('pointermove', { clientX: 10, clientY: 10 }));
    vi.advanceTimersByTime(50);

    expect(host.postMessage).not.toHaveBeenCalled();
  });

  it('卸载时移除宿主 message 监听（避免热更新后重复注册）', () => {
    const root = document.createElement('div');
    document.body.appendChild(root);
    const host = installHost();
    const { unmount } = mountWith(root);

    unmount();
    expect(host.removeEventListener).toHaveBeenCalledWith('message', expect.any(Function));
  });
});