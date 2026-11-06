import { useEffect, useState } from 'react';
import { motion, useSpring } from 'motion/react';
import type { ReactNode } from 'react';

/**
 * 设计系统原语（UI 重设计 2026-10-31）。
 * 视觉层在 tokens.css 的 .rbc-* 设计层（玻璃/光影/渐变/微交互），此处只组合。
 * 动效原则：按压回弹（scale .965）、数值弹簧、入场由 .rbc-fade-up 承担。
 */

/* ============================ 图标（stroke 风格）============================ */

function I({ d, size = 17 }: { d: string; size?: number | undefined }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.7}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden
    >
      <path d={d} />
    </svg>
  );
}

export const Icons = {
  gauge: (p: { size?: number | undefined }) => (
    <I size={p.size} d="M12 6v6l4 2M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0Z" />
  ),
  disk: (p: { size?: number | undefined }) => (
    <I size={p.size} d="M4 6c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3Zm0 0v12c0 1.7 3.6 3 8 3s8-1.3 8-3V6M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3" />
  ),
  trend: (p: { size?: number | undefined }) => (
    <I size={p.size} d="M3 17l6-6 4 4 8-8m0 0h-5m5 0v5" />
  ),
  pulse: (p: { size?: number | undefined }) => (
    <I size={p.size} d="M3 12h4l2-7 4 14 2-7h6" />
  ),
  shield: (p: { size?: number | undefined }) => (
    <I size={p.size} d="M12 3l8 3v6c0 4.5-3.2 7.7-8 9-4.8-1.3-8-4.5-8-9V6l8-3Zm-3 9l2 2 4-4" />
  ),
  sliders: (p: { size?: number | undefined }) => (
    <I size={p.size} d="M5 4v7m0 4v5m7-16v3m0 4v9m7-16v9m0 4v3M3 11h4m3 0h4m3 0h4M3 19h4m3 0h4m3 0h4" />
  ),
  download: (p: { size?: number | undefined }) => (
    <I size={p.size} d="M12 3v12m0 0l-4-4m4 4l4-4M4 21h16" />
  ),
  refresh: (p: { size?: number | undefined }) => (
    <I size={p.size} d="M21 12a9 9 0 1 1-2.6-6.4M21 3v6h-6" />
  ),
  layers: (p: { size?: number | undefined }) => (
    <I size={p.size} d="M12 3l9 5-9 5-9-5 9-5Zm-9 9l9 5 9-5m-18 5l9 5 9-5" />
  ),
};

/* ============================ 品牌 ============================ */

export function BrandMark({ size = 28 }: { size?: number | undefined }) {
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" aria-hidden>
      <defs>
        <linearGradient id="rbc-brand-a" x1="0" y1="1" x2="1" y2="0">
          <stop offset="0" stopColor="#2e86d6" />
          <stop offset="1" stopColor="#4cc2ff" />
        </linearGradient>
      </defs>
      <rect x="3.5" y="18.5" width="11" height="10" rx="2.5" fill="url(#rbc-brand-a)" opacity="0.95" />
      <rect x="9" y="9.5" width="11" height="10" rx="2.5" fill="#1e3a4d" stroke="rgba(76,194,255,0.7)" />
      <rect x="17.5" y="3.5" width="11" height="10" rx="2.5" fill="none" stroke="rgba(76,194,255,0.45)" />
    </svg>
  );
}

/* ============================ 弹簧数值 ============================ */

/** 数值变化时弹簧补间（避免跳变；reduced-motion 下 CSS 令牌不适用此处，spring 仍平滑但快） */
export function AnimatedValue({
  value,
  format = (n) => n.toLocaleString('zh-CN'),
  className,
}: {
  value: number;
  format?: (n: number) => string;
  className?: string | undefined;
}) {
  const spring = useSpring(value, { stiffness: 160, damping: 26 });
  const [text, setText] = useState(() => format(value));
  useEffect(() => {
    spring.set(value);
  }, [value, spring]);
  useEffect(
    () =>
      spring.on('change', (v) => {
        setText(format(v));
      }),
    [spring, format],
  );
  return <span className={className}>{text}</span>;
}

