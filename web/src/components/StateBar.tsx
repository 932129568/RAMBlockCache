import { motion } from 'motion/react';
import type { ConnectionPhase } from '@state/driver-store';
import { Icons } from '@components/ui';

/**
 * 四段状态条 —— WPF Controls/StateBar 的 web 移植。
 * 四段：连接 / 引擎 / 熔断 / 安全。每段三态语义色 + 图标；
 * 熔断与安全段激活时 motion 脉冲（呼吸），尊重 prefers-reduced-motion
 * （tokens.css 已把 --rbc-dur-* 归零，motion 时长取自同一令牌）。
 * [只读 OneWay 纪律] 段标签说明"现在是什么"，不承诺"正在做什么"。
 */

export interface StateBarProps {
  phase: ConnectionPhase;
  /** 引擎开关（RbStats.Enabled） */
  enabled: boolean | null;
  panic: boolean | null;
  safeMode: boolean | null;
}

type Seg = {
  label: string;
  tone: 'ok' | 'warn' | 'danger' | 'off';
  pulse: boolean;
  hint: string;
  icon: React.ReactNode;
};

const TONE: Record<Seg['tone'], { bg: string; fg: string; border: string }> = {
  ok: { bg: 'var(--rbc-success-bg)', fg: 'var(--rbc-success-fg)', border: 'var(--rbc-success-border)' },
  warn: { bg: 'var(--rbc-warning-bg)', fg: 'var(--rbc-warning-fg)', border: 'var(--rbc-warning-border)' },
  danger: { bg: 'var(--rbc-danger-bg)', fg: 'var(--rbc-danger-fg)', border: 'var(--rbc-danger-border)' },
  off: { bg: 'var(--rbc-neutral-bg)', fg: 'var(--rbc-neutral-fg)', border: 'var(--rbc-neutral-border)' },
};

export default function StateBar({ phase, enabled, panic, safeMode }: StateBarProps) {
  const segments: Seg[] = [
    {
      label: '连接',
      tone:
        phase === 'online' ? 'ok' : phase === 'degraded' ? 'warn' : phase === 'connecting' ? 'off' : 'danger',
      pulse: false,
      icon: <Icons.pulse size={13} />,
      hint:
        phase === 'online'
          ? '已与驱动握手'
          : phase === 'degraded'
            ? '连接存在降级'
            : phase === 'connecting'
              ? '握手中'
              : '未连接驱动',
    },
    {
      label: '引擎',
      tone: enabled === null ? 'off' : enabled ? 'ok' : 'off',
      pulse: false,
      icon: <Icons.layers size={13} />,
      hint: enabled === null ? '未采样' : enabled ? '缓存引擎开启' : '缓存引擎关闭',
    },
    {
      label: '熔断',
      tone: panic === null ? 'off' : panic ? 'warn' : 'ok',
      pulse: panic === true,
      icon: <Icons.shield size={13} />,
      hint: panic === null ? '未采样' : panic ? '全局直通中' : '未熔断',
    },
    {
      label: '安全',
      tone: safeMode === null ? 'off' : safeMode ? 'danger' : 'ok',
      pulse: safeMode === true,
      icon: <Icons.gauge size={13} />,
      hint: safeMode === null ? '未采样' : safeMode ? '安全模式生效' : '未进入安全模式',
    },
  ];

  return (
    <div className="flex gap-1.5" role="status" aria-label="驱动状态条">
      {segments.map((s) => {
        const t = TONE[s.tone];
        return (
          <motion.div
            key={s.label}
            title={s.hint}
            animate={s.pulse ? { opacity: [1, 0.55, 1] } : { opacity: 1 }}
            transition={s.pulse ? { duration: 1.2, repeat: Infinity, ease: 'easeInOut' } : { duration: 0 }}
            className="flex flex-1 items-center justify-center gap-1.5 rounded-[var(--rbc-radius-sm)] px-2 py-1.5 text-xs"
            style={{ background: t.bg, color: t.fg, border: `1px solid ${t.border}` }}
          >
            {s.icon}
            {s.label}
          </motion.div>
        );
      })}
    </div>
  );
}
