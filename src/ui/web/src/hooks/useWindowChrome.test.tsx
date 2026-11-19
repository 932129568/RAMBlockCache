/// <reference types="node" />
// @vitest-environment jsdom
// src/ui/web/src/hooks/useWindowChrome.test.tsx
//
// [284 2026-11-19] 窗口控制桥的判据。
//
// 这几条判据针对的是用户两条独立抱怨的**同一结构缺陷**：
//   「窗口固定在屏幕上，不能拖动」+「不能直接关闭」
// 成因：WindowChrome(CaptionHeight=0) + WebView2 铺满客户区 ⇒ 无系统标题栏、
// 无三个按钮，且 WebView2 是原生子 HWND，吃掉鼠标命中测试 ⇒ WPF 收不到拖动。
//
// 判据设计原则（吸取 289 的教训）：不测"代码里有这段"，只测**可观测后果**——
//   1) 宿主不存在时，不能画三个点了没用的按钮（hosted=false）；
//   2) 宿主存在时，每个动作必须真的把正确的 payload 发出去；
//   3) 窗口状态消息必须能被接收并反映到 maximized。

import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest';
import { renderHook, act } from '@testing-library/react';
import { useWindowChrome } from './useWindowChrome';

type Msg = Record<string, unknown>;

function installHost() {
  const posted: Msg[] = [];
  const listeners: Array<(ev: Event) => void> = [];
  const host = {
    postMessage: (m: unknown) => posted.push(m as Msg),
    addEventListener: (_t: string, fn: (ev: Event) => void) => listeners.push(fn),
    removeEventListener: (_t: string, fn: (ev: Event) => void) => {
      const i = listeners.indexOf(fn);
      if (i >= 0) listeners.splice(i, 1);
    },
  };
  (window as unknown as { chrome?: unknown }).chrome = { webview: host };
  return {
    posted,
    emit: (data: unknown) => {
      for (const fn of [...listeners]) fn({ data } as unknown as Event);
    },
  };
}

function removeHost() {
  delete (window as unknown as { chrome?: unknown }).chrome;
}

describe('W1: 宿主窗口控制桥', () => {
  beforeEach(() => removeHost());
  afterEach(() => {
    removeHost();
    vi.restoreAllMocks();
  });

  it('W1a: 浏览器直开（无 chrome.webview）时 hosted=false —— 绝不画无效按钮', () => {
    const { result } = renderHook(() => useWindowChrome());
    expect(result.current.hosted).toBe(false);
    // 降级路径下调用也不能抛（顶栏可能复用同一个回调）
    expect(() => result.current.beginDrag()).not.toThrow();
  });

  it('W1b: 宿主存在时 hosted=true，且四个动作各自发出正确 payload', () => {
    const { posted } = installHost();
    const { result } = renderHook(() => useWindowChrome());
    expect(result.current.hosted).toBe(true);

    act(() => result.current.beginDrag());
    act(() => result.current.minimize());
    act(() => result.current.toggleMaximize());
    act(() => result.current.close());

    expect(posted).toEqual([
      { type: 'rbc-window-drag' },
      { type: 'rbc-window-control', action: 'minimize' },
      { type: 'rbc-window-control', action: 'maximize' },
      { type: 'rbc-window-control', action: 'close' },
    ]);
  });

  it("W1c: 宿主推送 rbc-window-state 后 maximized 必须跟随（否则按钮图标永远是错的）", () => {
    const { emit } = installHost();
    const { result } = renderHook(() => useWindowChrome());
    expect(result.current.maximized).toBe(false);

    act(() => emit({ type: 'rbc-window-state', maximized: true }));
    expect(result.current.maximized).toBe(true);

    act(() => emit({ type: 'rbc-window-state', maximized: false }));
    expect(result.current.maximized).toBe(false);
  });

  it('W1d: 非 window-state 的杂消息不得改动 maximized（消息通道开着，必须窄解析）', () => {
    const { emit } = installHost();
    const { result } = renderHook(() => useWindowChrome());

    act(() => emit({ type: 'rbc-desktop-accent', h1: '206 88% 54%', h2: '262 60% 62%' }));
    expect(result.current.maximized).toBe(false);

    // 字段类型不对也必须忽略，不能把 undefined 当 false 收下
    act(() => emit({ type: 'rbc-window-state', maximized: 'yes' }));
    expect(result.current.maximized).toBe(false);
  });

  it('W1e: 卸载后必须摘掉监听（否则每次导航都叠一层，窗口状态会翻倍推送）', () => {
    const { emit } = installHost();
    const { result, unmount } = renderHook(() => useWindowChrome());

    act(() => emit({ type: 'rbc-window-state', maximized: true }));
    expect(result.current.maximized).toBe(true);

    unmount();
    // 卸载后 emit 不应再影响任何状态（监听已摘；这里只验不抛错）
    act(() => emit({ type: 'rbc-window-state', maximized: false }));
  });
});