// src/ui/web/src/utils/l2Diagnostics.ts

export const RB2F_PREFETCH = 0x1;
export const RB2F_HOT      = 0x2;
export const RB2F_L2       = 0x4;
export const RB2F_WBCOAL   = 0x8;  // 红线 R4 禁开，且与 L2 互斥 (R5)
export const RB2F_MEMGUARD = 0x10; // Rb2ReloadFlags (rbfeat.c:468) 置位即剥离
export const RB2F_ETW      = 0x20;
export const RB2F_THERMAL  = 0x40;

export interface L2TelemetrySample {
  dCoreMiss: number;            // dCoreMiss
  dL2Hits: number;              // dL2Hits
  dL2Miss: number;              // dL2Miss
  l2State: number;              // s_State (rbL2.c:844, 0=未懒开, 1=已打开)
  l2Halt: number;               // s_Halt (rbL2.c:543/882)
  featureFlags?: number;        // Features\Flags
  attachedVolumeCount?: number; // 当前 IsAttached=TRUE 的加速卷数量（单卷门 <= 1）
  blockSize?: number;           // 当前块大小（块几何门要求 BlockSize * 16 === 65536，即 4096）
  selfDisabled?: boolean;       // s_selfDisable 引导循环防护自禁用标志
}

export type L2DisplayState =
  | 'SELF_DISABLED'           // s_selfDisable 置位 (引导循环保护已介入)
  | 'GATE_BLOCKED_FLAGS'      // 未开启 RB2F_L2 (0x4) 或触发 RB2F_WBCOAL (0x8) 互斥
  | 'GATE_BLOCKED_MULTI_VOL'  // 违反单卷门 (attachedVolumeCount > 1)
  | 'GATE_BLOCKED_BLOCKSIZE'  // 违反块几何门 (blockSize * 16 !== 65536)
  | 'HALT_GUARDED'            // l2Halt 非零 (处于停机门保护态)
  | 'LAZY_UNOPENED'           // 三道门全通但 l2State 为 0 (等待首次 Core Read Miss 触发 RbL2LazyOpenInt)
  | 'INCONCLUSIVE_CORE_HIT'   // dCoreMiss 为 0 (被 rbio.c:534 Core 全命中内联吃掉)
  | 'PASS_SERVING'            // dCoreMiss >= 1 && dL2Hits > 0 && dL2Miss 为 0 (J6 三条真判据)
  | 'PARTIAL_SERVING'         // dCoreMiss >= 1 && dL2Hits > 0 && dL2Miss > 0
  | 'COLD_FILLING';           // dCoreMiss >= 1 && dL2Hits 为 0 && dL2Miss > 0

/**
 * 修 BUG-NEW-06：完整覆盖 234 §4.2 L2 三道开启门 + s_selfDisable + HANDOVER 三条真判据：
 * dCoreMiss >= 1 && dL2Hits > 0 && dL2Miss == 0 (一字节不放宽)
 */
