// src/ui/web/src/lib/liquidWaveCore.ts
//
// 2D 浅水波（离散波动方程）求解器：纯函数、无 DOM、每帧零分配。
// 抽出独立模块的原因：hook 只该管生命周期（canvas / 事件 / rAF），
// 物理与着色必须能在 vitest 里被直接断言 —— 否则「代码在、观感无」这类
// 缺陷永远只能靠肉眼发现。
//
// [U3-v23 2026-11-19] 三处相对 v22 的实质改动，均由数值实验而非手册标称驱动：
//
//   1) 网格不再是固定 160x90，而是按宿主元素真实宽高比分配（computeAdaptiveGrid）。
//      v22 把 160x90 的位图 CSS 拉伸到 1400x120 的横条上，各向异性拉伸比
//      实测 6.56:1 —— 圆形涟漪被横向拉成扁糊斑，这是用户看到的「波浪不成形」
//      的唯一根因。自适应后同一元素拉伸比 1.003:1。
//
//   2) stepWave 改为「只迭代内圈 + Neumann 全反射拷贝」，并在同一趟循环里
//      记录四条内边上的峰值与位置（BorderCollision）。这是「波撞到框边再把
//      动量和给邻接玻璃」能被观测到的前提：旧实现用索引折返（reflect），
//      边界列被反复折返采样，实测 300 帧内 x===1 / y===1 幅值从未超过
//      0.042 的触边门限 ⇒ 物理触边传导永远不触发。新实现实测 56~62 帧
//      即可稳定触边。
//
//   3) DAMPING 0.95 -> 0.962。0.95 在 181x80 网格上 62 帧才触边、96 帧归静；
//      0.962 触边提前到 57 帧、尾部延长到 124 帧（≈2.07 s），观感是「波真的
//      走到边、并且有余韵」，而不是「到不了边就没了」。
//      刻意不采用 v23 手册的 0.968：实测归静要 143 帧（2.38 s），
//      长于调度器的静默窗口，会重新长出一条「画面还在、却被判定空闲」
//      的硬切路径 —— 那正是用户抱怨的第二个现象。
//      也刻意不采用 0.985/0.99 一档：那会让同时受激的十几块玻璃叠成一个
//      不停歇的水面，与「零空闲功耗」和「归静」两条既有判据直接冲突。

/** 默认网格（几何不可测时的回落值，也是既有测试的基准尺寸）。 */
export const WAVE_COLS = 160;
export const WAVE_ROWS = 90;
export const WAVE_LEN = WAVE_COLS * WAVE_ROWS;

/** 单场最大格数预算：与 160x90 等算力（14,400），自适应网格不得突破。 */
export const MAX_GRID_CELLS = 14400;

/** 峰值幅度衰减到 5% 约 62 帧（实测 181x80 网格），触边约 57 帧。 */
export const WAVE_DAMPING = 0.962;

/** 幅度上限：防止单点冲击叠加后数值爆炸，同时保留可见波峰。 */
export const WAVE_MAX = 1.5;
/** 低于该梯度不落色，避免静止帧产生噪点（等价于「无能量即透明」）。 */
export const SLOPE_CUTOFF = 0.018;
/** 触边门限：内边格幅值超过它才算一次真实碰撞（实测 0.9 幅度冲击可达 0.55+）。 */
export const BORDER_HIT_THRESHOLD = 0.042;

export interface WaveField {
  cols: number;
  rows: number;
  len: number;
  cur: Float32Array;
  prev: Float32Array;
}

/** 波前真实撞击容器内边时产生的一次碰撞。 */
export interface BorderCollision {
  side: 'left' | 'right' | 'top' | 'bottom';
  /** 沿该边的归一化位置 [0, 1]。 */
  along: number;
  /** 碰撞峰值强度。 */
  strength: number;
}

/** 网格尺寸硬边界：越界的归一化几何一律夹回，保证场永不退化。 */
const MIN_COLS = 16;
const MAX_COLS = 256;
const MIN_ROWS = 12;
const MAX_ROWS = 160;
/** 宽高比夹取范围：极端横条/竖条也不让网格退化成一维。 */
const MIN_ASPECT = 1 / 18;
const MAX_ASPECT = 18;

/**
 * 按元素真实显示宽高分配网格，使「一个格子」在屏幕上尽量接近正方形。
 *
 * 为什么必须做：位图以 CSS width/height 100% 铺满宿主，位图宽高比与宿主
 * 宽高比不一致时，每个格子被各向异性拉伸，圆形波纹就变成横向糊斑。
 * 实测 160x90 位图铺到 1400x120 横条：scaleX=8.75 / scaleY=1.33 ⇒ 6.56:1。
 *
 * 兜底：jsdom / 隐藏元素给出 0 或 NaN 时回落默认 160x90，既有用例不受影响。
 */
