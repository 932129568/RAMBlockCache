// src/ui/web/src/hooks/useNativeGlassSync.ts  [295 Wave 1 2026-11-19]
//
// 原生 D3D11 + DirectComposition 宿主的**页面侧对接层**。
//
// 背景：293 已查证 CSS backdrop-filter 采不到桌面像素（Chromium 只模糊渲染
// 上下文内，窗口下方归 OS），模糊半径唯一来源是 DWM 而 DWM 材料不可调。
// 所以宿主改为自己接管合成：Visual 0 画折射后的真桌面，Visual 1 挂这个页面。
// 本 hook 的职责是**把页面上每块玻璃的位置告诉宿主**，让 Visual 0 知道
// 哪里该弯光。
//
// 零破坏（关键）：
//   · 宿主不发 rbc-dcomp-caps 时，本 hook 全程 inert —— 所有监听照常挂上但
//     回调第一行就 return。WPF 宿主（不带 dcomp 引擎）下行为与没有本 hook 完全一致。
//   · 不删任何既有 CSS 令牌、不动 --rbc-layer-* 的消费方，
//     host-mode.contract.test.ts 的 10 令牌 / 零死链判据不受影响。
//
// 相对方案原文（blk 188）的修正：
//   [F1] 方案写 `const panels = []`，strict 下推断成 never[]，push 即 tsc 报错。
//        这里显式标注面板类型。
//   [F2] MutationObserver 只 observe(childList) 时，`data-rbc-glass` 这类
//        **属性**变化不会触发重扫 ⇒ 懒加载路由上线后新玻璃永不注册。
//        加 attributes: true + attributeFilter。
//   [F3] 方案给 light-text 极性时定义了 --rbc-text-muted，全仓零定义
//        （grep 实测）。改用真实存在的 --rbc-text-secondary 与
//        --rbc-text-primary，不引入死令牌。
//   [F4] 指针节流：方案用单 rAF 合并，但漏了「rAF 触发时页面已不可见」的
//        情况 —— 此时 postMessage 仍会发一轮。已在 flush 里复查 visibilityState。

import { useEffect, type RefObject } from 'react';

/** 宿主注入的 chrome.webview 桥。WPF 宿主的 WebView2 也有，但没有 postMessage 之外的能力。 */
type HostWebView = {
  postMessage?: (msg: unknown) => void;
  addEventListener?: (type: string, fn: (ev: Event) => void) => void;
  removeEventListener?: (type: string, fn: (ev: Event) => void) => void;
};

function getWebView(): HostWebView | undefined {
  return (window as unknown as { chrome?: { webview?: HostWebView } }).chrome?.webview;
}

/** 发给宿主的单块玻璃参数。字段名与 native-host 的 GlassPanelGPU 一一对应。 */
export interface GlassPanelPayload {
  x: number;
  y: number;
  w: number;
  h: number;
  r: number;
  bezel: number;
  thickness: number;
  blur: number;
  profile: 0 | 1 | 2;
  tier: 1 | 2;
}

interface RbcDcompCaps {
  type: 'rbc-dcomp-caps';
  engine?: string;
  qualityTier?: string;
}

interface RbcApcaPolarity {
  type: 'rbc-apca-polarity';
  polarity?: string;
}

type HostMessage = RbcDcompCaps | RbcApcaPolarity | { type?: string };

/** 最多上报 32 块：D3D11 的 StructuredBuffer 在宿主侧按这个上限开。 */
const MAX_PANELS = 32;

/** 导航胶囊 vs 卡片的两档光学参数（bezel 宽度 / 玻璃厚度 / 低通半径）。
 *  数值来自 293 的观感调参，不是物理常量：bezel 越大边缘弯曲区越宽，
 *  thickness 越大折射位移越大（过大会把背后内容"拉花"）。 */
const NAV_GLASS = { bezel: 20, thickness: 28, blur: 2, profile: 1 as const, tier: 1 as const };
const CARD_GLASS = { bezel: 16, thickness: 22, blur: 3, profile: 0 as const, tier: 2 as const };

