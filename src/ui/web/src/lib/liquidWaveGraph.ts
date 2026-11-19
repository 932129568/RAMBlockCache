// src/ui/web/src/lib/liquidWaveGraph.ts
//
// U3 跨玻璃解析耦合内核：纯函数、无 DOM 副作用（除 map*/refreshSlot 读几何）。
//
// 两条传导通道并存，这是「波浪相互碰撞」这条需求的技术落地：
//
//   A) 事件驱动（planCoupling）—— 指针一动就按速度算出本地幅度，再按
//      「到目标边的距离 × 界面透射率」解析地投给邻接玻璃：
//         amp = baseAmp · e^(-d/λ) · κ · w · hopAtten
//         reach(a) = λ · ln(a·κ / minAmp)，随力度单调增
//      存在的理由：手势必须即时响应，等待波前走到边会让手感延迟 1 s。
//
//   B) 物理触边（planBorderCollisionCoupling）—— 波前真的撞到容器内边时
//      把动量投给父底板。存在的理由：只有 A 的话，任何位置的轻微抖动都在
//      投递，观感是「无处不在的联动」而不是「碰撞」。B 让底板上的亮点
//      出现在卡片框边的真实位置，晚于卡片自身起波 —— 这才读得出因果。
//
// [v23 2026-11-19] 上文曾断言「波永远走不到框边，所以只能事件驱动」。
// 该结论在旧实现下成立的原因不是阻尼太大，而是边界用索引折返采样
// （旧 liquidWaveCore.stepWave 对 x<0 折回 1、对 x>=cols 折回 cols-2），
// 边界列被反复折返，幅值永远上不去。改为内圈迭代 + Neumann 拷贝后，
// 实测 57 帧即可稳定触边，物理通道才真正可用。因此本文件现在两通道都有。

import type { BorderCollision } from './liquidWaveCore';

export type SurfaceLayer = 'section' | 'card';
export type EdgeSide = 'left' | 'right' | 'top' | 'bottom';

export interface NormRect {
  x0: number;
  y0: number;
  x1: number;
  y1: number;
}

export interface Point2 {
  x: number;
  y: number;
}

export interface WaveImpulseSample {
  nx: number;
  ny: number;
  amp: number;
}

/** 纯数据节点（不含 DOM），便于在 Node 环境直接单测 planCoupling。 */
export interface CouplingNode {
  id: string;
  layer: SurfaceLayer;
  parentId: string | null;
  childIds: string[];
  /** card 在父 section 内的归一化占位；无父节点或几何不可用时为 null。 */
  slot: NormRect | null;
}

/** DOM 侧节点：挂接宿主元素供 Hook 使用。 */
export interface SurfaceNode extends CouplingNode {
  host: HTMLElement;
}

export interface WaveGraph {
  nodes: Map<string, SurfaceNode>;
  byHost: Map<HTMLElement, string>;
}

export interface EdgeHit {
  side: EdgeSide;
  weight: number;
  distance: number;
}

export interface CouplingInput {
  /** 指针所在节点的 id（card 或 section）。 */
  sourceId: string;
  /** 指针在 source 内的归一化位置 [0, 1]。 */
  nx: number;
  ny: number;
  /** 归一化单位运动方向；无方向时传 (0, 0)。 */
  unx: number;
  uny: number;
  /** source 的本地注入幅度（已含层权重）。 */
  baseAmp: number;
  /** 当前指针所在的 card id（同一事件两次调用共用，用于防回声）。 */
  pointerCardId: string | null;
  /** 冷却表：key = `${sourceId}>${targetId}` -> 上次投递时刻 (ms)。 */
  cooldown: Map<string, number>;
  now: number;
}

export interface Injection {
  targetId: string;
  /** 目标局部归一化注入点 [0, 1]。 */
  nx: number;
  ny: number;
  amp: number;
  /** 第 1 跳（直接撞框传导）或第 2 跳（底层中继传导至兄弟卡片）。 */
  hop: 1 | 2;
  /** 物理触边碰撞传导时，碰撞发生在源的哪条边（用于给目标打撞击高光位置）。 */
  edgeSide?: EdgeSide;
  /** 碰撞点沿该边的归一化位置。 */
  edgeAlong?: number;
}