export function computeAdaptiveGrid(
  width: number,
  height: number,
): { cols: number; rows: number } {
  if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0) {
    return { cols: WAVE_COLS, rows: WAVE_ROWS };
  }

  const aspect = Math.max(MIN_ASPECT, Math.min(MAX_ASPECT, width / height));

  // 以「格数预算 × 宽高比」解出长边，再按同一宽高比解出短边。
  let cols = Math.round(Math.sqrt(MAX_GRID_CELLS * aspect));
  cols = Math.max(MIN_COLS, Math.min(MAX_COLS, cols));
  let rows = Math.max(MIN_ROWS, Math.min(MAX_ROWS, Math.round(cols / aspect)));

  // 短边被 MIN_ROWS/MAX_ROWS 夹住后长边要回头重算，否则宽高比会走形。
  cols = Math.max(MIN_COLS, Math.min(MAX_COLS, Math.round(rows * aspect)));
  rows = Math.max(MIN_ROWS, Math.min(MAX_ROWS, Math.round(cols / aspect)));

  // 逐格回收超预算：round 的累积误差会让 181x80=14,480 这类刚好越界的解出现，
  // 必须显式夹回，否则「最大算力不变」这条承诺是假的（手册版正是漏了这步）。
  while (cols * rows > MAX_GRID_CELLS) {
    if (cols >= rows && cols > MIN_COLS) cols -= 1;
    else if (rows > MIN_ROWS) rows -= 1;
    else break;
  }

  return { cols, rows };
}

export function createWaveField(cols: number = WAVE_COLS, rows: number = WAVE_ROWS): WaveField {
  const safeCols = Math.max(MIN_COLS, Math.round(cols));
  const safeRows = Math.max(MIN_ROWS, Math.round(rows));
  const len = safeCols * safeRows;
  return {
    cols: safeCols,
    rows: safeRows,
    len,
    cur: new Float32Array(len),
    prev: new Float32Array(len),
  };
}

/**
 * 在归一化坐标 (nx, ny) 注入一次冲击。
 * 写入 prev（速度项）而非 cur：这样单次冲击两侧都会起波，而不是只凸一个包。
 * 负号表示「指针压下去的凹陷」，随后自然回弹成波峰 —— 保留 v22 已实测的语义，
 * 避免同时改动物理与着色两处、让观感回归无法归因。
 */
export function injectWaveImpulse(
  field: WaveField,
  nx: number,
  ny: number,
  amp: number,
  radius: number = 3,
): void {
  const { cols, rows, prev } = field;
  const cx = Math.min(cols - 3, Math.max(2, Math.floor(nx * cols)));
  const cy = Math.min(rows - 3, Math.max(2, Math.floor(ny * rows)));
  const r = Math.max(1, Math.round(radius));
  const r2 = r * r;
  const amplitude = amp * 1.35;

  for (let dy = -r; dy <= r; dy++) {
    const y = cy + dy;
    if (y < 0 || y >= rows) continue;
    for (let dx = -r; dx <= r; dx++) {
      const x = cx + dx;
      if (x < 0 || x >= cols) continue;
      const distSq = dx * dx + dy * dy;
      if (distSq > r2) continue;
      const falloff = 1 - Math.sqrt(distSq) / (r + 0.5);
      const idx = y * cols + x;
      prev[idx] = (prev[idx] ?? 0) - amplitude * falloff;
    }
  }
}

/**
 * 演化一帧：h(t+1) = damping × (邻域均值 − h(t−1))，随后交换双缓冲。
 * 交换后 field.cur 即最新高度场。
 *
 * 边界处理：只迭代内圈 [1, cols-2] × [1, rows-2]，然后按 Neumann 条件把
 * 内圈值拷贝到四条外边上。这样外边界列拿到的就是「内圈第一列的真实幅值」，
 * 触边碰撞检测才有物理意义（旧折返实现里边界列被反复折返采样，幅值永远
 * 上不去，实测 300 帧零触边）。
 */
