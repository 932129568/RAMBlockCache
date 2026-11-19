/**
 * 设计系统原语 —— 容器/版式层（2026-11-12 从 ui.tsx 拆出，纯搬运）。
 * 视觉层全部由 tokens.css 的 .rbc-* 承担，本文件只组合。
 */

import type { ReactNode } from 'react';

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

/* ============================ Skeleton（加载骨架）============================ */

export function Skeleton({ w = '100%', h = 14 }: { w?: string | number | undefined; h?: number | undefined }) {
  return (
    <div
      /* [A7 2026-11-11] 原实现复用 .rbc-breach（SLO 违约暖黄扫光）——
        语义错位，且暖黄让加载态看起来像告警。改用独立 .rbc-skeleton
        （中性灰蓝 + 1.4s），并在减弱动效下自动退化为静态底色。 */
      className="rbc-skeleton"
      style={{ width: w, height: h, borderRadius: 6 }}
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
