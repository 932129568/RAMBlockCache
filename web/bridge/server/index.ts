/**
 * 本机桥接服务（Hono + @hono/node-server）
 *
 * 职责边界：
 *  - 唯一被允许触碰 \\.\RAMBlockCache 的 Node 侧进程。
 *  - 只监听 127.0.0.1，不绑定 0.0.0.0；控制面不外泄。
 *  - 与前端 src/ 无任何 import 关系，跨进程通信，天然无循环依赖。
 *
 * [FIX 2026-10-31] 原生插件已接入（路径 B：cargo + napi-rs，见 bridge/native）。
 *  - 插件加载与设备打开分离：/health 用 native / deviceOpen 两个字段如实区分
 *    "插件缺失"与"设备不可达"，不再混为一谈。
 *  - 设备在每次 /rpc 前惰性打开（S7-14 自愈语义：设备消失后恢复即自动重开）。
 *  - 读命令（1/4/7/9/10/14）结构化解码返回 JSON；写命令（2/6/8/13）params
 *    编码全部就绪（SET_CONFIG 全量 156B / SET_VOLUME / ATTACH / INVALIDATE_RANGE）；
 *    驱动侧写命令实测留待隔离 TestMode 机（attach 红线，43 号 §8.2）。
 */

import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { startLogSink } from './log-sink.ts';   /* [FEAT-LOGQ] 驱动日志落盘 */
import { readFile } from 'node:fs/promises';
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';   /* [FEAT-LOGUI] 日志目录访问 */
import { execFile } from 'node:child_process';
import path from 'node:path';
import { serve } from '@hono/node-server';
import { serveStatic } from '@hono/node-server/serve-static';
import { Hono, type Context } from 'hono';
import { pino } from 'pino';
import { z } from 'zod';
// [注意] Node 类型剥离（type-stripping）不解析无扩展名相对导入，必须写 .ts；
// 若未来把 bridge/ 纳入 tsc，需在 tsconfig 开 allowImportingTsExtensions。
import {
  decodeConfig,
  decodeEventList,
  decodeLogList,
  decodeStats,
  decodeVersion,
  decodeVolumes,
} from './proto-decode.ts';

const log = pino({ name: 'rb-bridge' });

// 本包是 "type": "module"，ESM 作用域没有全局 require；
// [FIX 2026-10-31] 原先裸 require 的 ReferenceError 会被 catch 吞掉，
// 会导致插件构建成功后仍永远 503。显式 createRequire 才能真正加载 .node。
const require = createRequire(import.meta.url);

// [88 B-3 2026-11-07] default 5199; env override is probe-only (isolated cwd/port test).
const PORT = Number(process.env.RBC_BRIDGE_PORT ?? 5199);
const HOST = '127.0.0.1';
/** [FEAT-LOGUI] 驱动日志落盘目录（log-sink 与 /logs 路由共用同一路径） */
const LOG_DIR = path.join(process.env.ProgramData ?? 'C:/ProgramData', 'RAMBlockCache', 'logs');

/**
 * 原生插件契约（见 bridge/native/README.md）。
 * 通过 createRequire 显式加载，避免打包工具把 .node 文件打进 bundle。
 */
/**
 * 原生插件契约（见 bridge/native/README.md）。
 * napi-derive 默认把 rb_open/rb_request/rb_close 导出为驼峰 rbOpen/rbRequest/rbClose。
 * 通过 createRequire 显式加载，避免打包工具把 .node 文件打进 bundle。
 */
interface NativeBridge {
  rbOpen(): void;
  rbRequest(type: number, payload: Buffer | null): Buffer;
  rbClose(): void;
  /** [FEAT-LOGQ] 分页日志（插件版本过旧时可能不存在） */
  rbLogPage?(fromSeq: number, maxCount: number): Buffer;
  rbBootCfg?(buf: Buffer): Buffer;
}

let native: NativeBridge | null = null;
let pluginError: string | null = null;
let deviceError: string | null = null;