/** 解析耦合参数（唯一调参点）。区间由 liquidWaveGraph.test.ts 的 V13 断言钉死。 */
export const COUPLING = Object.freeze({
  kappa: 0.52, // 界面幅度透射率
  lambda: 0.4, // 衰减长度（手感补偿系数）
  minAmp: 0.06, // 投递下限
  maxAmp: 0.45, // 单次投递安全阀
  secondaryMinWeight: 0.18, // 次边最低权重门槛
  cooldownMs: 45, // 同一源->目标冷却窗口 (ms)
  hopAtten: 0.55, // 第 2 跳额外衰减
  directionDotMin: 0.5, // 第 2 跳方向一致性最低余弦
  maxSecondaryTargets: 2, // 第 2 跳最多同时带动的兄弟卡片数
  vMin: 0.05, // 归一化速度方向门槛 (1/s)
});

/** 判定「落点落在 card 外」的容差（避免 1px 抖动丢掉整次投递）。 */
const EDGE_MARGIN = 0.06;

function clamp01(v: number): number {
  if (!(v > 0)) return 0;
  if (v >= 1) return 1;
  return v;
}

function clamp(v: number, lo: number, hi: number): number {
  if (!(v > lo)) return lo;
  if (v > hi) return hi;
  return v;
}

function classifyLayer(host: HTMLElement): SurfaceLayer {
  return host.classList.contains('rbc-card') ? 'card' : 'section';
}

/**
 * card 局部 [0,1] -> section 归一化 [0,1]。
 * offset 布局值优先（不触发重排），offsetParent 关系不成立时回退 rect。
 */
export function mapCardToSection(
  card: HTMLElement,
  section: HTMLElement,
  nx: number,
  ny: number,
): Point2 {
  const rw = section.clientWidth;
  const rh = section.clientHeight;
  const ow = card.offsetWidth;
  const oh = card.offsetHeight;

  if (rw > 0 && rh > 0 && ow > 0 && oh > 0 && card.offsetParent === section) {
    return {
      x: clamp01((card.offsetLeft + nx * ow) / rw),
      y: clamp01((card.offsetTop + ny * oh) / rh),
    };
  }

  const cRect = card.getBoundingClientRect();
  const sRect = section.getBoundingClientRect();
  if (sRect.width <= 0 || sRect.height <= 0) {
    return { x: clamp01(nx), y: clamp01(ny) };
  }
  return {
    x: clamp01((cRect.left - sRect.left + nx * cRect.width) / sRect.width),
    y: clamp01((cRect.top - sRect.top + ny * cRect.height) / sRect.height),
  };
}

/** section 归一化 [0,1] -> card 局部 [0,1]；落在 card 外（含容差）返回 null。 */
export function mapSectionToCard(
  card: HTMLElement,
  section: HTMLElement,
  sx: number,
  sy: number,
): Point2 | null {
  const rw = section.clientWidth;
  const rh = section.clientHeight;
  const ow = card.offsetWidth;
  const oh = card.offsetHeight;

  let nx: number;
  let ny: number;

  if (rw > 0 && rh > 0 && ow > 0 && oh > 0 && card.offsetParent === section) {
    nx = (sx * rw - card.offsetLeft) / ow;
    ny = (sy * rh - card.offsetTop) / oh;
  } else {
    const cRect = card.getBoundingClientRect();
    const sRect = section.getBoundingClientRect();
    if (cRect.width <= 0 || cRect.height <= 0 || sRect.width <= 0 || sRect.height <= 0) {
      return null;
    }
    nx = (sRect.left + sx * sRect.width - cRect.left) / cRect.width;
    ny = (sRect.top + sy * sRect.height - cRect.top) / cRect.height;
  }

  if (nx < -EDGE_MARGIN || nx > 1 + EDGE_MARGIN) return null;
  if (ny < -EDGE_MARGIN || ny > 1 + EDGE_MARGIN) return null;

  return { x: clamp01(nx), y: clamp01(ny) };
}

