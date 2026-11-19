/**
 * 设计系统原语 —— 图标层（2026-11-12 从 ui.tsx 拆出，纯搬运）。
 * stroke 风格 24×24 SVG，10 个图标；`I` 为内部渲染器，不对外导出。
 */

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
