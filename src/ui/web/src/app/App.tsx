import { useEffect, useRef } from 'react';
import { NavLink, Outlet, useLocation, useNavigate } from 'react-router';
import { motion } from 'motion/react';
import { ErrorBoundary, type FallbackProps } from 'react-error-boundary';
import { useHotkeys } from 'react-hotkeys-hook';
import { useDriverStore } from '@state/driver-store';
import StatsPoller from './StatsPoller';
import ConfirmDialog from '@components/ConfirmDialog';
import ToastHost from '@components/ToastHost';
import GlobalAlertBanner from '@components/GlobalAlertBanner';
import { Badge, BrandMark, Button, Icons } from '@components/ui';
import { DUR, EASE, pageEnter, useMotion } from '@lib/motion';
import { LiquidGlassFilter } from '../components/LiquidGlassFilter';
import { useLiquidGlassPointer } from '../hooks/useLiquidGlassPointer';
import { useLiquidWave } from '../hooks/useLiquidWave';
import { useVisibilityAwarePoll } from '../hooks/useVisibilityAwarePoll';
import { useWindowChrome } from '../hooks/useWindowChrome';

/**
 * 应用壳（布局重做 2026-10-31）：
 * 弃"侧栏管理后台"老套结构 → **顶部指令条 + 居中内容画布**。
 *  - 指令条：品牌 + 药丸导航 + 连接状态徽标（玻璃质感，吸顶）
 *  - 内容：max-w 居中画布 + page-fade；ErrorBoundary 单页隔离
 *  - 错误反馈走 Toast + 指令条徽标，不再设底栏
 * 数据纪律不变：轮询单例（StatsPoller），页面只读观察。
 */

const NAV = [
  { to: '/status', label: '状态', icon: Icons.gauge },
  { to: '/volumes', label: '加速', icon: Icons.disk },
  { to: '/benefit', label: '收益', icon: Icons.trend },
  { to: '/diagnostics', label: '诊断', icon: Icons.pulse },
  { to: '/emergency', label: '应急', icon: Icons.shield },
  { to: '/settings', label: '设置', icon: Icons.sliders },
] as const;

