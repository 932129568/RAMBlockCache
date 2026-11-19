import { useMemo, useState } from 'react';

/**
 * StripeMatrix —— 64 路分片热力矩阵（块缓存的「指纹」视觉）
 *
 * 为什么要有这个组件
 * ------------------
 * `RbStats.StripeRead[64]` / `StripeWrite[64]` / `StripeCount` 这三个字段从驱动
 * 一路传到前端（`protocol/types.ts:74-76`），但在此之前**没有任何消费方**。
 * 而它们恰好是唯一能回答「缓存正在干什么」的形状数据：
 *   - 哪些分片承压（热）/ 哪些一次都没被碰过（冷）
 *   - 读写在分片维度上是否失衡（写集中的盘，回写时会打爆单一 stripe）
 *   - 换了工作负载之后指纹怎么变（这是最直接的调优反馈）
 * 命中率只有一个标量，抹掉了全部形状。
 *
 * 为什么是条形矩阵而不是折线/雷达
 * ------------------------------
 * 64 个离散分片没有"相邻"语义（stripe 是哈希分片，索引 0 与 1 不相邻），
 * 折线图会暗示一个不存在的连续性。所以用**探针卡式条形阵列**：只编码
 * 档位、不假装精确，扫一眼就有结论。
 *
 * 编码纪律（重要）
 * ----------------
 * 1. **颜色 + 高度双通道**。只靠颜色编码对红绿色盲不可靠，所以 3 档同时
 *    长高 6px，用形态做第二次编码。
 * 2. **0 档必须可见**。冷分片给一条极淡描边，否则"没流量"与"渲染失败"
 *    长得一模一样。
 * 3. **对数刻度**。分片计数是长尾分布（个别 stripe 吃掉 90% 流量），
 *    线性刻度下除了一两根柱子以外全是零，矩阵退化成一根针。
 * 4. **不伪造数据**。全零时显示空态文案，不画一排"最低档"的假矩阵
 *    （那会让人以为"有流量但很小"）。
 */
export type StripeMatrixProps = {
  /** 每分片读次数（长度应为 StripeCount） */
  read: number[];
  /** 每分片写次数 */
  write: number[];
  /** 驱动声明的分片数（默认 64） */
  count?: number;
};

type Row = 'read' | 'write';

const LEVEL_LABELS = ['无流量', '低', '中', '高'] as const;

/** 对数归一化到 0~3 档；全零返回全 0，单一非零值返回 1 档（不虚报成 3 档） */
function normalize(a: number[], b: number[]): { ra: number[]; rb: number[] } {
  let max = 0;
  for (const v of a) if (v > max) max = v;
  for (const v of b) if (v > max) max = v;
  const lv = (c: number): number => {
    if (!Number.isFinite(c) || c <= 0) return 0;
    if (max <= 1) return 1;
    const t = Math.log1p(c) / Math.log1p(max);
    if (t < 0.34) return 1;
    if (t < 0.7) return 2;
    return 3;
  };
  return { ra: a.map(lv), rb: b.map(lv) };
}

export default function StripeMatrix({ read, write, count = 64 }: StripeMatrixProps) {
  const [hover, setHover] = useState<{ row: Row; i: number } | null>(null);

  const n = Math.max(1, count);
  const r = useMemo(() => Array.from({ length: n }, (_, i) => read[i] ?? 0), [read, n]);
  const w = useMemo(() => Array.from({ length: n }, (_, i) => write[i] ?? 0), [write, n]);

  const { ra, rb } = useMemo(() => normalize(r, w), [r, w]);

  const stats = useMemo(() => {
    let hot = 0;
    let cold = 0;
    for (let i = 0; i < n; i++) {
      const pr = r[i] ?? 0;
      const pw = w[i] ?? 0;
      if (pr === 0 && pw === 0) cold++;
      else if (ra[i] === 3 || rb[i] === 3) hot++;
    }
    return { hot, cold, active: n - cold };
  }, [r, w, ra, rb, n]);

  const allZero = stats.active === 0;

  const readout = hover
    ? (() => {
        const c = hover.row === 'read' ? (r[hover.i] ?? 0) : (w[hover.i] ?? 0);
        const lv = hover.row === 'read' ? ra[hover.i] : rb[hover.i];
        return `分片 #${hover.i} · ${hover.row === 'read' ? '读' : '写'} ${c.toLocaleString('zh-CN')} 次 · ${LEVEL_LABELS[lv ?? 0]}`;
      })()
    : null;

  return (
    <div className="flex flex-col gap-2.5">
      <div className="flex items-baseline justify-between gap-3">
        <span className="rbc-kicker">分片热力 · {n} 路</span>
        {/* 读数区固定占位，避免悬停时行高跳动 */}
        <span
          className="font-mono text-[10px]"
          style={{ color: hover ? 'var(--rbc-accent)' : 'var(--rbc-text-tertiary)' }}
          aria-live="polite"
        >
          {readout ??
            (allZero
              ? '尚无分片流量'
              : `活跃 ${stats.active}/${n} · 热点 ${stats.hot} · 冷分片 ${stats.cold}`)}
        </span>
      </div>

      <div
        className="rbc-matrix"
        role="img"
        aria-label={
          allZero
            ? `分片热力矩阵，共 ${n} 路，当前无读写信道流量`
            : `分片热力矩阵，共 ${n} 路，其中 ${stats.active} 路有流量，${stats.hot} 路为热点，${stats.cold} 路为冷分片`
        }
        onMouseLeave={() => setHover(null)}
      >
        {((
          [
            { key: 'read' as const, tag: 'READ', vals: r, lvs: ra },
            { key: 'write' as const, tag: 'WRITE', vals: w, lvs: rb },
          ]
        )).map((row) => (
          <div className="rbc-matrix-row" key={row.key}>
            <span className="rbc-matrix-tag">{row.tag}</span>
            {row.lvs.map((lv, i) => (
              <div
                key={i}
                className="rbc-stripe"
                data-lv={lv}
                data-hot={lv === 3 ? '1' : '0'}
                data-active={hover && hover.row === row.key && hover.i === i ? '1' : '0'}
                title={`#${i} ${row.tag} ${(row.vals[i] ?? 0).toLocaleString('zh-CN')} 次（${LEVEL_LABELS[lv] ?? '无流量'}）`}
                onMouseEnter={() => setHover({ row: row.key, i })}
              />
            ))}
          </div>
        ))}
      </div>

      <div className="rbc-matrix-legend">
        {LEVEL_LABELS.map((label, lv) => (
          <span key={label}>
            <i className="rbc-legend-swatch" data-lv={lv} />
            {label}
          </span>
        ))}
        <span style={{ marginLeft: 'auto', opacity: 0.75 }}>对数刻度 · 高度=热点</span>
      </div>
    </div>
  );
}
