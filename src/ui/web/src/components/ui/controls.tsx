/**
 * 设计系统原语 —— 交互控件层（2026-11-12 从 ui.tsx 拆出，纯搬运）。
 * 动效原则：按压回弹（scale .96/.965）、悬停位移 1px；减弱动效时直接不注册交互属性。
 */

import { motion } from 'motion/react';
import type { ReactNode } from 'react';
import { DUR, EASE, useMotion } from '@lib/motion';

/* ============================ Hint ============================ */
/* [UI-HINT 2026-11-09] 悬停详解：可见文案只留"是什么+提升"，鼠标靠近显示完整说明 */
export function Hint({ text, children }: { text: string; children: ReactNode }) {
  return (
    <span className="relative group">
      <span style={{ textDecoration: 'underline dotted', textUnderlineOffset: 3, cursor: 'help' }}>
        {children}
      </span>
      <span
        className="hidden group-hover:block absolute z-50 left-0 top-full mt-1 w-80 rounded-[var(--rbc-radius-sm)] text-xs"
        style={{
          background: 'var(--rbc-bg-elevated)',
          color: 'var(--rbc-text-secondary)',
          border: '1px solid var(--rbc-border-strong)',
          boxShadow: 'var(--rbc-shadow-card)',
          padding: '8px 10px',
          lineHeight: 1.6,
        }}
      >
        {text}
      </span>
    </span>
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
  const { reduce } = useMotion();
  const cls = {
    primary: 'rbc-btn-primary',
    secondary: 'rbc-btn-secondary',
    ghost: 'rbc-btn-ghost',
    danger: 'rbc-btn-danger',
  }[tone];
  /* [A7 2026-11-11] 交互反馈统一走 motion 原语：
   * - 悬停位移 1px + 按压 0.96 保持不变（已在真机验证过手感）；
   * - 新增 transition 显式声明，避免 motion 用默认 spring 覆盖 CSS 的
   *   background/border 过渡节奏（两套时长不一致时按钮会"手感和颜色不同步"）；
   * - 减弱动效时整体省略（而不是 duration:0）——直接不注册交互属性，
   *   连事件监听都不挂，零开销且绝无残留 transform。 */
  const interaction = disabled || reduce
    ? {}
    : {
        whileHover: { y: -1 },
        whileTap: { scale: 0.96 },
        transition: { duration: DUR.instant, ease: EASE.standard },
      };
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