/* ============================ PageHeader ============================ */

export function PageHeader({
  title,
  desc,
  actions,
}: {
  title: string;
  desc?: string | undefined;
  actions?: ReactNode;
}) {
  return (
    <header className="mb-4 flex items-start justify-between gap-4">
      <div>
        <h1 className="text-lg font-semibold tracking-tight" style={{ color: 'var(--rbc-text-primary)' }}>
          {title}
        </h1>
        {desc && (
          <p className="mt-1 text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
            {desc}
          </p>
        )}
      </div>
      {actions && <div className="flex shrink-0 items-center gap-2">{actions}</div>}
    </header>
  );
}

/* ============================ Card ============================ */

export function Card({
  title,
  subtitle,
  actions,
  children,
  accent,
  className,
}: {
  title?: string;
  subtitle?: string | undefined;
  actions?: ReactNode;
  children: ReactNode;
  accent?: boolean | undefined;
  className?: string | undefined;
}) {
  return (
    <section className={`rbc-card rbc-fade-up ${accent ? 'rbc-card-accent' : ''} ${className ?? ''}`}>
      {(title || actions) && (
        <header
          className="mb-3 flex items-center justify-between gap-3 border-b pb-2"
          style={{ borderColor: 'var(--rbc-border)' }}
        >
          <div>
            {title && (
              <h2 className="text-sm font-medium" style={{ color: 'var(--rbc-text-primary)' }}>
                {title}
              </h2>
            )}
            {subtitle && (
              <p className="mt-0.5 text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
                {subtitle}
              </p>
            )}
          </div>
          {actions && <div className="flex items-center gap-2">{actions}</div>}
        </header>
      )}
      {children}
    </section>
  );
}

/* ============================ Metric ============================ */

export function Metric({
  label,
  value,
  hint,
  tone,
}: {
  label: string;
  value: ReactNode;
  hint?: string | undefined;
  tone?: 'ok' | 'warn' | 'danger' | undefined;
}) {
  const toneColor =
    tone === 'ok'
      ? 'var(--rbc-success-fg)'
      : tone === 'warn'
        ? 'var(--rbc-warning-fg)'
        : tone === 'danger'
          ? 'var(--rbc-danger-fg)'
          : 'var(--rbc-text-primary)';
  return (
    <div className="rbc-card !rounded-[var(--rbc-radius-md)] !p-3">
      <div className="text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
        {label}
      </div>
      <div
        className={`mt-1 font-mono text-xl leading-tight ${tone ? '' : 'rbc-value-glow'}`}
        style={tone ? { color: toneColor } : undefined}
      >
        {value}
      </div>
      {hint && (
        <div className="mt-0.5 text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
          {hint}
        </div>
      )}
    </div>
  );
}

/* ============================ Badge ============================ */

export function Badge({
  tone,
  children,
}: {
  tone: 'ok' | 'warn' | 'danger' | 'info' | 'neutral';
  children: ReactNode;
}) {
  const map = {
    ok: { bg: 'var(--rbc-success-bg)', fg: 'var(--rbc-success-fg)', bd: 'var(--rbc-success-border)' },
    warn: { bg: 'var(--rbc-warning-bg)', fg: 'var(--rbc-warning-fg)', bd: 'var(--rbc-warning-border)' },
    danger: { bg: 'var(--rbc-danger-bg)', fg: 'var(--rbc-danger-fg)', bd: 'var(--rbc-danger-border)' },
    info: { bg: 'var(--rbc-info-bg)', fg: 'var(--rbc-info-fg)', bd: 'var(--rbc-info-border)' },
    neutral: { bg: 'var(--rbc-neutral-bg)', fg: 'var(--rbc-neutral-fg)', bd: 'var(--rbc-neutral-border)' },
  } as const;
  const t = map[tone];
  return (
    <span
      className="inline-flex items-center rounded-[var(--rbc-radius-pill)] px-2 py-0.5 text-xs"
      style={{ background: t.bg, color: t.fg, border: `1px solid ${t.bd}` }}
    >
      {children}
    </span>
  );
}

