/* AUTO-GENERATED — DO NOT EDIT.
 * 来源：src/ctl/rb_proto.h 的 typedef struct RB_CONFIG_T
 * 生成器：src/scripts/gen_proto_ts.py（python -B src/scripts/gen_proto_ts.py）
 * 漂移门禁：src/scripts/check_proto_codegen.py（磁盘文件与 C 头不一致即 FAIL）
 *
 * 手改本文件无效：下一次生成会被覆盖，且门禁当场报红。
 */

/** RB_CONFIG 字段的两种 C 形态：标量 u32 与 wchar_t 二维数组。 */
export type RbConfigField =
  | { readonly name: string; readonly offset: number; readonly width: 4; readonly kind: 'u32' }
  | { readonly name: string; readonly offset: number; readonly width: number; readonly kind: 'wchar2d'; readonly count: number; readonly len: number };

/**
 * RB_CONFIG —— 与 C 头逐字节一致（#pragma pack(8)）。
 * 字段名保留 C 原始 PascalCase 拼写，便于与 rb_proto.h 逐行对照。
 */
export interface RbConfig {
  /** @offset 0 · uint32_t · 总开关 */
  Enabled: number;
  /** @offset 4 · uint32_t */
  ReadCacheEnabled: number;
  /** @offset 8 · uint32_t */
  WriteCacheEnabled: number;
  /** @offset 12 · uint32_t · 1=回写 0=直写（可被每卷 WriteMode 覆盖） */
  WriteBack: number;
  /** @offset 16 · uint32_t · 0=动态自动 */
  CacheSizeMB: number;
  /** @offset 20 · uint32_t */
  FlushIntervalMs: number;
  /** @offset 24 · uint32_t */
  DirtyHighPct: number;
  /** @offset 28 · uint32_t */
  DirtyLowPct: number;
  /** @offset 32 · uint32_t · 是否允许缓存系统卷，默认 0，由用户定 */
  SystemVolumeCached: number;
  /** @offset 36 · uint32_t */
  IdleReclaimEnabled: number;
  /** @offset 40 · uint32_t */
  IdleReclaimSec: number;
  /** @offset 44 · uint32_t */
  ParallelFlush: number;
  /** @offset 48 · uint32_t · V8 块层忽略：块层下一切 I/O（含 paging/cached）均缓存 */
  CachePagingIo: number;
  /** @offset 52 · uint32_t */
  WriteRelease: number;
  /** @offset 56 · uint32_t */
  Frozen: number;
  /** @offset 60 · uint32_t */
  IdleFlush: number;
  /** @offset 64 · wchar_t[8][4] · 64 B */
  VolumeFilter: string[];
  /** @offset 128 · uint32_t */
  ReadaheadEnabled: number;
  /** @offset 132 · uint32_t */
  ReadaheadWindowKB: number;
  /** @offset 136 · uint32_t */
  AutoMemory: number;
  /** @offset 140 · uint32_t */
  MemReserveMB: number;
  /** @offset 144 · uint32_t */
  AutoUsePct: number;
  /** @offset 148 · uint32_t */
  BlockSize: number;
  /** @offset 152 · uint32_t · [F1 2026-11-06] 0=OFF 1=ON（末尾纯追加，偏移冻结） */
  TinyLFU: number;
}

/** sizeof(RB_CONFIG)，单位字节。 */
export const RB_CONFIG_SIZE = 156;

/**
 * RB_CONFIG 的全部字段，按声明顺序（= 偏移升序）。
 * 解码器 / 编码器 / zod 键集一律遍历本表，不得再手写偏移。
 */
export const RB_CONFIG_FIELDS: ReadonlyArray<RbConfigField> = [
  { name: 'Enabled', offset: 0, width: 4, kind: 'u32' },
  { name: 'ReadCacheEnabled', offset: 4, width: 4, kind: 'u32' },
  { name: 'WriteCacheEnabled', offset: 8, width: 4, kind: 'u32' },
  { name: 'WriteBack', offset: 12, width: 4, kind: 'u32' },
  { name: 'CacheSizeMB', offset: 16, width: 4, kind: 'u32' },
  { name: 'FlushIntervalMs', offset: 20, width: 4, kind: 'u32' },
  { name: 'DirtyHighPct', offset: 24, width: 4, kind: 'u32' },
  { name: 'DirtyLowPct', offset: 28, width: 4, kind: 'u32' },
  { name: 'SystemVolumeCached', offset: 32, width: 4, kind: 'u32' },
  { name: 'IdleReclaimEnabled', offset: 36, width: 4, kind: 'u32' },
  { name: 'IdleReclaimSec', offset: 40, width: 4, kind: 'u32' },
  { name: 'ParallelFlush', offset: 44, width: 4, kind: 'u32' },
  { name: 'CachePagingIo', offset: 48, width: 4, kind: 'u32' },
  { name: 'WriteRelease', offset: 52, width: 4, kind: 'u32' },
  { name: 'Frozen', offset: 56, width: 4, kind: 'u32' },
  { name: 'IdleFlush', offset: 60, width: 4, kind: 'u32' },
  { name: 'VolumeFilter', offset: 64, width: 64, kind: 'wchar2d', count: 8, len: 4 },
  { name: 'ReadaheadEnabled', offset: 128, width: 4, kind: 'u32' },
  { name: 'ReadaheadWindowKB', offset: 132, width: 4, kind: 'u32' },
  { name: 'AutoMemory', offset: 136, width: 4, kind: 'u32' },
  { name: 'MemReserveMB', offset: 140, width: 4, kind: 'u32' },
  { name: 'AutoUsePct', offset: 144, width: 4, kind: 'u32' },
  { name: 'BlockSize', offset: 148, width: 4, kind: 'u32' },
  { name: 'TinyLFU', offset: 152, width: 4, kind: 'u32' },
];
