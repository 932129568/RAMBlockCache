// src/ui/web/src/hooks/useWindowChrome.ts
//
// [284 2026-11-19] WebView2 宿主的窗口控制桥。
//
// 为什么需要它：这个控制台窗口的 WPF 侧是 WindowChrome{CaptionHeight=0,
// GlassFrameThickness=-1} + WebView2 铺满客户区。WebView2 是原生 Win32 子窗口
// （HWND Airspace），会吃掉所有鼠标命中测试 —— 于是既没有系统标题栏，
// 也没有最小化/最大化/关闭按钮，而且拖不动。用户的两条抱怨
// 「窗口固定在屏幕上不能拖动」「不能直接关闭」是同一个结构缺陷。
//
// 解法：顶栏在 Web 侧自己画，然后通过 chrome.webview.postMessage 把
// 拖动/最小化/最大化/关闭意图发给宿主，宿主用 Win32 原生路径执行
// （拖动走 ReleaseCapture + WM_NCLBUTTONDOWN(HTCAPTION)，这样还能触发 Aero Snap）。
//
// 浏览器直开网页时 chrome.webview 不存在：宿主能力整体降级为"无"，
// isHosted=false，顶栏不渲染窗口按钮 —— 网页形态下画三个点了没用的按钮
// 比不画更糟。

import { useCallback, useEffect, useState } from 'react';

type HostWebView = {
  postMessage?: (msg: unknown) => void;
  addEventListener?: (type: string, fn: (ev: Event) => void) => void;
  removeEventListener?: (type: string, fn: (ev: Event) => void) => void;
};

function getHost(): HostWebView | undefined {
  return (window as unknown as { chrome?: { webview?: HostWebView } }).chrome?.webview;
}

export interface WindowChrome {
  /** 是否运行在 WPF WebView2 宿主内。false 时顶栏不应渲染窗口按钮。 */
  hosted: boolean;
  /** 当前是否最大化，用于切换按钮图标。 */
  maximized: boolean;
  /** 请求宿主开始原生拖动。 */
  beginDrag: () => void;
  minimize: () => void;
  toggleMaximize: () => void;
  close: () => void;
}

export function useWindowChrome(): WindowChrome {
  const [hosted, setHosted] = useState(false);
  const [maximized, setMaximized] = useState(false);

  useEffect(() => {
    const host = getHost();
    if (!host?.postMessage) return;
    setHosted(true);

    // 宿主在 NavigationCompleted 与本 hook 的首帧之间可能各发一次；
    // 幂等处理即可。这里只关心最大化状态，不去碰透色令牌（那是另一个 hook 的职责）。
    const onMessage = (ev: Event) => {
      const data = (ev as MessageEvent).data as { type?: string; maximized?: unknown } | undefined;
      if (!data || data.type !== 'rbc-window-state') return;
      if (typeof data.maximized === 'boolean') setMaximized(data.maximized);
    };

    host.addEventListener?.('message', onMessage);
    return () => host.removeEventListener?.('message', onMessage);
  }, []);

  const send = useCallback((payload: Record<string, unknown>) => {
    try {
      getHost()?.postMessage?.(payload);
    } catch {
      /* 通道已关闭：静默忽略 */
    }
  }, []);

  const beginDrag = useCallback(() => send({ type: 'rbc-window-drag' }), [send]);
  const minimize = useCallback(() => send({ type: 'rbc-window-control', action: 'minimize' }), [send]);
  const toggleMaximize = useCallback(
    () => send({ type: 'rbc-window-control', action: 'maximize' }),
    [send],
  );
  const close = useCallback(() => send({ type: 'rbc-window-control', action: 'close' }), [send]);

  return { hosted, maximized, beginDrag, minimize, toggleMaximize, close };
}