/** 刷新子节点在父 section 内的归一化占位 slot。几何退化（<2%）时置 null。 */
export function refreshSlot(node: SurfaceNode, parent: SurfaceNode): boolean {
  const p0 = mapCardToSection(node.host, parent.host, 0, 0);
  const p1 = mapCardToSection(node.host, parent.host, 1, 1);
  const x0 = Math.min(p0.x, p1.x);
  const y0 = Math.min(p0.y, p1.y);
  const x1 = Math.max(p0.x, p1.x);
  const y1 = Math.max(p0.y, p1.y);

  if (x1 - x0 < 0.02 || y1 - y0 < 0.02) {
    node.slot = null;
    return false;
  }
  node.slot = { x0, y0, x1, y1 };
  return true;
}

/**
 * 基于 DOM 包含关系（parentElement.closest('.rbc-section')）构建波场拓扑图。
 * 孤儿 card（父 section 未被扫描到）parentId 保持 null ⇒ 退化为本地独立波场。
 */
export function buildWaveGraph(hosts: HTMLElement[]): WaveGraph {
  const nodes = new Map<string, SurfaceNode>();
  const byHost = new Map<HTMLElement, string>();

  hosts.forEach((host, idx) => {
    const id = host.dataset.rbcWaveId || `rbc-surf-${idx}`;
    host.dataset.rbcWaveId = id;
    nodes.set(id, {
      id,
      layer: classifyLayer(host),
      parentId: null,
      childIds: [],
      slot: null,
      host,
    });
    byHost.set(host, id);
  });

  for (const node of nodes.values()) {
    const parentEl = node.host.parentElement?.closest<HTMLElement>('.rbc-section') ?? null;
    if (!parentEl || parentEl === node.host) continue;
    const pid = byHost.get(parentEl);
    if (!pid || pid === node.id) continue;
    const parentNode = nodes.get(pid);
    if (!parentNode) continue;
    node.parentId = pid;
    parentNode.childIds.push(node.id);
    refreshSlot(node, parentNode);
  }

  return { nodes, byHost };
}

/** 边 + 沿边投影位置 -> 局部归一化坐标。 */
export function pointOnEdge(side: EdgeSide, along: number): Point2 {
  const t = clamp01(along);
  switch (side) {
    case 'left':
      return { x: 0, y: t };
    case 'right':
      return { x: 1, y: t };
    case 'top':
      return { x: t, y: 0 };
    case 'bottom':
    default:
      return { x: t, y: 1 };
  }
}

function distanceToSide(nx: number, ny: number, side: EdgeSide): number {
  const cx = clamp01(nx);
  const cy = clamp01(ny);
  switch (side) {
    case 'left':
      return cx;
    case 'right':
      return 1 - cx;
    case 'top':
      return cy;
    case 'bottom':
    default:
      return 1 - cy;
  }
}

/**
 * 指针位置 + 单位方向 -> 命中的边（1 或 2 条，权重降序）。
 * 方向退化（un=(0,0)）时返回单条最近边、权重 1.0。
 * 权重按 |dx| : |dy| 分配到水平边与垂直边（斜向滑动同时点亮两条边）。
 */
export function pickEdges(nx: number, ny: number, unx: number, uny: number): EdgeHit[] {
  const mag = Math.hypot(unx, uny);
  if (!(mag >= 1e-6)) {
    const sides: EdgeSide[] = ['left', 'right', 'top', 'bottom'];
    let bestSide: EdgeSide = 'left';
    let bestDist = Infinity;
    for (const s of sides) {
      const d = distanceToSide(nx, ny, s);
      if (d < bestDist) {
        bestDist = d;
        bestSide = s;
      }
    }
    return [{ side: bestSide, weight: 1.0, distance: bestDist }];
  }

  const dx = unx / mag;
  const dy = uny / mag;
  const ax = Math.abs(dx);
  const ay = Math.abs(dy);
  const denom = ax + ay || 1;
  const wx = ax / denom;
  const wy = ay / denom;

  const hSide: EdgeSide = dx >= 0 ? 'right' : 'left';
  const vSide: EdgeSide = dy >= 0 ? 'bottom' : 'top';
  const hHit: EdgeHit = { side: hSide, weight: wx, distance: distanceToSide(nx, ny, hSide) };
  const vHit: EdgeHit = { side: vSide, weight: wy, distance: distanceToSide(nx, ny, vSide) };

  const primary = wx >= wy ? hHit : vHit;
  const secondary = wx >= wy ? vHit : hHit;
  return secondary.weight >= COUPLING.secondaryMinWeight ? [primary, secondary] : [primary];
}

