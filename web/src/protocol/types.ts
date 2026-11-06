/**
 * 协议类型 —— 生成来源：src/ctl/rb_proto.h（RB_PROTOCOL_VERSION=9）
 *
 * 字段命名刻意保留 C 结构原始拼写（PascalCase），便于与 rb_proto.h 逐行对照，
 * 也便于 check_proto_sync 做字段级镜像校验。不要在渲染层重命名字段。
 *
 * 数值约定：C 的 uint64_t 在此映射为 number。
 * 当前统计量（字节计数）远低于 Number.MAX_SAFE_INTEGER（2^53），
 * 若未来出现累计字节数超过该值的场景，必须改为 BigInt 并同步序列化层。
 *
 * 本文件不 import 任何运行时代码，是纯类型与常量依赖的叶子模块。
 */

import type { RB_MSG, RbMsgType, RbWriteMode } from './constants';

/* ============================ 全局配置 ============================ */

export interface RbConfig {
  Enabled: number;
  ReadCacheEnabled: number;
  WriteCacheEnabled: number;
  WriteBack: number;
  CacheSizeMB: number;
  FlushIntervalMs: number;
  DirtyHighPct: number;
  DirtyLowPct: number;
  SystemVolumeCached: number;
  IdleReclaimEnabled: number;
  IdleReclaimSec: number;
  ParallelFlush: number;
  CachePagingIo: number;
  WriteRelease: number;
  Frozen: number;
  IdleFlush: number;
  /** 长度恒为 RB_MAX_VOLUMES(8)；驱动当前忽略该字段，勿在 UI 暴露为有效配置 */
  VolumeFilter: string[];
  ReadaheadEnabled: number;
  ReadaheadWindowKB: number;
  AutoMemory: number;
  MemReserveMB: number;
  AutoUsePct: number;
  BlockSize: number;
  /** [F1 2026-11-06] TinyLFU 频率准入 0=OFF（默认） 1=ON */
  TinyLFU: number;
}

/* ============================ 统计 ============================ */

export interface RbStats {
  ReadReq: number;
  ReadHit: number;
  ReadBytes: number;
  ReadHitBytes: number;
  WriteReq: number;
  WriteBytes: number;
  WriteCached: number;
  WbFlush: number;
  WbBytes: number;
  EvictBytes: number;
  CurBytes: number;
  DirtyBytes: number;
  ChunkCount: number;
  InstanceCount: number;
  CacheSizeMB: number;
  EffectiveCacheMB: number;
  WriteBackMode: number;
  Enabled: number;
  SafeMode: number;
  RmwReads: number;
  TrimCount: number;
  IdleReclaimBytes: number;
  LostDirtyBytes: number;
  /** 长度恒为 RB_STRIPES(64) */
  StripeRead: number[];
  StripeWrite: number[];
  StripeCount: number;
  PressureLevel: number;
  DynamicLimitMB: number;
  AvailablePhysMB: number;
  PeakBytes: number;
  ReadaheadBytes: number;
  ReadaheadHitBytes: number;
  PressureShrinkBytes: number;
  ReadaheadActive: number;
  /** 1=全局熔断中，一切 I/O 直通盘 */
  PanicActive: number;
  BootFails: number;
  DirtySnapshotMB: number;
  /** 命中读延迟 EWMA（微秒）。0=尚无样本，UI 不得画假值 */
  CacheReadLatencyUs: number;
  /** 含 miss 下盘读延迟 EWMA（微秒）。0=尚无样本 */
  DiskReadLatencyUs: number;
  /** [U1 2026-11-06] 命中读延迟 P50/P95/P99（微秒；log2 桶近似）。0=尚无样本 */
  CacheReadLatP50Us: number;
  CacheReadLatP95Us: number;
  CacheReadLatP99Us: number;
  /** [U1 2026-11-06] 含下盘读延迟 P50/P95/P99（微秒；log2 桶近似）。0=尚无样本 */
  DiskReadLatP50Us: number;
  DiskReadLatP95Us: number;
  DiskReadLatP99Us: number;
  /** [83 2026-11-07] 运行期几何重建统计（RB_STATS 末尾纯追加）。
   *  EngineBlockSize=引擎真实几何（重建窗口内允许 0）；旧驱动短回包由桥接填 0。 */
  EngineBlockSize: number;
  GeometryRebuildCount: number;
  GeometryRebuildLastStatus: number;
  GeometryRebuildInProgress: number;
  LastGeometryRebuildUtc: number;
}

/* ============================ 卷 ============================ */

export interface RbVolumeInfo {
  Letter: string;
  /** 热开关当前是否开启（正在加速） */
  Attached: number;
  ReadEnabled: number;
  WriteEnabled: number;
  IsSystem: number;
  CapacityMB: number;
  FreeMB: number;
  /** 过滤驱动是否已绑定到该卷栈 */
  Bound: number;
  WriteMode: RbWriteMode;
  LogicalSectorBytes: number;
  PhysicalSectorBytes: number;
}

