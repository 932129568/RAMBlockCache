// src/ui/web/src/hooks/useLiquidWave.ts
//
// U3 高性能二维浅水波 + 跨玻璃耦合。物理与着色在 @lib/liquidWaveCore，
// 耦合几何在 @lib/liquidWaveGraph，本文件只负责生命周期：
// 挂 canvas、转发指针、单条 rAF 循环、渲染预算、空闲休眠、清理。
//
// [v23 2026-11-19] 针对用户三条实机抱怨的两处实质修复：
//
//   A) 「横条上的波浪被拉扁」—— 旧实现每张画布都是固定 160×90 位图，
//      靠 CSS width/height 100% 铺满宿主。宿主是 1400×120 的横条时，
//      各向异性拉伸比 6.56:1，圆形涟漪被横向抹成条状糊斑。
//      现在每张画布按宿主真实宽高比分配网格（computeAdaptiveGrid），
//      并由 ResizeObserver 实时跟着窗口/布局变化重建，拉伸比实测 < 1.05。
//
//   B) 「鼠标跨到另一块玻璃后，前一块的波浪卡住 1.2s 等它自己消失」——
//      旧实现渲染预算 maxRendered=4，而单页 section+card 可达数十块；
//      超出预算的波场每帧只 stepWave、永不 putImageData，画面就冻在
//      注入那一帧，直到 IDLE_SLEEP_MS(1200ms) + 250ms 淡出才被清掉。
//      看上去就是「卡住不动，然后波突然消失」。
//      现在预算提到 16，且超预算改用「隔帧交错上传」（每张画布最多连续
//      跳过 1 帧），物理连续、画面连续，冻结窗口归零。
//      交错而不是无限扩大预算：真正的成本是 CPU→GPU 纹理上传，
//      16 张/帧已经是 1080p 上可接受的上限，再大就是拿帧率换波数。
//
// 相对 U2 的既有改动（保留）：
//   1) F24：section 波 z-index 0（卡片之下）、card 波 z-index 3（最顶装饰层）。
//   2) 视口可见性剔除：IntersectionObserver 存在时离屏波场不步进不上传。
//   3) 100ms 惰性包围盒缓存 + 冒泡事件共享元数据。
//   4) CSS 变量量化脏写（glow 变化 <0.005、rim 角度 <0.8° 不写）。
//
// [U3 与文档的一处偏离，已记录在 288 回执]
// 文档的 `__RBC_WAVE_PERF__.awakeCount` 取「本帧开始时的清醒数」，
// 于是"本帧全部入睡"也会报 awakeCount=2，与「静止后 awakeCount === 0」自相矛盾。
// 这里取「本帧处理完之后仍在跑的波场数」，语义与判据一致。

import { useEffect, type RefObject } from 'react';
import {
  computeAdaptiveGrid,
  createWaveField,
  injectWaveImpulse,
  renderWave,
  stepWave,
  wavePeak,
  type BorderCollision,
  type WaveField,
} from '@lib/liquidWaveCore';
import {
  buildKelvinWakeSamples,
  buildRickerClickSamples,
  buildWaveGraph,
  COUPLING,
  planBorderCollisionCoupling,
  planCoupling,
  refreshSlot,
  type SurfaceLayer,
  type WaveGraph,
} from '@lib/liquidWaveGraph';

export const DEFAULT_SELECTOR = '.rbc-section, .rbc-card';

/**
 * [299] 原生 D3D11+DComp 宿主是否已开闸。
 *
 * 唯一的判定依据是 useNativeGlassSync 在收到 rbc-dcomp-caps 后给 <html> 加的
 * 那个类 —— 不另设全局标志位，避免出现「两份状态不同步」。
 *
 * 为什么要退让：原生宿主在显存里做 GPU Ping-Pong 水波并把它叠加到玻璃法线上，
 * 产生的是真正的折射位移；本 hook 的 CPU 版只能把白色光斑 putImageData 盖在
 * 卡片上。两者同时开着 = 同一处出现两层波纹，一层在弯光、一层在贴白斑，
 * 观感是「波纹重影」，而且白烧一份 CPU + 每帧一次跨 PCIe 位图上传。
 */
const isNativeDcompHost = (): boolean =>
  typeof document !== 'undefined' &&
  document.documentElement.classList.contains('rbc-host-dcomp');

/**
 * 空闲休眠窗口。两个条件必须同时成立才强杀：空闲超时 **且** 画面已无可见像素。
 *
 * [v23] 旧实现只有 `now - lastActive > 1200` 一个条件，于是「画面还在、却被判定
 * 空闲」的硬切是必然发生的。现在时间窗退化为兜底（防异常卡死导致 rAF 永不停），
 * 主判据是**渲染结果**而不是峰值：
 *
 *   为什么不用峰值：实测指针移动注入后，落色像素在第 ~85 帧就归零，
 *   而峰值要到第 ~130 帧才降到千分之二。用峰值判「归静」会让每块玻璃
 *   在完全看不见之后还多跑 45 帧（0.75 s）—— 十几块同跑就是白烧 CPU，
 *   且这几帧里 rAF 不停，与「零空闲功耗」直接冲突。
 *   用 renderWave 的返回值判：>0 就继续画，==0 才允许入睡。
 *   这条判据的语义是「不画任何东西了」，比「能量小于某个数」更贴近观感。
 */