export default function App() {
  const phase = useDriverStore((s) => s.phase);
  const version = useDriverStore((s) => s.version);
  const lastError = useDriverStore((s) => s.lastError);
  const connect = useDriverStore((s) => s.connect);
  const location = useLocation();
  const navigate = useNavigate();
  /* [A6] 全局动效偏好：页面转场与导航指示器统一归一化时长 */
  const { reduce } = useMotion();

  /* [235 Phase 2] 液态玻璃：根容器 ref 供指针追踪与 WPF 宿主桌面色消息使用。
   * 只追加 ref 与两个脱离文档流的装饰节点，绝不改 h-screen / overflow-hidden。 */
  const rootRef = useRef<HTMLDivElement>(null);
  useLiquidGlassPointer(rootRef);
  /* [U2] 指针划过卡片的浅水波：只在壳内扫描，页面懒加载后由 MutationObserver 补挂。 */
  useLiquidWave({ root: rootRef });

  /* [284] 宿主窗口控制：拖动 / 最小化 / 最大化 / 关闭。
   * WebView2 吃掉命中测试，WPF 侧 WindowChrome(CaptionHeight=0) 又没有系统标题栏，
   * 所以这三个按钮必须在 Web 顶栏里画，动作经 postMessage 回传宿主用 Win32 执行。 */
  const win = useWindowChrome();

  useEffect(() => {
    void connect();
  }, [connect]);

  // 自动重连：离线/降级时 5s 重试握手（低频补偿控制通道 retry:0 纪律）。
  // [237] 改为可见性感知轮询：窗口隐藏/最小化时不留任何 5s 定时器。
  useVisibilityAwarePoll(
    () => { void connect(); },
    {
      intervalMs: 5000,
      enabled: phase === 'offline' || phase === 'degraded',
    },
  );

  // Alt+1..6 直达页面（react-hotkeys-hook，P1-5 键盘导航）
  const navCombos = NAV.map((_, i) => `alt+${i + 1}`).join(',');
  useHotkeys(
    navCombos,
    (_e, h) => {
      const idx = Number(h.keys?.[0]) - 1;
      const target = NAV[idx];
      if (target) navigate(target.to);
    },
    { preventDefault: true },
  );

  const connected = phase === 'online';

  return (
    /* [UI-2026-11-12] 从 `min-h-screen` 改为**固定视口高度 + 让 main 自己滚**。
     *
     * 原写法 `min-h-screen flex flex-col` 配合 tokens.css 的
     * `html, body, #root { height: 100% }`：`#root` 被钉死成视口高，
     * 而子元素的 `min-height:100vh` 又把内容撑出视口 —— 实测 703×770 下
     * 页面**完全滚不动**，"形状/延迟/环境"三段永远到不了（首屏底部
     * "TRIM 已处理"卡被切在视口边缘）。这是可访问性缺陷，不是观感问题。
     *
     * 正确的 app-shell：壳 = 100vh 且 overflow:hidden（内部不再产生文档流
     * 溢出），滚动条归 main 独占。附带好处：sticky header 不再需要 sticky，
     * 因为它本来就在不滚动的容器里。 */
    <div
      ref={rootRef}
      className="rbc-app-bg flex h-screen flex-col overflow-hidden"
      style={{ color: 'var(--rbc-text-primary)' }}
    >
      {/* Layer 0 桌面透光透色基底 + SVG 折射滤镜定义（均为脱离文档流的装饰节点） */}
      <div className="rbc-desktop-bleed-canvas" aria-hidden="true" />
      <LiquidGlassFilter />
      <StatsPoller />
      <ConfirmDialog />
      <ToastHost />
      <GlobalAlertBanner />

      {/* —— 顶部指令条 —— */}
      {/* [284] 顶栏同时充当标题栏：在宿主内双击可最大化、空白处可拖动。
          拖动必须由前端发起（WebView2 吃掉命中测试，WPF 收不到鼠标），
          所以这里用 onPointerDown 主动请求宿主进入原生拖拽循环。
          react 组件（nav / 徽标 / 按钮）一律 stopPropagation，否则点导航会变拖动。 */}
      <header
        className="rbc-glass z-40 shrink-0"
        style={{ borderBottom: '1px solid var(--rbc-border)' }}
        onPointerDown={
          win.hosted
            ? (e) => {
                // 只响应左键，且只在顶栏空白/非交互区域起拖。
                if (e.button !== 0) return;
                if ((e.target as HTMLElement).closest('[data-no-drag]')) return;
                win.beginDrag();
              }
            : undefined
        }
        onDoubleClick={
          win.hosted
            ? (e) => {
                if ((e.target as HTMLElement).closest('[data-no-drag]')) return;
                win.toggleMaximize();
              }
            : undefined
        }
      >
        <div
          className="mx-auto flex min-h-14 flex-wrap items-center gap-x-4 gap-y-1 px-4 py-1.5"
          style={{ width: '100%', maxWidth: 'var(--rbc-canvas-max)' }}
        >
          <div className="flex shrink-0 items-center gap-2.5 whitespace-nowrap" data-no-drag>
            <BrandMark size={24} />
            <span className="text-sm font-semibold tracking-tight">RAMBlockCache</span>
            <Badge tone="neutral">{version ? `v${version.Major}.${version.Minor} Build${version.Build}` : '未握手'}</Badge>
          </div>

          {/* [UI-2026-11-12] nav 允许换行（flex-wrap）而不是横向滚动。
              原因：横向滚动在 703px 宽下会把"诊断/应急/设置"三个板块推到
              可视区外，而滚动条被隐藏 ⇒ 用户根本不知道还有三页。
              换成换行后任何宽度下 6 个板块都可见。 */}
          <nav
            className="flex min-w-0 flex-1 flex-wrap items-center gap-1"
            aria-label="主导航"
            data-no-drag
          >
            {NAV.map(({ to, label, icon: Icon }, i) => (
              <NavLink
                key={to}
                to={to}
                title={`${label}（Alt+${i + 1}）`}
                /* [UI-2026-11-12] 原为 !rounded-[--rbc-radius-pill]：999px 胶囊
                 * 是 SaaS 后台语汇。本控制台的定调是工业仪器盘（窄圆角 + 发丝边），
                 * 所以改成 4px 方角；快捷键在 title 里可发现（此前 Alt+1..6 只能靠读源码）。 */
                className="rbc-nav-item relative !rounded-[var(--rbc-radius-sm)] !px-3 !py-1.5"
              >
                {({ isActive }) => (
                  <>
                    {isActive && (
                      <motion.span
                        layoutId="nav-pill"
                        className="absolute inset-0 rounded-[var(--rbc-radius-sm)]"
                        style={{
                          background: 'var(--rbc-accent-subtle)',
                          /* 原为 rgba(15,134,224,0.28) 冷蓝 —— 压在琥珀底上撞色 */
                          border: '1px solid var(--rbc-accent-border)',
                        }}
                        /* [A6] 原为硬编码 spring；减弱动效时退化为瞬时（duration 0
                           不可用于 spring，改用 tween 长度 0 才真正无动画）。 */
                        transition={
                          reduce
                            ? { duration: 0 }
                            : { type: 'spring', stiffness: 420, damping: 34 }
                        }
                      />
                    )}
                    <span
                      className="relative z-10 flex items-center gap-1.5"
                      style={{ color: isActive ? 'var(--rbc-accent)' : undefined }}
                    >
                      <Icon size={15} />
                      <span>{label}</span>
                    </span>
                  </>
                )}
              </NavLink>
            ))}
          </nav>

          {/* 连接状态徽标：点 + 阶段；错误时红色可悬停查看 */}
          <div
            data-no-drag
            title={lastError ? `最近错误：${lastError.message}` : undefined}
            className="rbc-liquid-pill flex shrink-0 items-center gap-2 whitespace-nowrap rounded-[var(--rbc-radius-pill)] px-3 py-1.5 text-xs"
            style={{
              background: connected
                ? 'var(--rbc-success-bg)'
                : phase === 'degraded'
                  ? 'var(--rbc-warning-bg)'
                  : 'var(--rbc-danger-bg)',
              border: `1px solid ${
                connected
                  ? 'var(--rbc-success-border)'
                  : phase === 'degraded'
                    ? 'var(--rbc-warning-border)'
                    : 'var(--rbc-danger-border)'
              }`,
              color: connected
                ? 'var(--rbc-success-fg)'
                : phase === 'degraded'
                  ? 'var(--rbc-warning-fg)'
                  : 'var(--rbc-danger-fg)',
            }}
          >
            <span
              aria-hidden
              className={connected ? 'rbc-pulse-dot' : ''}
              style={{
                width: 7,
                height: 7,
                borderRadius: '50%',
                background: 'currentColor',
              }}
            />
            {phaseLabel(phase)}
            {version && <span className="font-mono">· 协议 v{version.ProtocolVersion}</span>}
          </div>

          {/* [284] 窗口三键：仅宿主内渲染。
              浏览器直开时画三个点了没用的按钮比不画更糟，所以整体条件渲染。
              造型对齐 Windows 11：最小化/最大化 40x32、关闭 40x32 悬停转红。 */}
          {win.hosted && (
            <div className="ml-1 flex shrink-0 items-center" data-no-drag>
              <button
                type="button"
                aria-label="最小化"
                title="最小化"
                className="rbc-win-btn"
                onClick={(e) => {
                  e.stopPropagation();
                  win.minimize();
                }}
              >
                <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
                  <path d="M1 5h8" stroke="currentColor" strokeWidth="1" strokeLinecap="round" />
                </svg>
              </button>
              <button
                type="button"
                aria-label={win.maximized ? '还原' : '最大化'}
                title={win.maximized ? '还原' : '最大化'}
                className="rbc-win-btn"
                onClick={(e) => {
                  e.stopPropagation();
                  win.toggleMaximize();
                }}
              >
                {win.maximized ? (
                  <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
                    <rect x="1" y="3" width="6" height="6" fill="none" stroke="currentColor" strokeWidth="1" />
                    <path d="M3 3V1h6v6H7" fill="none" stroke="currentColor" strokeWidth="1" />
                  </svg>
                ) : (
                  <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
                    <rect x="1.5" y="1.5" width="7" height="7" fill="none" stroke="currentColor" strokeWidth="1" />
                  </svg>
                )}
              </button>
              <button
                type="button"
                aria-label="关闭"
                title="关闭（最小化到托盘）"
                className="rbc-win-btn rbc-win-btn-close"
                onClick={(e) => {
                  e.stopPropagation();
                  win.close();
                }}
              >
                <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden>
                  <path d="M1.5 1.5l7 7M8.5 1.5l-7 7" stroke="currentColor" strokeWidth="1" strokeLinecap="round" />
                </svg>
              </button>
            </div>
          )}
        </div>
      </header>

      {/* —— 内容画布 —— */}
      {/* 唯一的滚动容器：main 自己滚，页面内部不再与文档流抢滚动条 */}
      <main className="min-h-0 flex-1 overflow-y-auto">
        <div
          className="mx-auto w-full p-5"
          style={{ maxWidth: 'var(--rbc-canvas-max)' }}
        >
        <ErrorBoundary
          fallbackRender={(p: FallbackProps) =>
            isStaleChunkError(p.error) ? (
              <StaleChunkRecovery />
            ) : (
              <div className="rbc-card flex flex-col items-start gap-3 p-6" role="alert">
                <h2 className="text-sm font-medium" style={{ color: 'var(--rbc-danger-fg)' }}>
                  页面渲染异常
                </h2>
                <pre
                  className="max-h-40 overflow-auto font-mono text-xs"
                  style={{ color: 'var(--rbc-text-tertiary)' }}
                >
                  {(p.error as Error).message}
                </pre>
                <Button tone="secondary" onClick={() => p.resetErrorBoundary()}>
                  重试渲染
                </Button>
              </div>
            )
          }
          resetKeys={[location.pathname]}
        >
          {/* [FIX 2026-10-31] 页面级 motion 淡入在 hash 路由切换后可能滞留 opacity:0
              （与卡片级 .rbc-fade-up 双层动画叠加，实测标题透明）。移除页面级动画，
              入场动效由卡片级 rbc-fade-up 单层承担——确定性优先。

              [A6 2026-11-11] 恢复页面转场，但按"只入场、不退出"实现。
              三条约束直接对应上面那次事故：
               1. key 绑 pathname → 每次导航重建容器，入场动画必然重新触发
                  （复用同一节点时 initial 不重放，动画看似"没生效"）；
               2. 不使用 AnimatePresence / exit：退出动画是"节点已卸载但仍在
                  动画"的状态，正是 opacity 残留的高发区。旧页即时卸载，
                  新页从 initial 入场 —— 没有中间态；
               3. 容器只动 opacity/y，终态显式写死 opacity:1（见 pageEnter），
                  不依赖动画完成回调；即使被打断也停在可见态。
              容器内子元素的 `.rbc-fade-up` 保留：它动的是同一属性但由 CSS
              单层控制，与容器的 motion 分属不同元素，不构成"双写"。
              减弱动效时 duration=0，容器以终态直接挂载。 */}
          <motion.div
            key={location.pathname}
            variants={pageEnter}
            initial="initial"
            animate="animate"
            transition={{ duration: reduce ? 0 : DUR.slow, ease: EASE.decel }}
          >
            <Outlet />
          </motion.div>
        </ErrorBoundary>
        </div>
      </main>
    </div>
  );
}

