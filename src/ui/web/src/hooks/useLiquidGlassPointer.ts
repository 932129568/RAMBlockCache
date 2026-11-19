// src/ui/web/src/hooks/useLiquidGlassPointer.ts
//
// 两个职责：
//   1) 把指针在 .rbc-section 内的像素坐标写进 --rbc-lg-ptr-x / --rbc-lg-ptr-y，
//      供 liquid-glass.css 的镜面高光/焦散光束消费（U1/U2 契约，勿改语义：单位必须是 px）。
//   2) 收到 WPF WebView2 宿主推送的桌面主色时，给 <html> 挂 rbc-host-transparent，
//      这是「宿主真透色」的唯一开闸信号。
//
// [v23 2026-11-19] 第 2 项新增壁纸亮度自适应：
//   宿主的 scrim 是压在 DWM 亚克力之上的一层白纱。固定 0.62 时，合成白度约 0.71，
//   与不透明底色 #f3f5f9 几乎无法区分 —— 桌面彻底被遮死，整页看起来就是一块灰白板。
//   做法：宿主若在消息里附带 luma（0..1 壁纸亮度），scrim 在 0.10..0.38 之间线性插值；
//   不附带就用 tokens.css 的默认 0.14。亮度自适应的方向是「亮壁纸多盖一点、
//   暗壁纸少盖一点」，这样白字的对比度在两种壁纸上都还站得住。
//   luma 的取值与含义写在 rbc-desktop-accent 消息契约里；
//   宿主当前版本不发该字段，此时行为必须与「没有 luma」完全一致（回落到令牌默认值）。

import { useEffect, RefObject } from 'react';

/** 与 tokens.css 的 --rbc-layer-host-scrim 保持一致的低/高亮度端点。
 *
 * [292 2026-11-19] 0.10..0.38 -> 0.04..0.14，且插值方向反转（见下方使用处）。
 *
 * 旧端点的问题不是数值大小，是它服务的场景搞错了：0.38 的高端是为"白字压在
 * 亮壁纸上"准备的，而本应用正文是深色 #17233c（亮色主题）。深色文字在亮背景上
 * 本来对比度就高，盖 0.38 的白纱纯属自伤 —— 它把壁纸压成一块均匀的浅灰，
 * 用户看到的"磨砂玻璃，只能透色一点"里，这 0.38 贡献了一大半。
 * 新端点让整段区间都保持在"极薄"的范围内，最厚 0.14 也仍然透出 8 成桌面。 */
const SCRIM_ALPHA_MIN = 0.04;
const SCRIM_ALPHA_MAX = 0.14;

