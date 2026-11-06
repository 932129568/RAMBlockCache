/**
 * rb_proto.h → JSON 结构化解码（桥接侧独立副本）。
 *
 * 依赖纪律（§11.10）：本文件与 src/ 无任何 import 关系；协议复用走独立拷贝或
 * 未来代码生成（WR-06），生成前人工对齐。改动 src/ctl/rb_proto.h 后必须同步本文件。
 *
 * 布局来源：src/ctl/rb_proto.h（V9，RB_PROTOCOL_VERSION=9），
 * 经 src/ui/controller/.../Models/Models.cs 的 C# 镜像交叉核对（2026-10-31）。
 * C 结构按 MSVC x64 规则对齐：u64 成员 8 字节对齐，wchar_t = 2 字节。
 *
 * 输入为 RB_MSG 应答的载荷区（调用方须先剥掉 16 字节信封）。
 */

const WIDE = new TextDecoder('utf-16le');

/** 定长 wchar_t 数组按 NUL 截断为 JS 字符串 */
export function readWide(buf: Buffer, off: number, maxChars: number): string {
  let end = off;
  const limit = off + maxChars * 2;
  while (end + 2 <= limit && buf.readUInt16LE(end) !== 0) end += 2;
  return WIDE.decode(buf.subarray(off, end));
}

/** uint64 → number（统计量远低于 2^53；越界场景需整体切 BigInt，见 protocol/types.ts 注释） */
export function readU64(buf: Buffer, off: number): number {
  return Number(buf.readBigUInt64LE(off));
}

/* ============================ GET_VERSION (10) ============================ */

export function decodeVersion(p: Buffer) {
  return {
    ProtocolVersion: p.readUInt32LE(0),
    Major: p.readUInt32LE(4),
    Minor: p.readUInt32LE(8),
    Build: p.readUInt32LE(12),
    IsBlockFilter: p.readUInt32LE(16),
    Reserved: p.readUInt32LE(20),
    DriverFlags: readU64(p, 24),
    Name: readWide(p, 32, 16), // RB_NAME_LEN=16
    TestSigned: p.readUInt32LE(64),
    Reserved2: p.readUInt32LE(68),
    Signer: readWide(p, 72, 32), // RB_SIGNER_LEN=32
  };
}

/* ============================ GET_STATS (1) ============================ */

