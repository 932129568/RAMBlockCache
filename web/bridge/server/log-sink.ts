/**
 * [FEAT-LOGQ 2026-11-05] 驱动日志持久化（桥接侧）
 *
 * 问题：驱动日志环在内存中（512 条 × 96 字符），重启清空；旧 GET_LOG 只有
 * 64 条 × 48 字符，故障复盘时（尤其 0x7B 这类需要回溯"崩溃前最后几条"的
 * 场景）历史早被覆盖、文案被截断。
 *
 * 方案：桥接定时用 IOCTL_RB_LOGQ 按 Seq 游标增量拉取，追加写入
 *   %ProgramData%\RAMBlockCache\logs\driver-YYYYMMDD.log
 * 落盘格式：每行 `<ISO时间> <LEVEL> [<Src>] <Status> <文本>`
 *
 * 设计约束：
 *  - 只读拉取，绝不改变驱动行为；
 *  - 单日文件超 8MB 滚动为 .1/.2（最多保留 3 个），避免无界增长；
 *  - 拉取失败（插件旧版/设备离线）静默降级，绝不打断 /rpc；
 *  - 轮询间隔 2s，单次最多 32 条 —— 与驱动的写日志速率（低频）匹配。
 */
import { appendFileSync, existsSync, mkdirSync, renameSync, statSync } from 'node:fs';
import { join } from 'node:path';

const MAX_BYTES = 8 * 1024 * 1024;
const POLL_MS = 2000;
const BATCH = 32;
const HDR = 16;
const REC = 224;
const TEXT_WCHARS = 96;

export interface LogPageSrc {
  rbLogPage?: (fromSeq: number, maxCount: number) => Buffer;
}

interface DecodedRec {
  seq: number;
  ticks: bigint;
  level: number;
  src: number;
  status: number;
  text: string;
}

/** 按 src/ctl/rb_logq.h 解码一页应答。 */
function decodePage(buf: Buffer): { count: number; lastSeq: number; items: DecodedRec[] } {
  if (buf.length < HDR) return { count: 0, lastSeq: 0, items: [] };
  const count = Math.min(buf.readUInt32LE(0), Math.floor((buf.length - HDR) / REC));
  const rawLast = buf.readBigUInt64LE(8);
  const items: DecodedRec[] = [];
  for (let i = 0; i < count; i++) {
    const off = HDR + i * REC;
    let text = '';
    for (let c = 0; c < TEXT_WCHARS; c++) {
      const ch = buf.readUInt16LE(off + 32 + c * 2);
      if (ch === 0) break;
      text += String.fromCharCode(ch);
    }
    items.push({
      seq: Number(buf.readBigUInt64LE(off)),
      ticks: buf.readBigUInt64LE(off + 8),
      level: buf.readUInt32LE(off + 16),
      src: buf.readUInt32LE(off + 20),
      status: buf.readInt32LE(off + 24),
      text,
    });
  }
  return { count, lastSeq: Number(rawLast), items };
}

/** FILETIME(100ns since 1601) → ISO 8601 */
function filetimeToIso(ticks: bigint): string {
  const EPOCH_DIFF = 116444736000000000n; // 1601→1970
  const ms = Number((ticks - EPOCH_DIFF) / 10000n);
  return new Date(ms).toISOString();
}

const LEVEL = ['INFO ', 'WARN ', 'ERROR'];

function safeRotate(path: string): void {
  try {
    if (!existsSync(path)) return;
    if (statSync(path).size < MAX_BYTES) return;
    for (let i = 2; i >= 1; i--) {
      const from = `${path}.${i}`;
      const to = `${path}.${i + 1}`;
      if (existsSync(from)) renameSync(from, to);
    }
    renameSync(path, `${path}.1`);
  } catch {
    /* 轮转失败不影响继续写 */
  }
}

/**
 * 启动日志落盘器。返回 stop()。
 * 无插件/无设备时静默返回空停止函数（桥接其它功能不受影响）。
 */
export function startLogSink(src: LogPageSrc, logDir: string, onError?: (e: string) => void): () => void {
  if (typeof src.rbLogPage !== 'function') {
    onError?.('原生插件不含 rbLogPage（旧版本），跳过日志落盘');
    return () => {};
  }
  try {
    mkdirSync(logDir, { recursive: true });
  } catch {
    onError?.(`无法创建日志目录 ${logDir}`);
    return () => {};
  }

  let lastSeq = 0;
  let stopped = false;
  let misses = 0;

  const tick = (): void => {
    if (stopped) return;
    try {
      // 分页拉取直到没有新数据（最多 8 页，防单次卡太久）
      for (let page = 0; page < 8; page++) {
        const raw = src.rbLogPage!(lastSeq, BATCH);
        const { count, lastSeq: last, items } = decodePage(raw);
        if (count > 0) {
          const d = new Date();
          const day = `${d.getFullYear()}${String(d.getMonth() + 1).padStart(2, '0')}${String(d.getDate()).padStart(2, '0')}`;
          const path = join(logDir, `driver-${day}.log`);
          safeRotate(path);
          const lines = items
            .map(
              (r) =>
                `${filetimeToIso(r.ticks)} ${LEVEL[r.level] ?? 'INFO '} [${r.src}] ${
                  r.status !== 0 ? `0x${(r.status >>> 0).toString(16).padStart(8, '0')} ` : ''
                }${r.text}\n`,
            )
            .join('');
          appendFileSync(path, lines, 'utf8');
        }
        // 无新数据或已追平即停止翻页
        if (count === 0 || last <= lastSeq) {
          if (last > lastSeq) lastSeq = last;
          break;
        }
        lastSeq = last;
      }
      misses = 0;
    } catch (e) {
      misses++;
      // 连续 3 次失败才报告（设备重启/插件重载期间的瞬断不刷屏）
      if (misses === 3) onError?.(`日志拉取失败：${(e as Error).message}`);
      if (misses > 60) {
        onError?.('日志拉取连续失败 60 次，停止落盘');
        stopped = true;
      }
    }
  };

  const timer = setInterval(tick, POLL_MS);
  timer.unref?.();

  return () => {
    stopped = true;
    clearInterval(timer);
  };
}