export function useNativeGlassSync(rootRef: RefObject<HTMLElement | null>): void {
  useEffect(() => {
    const root = rootRef.current;
    const host = getWebView();
    if (!root || !host?.postMessage) return;

    let dcompActive = false;
    let layoutRaf = 0;
    let ptrRaf = 0;
    let lastPtrT = 0;
    let lastPtrX = 0;
    let lastPtrY = 0;
    let pendingPtr: { x: number; y: number; speedNorm: number; click: boolean } | null = null;

    const collectAndSendLayout = (): void => {
      layoutRaf = 0;
      if (!dcompActive) return;

      const dpr = window.devicePixelRatio || 1;
      const vw = window.innerWidth;
      const vh = window.innerHeight;

      const nodes = root.querySelectorAll<HTMLElement>(
        '[data-rbc-glass="nav"], [data-rbc-glass="card"], .rbc-section',
      );

      const panels: GlassPanelPayload[] = [];
      for (const el of nodes) {
        if (panels.length >= MAX_PANELS) break;
        if (el.dataset.rbcGlass === 'off') continue;

        const rect = el.getBoundingClientRect();
        // 完全在视口外的不上报：超出裁剪区的玻璃不给宿主，省一次画。
        if (
          rect.width < 8 ||
          rect.height < 8 ||
          rect.bottom < 0 ||
          rect.top > vh ||
          rect.right < 0 ||
          rect.left > vw
        ) {
          continue;
        }

        const isNav = el.dataset.rbcGlass === 'nav';
        const glass = isNav ? NAV_GLASS : CARD_GLASS;

        panels.push({
          x: Math.round(rect.left * dpr),
          y: Math.round(rect.top * dpr),
          w: Math.round(rect.width * dpr),
          h: Math.round(rect.height * dpr),
          // 圆角读运行时生效值而非令牌字面量：主题切换会用不同的 radius。
          r: Math.round((parseFloat(getComputedStyle(el).borderTopLeftRadius) || 14) * dpr),
          bezel: Math.round(glass.bezel * dpr),
          thickness: Math.round(glass.thickness * dpr),
          blur: glass.blur,
          profile: glass.profile,
          tier: glass.tier,
        });
      }

      host.postMessage?.({ type: 'rbc-glass-layout', dpr, panels });
    };

    const scheduleLayout = (): void => {
      if (dcompActive && !layoutRaf) {
        layoutRaf = requestAnimationFrame(collectAndSendLayout);
      }
    };

    const flushPointer = (): void => {
      ptrRaf = 0;
      // [F4] rAF 触发时页面可能已经不可见：此时不发。
      if (!dcompActive || !pendingPtr || document.visibilityState !== 'visible') {
        pendingPtr = null;
        return;
      }
      host.postMessage?.({ type: 'rbc-glass-pointer', ...pendingPtr });
      pendingPtr = null;
    };

    const onPointerMove = (e: PointerEvent): void => {
      if (!dcompActive || document.visibilityState !== 'visible') return;
      const now = performance.now();
      const dtMs = Math.max(now - lastPtrT, 8);
      const speed = Math.hypot(e.clientX - lastPtrX, e.clientY - lastPtrY) / (dtMs / 1000);
      lastPtrT = now;
      lastPtrX = e.clientX;
      lastPtrY = e.clientY;
      const dpr = window.devicePixelRatio || 1;
      pendingPtr = {
        x: Math.round(e.clientX * dpr),
        y: Math.round(e.clientY * dpr),
        // 归一化到 0..1（1200 px/s 视为满速）。满速才激起最深的波谷。
        speedNorm: Math.min(speed / 1200, 1),
        click: false,
      };
      if (!ptrRaf) ptrRaf = requestAnimationFrame(flushPointer);
    };

    const onPointerDown = (e: PointerEvent): void => {
      if (!dcompActive || document.visibilityState !== 'visible') return;
      const dpr = window.devicePixelRatio || 1;
      host.postMessage?.({
        type: 'rbc-glass-pointer',
        x: Math.round(e.clientX * dpr),
        y: Math.round(e.clientY * dpr),
        speedNorm: 1,
        click: true,
      });
    };

    const onMessage = (ev: Event): void => {
      const data = (ev as MessageEvent).data as HostMessage | undefined;
      if (!data || typeof data !== 'object') return;

      if (data.type === 'rbc-dcomp-caps') {
        const caps = data as RbcDcompCaps;
        // 只有真 dcomp 宿主才开闸。WPF 宿主发的是别的 engine 值，走到这里不动。
        if (caps.engine !== 'd3d11-dcomp') return;
        dcompActive = true;
        document.documentElement.classList.add('rbc-host-transparent', 'rbc-host-dcomp');
        if (caps.qualityTier) document.documentElement.dataset.rbcGlassTier = caps.qualityTier;
        scheduleLayout();
        return;
      }

      if (data.type === 'rbc-apca-polarity') {
        const apca = data as RbcApcaPolarity;
        if (!apca.polarity) return;
        // 宿主判出背景极性后翻文字色。[F3] 只用真实存在的令牌。
        document.documentElement.dataset.rbcApcaPolarity =
          apca.polarity === 'light-text' ? 'light-text' : 'dark-text';
      }
    };

    // 尺寸变化 + 结构变化 + 属性变化都要重扫。
    // [F2] attributes 必须显式开：懒加载路由上线时是改 data-rbc-glass，
    // 只 observe childList 会漏掉"新玻璃出现"这件事。
    const ro = new ResizeObserver(scheduleLayout);
    ro.observe(root);

    const mo = new MutationObserver(scheduleLayout);
    mo.observe(root, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ['data-rbc-glass', 'class', 'style'],
    });

    window.addEventListener('scroll', scheduleLayout, { capture: true, passive: true });
    root.addEventListener('pointermove', onPointerMove, { passive: true });
    root.addEventListener('pointerdown', onPointerDown, { passive: true });
    host.addEventListener?.('message', onMessage);
    host.postMessage?.({ type: 'rbc-web-ready', supportsDcompGlass: true });

    return () => {
      ro.disconnect();
      mo.disconnect();
      window.removeEventListener('scroll', scheduleLayout, { capture: true });
      root.removeEventListener('pointermove', onPointerMove);
      root.removeEventListener('pointerdown', onPointerDown);
      host.removeEventListener?.('message', onMessage);
      if (layoutRaf) cancelAnimationFrame(layoutRaf);
      if (ptrRaf) cancelAnimationFrame(ptrRaf);
    };
  }, [rootRef]);
}