export function decodeStats(p: Buffer) {
  const s: Record<string, number | number[]> = {};
  const U64_HEAD = [
    'ReadReq', 'ReadHit', 'ReadBytes', 'ReadHitBytes', 'WriteReq', 'WriteBytes',
    'WriteCached', 'WbFlush', 'WbBytes', 'EvictBytes', 'CurBytes', 'DirtyBytes', 'ChunkCount',
  ] as const;
  U64_HEAD.forEach((n, i) => (s[n] = readU64(p, i * 8)));

  const U32_MID = [
    'InstanceCount', 'CacheSizeMB', 'EffectiveCacheMB', 'WriteBackMode',
    'Enabled', 'SafeMode', 'RmwReads', 'TrimCount',
  ] as const;
  U32_MID.forEach((n, i) => (s[n] = p.readUInt32LE(104 + i * 4)));

  s.IdleReclaimBytes = readU64(p, 136);
  s.LostDirtyBytes = readU64(p, 144);
  s.StripeRead = Array.from({ length: 64 }, (_, i) => readU64(p, 152 + i * 8));
  s.StripeWrite = Array.from({ length: 64 }, (_, i) => readU64(p, 664 + i * 8));
  s.StripeCount = p.readUInt32LE(1176);
  s.PressureLevel = p.readUInt32LE(1180);
  s.DynamicLimitMB = p.readUInt32LE(1184);
  // DynamicLimitMB(u32)@1184 后 4 字节对齐填充，AvailablePhysMB(u64)@1192
  s.AvailablePhysMB = readU64(p, 1192);
  s.PeakBytes = readU64(p, 1200);
  s.ReadaheadBytes = readU64(p, 1208);
  s.ReadaheadHitBytes = readU64(p, 1216);
  s.PressureShrinkBytes = readU64(p, 1224);
  s.ReadaheadActive = p.readUInt32LE(1232);
  s.PanicActive = p.readUInt32LE(1236);
  s.BootFails = p.readUInt32LE(1240);
  s.DirtySnapshotMB = p.readUInt32LE(1244);
  s.CacheReadLatencyUs = p.readUInt32LE(1248);
  s.DiskReadLatencyUs = p.readUInt32LE(1252);
  // [U1 2026-11-06] RB_STATS 末尾纯追加：读延迟分位数（log2 桶近似；0=尚无样本）
  s.CacheReadLatP50Us = p.readUInt32LE(1256);
  s.CacheReadLatP95Us = p.readUInt32LE(1260);
  s.CacheReadLatP99Us = p.readUInt32LE(1264);
  s.DiskReadLatP50Us = p.readUInt32LE(1268);
  s.DiskReadLatP95Us = p.readUInt32LE(1272);
  s.DiskReadLatP99Us = p.readUInt32LE(1276);
  /* [83 2026-11-07] RB_STATS 末尾五字段（u32@1280/1284/1288/1292 + u64@1296，
   * sizeof=1304）。旧驱动回包可能只有 1280 字节：每个字段按实际长度守卫，
   * 不足则填 0，绝不 readUInt32LE/readBigUInt64LE 抛 RangeError（66 号白屏同款防线）。 */
  s.EngineBlockSize = p.length >= 1284 ? p.readUInt32LE(1280) : 0;
  s.GeometryRebuildCount = p.length >= 1288 ? p.readUInt32LE(1284) : 0;
  s.GeometryRebuildLastStatus = p.length >= 1292 ? p.readUInt32LE(1288) : 0;
  s.GeometryRebuildInProgress = p.length >= 1296 ? p.readUInt32LE(1292) : 0;
  s.LastGeometryRebuildUtc = p.length >= 1304 ? readU64(p, 1296) : 0;
  return s;
}

/* ============================ GET_VOLUMES (7) ============================ */

/** RB_VOLUME_INFO = 56B：Letter(wchar)@0 +4 Attached/Read/Write/IsSystem u32
 *  +4 填充 → CapacityMB u64@24 / FreeMB u64@32 → Bound/WriteMode/LogSect/PhysSect u32@40..56 */
export function decodeVolumes(p: Buffer) {
  const count = p.readUInt32LE(0);
  const items = [];
  for (let i = 0; i < Math.min(count, 8); i++) {
    const b = 8 + i * 56; // Items 对齐到 8
    items.push({
      Letter: readWide(p, b, 1),
      Attached: p.readUInt32LE(b + 4),
      ReadEnabled: p.readUInt32LE(b + 8),
      WriteEnabled: p.readUInt32LE(b + 12),
      IsSystem: p.readUInt32LE(b + 16),
      CapacityMB: readU64(p, b + 24),
      FreeMB: readU64(p, b + 32),
      Bound: p.readUInt32LE(b + 40),
      WriteMode: p.readUInt32LE(b + 44),
      LogicalSectorBytes: p.readUInt32LE(b + 48),
      PhysicalSectorBytes: p.readUInt32LE(b + 52),
    });
  }
  return { Count: count, Items: items };
}

/* ============================ GET_LOG (9) ============================ */

/** RB_LOG_REC = 120B：Ticks u64@0 / Level@8 / Src@12 / Status i32@16 / Text wchar[48]@20..116（对齐到 120） */
export function decodeLogList(p: Buffer) {
  const count = Math.min(p.readUInt32LE(0), 64);
  const items = [];
  for (let i = 0; i < count; i++) {
    const b = 8 + i * 120; // Items 对齐到 8
    items.push({
      Ticks: readU64(p, b),
      Level: p.readUInt32LE(b + 8),
      Src: p.readUInt32LE(b + 12),
      Status: p.readInt32LE(b + 16),
      Text: readWide(p, b + 20, 48),
    });
  }
  return { Count: count, Cursor: p.readUInt32LE(4), Items: items };
}

