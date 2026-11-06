import { useEffect } from 'react';
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

  useEffect(() => {
    void connect();
  }, [connect]);

  // 自动重连：离线/降级时 5s 重试握手（低频补偿控制通道 retry:0 纪律）
  useEffect(() => {
    if (phase !== 'offline' && phase !== 'degraded') return;
    const t = setInterval(() => {
      void connect();
    }, 5000);
    return () => clearInterval(t);
  }, [phase, connect]);

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
    <div className="rbc-app-bg flex min-h-screen flex-col" style={{ color: 'var(--rbc-text-primary)' }}>
      <StatsPoller />
      <ConfirmDialog />
      <ToastHost />
      <GlobalAlertBanner />

      {/* —— 顶部指令条 —— */}
      <header
        className="rbc-glass sticky top-0 z-40"
        style={{ borderBottom: '1px solid var(--rbc-border)' }}
      >
        <div
          className="mx-auto flex h-14 max-w-6xl items-center gap-4 px-4"
          style={{ width: '100%' }}
        >
          <div className="flex items-center gap-2.5">
            <BrandMark size={24} />
            <span className="text-sm font-semibold tracking-tight">RAMBlockCache</span>
            <Badge tone="neutral">{version ? `v${version.Major}.${version.Minor} Build${version.Build}` : '未握手'}</Badge>
          </div>

          <nav className="ml-4 flex items-center gap-1" aria-label="主导航">
            {NAV.map(({ to, label, icon: Icon }) => (
              <NavLink key={to} to={to} className="rbc-nav-item relative !rounded-[var(--rbc-radius-pill)] !px-3 !py-1.5">
                {({ isActive }) => (
                  <>
                    {isActive && (
                      <motion.span
                        layoutId="nav-pill"
                        className="absolute inset-0 rounded-[var(--rbc-radius-pill)]"
                        style={{
                          background: 'var(--rbc-accent-subtle)',
                          border: '1px solid rgba(15,134,224,0.28)',
                        }}
                        transition={{ type: 'spring', stiffness: 420, damping: 34 }}
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

          <div className="flex-1" />

          {/* 连接状态徽标：点 + 阶段；错误时红色可悬停查看 */}
          <div
            title={lastError ? `最近错误：${lastError.message}` : undefined}
            className="flex items-center gap-2 rounded-[var(--rbc-radius-pill)] px-3 py-1.5 text-xs"
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
        </div>
      </header>

      {/* —— 内容画布 —— */}
      <main className="mx-auto w-full max-w-6xl flex-1 p-5">
        <ErrorBoundary
          fallbackRender={(p: FallbackProps) => (
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
          )}
          resetKeys={[location.pathname]}
        >
          {/* [FIX 2026-10-31] 页面级 motion 淡入在 hash 路由切换后可能滞留 opacity:0
              （与卡片级 .rbc-fade-up 双层动画叠加，实测标题透明）。移除页面级动画，
              入场动效由卡片级 rbc-fade-up 单层承担——确定性优先。 */}
          <Outlet />
        </ErrorBoundary>
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