export function classifyL2Window(sample: L2TelemetrySample): {
  state: L2DisplayState;
  badgeText: string;
  detailText: string;
} {
  if (sample.selfDisabled) {
    return {
      state: 'SELF_DISABLED',
      badgeText: '引导保护自禁用 (s_selfDisable=1)',
      detailText: 'BootLoopGuard 已介入：驱动不挂栈以确保系统安全进桌面，L2 处于旁路状态',
    };
  }
  if (sample.l2Halt !== 0) {
    return {
      state: 'HALT_GUARDED',
      badgeText: 'HALT 停机门激活',
      detailText: 's_Halt=1（延迟闩 / 卷身份变更或排空保护中，RbL2IdxSaveGuarded 拒存保护已生效）',
    };
  }
  if (
    sample.featureFlags !== undefined &&
    ((sample.featureFlags & RB2F_L2) === 0 || (sample.featureFlags & RB2F_WBCOAL) !== 0)
  ) {
    return {
      state: 'GATE_BLOCKED_FLAGS',
      badgeText: 'L2 位门未开启或互斥拦截',
      detailText: '需置位 RB2F_L2 (0x4) 且保持 RB2F_WBCOAL (0x8) 关闭（红线 R4/R5 互斥）',
    };
  }
  if (sample.attachedVolumeCount !== undefined && sample.attachedVolumeCount > 1) {
    return {
      state: 'GATE_BLOCKED_MULTI_VOL',
      badgeText: '单卷门拦截 (加速卷大于 1)',
      detailText: `当前已挂载 ${sample.attachedVolumeCount} 个加速卷，L2 单卷门要求被加速卷不超过 1 个`,
    };
  }
  if (sample.blockSize !== undefined && sample.blockSize * 16 !== 65536) {
    return {
      state: 'GATE_BLOCKED_BLOCKSIZE',
      badgeText: '块几何门拦截 (需 4KB 块)',
      detailText: `当前 BlockSize=${sample.blockSize}B，不满足 BlockSize*16=64KB (4096B) 哈希槽几何契约`,
    };
  }
  if (sample.l2State === 0) {
    return {
      state: 'LAZY_UNOPENED',
      badgeText: '懒加载待命 (L2state=0)',
      detailText: '三道开启门已满足；等待首次穿过 rbio.c:534 的 Core Read Miss 触发 RbL2LazyOpenInt 置 s_State=1',
    };
  }
  if (sample.dCoreMiss < 1) {
    return {
      state: 'INCONCLUSIVE_CORE_HIT',
      badgeText: 'INCONCLUSIVE (Core 全命中内联拦截)',
      detailText: 'dCoreMiss=0：读请求已被 rbio.c:534 一级缓存全部内联满足，未下探至 L2',
    };
  }
  if (sample.dL2Hits > 0 && sample.dL2Miss === 0) {
    return {
      state: 'PASS_SERVING',
      badgeText: 'L2 全命中回填中',
      detailText: `满足 dCoreMiss(${sample.dCoreMiss})>=1, dL2Hits(${sample.dL2Hits})>0, dL2Miss(0)=0`,
    };
  }
  if (sample.dL2Hits > 0) {
    return {
      state: 'PARTIAL_SERVING',
      badgeText: 'L2 混合命中服务中',
      detailText: `dCoreMiss=${sample.dCoreMiss}, dL2Hits=${sample.dL2Hits}, dL2Miss=${sample.dL2Miss}`,
    };
  }
  return {
    state: 'COLD_FILLING',
    badgeText: 'L2 哈希槽冷态填充中',
    detailText: `dCoreMiss=${sample.dCoreMiss}, dL2Miss=${sample.dL2Miss}（正在按 64KB 切片写入槽位）`,
  };
}

/**
 * 针对 src/driver/rbctl.c:268-272:
 * RbLog(0, __LINE__, (LONG)msg->Data.Config.SystemVolumeCached, L"system volume opt-in ON/OFF ...")
 * 修 C-01 / F-07 / BUG-NEW-04：同时兼容现行驱动（Status=0/1）与 Phase 4 修复后驱动（Status=0，由文本区分 ON/OFF）
 */
export function formatRbLogStatus(entry: {
  level: number;
  srcLine: number;
  status: number;
  text: string;
}): { label: string; tone: 'emerald' | 'cyan' | 'amber' | 'rose' } {
  if (entry.text.toLowerCase().includes('system volume opt-in')) {
    const isOptInOn =
      entry.status === 1 || /\bopt-in\s+on\b/i.test(entry.text);
    return isOptInOn
      ? { label: 'OPT-IN ON (1)', tone: 'cyan' }
      : { label: 'OPT-IN OFF (0)', tone: 'emerald' };
  }
  if (entry.status === 0) {
    return { label: 'STATUS_SUCCESS (0x0)', tone: 'emerald' };
  }
  const hex = `0x${(entry.status >>> 0).toString(16).toUpperCase().padStart(8, '0')}`;
  return {
    label: hex,
    tone: entry.status < 0 ? 'rose' : 'amber',
  };
}