export interface RbVolumeList {
  Count: number;
  Items: RbVolumeInfo[];
}

export interface RbVolumeSet {
  Letter: string;
  ReadEnabled: number;
  WriteEnabled: number;
  WriteMode: RbWriteMode;
}

export interface RbVolumeAttach {
  Letter: string;
  /** 1=热启用 0=热停用（停用前先 flush 脏数据） */
  Attach: number;
  ReadEnabled: number;
  WriteEnabled: number;
  /** RB_WRITEMODE_GLOBAL 表示保持原值 */
  WriteMode: RbWriteMode;
}

/* ============================ 版本 ============================ */

export interface RbVersion {
  ProtocolVersion: number;
  Major: number;
  Minor: number;
  Build: number;
  IsBlockFilter: number;
  Reserved: number;
  DriverFlags: number;
  Name: string;
  /** 1=测试签名/未知（保守默认）；取不到时按测试签名处理 */
  TestSigned: number;
  Reserved2: number;
  Signer: string;
}

/* ============================ 事件 ============================ */

export interface RbEventRec {
  Seq: number;
  OpType: number;
  /** 0-100；不确定阶段（如排空等待）置 0，由 StageText 说明 */
  Percent: number;
  Cancelable: number;
  Phase: number;
  RecoveredBytes: number;
  StageText: string;
}

export interface RbEventQuery {
  Cursor: number;
  MaxCount: number;
}

export interface RbEventList {
  Cursor: number;
  Count: number;
  Reserved: number;
  Items: RbEventRec[];
}

/* ============================ 日志 ============================ */

export interface RbLogRec {
  Ticks: number;
  Level: number;
  Src: number;
  Status: number;
  Text: string;
}

export interface RbLogList {
  Count: number;
  Cursor: number;
  Items: RbLogRec[];
}

/* ============================ 范围失效 ============================ */

export interface RbInvalidateRange {
  Letter: string;
  Reserved: number;
  Offset: number;
  Length: number;
}

/* ============================ 请求 / 应答信封 ============================
 *
 * 可辨识联合是本前端的核心类型设计：以 Type 字段为判别标签，
 * 使 JSX 中的 switch / Record 映射可以获得逐分支的精确收窄。
 */

interface Envelope<TType extends RbMsgType, TData> {
  Type: TType;
  Size: number;
  /** 应答：NTSTATUS。0 表示成功，非 0 必须由上层显式处理，禁止假成功 */
  Status: number;
  Data: TData;
}

export type RbResponse =
  | Envelope<typeof RB_MSG.GET_VERSION, RbVersion>
  | Envelope<typeof RB_MSG.GET_STATS, RbStats>
  | Envelope<typeof RB_MSG.GET_CONFIG, RbConfig>
  | Envelope<typeof RB_MSG.GET_VOLUMES, RbVolumeList>
  | Envelope<typeof RB_MSG.GET_EVENTS, RbEventList>
  | Envelope<typeof RB_MSG.GET_LOG, RbLogList>
  | Envelope<typeof RB_MSG.SET_CONFIG, RbConfig>
  | Envelope<typeof RB_MSG.SET_VOLUME, RbVolumeSet>
  | Envelope<typeof RB_MSG.ATTACH_VOLUME, RbVolumeAttach>
  | Envelope<typeof RB_MSG.FLUSH, Record<string, never>>
  | Envelope<typeof RB_MSG.PANIC, Record<string, never>>
  | Envelope<typeof RB_MSG.PANIC_CLEAR, Record<string, never>>
  | Envelope<typeof RB_MSG.EXIT_SAFE_MODE, Record<string, never>>
  | Envelope<typeof RB_MSG.INVALIDATE_RANGE, RbInvalidateRange>;

export type RbResponseOf<T extends RbMsgType> = Extract<RbResponse, { Type: T }>;

/** 请求：无载荷的消息不带 Data 字段 */
export type RbRequest =
  | { Type: typeof RB_MSG.GET_VERSION }
  | { Type: typeof RB_MSG.GET_STATS }
  | { Type: typeof RB_MSG.GET_CONFIG }
  | { Type: typeof RB_MSG.GET_VOLUMES }
  | { Type: typeof RB_MSG.GET_LOG }
  | { Type: typeof RB_MSG.GET_EVENTS; Data: RbEventQuery }
  | { Type: typeof RB_MSG.SET_CONFIG; Data: RbConfig }
  | { Type: typeof RB_MSG.SET_VOLUME; Data: RbVolumeSet }
  | { Type: typeof RB_MSG.ATTACH_VOLUME; Data: RbVolumeAttach }
  | { Type: typeof RB_MSG.INVALIDATE_RANGE; Data: RbInvalidateRange }
  | { Type: typeof RB_MSG.FLUSH }
  | { Type: typeof RB_MSG.PANIC }
  | { Type: typeof RB_MSG.PANIC_CLEAR }
  | { Type: typeof RB_MSG.EXIT_SAFE_MODE };