/** 单次界面透射幅度（纯函数）。 */
export function coupledAmp(
  baseAmp: number,
  distance: number,
  weight: number,
  hop: 1 | 2 = 1,
): number {
  if (!(baseAmp > 0) || !(weight > 0)) return 0;
  const d = distance > 0 ? distance : 0;
  const hopFactor = hop === 2 ? COUPLING.hopAtten : 1.0;
  const raw = baseAmp * Math.exp(-d / COUPLING.lambda) * COUPLING.kappa * weight * hopFactor;
  return clamp(raw, 0, COUPLING.maxAmp);
}

/** 可达最远归一化距离：reach(a) = λ · ln(a·κ / minAmp)。 */
export function reach(baseAmp: number): number {
  if (!(baseAmp > 0)) return 0;
  const ratio = (baseAmp * COUPLING.kappa) / COUPLING.minAmp;
  if (!(ratio > 0)) return 0;
  return COUPLING.lambda * Math.log(ratio);
}

/**
 * 点击瞬间的 Ricker（墨西哥帽）双环冲击波采样：
 * 中心主脉冲 + 四周反向毛细波谷。双击（谐振）幅度 1.15x 限幅。
 */
export function buildRickerClickSamples(
  nx: number,
  ny: number,
  baseAmp: number,
  isDoubleClick = false,
): WaveImpulseSample[] {
  const peak = clamp(baseAmp * (isDoubleClick ? 1.15 : 1.0), 0, 1.15);
  const ringAmp = -peak * 0.28;
  const r = isDoubleClick ? 0.044 : 0.036;
  const cx = clamp01(nx);
  const cy = clamp01(ny);
  return [
    { nx: cx, ny: cy, amp: peak },
    { nx: clamp01(cx - r), ny: cy, amp: ringAmp },
    { nx: clamp01(cx + r), ny: cy, amp: ringAmp },
    { nx: cx, ny: clamp01(cy - r), amp: ringAmp },
    { nx: cx, ny: clamp01(cy + r), amp: ringAmp },
  ];
}

/**
 * 开尔文（Kelvin）V 型尾迹伴生采样：
 * 速度越高，尾迹半角从 30° 收窄至 19.47°（开尔文物理极限）；
 * 超高速滑动（baseAmp >= 0.85）在尾流中轴伴生一道负向 Stern Trough 波谷。
 */
export function buildKelvinWakeSamples(
  nx: number,
  ny: number,
  unx: number,
  uny: number,
  baseAmp: number,
): WaveImpulseSample[] {
  const mag = Math.hypot(unx, uny);
  if (!(mag >= 1e-6) || baseAmp < 0.24) return [];

  const dx = unx / mag;
  const dy = uny / mag;
  const speedRatio = clamp((baseAmp - 0.24) / 0.66, 0, 1);
  const wakeDeg = 30.0 - speedRatio * (30.0 - 19.47);
  const angle = (wakeDeg * Math.PI) / 180;
  const cosA = Math.cos(angle);
  const sinA = Math.sin(angle);
  const trailDist = 0.028;

  const bx = -dx;
  const by = -dy;
  const w1x = bx * cosA - by * sinA;
  const w1y = bx * sinA + by * cosA;
  const w2x = bx * cosA + by * sinA;
  const w2y = -bx * sinA + by * cosA;

  const lobeAmp = baseAmp * 0.34;
  const cx = clamp01(nx);
  const cy = clamp01(ny);
  const samples: WaveImpulseSample[] = [
    { nx: clamp01(cx + w1x * trailDist), ny: clamp01(cy + w1y * trailDist), amp: lobeAmp },
    { nx: clamp01(cx + w2x * trailDist), ny: clamp01(cy + w2y * trailDist), amp: lobeAmp },
  ];

  if (baseAmp >= 0.85) {
    samples.push({
      nx: clamp01(cx + bx * 0.038),
      ny: clamp01(cy + by * 0.038),
      amp: -baseAmp * 0.2,
    });
  }

  return samples;
}

