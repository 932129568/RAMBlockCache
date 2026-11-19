/**
 * 设计系统原语 —— 数值展示层（2026-11-12 从 ui.tsx 拆出，纯搬运）。
 * AnimatedValue：弹簧补间数字；Donut：弹簧弧长百分比环；Metric：指标卡。
 * 三者同源共用 `@lib/motion` 的 SPRING.value，保证环与数字永远一致。
 */

import { useEffect, useState } from 'react';
import { motion, useSpring, useTransform } from 'motion/react';
import type { ReactNode } from 'react';
import { DUR, SPRING, useMotion } from '@lib/motion';

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

/* ============================ Donut（百分比环）============================ */

/**
 * [A7 2026-11-11] 原来只有 `initial opacity:0 → 1` 的淡入：弧长是硬跳的。
 * 命中率每 1s 刷新一次，硬跳会让人误以为"数据抖了一下"。
 * 改为弧长弹簧补间（与 AnimatedValue 同一 SPRING.value 参数），
 * 数值文字同步使用弹簧后的值 —— 环与数字永远一致，不会出现
 * "环已到位、数字还在旧值"的错位。
 */
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
  const { reduce } = useMotion();

  /* 弹簧目标值：无数据时停在 0（环不画），有数据时逼近真实百分比 */
  const spring = useSpring(shown ?? 0, SPRING.value);
  useEffect(() => {
    spring.set(shown ?? 0);
  }, [shown, spring]);
  const [live, setLive] = useState(shown ?? 0);
  useEffect(
    () =>
      spring.on('change', (v) => {
        setLive(v);
      }),
    [spring],
  );

  const dash = useTransform(spring, (v) => `${(Math.max(0, Math.min(100, v)) / 100) * C} ${C}`);

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
          /* 弧长由弹簧 MotionValue 驱动（SVG 属性接受 MotionValue） */
          style={{ strokeDasharray: reduce ? `${(shown / 100) * C} ${C}` : dash }}
          transform={`rotate(-90 ${size / 2} ${size / 2})`}
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          transition={{ duration: reduce ? 0 : DUR.base }}
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
        {shown === null ? '—' : `${live.toFixed(1)}%`}
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