const IDLE_SLEEP_MS = 2200;
const RECT_CACHE_TTL_MS = 100;
const SLOW_FRAME_THRESHOLD_MS = 12;
const SLOW_FRAME_STREAK_LIMIT = 30;
/** 速度 -> 幅度的除数：1200 px/s 时达到满幅（实测手感值）。 */
const SPEED_DIVISOR = 1200;
/**
 * 默认渲染预算。16 张/帧是 1080p 下的可接受上限；
 * 超出部分走隔帧交错上传，画面不冻结（见文件头 B）。
 */
const DEFAULT_MAX_RENDERED = 16;

/**
 * 层权重：顶层厚玻璃（card）全幅，底层薄玻璃（section）0.6。
 * 用户需求「波的力度由速度决定」在这里落地 —— 同一指针速度下，
 * 底层只会得到顶层的 60%，从而视觉上有明确的厚薄主次。
 * [v23] 0.55 -> 0.6：底板自身不承载指针手势（它只在卡片空隙里被扫到），
 * 0.55 让它太容易低于耦合门槛而完全不传导，观感是「底板是死的」。
 */
export const LAYER_WEIGHT: Record<SurfaceLayer, number> = {
  card: 1.0,
  section: 0.6,
};

export interface WavePerfTelemetry {
  awakeCount: number;
  visibleCount: number;
  lastTickMs: number;
  skippedUploads: number;
  hop2Disabled: boolean;
  /**
   * [v23] 单张画布「最长连续未上传帧数」的全局最大值。
   * 这是「画面冻结」的直接量化指标：旧实现里超预算画布永不恢复，
   * 该值随冻结时长线性增长；交错上传后上界恒为 1。
   * 判据写在 liquidWaveVisualContract.test.ts（V17b）。
   */
  maxSkippedFrames: number;
  /** [v23] 波前触边碰撞累计触发次数（物理传导通道是否真的活着）。 */
  borderCollisions: number;
}

declare global {
  interface Window {
    __RBC_WAVE_PERF__?: WavePerfTelemetry;
  }
}

export interface UseLiquidWaveOptions {
  /** 只在这些节点内部扫描（避免装饰层也被挂上画布）。 */
  root?: RefObject<HTMLElement | null>;
  selector?: string;
  /** 是否启用跨玻璃耦合（默认 true）。 */
  coupling?: boolean;
  /** 每帧最多「无条件」上传的画布数；超出部分隔帧交错上传（默认 16）。 */
  maxRendered?: number;
}

interface CachedBox {
  left: number;
  top: number;
  width: number;
  height: number;
  ts: number;
}

interface SharedPointerMeta {
  now: number;
  pointerCardId: string | null;
}

interface Surface {
  id: string;
  slotIndex: number;
  layer: SurfaceLayer;
  host: HTMLElement;
  canvas: HTMLCanvasElement;
  ctx: CanvasRenderingContext2D;
  field: WaveField;
  pixels: Uint32Array;
  image: ImageData;
  lastActive: number;
  lastImpulseTs: number;
  sleeping: boolean;
  isVisible: boolean;
  frameParity: number;
  lastX: number | null;
  lastY: number | null;
  lastT: number;
  lastSpeedPx: number;
  lastUnx: number;
  lastUny: number;
  impactGlow: number;
  lastAppliedGlow: number;
  lastAppliedRimAngle: number;
  cachedBox: CachedBox | null;
  restoredPosition: boolean;
  /** 上一次真正 putImageData 的帧号；0 表示还没上传过。 */
  lastUploadFrame: number;
  /**
   * [v23] 距上一次上传过了多少帧。每帧自增、上传即清零。
   * 为什么不用 `frameCounter - lastUploadFrame`：从未上传时 lastUploadFrame=0，
   * 差值没有意义，于是「永不上传」这个最坏情况反而统计不到（变异测试 M2 抓到的漏洞）。
   * 用一个自增计数器，任何路径都绕不过它。
   */
  framesSinceUpload: number;
  /** 本张画布历史最长连续跳过上传帧数。 */
  maxSkippedFrames: number;
  detach: () => void;
}

function clamp(v: number, lo: number, hi: number): number {
  if (!(v > lo)) return lo;
  if (v > hi) return hi;
  return v;
}

/** 速度 -> 本地注入幅度：下限 0.15 保证慢速也有微光，满幅 1.0。 */
function computeMoveBaseAmp(speedPxPerSec: number, layer: SurfaceLayer): number {
  return clamp(speedPxPerSec / SPEED_DIVISOR, 0.15, 1.0) * 0.9 * LAYER_WEIGHT[layer];
}

