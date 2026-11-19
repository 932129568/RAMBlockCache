/**
 * 设计系统原语 —— 品牌层（2026-11-12 从 ui.tsx 拆出，纯搬运）。
 */

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