/* ============================ EmptyState ============================ */

export function EmptyState({
  icon,
  title,
  body,
  action,
}: {
  icon: ReactNode;
  title: string;
  body: string;
  action?: ReactNode;
}) {
  return (
    <div
      role="status"
      className="rbc-card rbc-fade-up flex flex-col items-center gap-2 px-8 py-10 text-center"
      style={{ borderStyle: 'dashed' }}
    >
      <div
        className="mb-1 flex h-12 w-12 items-center justify-center rounded-[var(--rbc-radius-lg)]"
        style={{
          background: 'var(--rbc-bg-input)',
          boxShadow: 'inset 0 1px 0 rgba(255,255,255,0.05), 0 4px 14px rgba(0,0,0,0.3)',
          color: 'var(--rbc-text-disabled)',
        }}
      >
        {icon}
      </div>
      <div className="text-sm font-medium" style={{ color: 'var(--rbc-text-secondary)' }}>
        {title}
      </div>
      <div className="max-w-md text-xs leading-relaxed" style={{ color: 'var(--rbc-text-tertiary)' }}>
        {body}
      </div>
      {action && <div className="mt-2">{action}</div>}
    </div>
  );
}

/* ============================ Toggle（开关）============================ */

export function Toggle({
  checked,
  onChange,
  ariaLabel,
  disabled = false,
}: {
  checked: boolean;
  onChange: (b: boolean) => void;
  ariaLabel?: string | undefined;
  /** [UX 2026-11-05] 异步写入期间禁用，防连点产生竞态 */
  disabled?: boolean;
}) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      aria-label={ariaLabel}
      aria-disabled={disabled}
      disabled={disabled}
      onClick={() => { if (!disabled) onChange(!checked); }}
      className="relative inline-flex h-5 w-9 shrink-0 items-center rounded-full"
      style={{
        background: checked ? 'var(--rbc-accent)' : 'var(--rbc-border-strong)',
        boxShadow: checked ? 'var(--rbc-glow-accent)' : 'inset 0 1px 3px rgba(16,24,40,0.18)',
        transition: 'background var(--rbc-dur-180) ease, box-shadow var(--rbc-dur-180) ease',
      }}
    >
      {/* [UI-FIX 2026-11-05] 白圆点位置：原实现同时用 framer-motion 的 `layout`
          与手写 transform，两者互相覆盖——开启状态下圆点仍停在左侧（视觉上
          "只是变蓝"，看不出已开）。改为纯 CSS transform + transition：
          关闭时靠左（灰底），开启时右移 16px（蓝底）。 */}
      <span
        aria-hidden
        className="absolute top-0.5 left-0.5 h-4 w-4 rounded-full bg-white"
        style={{
          transform: checked ? 'translateX(16px)' : 'translateX(0)',
          boxShadow: '0 1px 3px rgba(16,24,40,0.3)',
          transition: 'transform var(--rbc-dur-180) cubic-bezier(0.34, 1.4, 0.64, 1)',
        }}
      />
    </button>
  );
}

/* ============================ Donut（百分比环）============================ */