export function useLiquidWave(options: UseLiquidWaveOptions = {}): void {
  const {
    root,
    selector = DEFAULT_SELECTOR,
    coupling = true,
    maxRendered = DEFAULT_MAX_RENDERED,
  } = options;

  useEffect(() => {
    if (typeof window === 'undefined' || typeof document === 'undefined') return;

    // 尊重系统减弱动态效果：零 canvas、零 rAF、零注入。
    const mq = window.matchMedia?.('(prefers-reduced-motion: reduce)');
    if (mq?.matches) return;

    const container: ParentNode = root?.current ?? document;
    const surfacesByHost = new Map<HTMLElement, Surface>();
    const surfacesById = new Map<string, Surface>();
    const cooldown = new Map<string, number>();
    const eventMetaCache = new WeakMap<PointerEvent, SharedPointerMeta>();

    let graph: WaveGraph = { nodes: new Map(), byHost: new Map() };
    let rafId: number | null = null;
    let frameCounter = 0;
    let slowFrameStreak = 0;
    let hop2Disabled = false;
    let skippedUploads = 0;
    let borderCollisions = 0;
    let globalMaxSkipped = 0;

    const io =
      typeof IntersectionObserver !== 'undefined'
        ? new IntersectionObserver(
            (entries) => {
              for (const entry of entries) {
                const surf = surfacesByHost.get(entry.target as HTMLElement);
                if (surf) surf.isVisible = entry.isIntersecting;
              }
            },
            { threshold: 0.01 },
          )
        : null;

    const getSurfaceBox = (surf: Surface, now: number): CachedBox => {
      const cached = surf.cachedBox;
      if (cached && cached.width > 0 && cached.height > 0 && now - cached.ts < RECT_CACHE_TTL_MS) {
        return cached;
      }
      const r = surf.host.getBoundingClientRect();
      const box: CachedBox = {
        left: r.left,
        top: r.top,
        width: r.width,
        height: r.height,
        ts: now,
      };
      surf.cachedBox = box;
      return box;
    };

    /** 同一次冒泡里 card 与父 section 复用同一 now / pointerCardId（避免重复查询 DOM）。 */
    const getSharedEventMeta = (ev: PointerEvent): SharedPointerMeta => {
      const hit = eventMetaCache.get(ev);
      if (hit) return hit;
      const targetEl = ev.target instanceof Element ? ev.target : null;
      const cardHost = targetEl?.closest<HTMLElement>('.rbc-card') ?? null;
      const meta: SharedPointerMeta = {
        now: performance.now(),
        pointerCardId: cardHost ? (surfacesByHost.get(cardHost)?.id ?? null) : null,
      };
      eventMetaCache.set(ev, meta);
      return meta;
    };

    const syncGlowVar = (surf: Surface, force = false): void => {
      if (force || Math.abs(surf.impactGlow - surf.lastAppliedGlow) >= 0.005) {
        surf.lastAppliedGlow = surf.impactGlow;
        surf.host.style.setProperty('--rbc-impact-glow', surf.impactGlow.toFixed(3));
      }
    };

    const triggerImpactGlow = (surf: Surface, nx: number, ny: number, strength: number): void => {
      surf.impactGlow = clamp(Math.max(surf.impactGlow, strength), 0, 1);
      surf.host.style.setProperty('--rbc-impact-x', `${(nx * 100).toFixed(1)}%`);
      surf.host.style.setProperty('--rbc-impact-y', `${(ny * 100).toFixed(1)}%`);
      syncGlowVar(surf, true);
    };

    const updateRimAngle = (surf: Surface, nx: number, ny: number): void => {
      const angleDeg = (Math.atan2(ny - 0.5, nx - 0.5) * 180) / Math.PI + 90;
      if (Math.abs(angleDeg - surf.lastAppliedRimAngle) >= 0.8) {
        surf.lastAppliedRimAngle = angleDeg;
        surf.host.style.setProperty('--rbc-rim-angle', `${angleDeg.toFixed(1)}deg`);
      }
    };

    const wake = (surf: Surface, now: number): void => {
      surf.lastActive = now;
      surf.lastImpulseTs = now;
      if (surf.sleeping) {
        surf.sleeping = false;
        surf.canvas.style.willChange = 'opacity, contents';
        surf.canvas.style.opacity = '1';
      }
    };

    const sleepSurface = (surf: Surface): void => {
      surf.sleeping = true;
      surf.canvas.style.opacity = '0';
      surf.canvas.style.willChange = 'auto';
      surf.field.cur.fill(0);
      surf.field.prev.fill(0);
      surf.pixels.fill(0);
      surf.lastUploadFrame = 0;
      surf.framesSinceUpload = 0;
      surf.maxSkippedFrames = 0;
      if (surf.impactGlow > 0 || surf.lastAppliedGlow > 0) {
        surf.impactGlow = 0;
        syncGlowVar(surf, true);
      }
    };

    /**
     * 按宿主当前真实宽高重建网格。
     * 尺寸没变就直接返回（ResizeObserver 会在滚动/布局抖动时频繁触发）。
     */
    const resizeSurfaceGrid = (surf: Surface): void => {
      const r = surf.host.getBoundingClientRect();
      const { cols, rows } = computeAdaptiveGrid(r.width, r.height);
      if (cols === surf.field.cols && rows === surf.field.rows) return;

      const field = createWaveField(cols, rows);
      // 走 ctx.createImageData 而不是 new ImageData(...)：与挂载路径同一来源，
      // 也让 jsdom 单测里被 mock 的 getContext 能覆盖到重建分支（重建是
      // 「窗口拉伸后网格跟着变」的唯一入口，它必须可测）。
      const image = surf.ctx.createImageData(cols, rows);
      const pixels = new Uint32Array(image.data.buffer);
      surf.canvas.width = cols;
      surf.canvas.height = rows;
      surf.field = field;
      surf.pixels = pixels;
      surf.image = image;
      // 网格换了，旧的脏写缓存没有意义。
      surf.cachedBox = null;
    };

    const ro =
      typeof ResizeObserver !== 'undefined'
        ? new ResizeObserver((entries) => {
            for (const entry of entries) {
              const surf = surfacesByHost.get(entry.target as HTMLElement);
              if (!surf) continue;
              resizeSurfaceGrid(surf);
              const node = graph.nodes.get(surf.id);
              if (node?.parentId) {
                const parent = graph.nodes.get(node.parentId);
                if (parent) refreshSlot(node, parent);
              }
            }
          })
        : null;

    const dispatchCoupling = (
      surf: Surface,
      nx: number,
      ny: number,
      unx: number,
      uny: number,
      baseAmp: number,
      pointerCardId: string | null,
      now: number,
    ): void => {
      if (!coupling) return;
      // surf.id 在建图前为空串：planCoupling 查不到节点直接返回 []，天然安全。
      const injections = planCoupling(
        { sourceId: surf.id, nx, ny, unx, uny, baseAmp, pointerCardId, cooldown, now },
        graph.nodes,
      );

      for (const inj of injections) {
        if (hop2Disabled && inj.hop === 2) continue;
        const target = surfacesById.get(inj.targetId);
        if (!target) continue;
        injectWaveImpulse(target.field, inj.nx, inj.ny, inj.amp);
        // 边界折射内侧伴生一道微弱毛细前驱波：撞击边框时「边缘先起涟漪、随后向内扩散」。
        injectWaveImpulse(
          target.field,
          inj.nx + (0.5 - inj.nx) * 0.06,
          inj.ny + (0.5 - inj.ny) * 0.06,
          inj.amp * 0.25,
        );
        triggerImpactGlow(target, inj.nx, inj.ny, clamp(inj.amp * 2.2, 0.25, 1.0));
        wake(target, now);
        cooldown.set(`${surf.id}>${target.id}`, now);
      }
    };

    const tick = (now: number): void => {
      if (document.hidden) {
        rafId = null;
        return;
      }

      // [299] 原生宿主开闸 => 全面退让：不步进、不上传、连 rAF 一起停。
      // 放在 document.hidden 之后、一切计算之前：省掉的不只是 putImageData，
      // 还有 stepWave 的物理演进（这才是 CPU 成本的大头）。
      // rafId 置 null 而不是继续排队 —— 由 pointermove 里的 ensureLoop 重新唤醒，
      // 而 pointermove 本身也已提前 return，所以不会出现「退了又醒」的抖动。
      if (isNativeDcompHost()) {
        rafId = null;
        for (const surf of Array.from(surfacesByHost.values())) {
          if (!surf.sleeping) sleepSurface(surf);
        }
        window.__RBC_WAVE_PERF__ = {
          awakeCount: 0,
          visibleCount: 0,
          lastTickMs: 0,
          skippedUploads,
          hop2Disabled,
          maxSkippedFrames: globalMaxSkipped,
          borderCollisions,
        };
        return;
      }

      const t0 = performance.now();
      frameCounter += 1;
      const awake: Surface[] = [];
      let visibleCount = 0;

      for (const surf of Array.from(surfacesByHost.values())) {
        if (!surf.host.isConnected) {
          surf.detach();
          continue;
        }
        if (surf.isVisible) visibleCount += 1;
        if (!surf.sleeping && surf.isVisible) awake.push(surf);
      }

      if (awake.length === 0) {
        rafId = null;
        window.__RBC_WAVE_PERF__ = {
          awakeCount: 0,
          visibleCount,
          lastTickMs: 0,
          skippedUploads,
          hop2Disabled,
          maxSkippedFrames: globalMaxSkipped,
          borderCollisions,
        };
        return;
      }

      // 最近活跃的优先拿到无条件预算。
      awake.sort((a, b) => b.lastActive - a.lastActive);
      const budget = Math.max(1, maxRendered);
      const collisions: BorderCollision[] = [];

      for (let i = 0; i < awake.length; i++) {
        const surf = awake[i];
        if (!surf) continue;
        surf.frameParity = (surf.frameParity + 1) & 1;

        collisions.length = 0;
        stepWave(surf.field, undefined, coupling ? collisions : undefined);

        // 物理通道：波前真的撞到内边 => 把动量投给父底板。
        // 与事件驱动通道并存（见 liquidWaveGraph.ts 文件头 A/B 分工说明）。
        if (coupling && collisions.length > 0) {
          borderCollisions += collisions.length;
          const borderInj = planBorderCollisionCoupling(
            surf.id,
            collisions,
            graph.nodes,
            cooldown,
            now,
          );
          for (const inj of borderInj) {
            const target = surfacesById.get(inj.targetId);
            if (!target) continue;
            injectWaveImpulse(target.field, inj.nx, inj.ny, inj.amp);
            triggerImpactGlow(target, inj.nx, inj.ny, clamp(inj.amp * 2.4, 0.2, 0.95));
            wake(target, now);
            cooldown.set(`border:${surf.id}>${target.id}`, now);
          }
        }

        if (surf.impactGlow > 0.005) {
          surf.impactGlow *= 0.88;
          syncGlowVar(surf, false);
        } else if (surf.impactGlow > 0) {
          surf.impactGlow = 0;
          syncGlowVar(surf, true);
        }

        const peak = wavePeak(surf.field);

        // 强杀兜底：只在「早就没输入」且「已超时」时生效。
        // 峰值门槛 0.02 只用于判断「画面此时已基本无可辨识内容」，
        // 真正的归静判定交给下面的 renderWave 返回值（见 IDLE_SLEEP_MS 注释）。
        if (peak < 0.0025 || (now - surf.lastActive > IDLE_SLEEP_MS && peak < 0.02)) {
          sleepSurface(surf);
          continue;
        }

        // ---- 上传决策 ----
        // 预算内：每帧上传（低能量尾迹隔帧降载，物理不受影响）。
        // 超预算：隔帧交错，保证每张画布最多连续跳过 1 帧 —— 画面连续，不冻结。
        const inBudget = i < budget;
        let upload: boolean;
        if (inBudget) {
          upload = !(peak < 0.032 && now - surf.lastImpulseTs > 220 && surf.frameParity === 1);
          if (!upload) skippedUploads += 1;
        } else {
          upload = (frameCounter + surf.slotIndex) % 2 === 0;
        }

        /* [v23] 冻结指标：每帧先自增，上传时清零。
         *
         * 「连续跳过上传的帧数」= framesSinceUpload - 1（本帧若上传则为 0）。
         * 为什么用自增计数器而不是 frameCounter 差值：从未上传时基准是 0，
         * 差值无意义，于是「永不上传」这个最坏情况（也就是真正要抓的 bug）
         * 反而统计不到 —— 变异测试 M2 让它全绿，就是这条漏洞。
         *
         * 语义：每帧上传 ⇒ 恒为 0；隔帧交错 ⇒ 最多 1；完全不上传 ⇒ 持续增长。 */
        surf.framesSinceUpload += 1;
        const skippedNow = surf.framesSinceUpload - 1;
        if (skippedNow > surf.maxSkippedFrames) {
          surf.maxSkippedFrames = skippedNow;
          if (skippedNow > globalMaxSkipped) globalMaxSkipped = skippedNow;
        }

        if (upload) {
          // [v23] 归静主判据：这一帧一笔都画不出来 ⇒ 立即入睡。
          // 用渲染结果而不是峰值：实测落色像素第 ~85 帧归零、峰值要到第 ~130 帧
          // 才够小；按峰值判会让每块玻璃在完全看不见后多烧 0.75 s 的 rAF。
          if (renderWave(surf.field, surf.pixels) === 0) {
            sleepSurface(surf);
            continue;
          }
          surf.ctx.putImageData(surf.image, 0, 0);
          surf.lastUploadFrame = frameCounter;
          surf.framesSinceUpload = 0;
        }
      }

      // awakeCount 取「本帧处理完之后仍在跑的波场数」——静止后必须归 0。
      let stillAwake = 0;
      for (const surf of awake) {
        if (!surf.sleeping) stillAwake += 1;
      }

      const elapsed = performance.now() - t0;
      if (elapsed > SLOW_FRAME_THRESHOLD_MS) {
        slowFrameStreak += 1;
        if (slowFrameStreak >= SLOW_FRAME_STREAK_LIMIT) hop2Disabled = true;
      } else {
        slowFrameStreak = 0;
      }

      window.__RBC_WAVE_PERF__ = {
        awakeCount: stillAwake,
        visibleCount,
        lastTickMs: Number(elapsed.toFixed(3)),
        skippedUploads,
        hop2Disabled,
        maxSkippedFrames: globalMaxSkipped,
        borderCollisions,
      };

      rafId = stillAwake > 0 ? requestAnimationFrame(tick) : null;
    };

    const ensureLoop = (): void => {
      if (rafId === null && !document.hidden) rafId = requestAnimationFrame(tick);
    };

    const attachSurface = (host: HTMLElement, slotIndex: number): void => {
      if (surfacesByHost.has(host)) return;
      // [299] 原生宿主下不挂 canvas 本身，而不是挂上去再隐藏 ——
      // 每块玻璃多一个绝对定位的 canvas 就是多一个合成层，哪怕 opacity:0
      // 也仍然占着显存中的图层，与「GPU 预算」这条设计原则正面冲突。
      if (isNativeDcompHost()) return;
      if (typeof host.getBoundingClientRect !== 'function') return;

      const layer: SurfaceLayer = host.classList.contains('rbc-card') ? 'card' : 'section';

      let restoredPosition = false;
      if (window.getComputedStyle(host).position === 'static') {
        host.style.position = 'relative';
        restoredPosition = true;
      }

      // [v23] 网格按宿主真实宽高分配 —— 这是「圆形涟漪不再被拉扁」的唯一落点。
      const rect0 = host.getBoundingClientRect();
      const grid = computeAdaptiveGrid(rect0.width, rect0.height);

      const canvas = document.createElement('canvas');
      canvas.className = 'rbc-card-wave';
      canvas.setAttribute('aria-hidden', 'true');
      canvas.width = grid.cols;
      canvas.height = grid.rows;
      Object.assign(canvas.style, {
        position: 'absolute',
        inset: '0',
        width: '100%',
        height: '100%',
        borderRadius: 'inherit',
        pointerEvents: 'none',
        mixBlendMode: 'screen',
        // F24 修复：section 波沉到卡片之下，card 波浮在卡片最顶装饰层。
        zIndex: layer === 'section' ? '0' : '3',
        opacity: '0',
        transition: 'opacity 0.25s ease',
      });

      const ctx = canvas.getContext('2d', { alpha: true });
      if (!ctx) return;

      host.appendChild(canvas);

      const image = ctx.createImageData(grid.cols, grid.rows);
      const pixels = new Uint32Array(image.data.buffer);

      const surf: Surface = {
        id: '',
        slotIndex,
        layer,
        host,
        canvas,
        ctx,
        field: createWaveField(grid.cols, grid.rows),
        pixels,
        image,
        lastActive: 0,
        lastImpulseTs: 0,
        sleeping: true,
        isVisible: true,
        frameParity: 0,
        lastX: null,
        lastY: null,
        lastT: 0,
        lastSpeedPx: 0,
        lastUnx: 0,
        lastUny: 0,
        impactGlow: 0,
        lastAppliedGlow: 0,
        lastAppliedRimAngle: Number.NaN,
        cachedBox: null,
        restoredPosition,
        lastUploadFrame: 0,
        framesSinceUpload: 0,
        maxSkippedFrames: 0,
        detach: () => {},
      };

      io?.observe(host);
      ro?.observe(host);

      const onPointerMove = (ev: PointerEvent): void => {
        // [299] 原生宿主下不进注波通道：注入进去也不会有人 stepWave（tick 已退让），
        // 只会让 field 里堆一批永不演进的脏高度值。
        if (isNativeDcompHost()) return;
        const meta = getSharedEventMeta(ev);
        const now = meta.now;
        const rect = getSurfaceBox(surf, now);
        if (rect.width <= 0 || rect.height <= 0) return;

        const nx = clamp((ev.clientX - rect.left) / rect.width, 0, 1);
        const ny = clamp((ev.clientY - rect.top) / rect.height, 0, 1);

        updateRimAngle(surf, nx, ny);

        let baseAmp = computeMoveBaseAmp(0, layer);
        let speedPx = 0;
        let unx = 0;
        let uny = 0;

        if (surf.lastX !== null && surf.lastY !== null) {
          const dtSec = Math.max(4, now - surf.lastT) / 1000;
          const dxPx = (nx - surf.lastX) * rect.width;
          const dyPx = (ny - surf.lastY) * rect.height;
          speedPx = Math.hypot(dxPx, dyPx) / dtSec;
          baseAmp = computeMoveBaseAmp(speedPx, layer);

          const vnx = (nx - surf.lastX) / dtSec;
          const vny = (ny - surf.lastY) / dtSec;
          const vNormMag = Math.hypot(vnx, vny);
          if (vNormMag >= COUPLING.vMin) {
            unx = vnx / vNormMag;
            uny = vny / vNormMag;
          }
        }

        // 1. 本地主波（置于首位，保证「层权重」基线可测）。
        injectWaveImpulse(surf.field, nx, ny, baseAmp);

        // 2. 轨迹连续插值 + 马赫角自适应 Kelvin V 型尾迹。
        if (surf.lastX !== null && surf.lastY !== null) {
          injectWaveImpulse(
            surf.field,
            surf.lastX + (nx - surf.lastX) * 0.33,
            surf.lastY + (ny - surf.lastY) * 0.33,
            baseAmp * 0.7,
          );
          injectWaveImpulse(
            surf.field,
            surf.lastX + (nx - surf.lastX) * 0.66,
            surf.lastY + (ny - surf.lastY) * 0.66,
            baseAmp * 0.85,
          );

          if (speedPx > 280) {
            for (const lobe of buildKelvinWakeSamples(nx, ny, unx, uny, baseAmp)) {
              injectWaveImpulse(surf.field, lobe.nx, lobe.ny, lobe.amp);
            }
          }
        }

        wake(surf, now);

        // 3. 解析耦合传导至相邻玻璃（父底板 / 兄弟卡片）。
        dispatchCoupling(surf, nx, ny, unx, uny, baseAmp, meta.pointerCardId, now);

        surf.lastX = nx;
        surf.lastY = ny;
        surf.lastT = now;
        surf.lastSpeedPx = speedPx;
        surf.lastUnx = unx;
        surf.lastUny = uny;
        ensureLoop();
      };

      const onPointerDown = (ev: PointerEvent): void => {
        if (isNativeDcompHost()) return;
        const meta = getSharedEventMeta(ev);
        const now = meta.now;
        const rect = getSurfaceBox(surf, now);
        if (rect.width <= 0 || rect.height <= 0) return;

        const nx = clamp((ev.clientX - rect.left) / rect.width, 0, 1);
        const ny = clamp((ev.clientY - rect.top) / rect.height, 0, 1);
        const isDouble = ev.detail === 2;
        const fullAmp = 1.0 * LAYER_WEIGHT[layer];

        for (const s of buildRickerClickSamples(nx, ny, fullAmp, isDouble)) {
          injectWaveImpulse(surf.field, s.nx, s.ny, s.amp);
        }
        triggerImpactGlow(surf, nx, ny, (isDouble ? 0.92 : 0.75) * LAYER_WEIGHT[layer]);
        wake(surf, now);

        dispatchCoupling(surf, nx, ny, 0, 0, fullAmp, meta.pointerCardId, now);
        ensureLoop();
      };

      // 跨界入场：在鼠标真实越界点自然起波。
      // [v23] 取消 snapToNearestBorder：把落点强行吸到 2% 边缘，画面读起来是
      // 「卡片边缘凭空爆开一个涟漪」，而不是「指针从哪儿进来、水就从哪儿凹下去」。
      // 弯月面湿润感改由「入场幅度略低于移动幅度」表达，位置必须诚实。
      const onPointerEnter = (ev: PointerEvent): void => {
        if (isNativeDcompHost()) return;
        if (layer !== 'card') return;
        const meta = getSharedEventMeta(ev);
        const now = meta.now;
        const rect = getSurfaceBox(surf, now);
        if (rect.width <= 0 || rect.height <= 0) return;

        const nx = clamp((ev.clientX - rect.left) / rect.width, 0, 1);
        const ny = clamp((ev.clientY - rect.top) / rect.height, 0, 1);

        const parentId = graph.nodes.get(surf.id)?.parentId ?? null;
        const inheritedSpeed = (parentId ? surfacesById.get(parentId)?.lastSpeedPx : 0) ?? 0;
        const entryAmp =
          clamp(0.22 + inheritedSpeed / 3000, 0.22, 0.52) * LAYER_WEIGHT[layer];

        injectWaveImpulse(surf.field, nx, ny, entryAmp);
        triggerImpactGlow(surf, nx, ny, clamp(entryAmp * 1.25, 0.35, 0.78));
        wake(surf, now);
        surf.lastX = nx;
        surf.lastY = ny;
        surf.lastT = now;
        ensureLoop();
      };

      // 跨界离场：沿离场速度矢量把动量甩给父容器与邻近卡片。
      const onPointerLeave = (ev: PointerEvent): void => {
        if (isNativeDcompHost()) return;
        const meta = getSharedEventMeta(ev);
        const now = meta.now;
        const rect = getSurfaceBox(surf, now);
        if (rect.width > 0 && rect.height > 0) {
          const nx = clamp((ev.clientX - rect.left) / rect.width, 0, 1);
          const ny = clamp((ev.clientY - rect.top) / rect.height, 0, 1);

          let vExitPx = surf.lastSpeedPx;
          let unx = surf.lastUnx;
          let uny = surf.lastUny;
          if (surf.lastX !== null && surf.lastY !== null) {
            const dtSec = Math.max(4, now - surf.lastT) / 1000;
            vExitPx =
              Math.hypot((nx - surf.lastX) * rect.width, (ny - surf.lastY) * rect.height) / dtSec;

            const vnx = (nx - surf.lastX) / dtSec;
            const vny = (ny - surf.lastY) / dtSec;
            const mag = Math.hypot(vnx, vny);
            if (mag >= COUPLING.vMin) {
              unx = vnx / mag;
              uny = vny / mag;
            }
          }

          const ampExit = clamp(0.6 + vExitPx / 2400, 0.6, 1.0) * LAYER_WEIGHT[layer];
          injectWaveImpulse(surf.field, nx, ny, ampExit);
          triggerImpactGlow(surf, nx, ny, clamp(ampExit * 0.85, 0.35, 0.9));
          wake(surf, now);

          dispatchCoupling(surf, nx, ny, unx, uny, ampExit, meta.pointerCardId, now);
          ensureLoop();
        }

        surf.lastX = null;
        surf.lastY = null;
      };

      host.addEventListener('pointermove', onPointerMove, { passive: true });
      host.addEventListener('pointerdown', onPointerDown, { passive: true });
      host.addEventListener('pointerenter', onPointerEnter, { passive: true });
      host.addEventListener('pointerleave', onPointerLeave, { passive: true });

      surf.detach = () => {
        io?.unobserve(host);
        ro?.unobserve(host);
        host.removeEventListener('pointermove', onPointerMove);
        host.removeEventListener('pointerdown', onPointerDown);
        host.removeEventListener('pointerenter', onPointerEnter);
        host.removeEventListener('pointerleave', onPointerLeave);
        canvas.remove();
        if (surf.restoredPosition) host.style.position = '';
        surfacesByHost.delete(host);
        if (surf.id) surfacesById.delete(surf.id);
      };

      surfacesByHost.set(host, surf);
    };

    const rebuildGraph = (): void => {
      graph = buildWaveGraph(Array.from(surfacesByHost.keys()));
      surfacesById.clear();
      for (const [host, id] of graph.byHost.entries()) {
        const surf = surfacesByHost.get(host);
        if (!surf) continue;
        surf.id = id;
        surf.cachedBox = null;
        surfacesById.set(id, surf);
      }
    };

    const scan = (): void => {
      for (const surf of Array.from(surfacesByHost.values())) {
        if (!surf.host.isConnected) surf.detach();
      }
      const hosts = Array.from(container.querySelectorAll<HTMLElement>(selector));
      hosts.forEach((host, idx) => attachSurface(host, idx));
      rebuildGraph();
    };

    scan();

    let mutQueued = false;
    const observer = new MutationObserver(() => {
      if (mutQueued) return;
      mutQueued = true;
      queueMicrotask(() => {
        mutQueued = false;
        scan();
      });
    });

    const observeTarget =
      container instanceof Document ? (container.body ?? container.documentElement) : container;
    if (observeTarget) observer.observe(observeTarget, { childList: true, subtree: true });

    const onResize = (): void => {
      for (const surf of surfacesByHost.values()) {
        surf.cachedBox = null;
        resizeSurfaceGrid(surf);
      }
      for (const node of graph.nodes.values()) {
        if (!node.parentId) continue;
        const parent = graph.nodes.get(node.parentId);
        if (parent) refreshSlot(node, parent);
      }
    };

    /**
     * [299] 原生宿主开闸时把已挂载的 CPU 波全部拆掉。
     *
     * 这个函数存在的理由：.rbc-host-dcomp 是**异步**加的（等宿主回 rbc-dcomp-caps）。
     * 如果只在 effect 入口判一次，那么「页面先起来、宿主稍后才握手」这条真实时序
     * 下，上面所有 isNativeDcompHost() 守卫都会读到 false，退让一次都不会发生 ——
     * 这恰好是「判据在消费点上失效」的同族缺陷：判断写对了，但判据被跳过了。
     * 所以额外监听 <html> 的 class 变化，命中就立刻清扫。
     */
    let dcompObserver: MutationObserver | null = null;
    /**
     * [299] 宿主能力位变化时的收尾/恢复。
     *
     * 两个方向都要管，缺一不可：
     *   开闸 → 拆掉所有 CPU 波（否则同一处两层波纹 + 白烧 CPU/PCIe）；
     *   关闸 → 重新 scan 把 CPU 波装回去。只拆不装的话，宿主降级
     *   （DComp 初始化失败退回 WPF 壳 / 用户关掉硬件加速）之后整个页面
     *   就永久没有指针水波了，而且不会有任何报错 —— 观感上就是
     *   「玻璃变哑了」。第一版只写了拆，被 R3 抓出来（退让不是单向自毁）。
     */
    const syncCapability = (): void => {
      if (isNativeDcompHost()) {
        if (rafId !== null) {
          cancelAnimationFrame(rafId);
          rafId = null;
        }
        for (const surf of Array.from(surfacesByHost.values())) surf.detach();
        surfacesById.clear();
        // 把遥测归零：否则上一次 tick 留下的 awakeCount > 0 会被读成
        // 「还在跑」，而实际上一条 rAF 都不在了 —— 又一处假读数。
        window.__RBC_WAVE_PERF__ = {
          awakeCount: 0,
          visibleCount: 0,
          lastTickMs: 0,
          skippedUploads,
          hop2Disabled,
          maxSkippedFrames: globalMaxSkipped,
          borderCollisions,
        };
      } else {
        // 关闸：重扫一次。attachSurface 自己的 isNativeDcompHost() 守卫此时
        // 读到 false，会把 canvas 正常挂回去；scan 内部还会重建 graph。
        scan();
      }
    };
    if (typeof MutationObserver !== 'undefined') {
      dcompObserver = new MutationObserver(syncCapability);
      dcompObserver.observe(document.documentElement, {
        attributes: true,
        attributeFilter: ['class'],
      });
      // 挂上观察器后立刻判一次：可能握手已经完成（effect 晚于 caps 到达）。
      // 只判一次不做 scan —— 此刻 scan() 已经在下面走过一遍了。
      if (isNativeDcompHost()) syncCapability();
    }

    const onVisibilityChange = (): void => {
      if (document.hidden && rafId !== null) {
        cancelAnimationFrame(rafId);
        rafId = null;
      }
    };

    window.addEventListener('resize', onResize, { passive: true });
    document.addEventListener('visibilitychange', onVisibilityChange);

    return () => {
      if (rafId !== null) cancelAnimationFrame(rafId);
      dcompObserver?.disconnect();
      io?.disconnect();
      ro?.disconnect();
      observer.disconnect();
      window.removeEventListener('resize', onResize);
      document.removeEventListener('visibilitychange', onVisibilityChange);
      for (const surf of Array.from(surfacesByHost.values())) surf.detach();
      surfacesById.clear();
      cooldown.clear();
    };
  }, [root, selector, coupling, maxRendered]);
}