/* ============================ GET_EVENTS (14) ============================ */

/** RB_EVENT_REC = 128B：Seq u64@0 / OpType@8 / Percent@12 / Cancelable@16 / Phase@20
 *  / RecoveredBytes u64@24 / StageText wchar[48]@32..128 */
export function decodeEventList(p: Buffer) {
  const count = Math.min(p.readUInt32LE(8), 64);
  const items = [];
  for (let i = 0; i < count; i++) {
    const b = 16 + i * 128;
    items.push({
      Seq: readU64(p, b),
      OpType: p.readUInt32LE(b + 8),
      Percent: p.readUInt32LE(b + 12),
      Cancelable: p.readUInt32LE(b + 16),
      Phase: p.readUInt32LE(b + 20),
      RecoveredBytes: readU64(p, b + 24),
      StageText: readWide(p, b + 32, 48),
    });
  }
  // Cursor = 新消费游标（最后一条 Seq；无事件=入参原值），前端据此增量轮询
  return { Cursor: readU64(p, 0), Count: count, Reserved: p.readUInt32LE(12), Items: items };
}

/* ============================ GET_CONFIG (4) ============================ */

export function decodeConfig(p: Buffer) {
  const U32_HEAD = [
    'Enabled', 'ReadCacheEnabled', 'WriteCacheEnabled', 'WriteBack',
    'CacheSizeMB', 'FlushIntervalMs', 'DirtyHighPct', 'DirtyLowPct',
    'SystemVolumeCached', 'IdleReclaimEnabled', 'IdleReclaimSec', 'ParallelFlush',
    'CachePagingIo', 'WriteRelease', 'Frozen', 'IdleFlush',
  ] as const;
  const s: Record<string, number | string[]> = {};
  U32_HEAD.forEach((n, i) => (s[n] = p.readUInt32LE(i * 4)));
  // VolumeFilter wchar[8][4]@64..128（驱动明确忽略该字段，按原样解出备查）
  s.VolumeFilter = Array.from({ length: 8 }, (_, i) => readWide(p, 64 + i * 8, 4));
  // [F1 2026-11-06] TinyLFU@152：RB_CONFIG 末尾「纯追加」（sizeof 152→156）。
  // 偏移来源 src/ctl/rb_proto.h 的契约冻结规则——既有字段偏移不动、新字段只追加末尾；
  // 必须与 check_proto_sync.py 的「三副本字段级比对」纪律联动（C 头 ↔ C# 三镜像），
  // 并与本桥接 index.ts 的 SET_CONFIG 编码器、http-transport.ts 的 configSchema、
  // protocol/types.ts 的 RbConfig 四处保持字段名/偏移同步。
  const U32_TAIL: Array<[string, number]> = [
    ['ReadaheadEnabled', 128],
    ['ReadaheadWindowKB', 132],
    ['AutoMemory', 136],
    ['MemReserveMB', 140],
    ['AutoUsePct', 144],
    ['BlockSize', 148],
    ['TinyLFU', 152],
  ];
  for (const [n, off] of U32_TAIL) {
    // [旧驱动兼容] 老驱动 RB_CONFIG 只有 152B、不含该字段，直接 readUInt32LE(152)
    // 会 RangeError。0=OFF 缺省（新特性默认全关红线），是有意的向后兼容：
    // 只放宽这一个末尾纯追加字段，其余字段仍按固定偏移严格解码、不静默吞其它错误。
    s[n] = n === 'TinyLFU' ? (p.length >= 156 ? p.readUInt32LE(152) : 0) : p.readUInt32LE(off);
  }
  return s;
}