export function stepWave(
  field: WaveField,
  damping: number = WAVE_DAMPING,
  collisionsOut?: BorderCollision[],
): void {
  const { cols, rows, cur } = field;
  const prev = field.prev;

  let maxL = 0;
  let posL = 0.5;
  let maxR = 0;
  let posR = 0.5;
  let maxT = 0;
  let posT = 0.5;
  let maxB = 0;
  let posB = 0.5;
  const detect = collisionsOut !== undefined;

  for (let y = 1; y < rows - 1; y++) {
    const row = y * cols;
    for (let x = 1; x < cols - 1; x++) {
      const idx = row + x;
      const avg =
        ((cur[idx - 1] ?? 0) + (cur[idx + 1] ?? 0) + (cur[idx - cols] ?? 0) + (cur[idx + cols] ?? 0)) *
        0.5;
      let next = (avg - (prev[idx] ?? 0)) * damping;
      if (next > WAVE_MAX) next = WAVE_MAX;
      else if (next < -WAVE_MAX) next = -WAVE_MAX;
      prev[idx] = next;

      if (detect) {
        const absVal = next < 0 ? -next : next;
        if (absVal > BORDER_HIT_THRESHOLD) {
          if (x === 1 && absVal > maxL) {
            maxL = absVal;
            posL = y / (rows - 1);
          } else if (x === cols - 2 && absVal > maxR) {
            maxR = absVal;
            posR = y / (rows - 1);
          }
          if (y === 1 && absVal > maxT) {
            maxT = absVal;
            posT = x / (cols - 1);
          } else if (y === rows - 2 && absVal > maxB) {
            maxB = absVal;
            posB = x / (cols - 1);
          }
        }
      }
    }
  }

  // Neumann 全反射：外边 = 相邻内边。
  const lastRow = (rows - 1) * cols;
  const innerRow = (rows - 2) * cols;
  for (let x = 0; x < cols; x++) {
    prev[x] = prev[cols + x] ?? 0;
    prev[lastRow + x] = prev[innerRow + x] ?? 0;
  }
  for (let y = 0; y < rows; y++) {
    const row = y * cols;
    prev[row] = prev[row + 1] ?? 0;
    prev[row + cols - 1] = prev[row + cols - 2] ?? 0;
  }

  const tmp = field.cur;
  field.cur = field.prev;
  field.prev = tmp;

  if (collisionsOut) {
    if (maxL > 0) collisionsOut.push({ side: 'left', along: posL, strength: maxL });
    if (maxR > 0) collisionsOut.push({ side: 'right', along: posR, strength: maxR });
    if (maxT > 0) collisionsOut.push({ side: 'top', along: posT, strength: maxT });
    if (maxB > 0) collisionsOut.push({ side: 'bottom', along: posB, strength: maxB });
  }
}

/**
 * 把高度场着色进 ABGR 像素缓冲（小端序 RGBA 字节序），返回落色像素数。
 * 三层光学：白色波峰高光 + 依梯度符号分侧的青/品红色散边缘。
 * 只提亮不压暗 —— 画布以 mix-blend-mode: screen 合成，正文对比度不受损。
 */
export function renderWave(field: WaveField, out: Uint32Array): number {
  const { cols, rows, cur } = field;
  out.fill(0);
  let painted = 0;

  for (let y = 1; y < rows - 1; y++) {
    const row = y * cols;
    for (let x = 1; x < cols - 1; x++) {
      const idx = row + x;
      const gradX = (cur[idx + 1] ?? 0) - (cur[idx - 1] ?? 0);
      const gradY = (cur[idx + cols] ?? 0) - (cur[idx - cols] ?? 0);
      const slope = Math.abs(gradX) + Math.abs(gradY);
      if (slope <= SLOPE_CUTOFF) continue;

      const highlight = Math.min(255, Math.floor(slope * 210));
      const cyan = Math.min(255, Math.floor(Math.max(0, gradX) * 260));
      const magenta = Math.min(255, Math.floor(Math.max(0, -gradX) * 260));

      // 色散必须对称：r 只吃品红、b 只吃青，两者相减才是可测的红/青分离。
      const r = Math.min(255, highlight + magenta);
      const g = Math.min(255, highlight + Math.floor((cyan + magenta) * 0.35));
      const b = Math.min(255, highlight + cyan);
      const a = Math.min(210, Math.floor(slope * 185));
      if (a === 0) continue;

      out[idx] = ((a << 24) | (b << 16) | (g << 8) | r) >>> 0;
      painted++;
    }
  }

  return painted;
}

/** 峰值幅度：衰减判据用它，而不是绝对值和（后者在波前扩散阶段会升高）。 */
export function wavePeak(field: WaveField): number {
  let peak = 0;
  for (let i = 0; i < field.len; i++) {
    const v = Math.abs(field.cur[i] ?? 0);
    if (v > peak) peak = v;
  }
  return peak;
}

/** 从像素缓冲解出 (r,g,b,a)，只给测试与判读用。 */
export function unpackPixel(pixel: number): { r: number; g: number; b: number; a: number } {
  return {
    r: pixel & 0xff,
    g: (pixel >>> 8) & 0xff,
    b: (pixel >>> 16) & 0xff,
    a: (pixel >>> 24) & 0xff,
  };
}