export function useLiquidGlassPointer(rootRef: RefObject<HTMLElement | null>) {
  useEffect(() => {
    const root = rootRef.current;
    if (!root) return;

    const mq = window.matchMedia('(prefers-reduced-motion: reduce)');
    let rafId = 0;
    let lastEvent: PointerEvent | null = null;

    const updateVars = () => {
      rafId = 0;
      if (!lastEvent || !(lastEvent.target instanceof Element)) return;
      const card = lastEvent.target.closest<HTMLElement>('.rbc-section');
      if (!card) return;
      const rect = card.getBoundingClientRect();
      card.style.setProperty('--rbc-lg-ptr-x', `${Math.round(lastEvent.clientX - rect.left)}px`);
      card.style.setProperty('--rbc-lg-ptr-y', `${Math.round(lastEvent.clientY - rect.top)}px`);
    };

    const handleMove = (ev: PointerEvent) => {
      if (mq.matches || document.visibilityState === 'hidden') return;
      lastEvent = ev;
      if (!rafId) {
        rafId = window.requestAnimationFrame(updateVars);
      }
    };

    // 修 BUG-NEW-07：接收运行在交互会话 (Session >= 1) 的 WPF WebView2 宿主推送的真实 DWM 桌面色，
    // 不依赖 Session 0 SYSTEM 计划任务的 Node Bridge，确保 Gate5 check_node_contract.py 零冲突
    //
    // [U3 2026-11-19] 收到桌面色时额外给 <html> 挂 rbc-host-transparent：宿主真透色开闸信号。
    // 只监听 chrome.webview（不是 window）：浏览器直开网页时宿主不存在 ⇒ 监听不注册
    // ⇒ 类永不加 ⇒ Web 侧保持原有不透明兜底。挂 window 会让同页 iframe / 桥接心跳
    // 也能开闸，那是一条把网页变成"透明白洞"的真实故障路径。
    const handleWebViewMessage = (ev: Event) => {
      const customEv = ev as MessageEvent;
      const data = customEv?.data as
        | { type?: string; h1?: unknown; h2?: unknown; luma?: unknown }
        | null
        | undefined;
      if (!data || data.type !== 'rbc-desktop-accent' || typeof data.h1 !== 'string') return;

      const docEl = document.documentElement;
      docEl.style.setProperty('--rbc-lg-desktop-h1', data.h1);
      if (typeof data.h2 === 'string') {
        docEl.style.setProperty('--rbc-lg-desktop-h2', data.h2);
      }

      // 幂等：重复收到消息不产生副作用，零组件树分叉。
      docEl.classList.add('rbc-host-transparent');

      // [284 可观测性] 把"透色是否真的挂上"回传宿主日志。
      // 这是整条链路唯一能被验证的落点：后端日志只能证明"消息发出去了"，
      // 不能证明"前端收下并把它变成了视觉状态"。

      // 壁纸亮度自适应：只在宿主显式给出**区间内的有限** luma 时覆盖令牌默认值。
      // [292 2026-11-19] 判据从 `isFinite` 收紧到 `0 <= luma <= 1`。
      // 为什么必须收紧：宿主现在用 -1 表示"无此数据"（见 DwmLiquidGlassHelper
      // 的 292 记录）。-1 是有限数，isFinite 会放行，随后 Math.max(0, -1) 把它夹成 0，
      // 于是"没有数据"被当成"纯黑壁纸"，把 scrim 压到区间最低端 —— 一个反向的错。
      // 正确的语义是：越界即无效，一律不覆盖，让 tokens.css 的默认值生效。
      if (typeof data.luma === 'number' && Number.isFinite(data.luma) && data.luma >= 0 && data.luma <= 1) {
        const luma = data.luma;
        // [292] 插值方向反转：luma 越低（壁纸越暗）、白纱越厚。
        // 理由：正文是深色 #17233c，暗壁纸才是对比度的真威胁 —— 深字压深底会糊。
        // 亮壁纸不需要白纱，深字本来就站得住。旧代码方向相反，等于"越亮越盖"，
        // 把最不需要遮盖的情形盖得最狠。
        const alpha = SCRIM_ALPHA_MAX - luma * (SCRIM_ALPHA_MAX - SCRIM_ALPHA_MIN);
        docEl.style.setProperty('--rbc-layer-host-scrim', `rgba(255, 255, 255, ${alpha.toFixed(3)})`);
      }

      // [292 2026-11-19] ACK 必须放在 luma 覆盖**之后**。
      // 旧顺序是先 ACK 后覆盖，导致日志里永远是 `scrim=(default)` —— 即便
      // luma 把令牌顶到了 0.340。这是个判据时序缺陷：采样点读的是"覆盖前的值"，
      // 于是最有诊断价值的那一行永远显示"正常"，而用户看到的是 0.59 的白。
      // 顺序换过来之后，ACK 里的 scrim 才是**实际生效**的值。
      if (!docEl.dataset.rbcTransparentAck) {
        docEl.dataset.rbcTransparentAck = '1';

        /* [292 2026-11-19] ACK 必须**延后两个 rAF**再采样，不能同步取。
         *
         * 实测证据（11:01:47 那行日志）：
         *   scrimAlpha=0.06 sectionAlpha=0 cardAlpha=0 composited=0.06
         * 0.06 是真实的（scrim 来自内联覆盖，随时可读），但 section/card 读到 0，
         * 原因是这行消息在 NavigationCompleted 之后立刻重放，此时 React 还没把
         * .rbc-section / .rbc-card 渲染进 DOM —— querySelector 返回 null，
         * maxAlpha(null) 走 "if (!s) return 0" 分支，于是"没渲染"被记成"alpha=0"。
         * 这是第二次踩"空集判 PASS"：判据在空集上算出一个漂亮数字（透射率 0.94），
         * 而真实页面根本还没画出来。**空集必须与真零区分开**，否则这条判据比没有更坏。
         *
         * 两处修正：
         *   a) 用双 rAF 把采样推到首帧绘制之后（单 rAF 仍可能早于首次 commit）；
         *   b) 目标元素缺失时记 null 而不是 0，让日志里能一眼看出"没测到"。
         */
        const sampleAndAck = () => {
          let probe: Record<string, unknown> = {};
          try {
            const bodyEl = document.body;
            const appBg = document.querySelector<HTMLElement>('.rbc-app-bg');
            const bleed = document.querySelector<HTMLElement>('.rbc-desktop-bleed-canvas');
            const section = document.querySelector<HTMLElement>('.rbc-section');
            const card = document.querySelector<HTMLElement>(
              '.rbc-section .rbc-card:not(.rbc-card-accent):not(.rbc-card-danger)',
            );
            const cs = (el: Element | null) => (el ? getComputedStyle(el) : null);
            const bg = (el: Element | null) => {
              const s = cs(el);
              if (!s) return 'none';
              return `${s.backgroundColor}|${s.backgroundImage.slice(0, 42)}`;
            };

          /* 取某层填充的最大 alpha。
           * 为什么取最大而不是首段：渐变的首段通常最不透明，但三个令牌里
           * card 的 55% 档可能更实，取 max 才能代表"这一层最厚的地方"。
           * [292] 元素缺失时返回 **null**，不是 0。
           * "没渲染"与"渲染了但全透明"是两件完全不同的事：
           *   - 没渲染 ⇒ 判据无意义，必须在日志里显形（null / missing）；
           *   - 真透明 ⇒ 才是合法的 alpha=0。
           * 上一版统一返回 0，于是"DOM 还没画出来"被算成"这一层完全透明"，
           * 合成出 transmittance=0.94 这种漂亮但虚假的数字。 */
          const maxAlpha = (el: Element | null): number | null => {
            const s = cs(el);
            if (!s) return null;
            const alphas = Array.from(
              (s.backgroundImage || '').matchAll(/rgba?\([^)]*?,\s*([0-9.]+)\s*\)/g),
              (m) => Number(m[1]),
            );
            const own = s.backgroundColor.match(/rgba?\([^)]*?,\s*([0-9.]+)\s*\)/);
            if (own) alphas.push(Number(own[1]));
            return alphas.length > 0 ? Math.max(...alphas) : 0;
          };

          // 合成白度：三层白色依次叠加。顺序不影响结果（都是白），
          // 但三项都必须参与 —— 只看 scrim 会漏掉"填充太实"这条路径。
          const scrimA = (() => {
            const m = docEl.style.getPropertyValue('--rbc-layer-host-scrim').match(/([0-9.]+)\s*\)/);
            return m ? Number(m[1]) : 0.06;
          })();
          const secA = maxAlpha(section);
          const cardA = maxAlpha(card);

          // 只有三层都测到才给合成数字；缺任何一层就明确报 missing，
          // 绝不用 0 顶替后算一个看起来正常的结论。
          const measured = secA !== null && cardA !== null;
          const composited = measured
            ? 1 - (1 - scrimA) * (1 - secA!) * (1 - cardA!)
            : null;

          probe = {
            html: `${getComputedStyle(docEl).backgroundColor}`,
            body: bg(bodyEl),
            appBg: bg(appBg),
            bleed: bg(bleed),
            // [292] 透射率 = 1 - 合成白度。这个数字才是用户"能不能看见桌面"的判据。
            scrimAlpha: scrimA,
            // 缺失记字符串，避免被下游当成数值 0
            sectionAlpha: secA === null ? 'MISSING' : secA,
            cardAlpha: cardA === null ? 'MISSING' : cardA,
            composited: composited === null ? 'MISSING' : Number(composited.toFixed(3)),
            transmittance:
              composited === null ? 'MISSING' : Number((1 - composited).toFixed(3)),
          };
        } catch {
          probe = { error: 'probe failed' };
        }

        try {
          (chromeWebview as { postMessage?: (m: unknown) => void } | undefined)?.postMessage?.({
            type: 'rbc-transparent-ack',
            scrim: docEl.style.getPropertyValue('--rbc-layer-host-scrim') || '(default)',
            h1: data.h1,
            probe,
          });
        } catch {
          /* 通道不可用：静默忽略，不影响视觉效果 */
        }
        };

        /* 双 rAF：第一个 rAF 在本轮样式计算后、下一个绘制前的回调里执行，
         * 第二个确保首帧已提交。
         *
         * [292 实测] 双 rAF 仍然不够：本机日志依旧出现
         *   sectionAlpha=MISSING cardAlpha=MISSING
         * 原因是消息重放发生在导航完成瞬间，此时 React 首帧可能还没 commit
         * （bundle 解析、hydration、图表初始化都在这一帧里）。
         * 因此不能"等固定帧数"，必须**等到元素真的出现**再采样：
         * 先双 rAF，然后在最多 3 秒内每 100ms 重试，拿到就停。
         * 3 秒后仍缺失 ⇒ 如实报 MISSING（这是合法的"页面没画出 section"，
         * 不是判据故障）。绝不为了凑一个数字而把缺失当 0。 */
        const POLL_INTERVAL_MS = 100;
        const POLL_MAX_TRIES = 30;

        const trySample = (attempt: number) => {
          const hasSection = document.querySelector('.rbc-section') !== null;
          const hasCard = document.querySelector('.rbc-card') !== null;
          if ((hasSection && hasCard) || attempt >= POLL_MAX_TRIES) {
            sampleAndAck();
            return;
          }
          window.setTimeout(() => trySample(attempt + 1), POLL_INTERVAL_MS);
        };

        requestAnimationFrame(() => requestAnimationFrame(() => trySample(0)));
      }
    };

    root.addEventListener('pointermove', handleMove, { passive: true });
    const chromeWebview = (window as unknown as { chrome?: { webview?: EventTarget } }).chrome?.webview;
    chromeWebview?.addEventListener('message', handleWebViewMessage);

    // [284 2026-11-19] 主动上报"前端已挂载"。
    //
    // 这是修复"永远蓝白色底板"的关键一步。宿主的推送时机是 NavigationCompleted，
    // 那一刻 React 还没跑完首次 effect，chrome.webview 上的 message 监听尚未注册
    // ⇒ 宿主发来的 rbc-desktop-accent 被直接丢弃 ⇒ rbc-host-transparent 永不加。
    //
    // 只延时重试是碰运气（慢机器上照样丢）。正确做法是把消息变成"可重放"：
    // 前端一挂载就告诉宿主"我准备好了，请再发一次"，宿主随即补发同一份 payload。
    // 该消息只在 WebView2 宿主内发出；浏览器直开时 chromeWebview 不存在，不上报。
    const postReady = () => {
      try {
        (chromeWebview as { postMessage?: (m: unknown) => void } | undefined)?.postMessage?.({
          type: 'rbc-web-ready',
        });
      } catch {
        /* 宿主不存在或通道已关闭：静默忽略，不影响 Web 兜底形态 */
      }
    };
    postReady();

    return () => {
      root.removeEventListener('pointermove', handleMove);
      chromeWebview?.removeEventListener('message', handleWebViewMessage);
      if (rafId) window.cancelAnimationFrame(rafId);
    };
  }, [rootRef]);
}