/** 把 source 局部坐标投影到该 source 在父容器内的占位框。 */
function projectIntoSlot(pt: Point2, slot: NormRect): Point2 {
  return {
    x: clamp01(slot.x0 + clamp01(pt.x) * (slot.x1 - slot.x0)),
    y: clamp01(slot.y0 + clamp01(pt.y) * (slot.y1 - slot.y0)),
  };
}

/**
 * 从 section 内一点向 slot 中心射一条射线，求其与 slot 边界的交点（slot 局部 [0,1]）。
 * 用于让第 2 跳的波「从最近的那一侧撞进来」，而不是总从中心起波。
 */
function raySlotEntryLocal(px: number, py: number, slot: NormRect): Point2 {
  const cx = (slot.x0 + slot.x1) * 0.5;
  const cy = (slot.y0 + slot.y1) * 0.5;
  const dx = cx - px;
  const dy = cy - py;
  const halfW = Math.max(1e-4, (slot.x1 - slot.x0) * 0.5);
  const halfH = Math.max(1e-4, (slot.y1 - slot.y0) * 0.5);

  if (Math.abs(dx) < 1e-6 && Math.abs(dy) < 1e-6) return { x: 0.5, y: 0.5 };

  const tx = Math.abs(dx) > 1e-6 ? halfW / Math.abs(dx) : Infinity;
  const ty = Math.abs(dy) > 1e-6 ? halfH / Math.abs(dy) : Infinity;
  const t = Math.min(tx, ty);
  const entrySx = cx - dx * t;
  const entrySy = cy - dy * t;

  return {
    x: clamp01((entrySx - slot.x0) / (slot.x1 - slot.x0)),
    y: clamp01((entrySy - slot.y0) / (slot.y1 - slot.y0)),
  };
}

/**
 * 解析耦合主入口（纯函数，无 DOM 副作用）：
 *   第 1 跳：card -> parent section（或嵌套 section -> parent section）
 *   第 2 跳：section -> 同组兄弟 card（排除 pointerCardId 防回声、排除自身防自激）
 */
export function planCoupling(
  input: CouplingInput,
  nodes: Map<string, CouplingNode>,
): Injection[] {
  const src = nodes.get(input.sourceId);
  if (!src || !(input.baseAmp > 0)) return [];

  const results: Injection[] = [];
  const mag = Math.hypot(input.unx, input.uny);
  const hasDir = mag >= 1e-6;
  const dirX = hasDir ? input.unx / mag : 0;
  const dirY = hasDir ? input.uny / mag : 0;

  // ---- 第 1 跳：向父级容器投递 ----
  if (src.parentId && src.parentId !== src.id) {
    const targetId = src.parentId;
    const lastTs = input.cooldown.get(`${src.id}>${targetId}`);
    const inCooldown = lastTs !== undefined && input.now - lastTs < COUPLING.cooldownMs;

    if (!inCooldown) {
      let bestAmp = 0;
      let bestPt: Point2 | null = null;

      for (const edge of pickEdges(input.nx, input.ny, input.unx, input.uny)) {
        const amp = coupledAmp(input.baseAmp, edge.distance, edge.weight, 1);
        if (amp >= COUPLING.minAmp && amp > bestAmp) {
          const along = edge.side === 'left' || edge.side === 'right' ? input.ny : input.nx;
          const edgeLocal = pointOnEdge(edge.side, along);
          bestPt = src.slot ? projectIntoSlot(edgeLocal, src.slot) : edgeLocal;
          bestAmp = amp;
        }
      }

      if (bestPt && bestAmp >= COUPLING.minAmp) {
        results.push({ targetId, nx: bestPt.x, ny: bestPt.y, amp: bestAmp, hop: 1 });
      }
    }
  }

  // ---- 第 2 跳：section 向同组兄弟 card 投递 ----
  if (src.layer === 'section' && src.childIds.length > 0) {
    const px = clamp01(input.nx);
    const py = clamp01(input.ny);
    const candidates: Injection[] = [];

    for (const childId of src.childIds) {
      if (childId === src.id || childId === input.pointerCardId) continue;
      const child = nodes.get(childId);
      if (!child || !child.slot) continue;

      const lastTs = input.cooldown.get(`${src.id}>${childId}`);
      if (lastTs !== undefined && input.now - lastTs < COUPLING.cooldownMs) continue;

      const slot = child.slot;
      const dx = (slot.x0 + slot.x1) * 0.5 - px;
      const dy = (slot.y0 + slot.y1) * 0.5 - py;
      const distCenter = Math.hypot(dx, dy);
      if (distCenter < 1e-6) continue;

      if (hasDir && (dx / distCenter) * dirX + (dy / distCenter) * dirY < COUPLING.directionDotMin) {
        continue;
      }

      const halfW = (slot.x1 - slot.x0) * 0.5;
      const halfH = (slot.y1 - slot.y0) * 0.5;
      const gapX = Math.max(0, Math.abs(dx) - halfW);
      const gapY = Math.max(0, Math.abs(dy) - halfH);

      let dist: number;
      let weight: number;
      if (hasDir) {
        const ax = Math.abs(dirX);
        const ay = Math.abs(dirY);
        const denom = ax + ay || 1;
        if (ax >= ay) {
          dist = gapX;
          weight = ax / denom;
        } else {
          dist = gapY;
          weight = ay / denom;
        }
      } else {
        dist = Math.hypot(gapX, gapY);
        weight = 1.0;
      }

      const amp = coupledAmp(input.baseAmp, dist, weight, 2);
      if (amp < COUPLING.minAmp) continue;

      const entry = raySlotEntryLocal(px, py, slot);
      candidates.push({ targetId: childId, nx: entry.x, ny: entry.y, amp, hop: 2 });
    }

    candidates.sort((a, b) => b.amp - a.amp);
    results.push(...candidates.slice(0, COUPLING.maxSecondaryTargets));
  }

  return results;
}