function phaseLabel(phase: string): string {
  switch (phase) {
    case 'online':
      return '已连接';
    case 'connecting':
      return '握手中';
    case 'degraded':
      return '降级运行';
    case 'offline':
      return '未连接';
    default:
      return '待握手';
  }
}

/**
 * [UI-2026-11-12] 「构建产物已更新」自愈。
 *
 * 现象（本轮实测）：`vite build` 重新生成 dist 后，已经打开的控制台仍持有旧
 * `index-*.js` 里的 chunk 名表；点侧栏切到懒加载路由时请求
 * `/assets/VolumesPage-<旧hash>.js` → 404 → 路由抛
 * `Failed to fetch dynamically imported module`，ErrorBoundary 显示
 * 「页面渲染异常 / 重试渲染」。但**重试永远失败**——它拿的还是同一张旧表。
 *
 * 唯一出路是整页 reload（重新拉 index.html 拿到新表）。这里是本地桥接服务的
 * 控制台，dist 会被反复重建，所以必须自动恢复而不是让用户对着红色报错发愣。
 * 用 sessionStorage 记一次时间戳，15 秒内不重复自动 reload，避免真死循环。
 */
function StaleChunkRecovery() {
  useEffect(() => {
    const KEY = 'rbc.staleChunkRetryAt';
    let last = 0;
    try {
      last = Number(window.sessionStorage.getItem(KEY) ?? 0);
    } catch {
      /* 隐私模式下 sessionStorage 可能抛，退化为"每次都 reload"一次机会 */
    }
    if (Date.now() - last < 15000) return;
    try {
      window.sessionStorage.setItem(KEY, String(Date.now()));
    } catch {
      /* 同上 */
    }
    window.location.reload();
  }, []);
  return (
    <div className="rbc-card p-6 text-sm" role="status" style={{ color: 'var(--rbc-text-secondary)' }}>
      控制台已更新，正在重新加载…
    </div>
  );
}

/** 识别「chunk 已失效」这一类错误（不同打包器/浏览器措辞不同） */
function isStaleChunkError(err: unknown): boolean {
  const m = String((err as Error)?.message ?? '');
  return /dynamically imported module|Importing a module script failed|ChunkLoadError|Loading chunk \d+ failed/i.test(m);
}
