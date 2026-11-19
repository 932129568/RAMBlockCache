import { useReducedMotion } from 'motion/react';
import type { Transition, Variants } from 'motion/react';

/**
 * 统一动效原语层（2026-11-11）。
 *
 * 【为什么需要这一层】
 * 此前动效散落在各处：CSS 用 `--rbc-dur-*`，motion 组件用硬编码
 * `transition={{ duration: 1.2 }}`。两套体系互不相通，导致
 * `@media (prefers-reduced-motion: reduce)` 只对 CSS 生效，
 * JS 动画照常播放——无障碍承诺是空的（F-UI2）。
 *
 * 本模块是 JS 侧唯一事实源：所有 motion 组件的时长/缓动都从这里取，
 * 且统一经 `useMotion()` 归一化 → 系统减弱动效时全局归零。
 *
 * 【设计依据】
 * - 时长：Material Motion / Apple HIG 的 UI 动效区间 100–300ms。
 *   超过 320ms 的 UI 转场会被感知为"卡"。
 * - 缓动：enter 用 decelerate（快进慢停），exit 用 accelerate（慢进快出），
 *   位移用 standard（对称）。这是 iOS / Material 的共同选择。
 * - 只动 `transform` 与 `opacity`：两者走合成层，不触发 layout/paint。
 *
 * 【与 tokens.css 的关系】
 * 时长数值必须与 `--rbc-dur-*` 保持一致（90/120/180/240/320/480）。
 * 改一侧必须同步另一侧；本文件顶部的 DUR 表是 JS 侧镜像。
 */

/** 时长（秒），镜像 tokens.css 的 --rbc-dur-*（ms → s） */
export const DUR = {
  /** 90ms —— 按压回弹、即时反馈 */
  instant: 0.09,
  /** 120ms —— 悬停、颜色变化 */
  fast: 0.12,
  /** 180ms —— 弹层进出场、卡片入场 */
  base: 0.18,
  /** 240ms —— 页面入场、列表 stagger */
  slow: 0.24,
  /** 320ms —— 进度条、图表首次绘制 */
  slower: 0.32,
  /** 480ms —— 仅用于强调性一次性动画 */
  emphasis: 0.48,
} as const;

/**
 * 缓动曲线。
 * - standard：对称，用于位移与尺寸
 * - decel：快进慢停，用于"进入"
 * - accel：慢进快出，用于"离开"
 *
 * 类型必须是 motion 的 `Easing`（其定义为 `[number,number,number,number]`
 * 四元组而非普通 number[]）——普通数组会被 exactOptionalPropertyTypes 拒绝。
 */
export const EASE = {
  standard: [0.2, 0, 0, 1],
  decel: [0, 0, 0.2, 1],
  accel: [0.4, 0, 1, 1],
} as const satisfies Record<string, readonly [number, number, number, number]>;

/** 弹簧预设：按压/回弹（高刚度 = 跟手，不拖沓） */
export const SPRING = {
  /** 按压回弹：瞬时响应，轻微过冲 */
  press: { type: 'spring', stiffness: 520, damping: 32, mass: 0.6 },
  /** 布局位移：导航指示器、卡片重排 */
  layout: { type: 'spring', stiffness: 420, damping: 34 },
  /** 数值：避免跳动，平滑逼近 */
  value: { stiffness: 160, damping: 26 },
} as const;

/**
 * 动效偏好（唯一入口）。
 *
 * `useReducedMotion()` 在 SSR/首帧可能返回 null —— 一律按 false 处理
 * （先动画后归零比先静止后突跳更糟，且首帧动画通常来不及被看到）。
 */
export type Ease4 = readonly [number, number, number, number];

export function useMotion(): {
  /** 系统是否要求减弱动效 */
  reduce: boolean;
  /** 归一化时长：reduce 时恒为 0 */
  dur: (seconds: number) => number;
  /** 归一化 transition：reduce 时时长归零 */
  t: (seconds: number, ease?: Ease4) => Transition;
} {
  const reduce = useReducedMotion() ?? false;
  return {
    reduce,
    dur: (seconds) => (reduce ? 0 : seconds),
    t: (seconds, ease) =>
      ({
        duration: reduce ? 0 : seconds,
        ease: [...(ease ?? EASE.standard)],
      }) as Transition,
  };
}

/* ============================================================================
 * 变体（Variants）—— 供 AnimatePresence 与 stagger 编排使用
 * ========================================================================= */

/**
 * 页面入场。
 *
 * 【历史坑，必须遵守】App.tsx 2026-10-31 曾因页面级 motion 淡入与卡片级
 * `.rbc-fade-up` 双层动画叠加，导致 hash 路由切换后容器滞留 opacity:0
 * （实测标题透明）。根因是「同一视觉属性由两套动画系统同时写」。
 *
 * 因此本变体的约束是：
 *  1. 只由页面容器使用，容器内不得再挂 `.rbc-fade-up`（改由 stagger 接管）；
 *  2. **不做 exit**（配合 mode="popLayout" 之外的 enter-only 语义）；
 *  3. 终态显式写死 `opacity: 1`，不依赖动画完成回调。
 */
export const pageEnter: Variants = {
  initial: { opacity: 0, y: 6 },
  animate: { opacity: 1, y: 0 },
};

/** 卡片/列表项 stagger 容器：子项依次入场 */
export function staggerContainer(step = 0.04, delay = 0): Variants {
  return {
    initial: {},
    animate: {
      transition: { staggerChildren: step, delayChildren: delay },
    },
  };
}

/** stagger 子项：与 pageEnter 同形但位移更小（避免"整页在飘"） */
export const staggerItem: Variants = {
  initial: { opacity: 0, y: 8 },
  animate: {
    opacity: 1,
    y: 0,
    transition: { duration: DUR.slow, ease: EASE.decel },
  },
};

/** 弹层（Dialog/Popover）：缩放 + 位移，终态显式 scale:1 */
export const overlayIn: Variants = {
  initial: { opacity: 0, scale: 0.97, y: 6 },
  animate: { opacity: 1, scale: 1, y: 0 },
  exit: { opacity: 0, scale: 0.98, y: 4 },
};

/** 单行列表项（Toast、告警条）：水平滑入 */
export const slideIn: Variants = {
  initial: { opacity: 0, x: 16 },
  animate: { opacity: 1, x: 0 },
  exit: { opacity: 0, x: 24 },
};

/** 顶部横幅：垂直滑入 */
export const bannerIn: Variants = {
  initial: { opacity: 0, y: -8, height: 0 },
  animate: { opacity: 1, y: 0, height: 'auto' },
  exit: { opacity: 0, y: -8, height: 0 },
};

/* ============================================================================
 * 工具
 * ========================================================================= */

/**
 * 把变体包装成"减弱动效时退化为纯 opacity 瞬时切换"。
 *
 * 用于必须保留结构变化（如高度展开）但不应有位移的场景：
 * 直接 set duration 0 会让 height 突变，观感更差；这里只保留 opacity。
 */
export function withReducedMotion(v: Variants, reduce: boolean): Variants {
  if (!reduce) return v;
  const only = (k: string): Record<string, number> => {
    const src = v[k];
    if (!src || typeof src !== 'object') return {};
    const out: Record<string, number> = {};
    for (const [key, val] of Object.entries(src)) {
      if (key === 'transition') continue;
      if (typeof val === 'number') out[key] = val;
    }
    return out;
  };
  return {
    initial: only('initial'),
    animate: only('animate'),
    exit: only('exit'),
  };
}