/**
 * 物理触边传导：波前真的撞到容器内边时，把动量投给邻接玻璃。
 *
 * 与 planCoupling 的分工：planCoupling 是「指针动一下就投递」的事件驱动通道
 * （保证手感即时），本函数是「波自己走到边」的物理通道（保证观感是真的
 * 传导而不是无所不在的联动）。两条通道都在，才有「碰撞」这件事本身：
 * 用户看到的应该是卡片先起波、波走到框边、底板在对应位置才被点亮。
 *
 * 只处理 card -> 父 section 一跳。反向（section -> 卡）不做：
 * section 的波面覆盖整块底板，撞边时沿边的位置与哪张卡片对应关系不稳定，
 * 强行投射会退化成「底板一动，所有卡片都亮」——那正是要消除的无源联动。
 */
export function planBorderCollisionCoupling(
  sourceId: string,
  collisions: BorderCollision[],
  nodes: Map<string, CouplingNode>,
  cooldown: Map<string, number>,
  now: number,
): Injection[] {
  const src = nodes.get(sourceId);
  if (!src || src.layer !== 'card' || !src.parentId) return [];
  const parent = nodes.get(src.parentId);
  if (!parent || parent.id === src.id) return [];
  if (collisions.length === 0) return [];

  const targetId = parent.id;
  // 触边传导比事件驱动通道慢一拍（波要走过去），冷却窗口给足，避免同一波前
  // 连续数帧反复撞边导致目标持续被灌能量。
  const cdMs = COUPLING.cooldownMs * 2;
  const last = cooldown.get(`border:${src.id}>${targetId}`) ?? Number.NEGATIVE_INFINITY;
  if (now - last < cdMs) return [];

  // 同一次步进可能同时记录两条边（斜向波撞到角），只取最强的一条：
  // 角上同时投两点会让目标在角部叠出一个人为热点。
  let best: BorderCollision | null = null;
  for (const col of collisions) {
    if (!best || col.strength > best.strength) best = col;
  }
  if (!best) return [];

  const amp = Math.min(COUPLING.maxAmp, best.strength * COUPLING.kappa);
  if (amp < COUPLING.minAmp) return [];

  const edgeLocal = pointOnEdge(best.side, best.along);
  const pt = src.slot ? projectIntoSlot(edgeLocal, src.slot) : edgeLocal;

  return [
    {
      targetId,
      nx: pt.x,
      ny: pt.y,
      amp,
      hop: 1,
      edgeSide: best.side,
      edgeAlong: best.along,
    },
  ];
}