export function Donut({
  percent,
  size = 150,
  stroke = 12,
  label,
  sub,
}: {
  percent: number | null;
  size?: number | undefined;
  stroke?: number | undefined;
  label: string;
  sub?: string | undefined;
}) {
  const R = (size - stroke) / 2;
  const C = 2 * Math.PI * R;
  const shown = percent === null ? null : Math.max(0, Math.min(100, percent));
  return (
    <svg width={size} height={size} role="img" aria-label={`${label} ${shown === null ? '无数据' : `${shown.toFixed(1)}%`}`}>
      <defs>
        <linearGradient id="rbc-donut-grad" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="var(--rbc-accent-hover)" />
          <stop offset="1" stopColor="var(--rbc-accent-active)" />
        </linearGradient>
      </defs>
      <circle cx={size / 2} cy={size / 2} r={R} fill="none" stroke="var(--rbc-track-bg)" strokeWidth={stroke} />
      {shown !== null && shown > 0 && (
        <motion.circle
          cx={size / 2}
          cy={size / 2}
          r={R}
          fill="none"
          stroke="url(#rbc-donut-grad)"
          strokeWidth={stroke}
          strokeLinecap="round"
          strokeDasharray={`${(shown / 100) * C} ${C}`}
          transform={`rotate(-90 ${size / 2} ${size / 2})`}
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          transition={{ duration: 0.4 }}
        />
      )}
      <text
        x={size / 2}
        y={size / 2 - 2}
        textAnchor="middle"
        fontSize={size / 5.2}
        fontWeight={600}
        fontFamily="var(--rbc-font-mono)"
        fill="var(--rbc-text-primary)"
      >
        {shown === null ? '—' : `${shown.toFixed(1)}%`}
      </text>
      <text x={size / 2} y={size / 2 + 18} textAnchor="middle" fontSize={11} fill="var(--rbc-text-tertiary)">
        {label}
      </text>
      {sub && (
        <text x={size / 2} y={size / 2 + 34} textAnchor="middle" fontSize={10} fill="var(--rbc-text-tertiary)">
          {sub}
        </text>
      )}
    </svg>
  );
}

/* ============================ NavItem（壳内导航复用）============================ */

/* Toggle / Donut 见上；NavItem 由 App 直接组合。 */

/* ============================ Skeleton（加载骨架）============================ */

export function Skeleton({ w = '100%', h = 14 }: { w?: string | number | undefined; h?: number | undefined }) {
  return (
    <div
      className="rbc-breach"
      style={{
        width: w,
        height: h,
        borderRadius: 6,
        // 中性扫光：复用 shimmer 动画但去色（灰白扫过）
        background:
          'linear-gradient(100deg, var(--rbc-track-bg) 30%, var(--rbc-bg-surface-hover) 50%, var(--rbc-track-bg) 70%)',
        backgroundSize: '220% 100%',
      }}
      aria-hidden
    />
  );
}

export function SkeletonCard({ rows = 2 }: { rows?: number | undefined }) {
  return (
    <div className="rbc-card flex flex-col gap-2" aria-busy>
      <Skeleton w="30%" h={12} />
      {Array.from({ length: rows }).map((_, i) => (
        <Skeleton key={i} w={`${100 - i * 18}%`} h={20} />
      ))}
    </div>
  );
}

/* ============================ Button ============================ */

export function Button({
  children,
  onClick,
  tone = 'secondary',
  disabled,
  type = 'button',
  ariaLabel,
  icon,
}: {
  children: ReactNode;
  onClick?: (() => void) | undefined;
  tone?: 'primary' | 'secondary' | 'ghost' | 'danger';
  disabled?: boolean | undefined;
  type?: 'button' | 'submit' | undefined;
  ariaLabel?: string | undefined;
  icon?: ReactNode;
}) {
  const cls = {
    primary: 'rbc-btn-primary',
    secondary: 'rbc-btn-secondary',
    ghost: 'rbc-btn-ghost',
    danger: 'rbc-btn-danger',
  }[tone];
  // [exactOptionalPropertyTypes] motion 的可选 props 不接受显式 undefined——
  // 禁用时整体省略交互属性
  const interaction = disabled
    ? {}
    : { whileHover: { y: -1 }, whileTap: { scale: 0.96 } };
  return (
    <motion.button
      type={type}
      onClick={onClick}
      disabled={disabled}
      aria-label={ariaLabel}
      {...interaction}
      className={`rbc-btn ${cls}`}
    >
      {icon}
      {children}
    </motion.button>
  );
}