function messageOf(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

try {
  // 插件产物缺失时进入 catch（构建方式见 bridge/native/README.md）。
  // 注意：createRequire 以本文件（bridge/server/）为基准，../native 即 bridge/native。
  const loaded = require('../native/build/Release/rb_native.node') as NativeBridge;
  native = loaded;
  log.info('原生桥接插件已加载');
  try {
    loaded.rbOpen();
    log.info('控制设备 \\\\.\\RAMBlockCache 已打开');
  } catch (e) {
    // 设备不可达（驱动未启动/未以管理员运行）只记录，不否定插件本身
    deviceError = messageOf(e);
    log.warn({ deviceError }, '控制设备暂不可达，/rpc 前会惰性重试');
  }
} catch (e) {
  pluginError = messageOf(e);
  log.warn({ pluginError }, '原生桥接插件不可用，/rpc 将返回 503');
}

/**
 * 读命令 → 结构化解码器（载荷 = 应答缓冲剥掉 16B 信封后的部分）。
 * 未列出的消息回 base64 原文。
 */
const DECODERS: Record<number, (p: Buffer) => unknown> = {
  1: decodeStats, // GET_STATS
  4: decodeConfig, // GET_CONFIG
  7: decodeVolumes, // GET_VOLUMES
  9: decodeLogList, // GET_LOG
  10: decodeVersion, // GET_VERSION
  14: decodeEventList, // GET_EVENTS
};

/**
 * params(JSON) → 二进制载荷。入口先过 zod（服务端输入门，[适配 2026-10-31]），
 * 编码严格按 rb_proto.h 字段偏移写入；未列出的消息无载荷（返回 null）。
 */
const volumeSetSchema = z.object({
  Letter: z.string().min(1).max(2),
  ReadEnabled: z.number(),
  WriteEnabled: z.number(),
  WriteMode: z.number(),
});
const volumeAttachSchema = z.object({
  Letter: z.string().min(1).max(2),
  Attach: z.number(),
  ReadEnabled: z.number(),
  WriteEnabled: z.number(),
  WriteMode: z.number(),
});
const configSchema = z.record(z.string(), z.union([z.number(), z.string(), z.array(z.string())]));
const invalidateSchema = z.object({
  Letter: z.string().min(1).max(2),
  Offset: z.number(),
  Length: z.number(),
});
const eventQuerySchema = z.object({
  Cursor: z.number().nonnegative().default(0),
  MaxCount: z.number().int().min(1).max(64).default(64),
});

function encodeParams(method: number, params: unknown): Buffer | null {
  if (method === 14) {
    // RB_EVENT_QUERY：Cursor u64@0 / MaxCount u32@8（<=RB_EVENT_MAX）/ 对齐填充@12
    const q = eventQuerySchema.parse(params ?? {});
    const b = Buffer.alloc(16);
    b.writeBigUInt64LE(BigInt(q.Cursor), 0);
    b.writeUInt32LE(q.MaxCount, 8);
    return b;
  }
  if (method === 6) {
    // RB_VOLUME_SET：Letter wchar@0 / ReadEnabled u32@4 / WriteEnabled@8 / WriteMode@12（16B）
    const p = volumeSetSchema.parse(params ?? {});
    const b = Buffer.alloc(16);
    b.writeUInt16LE(letterCode(p.Letter), 0);
    b.writeUInt32LE(flag(p.ReadEnabled), 4);
    b.writeUInt32LE(flag(p.WriteEnabled), 8);
    b.writeUInt32LE(writemode(p.WriteMode), 12);
    return b;
  }
  if (method === 8) {
    // RB_VOLUME_ATTACH：Letter wchar@0 / Attach@4 / ReadEnabled@8 / WriteEnabled@12 / WriteMode@16（20B）
    const p = volumeAttachSchema.parse(params ?? {});
    const b = Buffer.alloc(20);
    b.writeUInt16LE(letterCode(p.Letter), 0);
    b.writeUInt32LE(flag(p.Attach), 4);
    b.writeUInt32LE(flag(p.ReadEnabled), 8);
    b.writeUInt32LE(flag(p.WriteEnabled), 12);
    b.writeUInt32LE(writemode(p.WriteMode), 16);
    return b;
  }
  if (method === 2) {
    // RB_CONFIG 全量回写（156B）：16×u32@0..64 + VolumeFilter wchar[8][4]@64..128
    // + 7×u32@128..156，末尾纯追加 TinyLFU@152（F1，sizeof 152→156）。
    // 与 rb_proto.h / proto-decode.decodeConfig / http-transport.configSchema 四处同步：
    // 少编码一个字段，native lib.rs 的 8224B 全零缓冲会让驱动 RbApplyConfig 收到 0
    // 并持久化（Web 保存时静默把 TinyLFU 清成 OFF）。
    // 字段清单与 proto-decode.decodeConfig 严格对称（改动必须双向同步）。
    const c = configSchema.parse(params ?? {});
    const head: Array<[string, number]> = [
      ['Enabled', 0], ['ReadCacheEnabled', 4], ['WriteCacheEnabled', 8], ['WriteBack', 12],
      ['CacheSizeMB', 16], ['FlushIntervalMs', 20], ['DirtyHighPct', 24], ['DirtyLowPct', 28],
      ['SystemVolumeCached', 32], ['IdleReclaimEnabled', 36], ['IdleReclaimSec', 40],
      ['ParallelFlush', 44], ['CachePagingIo', 48], ['WriteRelease', 52], ['Frozen', 56], ['IdleFlush', 60],
    ];
    const tail: Array<[string, number]> = [
      ['ReadaheadEnabled', 128], ['ReadaheadWindowKB', 132], ['AutoMemory', 136],
      ['MemReserveMB', 140], ['AutoUsePct', 144], ['BlockSize', 148], ['TinyLFU', 152],
    ];
    const b = Buffer.alloc(156);
    for (const [k, off] of head) b.writeUInt32LE(u32(c[k]), off);
    const vf = Array.isArray(c.VolumeFilter) ? c.VolumeFilter : [];
    for (let i = 0; i < 8; i++) {
      const entry: string = typeof vf[i] === 'string' ? (vf[i] as string) : '';
      for (let j = 0; j < 4; j++) {
        b.writeUInt16LE(j < entry.length ? entry.charCodeAt(j) & 0xffff : 0, 64 + i * 8 + j * 2);
      }
    }
    for (const [k, off] of tail) b.writeUInt32LE(u32(c[k]), off);
    return b;
  }
  if (method === 13) {
    // RB_INVALIDATE_RANGE：Letter wchar@0 / Reserved u32@4 / Offset u64@8 / Length u64@16（24B）
    const p = invalidateSchema.parse(params ?? {});
    const b = Buffer.alloc(24);
    b.writeUInt16LE(letterCode(p.Letter), 0);
    b.writeBigUInt64LE(BigInt(clampNonNeg(p.Offset)), 8);
    b.writeBigUInt64LE(BigInt(clampNonNeg(p.Length)), 16);
    return b;
  }
  return null;
}

/** u32 字段：非法/越界一律 0（SET_CONFIG 是全量回写，缺字段按 0 会关功能——
 * 因此调用方必须传完整 RbConfig，前端表单以 GET_CONFIG 原文为基底做读改写） */
function u32(v: unknown): number {
  const n = Number(v);
  if (!Number.isFinite(n) || n < 0) return 0;
  return Math.min(Math.trunc(n), 0xffffffff);
}

/** u64 字段：负数/非法按 0，上界按 2^53-1（协议数值约定见 protocol/types.ts） */
function clampNonNeg(v: unknown): number {
  const n = Number(v);
  if (!Number.isFinite(n) || n < 0) return 0;
  return Math.min(Math.trunc(n), Number.MAX_SAFE_INTEGER);
}

/** wchar 卷盘符：'D' → 0x0044；非法输入按 0（驱动按空盘符拒绝，不猜默认卷） */
function letterCode(v: unknown): number {
  if (typeof v !== 'string' || v.length === 0) return 0;
  return v.charCodeAt(0) & 0xffff;
}

/** 0/1 开关字段：仅 1 视为真，其余一律 0（保守，不猜） */
function flag(v: unknown): number {
  return Number(v) === 1 ? 1 : 0;
}

/** 写模式：仅 0/1/2 合法（RB_WRITEMODE_*），越界回退 2=GLOBAL（跟随全局，最少意外） */
function writemode(v: unknown): number {
  const n = Number(v);
  return n === 0 || n === 1 ? n : 2;
}

const app = new Hono();

const healthHandler = (c: Context) =>
  c.json({
    ok: true,
    native: native !== null,
    deviceOpen: native !== null && deviceError === null,
    pluginError,
    deviceError,
    pid: process.pid,
    node: process.version,
  });

app.get('/health', healthHandler);

/* ===== [FEAT-LOGUI 2026-11-05] 落盘日志访问面 =====
 * 背景：驱动日志已由 log-sink 落盘到 LOG_DIR（512 条环×96 字符的持久化副本），
 * 但用户/支持人员无法直接访问——故障复盘仍需手工找路径。本组路由把"看日志"
 * 变成一次点击：列表 / 尾部读取 / 在资源管理器中打开目录。
 * 安全：仅本机 127.0.0.1；文件名白名单（driver-YYYYMMDD.log[.N]），
 * 绝不拼接用户输入的任意路径（防目录穿越）。 */
function listLogFiles(): { name: string; size: number; mtime: number }[] {
  try {
    return readdirSync(LOG_DIR)
      .filter((n) => /^driver-\d{8}\.log(\.\d)?$/.test(n))
      .map((name) => {
        const st = statSync(path.join(LOG_DIR, name));
        return { name, size: st.size, mtime: st.mtimeMs };
      })
      .sort((a, b) => b.name.localeCompare(a.name));
  } catch {
    return [];
  }
}

/* [USER-REQ 2026-11-05] 开机行为策略（IOCTL 0x80A）：
 * 读写 Parameters\AutoAttach 与 ResumeOnBoot，并回传上次加速卷列表。
 * 语义：默认 AutoAttach=0 / ResumeOnBoot=1 —— 开机不自动全开，只恢复
 * 用户上次手动加速过的盘。 */
app.get('/rb/bootcfg', async (c) => {
  if (!native) return c.json({ error: '原生插件未加载' }, 503);
  try {
    const buf = Buffer.alloc(80);
    buf.writeUInt32LE(1, 0);   // Get = 1
    const out = native.rbBootCfg?.(buf);
    if (!out) return c.json({ error: '插件版本过旧（缺 rbBootCfg）' }, 501);
    return c.json({
      AutoAttach: out.readUInt32LE(4),
      ResumeOnBoot: out.readUInt32LE(8),
      ResumeLetters: out.toString('utf16le', 16, 16 + 64).replace(/\u0000+$/, ''),
    });
  } catch (e) {
    return c.json({ error: messageOf(e) }, 500);
  }
});

app.post('/rb/bootcfg', async (c) => {
  if (!native) return c.json({ error: '原生插件未加载' }, 503);
  try {
    const body = (await c.req.json()) as { AutoAttach?: number; ResumeOnBoot?: number };
    const buf = Buffer.alloc(80);
    buf.writeUInt32LE(0, 0);   // Get = 0（写入）
    buf.writeUInt32LE(body.AutoAttach === undefined ? 0xff : body.AutoAttach ? 1 : 0, 4);
    buf.writeUInt32LE(body.ResumeOnBoot === undefined ? 0xff : body.ResumeOnBoot ? 1 : 0, 8);
    const out = native.rbBootCfg?.(buf);
    if (!out) return c.json({ error: '插件版本过旧（缺 rbBootCfg）' }, 501);
    return c.json({
      ok: true,
      AutoAttach: out.readUInt32LE(4),
      ResumeOnBoot: out.readUInt32LE(8),
      ResumeLetters: out.toString('utf16le', 16, 16 + 64).replace(/\u0000+$/, ''),
    });
  } catch (e) {
    return c.json({ error: messageOf(e) }, 500);
  }
});

app.get('/rb/logs', (c) =>
  c.json({ dir: LOG_DIR, exists: existsSync(LOG_DIR), files: listLogFiles() }),
);

app.get('/rb/logs/:name', (c) => {
  const name = c.req.param('name');
  if (!/^driver-\d{8}\.log(\.\d)?$/.test(name)) {
    return c.json({ error: '非法文件名' }, 400);
  }
  const full = path.join(LOG_DIR, name);
  if (!existsSync(full)) return c.json({ error: '文件不存在' }, 404);
  try {
    const text = readFileSync(full, 'utf8');
    /* 只回尾部 512KB：日志文件可达 8MB，整包回传会卡住前端 */
    const MAX = 512 * 1024;
    const tail = text.length > MAX ? text.slice(text.length - MAX) : text;
    return c.json({ name, size: text.length, truncated: text.length > MAX, text: tail });
  } catch (e) {
    return c.json({ error: String(e) }, 500);
  }
});

app.post('/rb/logs/open', (c) => {
  try {
    if (!existsSync(LOG_DIR)) return c.json({ ok: false, error: '日志目录尚不存在（驱动未产生日志）' });
    /* explorer 选中目录：Windows 专用，失败不影响其它功能 */
    execFile('explorer.exe', [LOG_DIR], () => {});
    return c.json({ ok: true, dir: LOG_DIR });
  } catch (e) {
    return c.json({ ok: false, error: String(e) });
  }
});

// [适配 2026-10-31] SPA 同源形态：控制台由本服务直接托管（5199），
// 前端 http-transport 的相对前缀 /rb 命中同端口，不再依赖 vite 代理。
app.get('/rb/health', healthHandler);

const rpcHandler = async (c: Context) => {
  if (!native) {
    // 插件未链接 → 前端映射 DRIVER_ABSENT（见 http-transport mapNetworkError 503 分支）
    return c.json(
      {
        status: -1,
        message: '原生桥接插件未链接。请先构建 bridge/native（见该目录 README.md），再重启本服务。',
        pluginError,
      },
      503,
    );
  }

  // 惰性打开设备（幂等）：设备级失效（Win32 2/6/21）后插件已自行作废句柄，
  // 这里重试 open 即完成自愈；失败 → 503，前端映射 DRIVER_ABSENT。
  try {
    native.rbOpen();
    deviceError = null;
  } catch (e) {
    deviceError = messageOf(e);
    return c.json({ status: -1, message: deviceError, deviceError }, 503);
  }

  let body: { method?: number; params?: unknown };
  try {
    body = await c.req.json();
  } catch {
    return c.json({ status: -1, message: '请求体不是合法 JSON' }, 400);
  }

  if (typeof body.method !== 'number') {
    return c.json({ status: -1, message: '缺少数值型 method 字段' }, 400);
  }

  try {
    const payload =
      body.params === undefined ? null : encodeParams(body.method, body.params);
    /* [PERF-UI 2026-11-04] 慢命令说明（不能伪异步）：napi 调用绑定本线程
     * 的 IOCTL 句柄，无法搬到 Worker；libuv 线程池只服务文件系统调用。
     * detach 强制前置 flush 实测 4.7s——此前前端 1500ms 超时制造"控制通道
     * 无响应"假报警（实际命令在后台完整成功）。前端已按方法分档超时
     * （http-transport.SLOW_MS：flush/setVolume/attach=30s），服务端保持
     * 同步真值语义。 */
    const raw = native.rbRequest(body.method, payload);
    const decode = DECODERS[body.method];
    return c.json({
      status: 0,
      data: decode ? decode(raw.subarray(16)) : raw.toString('base64'),
    });
  } catch (e) {
    // 驱动 NTSTATUS / Win32 错误：以 200 信封回传（status=-1），
    // 前端走 NT_STATUS 分支显示真实原因，而不是丢失语义的 HTTP 500。
    const message = messageOf(e);
    log.error({ message, method: body.method }, 'IOCTL 调用失败');
    return c.json({ status: -1, message });
  }
};

// [FIX D5 2026-11-03] drive-by 防线：浏览器跨源简单 POST（text/plain）不发预检、
// 必带 Origin 头——凡带 Origin 且 host 非本机 5199 的请求一律 403（副作用拦截；
// 响应本就不可读）。同源/无 Origin（curl/CLI）放行。与 D2 叠加的"无权限打 PANIC/
// 卡死控制面"攻击链在此切断。
app.use('/rpc', async (c, next) => {
  const origin = c.req.header('origin');
  if (origin) {
    const host = (() => { try { return new URL(origin).host; } catch { return ''; } })();
    if (host !== '127.0.0.1:5199' && host !== 'localhost:5199') {
      return c.json({ status: -1, message: 'cross-origin control forbidden' }, 403);
    }
  }
  await next();
});
app.post('/rpc', rpcHandler);
app.post('/rb/rpc', rpcHandler);

// —— SPA 托管（生产形态：npm run build 后由桥接同端口直出，无需 vite dev）——
// Hash 路由设计（41 号 §11.9 router.tsx）使服务端只需兜底 '/' 与静态资源。
// [88 B-3 2026-11-07] cwd-independent: bridge/server/index.ts -> up two levels
// to webconsole/ (dev: web/) -> dist/. SYSTEM/ONSTART tasks default their
// working directory to the system directory, so cwd MUST NOT participate here.
const distDir = fileURLToPath(new URL('../../dist', import.meta.url));

app.get('/', async (c) => {
  try {
    const html = await readFile(path.join(distDir, 'index.html'), 'utf8');
    return c.html(html);
  } catch {
    return c.text(
      '控制台静态资源缺失：先在 web/ 下执行 npm run build（生成 dist/），再重启桥接。',
      503,
    );
  }
});

// [88 B-3] Hono serveStatic uses join(root, req.path); an absolute root is
// accepted (verified in @hono/node-server/dist/serve-static.mjs:71,87).
app.use('/assets/*', serveStatic({ root: distDir }));

/* [HARDEN 2026-11-05] 进程级容错：桥接是控制台的单点，任何未捕获异常导致
 * 退出都表现为"控制台突然失联"，而此前完全没有兜底——
 *   · unhandledRejection：记录后继续（Node 默认会在未来版本里直接退出）
 *   · uncaughtException：记录后继续（最坏情况某请求失败，但控制台仍可用）
 *   · SIGINT/SIGTERM：有序关闭（停日志落盘 → 关设备 → 退出）
 * 纪律：绝不因为单次异常而让用户失去整个控制面。 */
let logSinkStop: (() => void) | null = null;
process.on('unhandledRejection', (reason) => {
  log.error({ reason: String(reason) }, '未捕获的 Promise 拒绝（已忽略，服务继续）');
});
process.on('uncaughtException', (err) => {
  log.error({ err: String(err?.stack ?? err) }, '未捕获异常（已忽略，服务继续）');
});
for (const sig of ['SIGINT', 'SIGTERM'] as const) {
  process.on(sig, () => {
    log.info(`收到 ${sig}，开始有序关闭`);
    try { logSinkStop?.(); } catch { /* 忽略 */ }
    try { native?.rbClose(); } catch { /* 忽略 */ }
    process.exit(0);
  });
}

serve({ fetch: app.fetch, port: PORT, hostname: HOST }, (info) => {
  log.info(`桥接服务已监听 http://${HOST}:${info.port}`);
  /* [FEAT-LOGQ 2026-11-05] 驱动日志持久化：2s 增量拉取（512 条环 × 96 字符），
   * 落盘 %ProgramData%\RAMBlockCache\logs\driver-YYYYMMDD.log。
   * 旧 GET_LOG 只有 64×48，故障复盘时历史已覆盖/文案被截断。 */
  if (native) {
    const logDir = LOG_DIR;
    logSinkStop = startLogSink(native, LOG_DIR, (msg) => log.warn({ msg }, '日志落盘'));
    log.info({ logDir }, '驱动日志落盘已启用');
  }
});
