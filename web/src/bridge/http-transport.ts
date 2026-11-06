/**
 * HTTP 传输实现 —— 浏览器侧唯一出口，只与同机桥接进程通信。
 * 绝不直接触碰驱动设备；IOCTL 由 bridge/server 在特权上下文执行。
 *
 * 运行时校验使用 zod：协议漂移（字段缺失 / 类型变化）会在此暴露为 DECODE 错误，
 * 而不是以 undefined 悄悄流入渲染层。
 */

import ky, { HTTPError, TimeoutError } from 'ky';
import { z } from 'zod';
import { RB_MSG } from '@protocol/constants';
import type {
  RbConfig,
  RbEventQuery,
  RbInvalidateRange,
  RbStats,
  RbVolumeAttach,
  RbVolumeList,
  RbVolumeSet,
} from '@protocol/types';
import type { BridgeError, RbTransport, Result, TraceEvent } from './transport';
import { err, ok } from './transport';

/* ---------------- 应答结构校验 ---------------- */

const versionSchema = z.object({
  ProtocolVersion: z.number(),
  Major: z.number(),
  Minor: z.number(),
  Build: z.number(),
  IsBlockFilter: z.number(),
  Reserved: z.number(),
  DriverFlags: z.number(),
  Name: z.string(),
  TestSigned: z.number(),
  Reserved2: z.number(),
  Signer: z.string(),
});

const volumeInfoSchema = z.object({
  Letter: z.string(),
  Attached: z.number(),
  ReadEnabled: z.number(),
  WriteEnabled: z.number(),
  IsSystem: z.number(),
  CapacityMB: z.number(),
  FreeMB: z.number(),
  Bound: z.number(),
  WriteMode: z.number(),
  LogicalSectorBytes: z.number(),
  PhysicalSectorBytes: z.number(),
});

const volumeListSchema = z.object({
  Count: z.number(),
  Items: z.array(volumeInfoSchema),
});

/**
 * 统计结构较大且持续追加字段，这里只强约束消费字段，
 * 其余字段用 looseObject 放行，避免"协议新增字段即前端白屏"。
 * 需要新增消费字段时，必须同时在此处补充声明。
 */
const statsSchema = z.looseObject({
  ReadReq: z.number(),
  ReadHit: z.number(),
  ReadBytes: z.number(),
  ReadHitBytes: z.number(),
  CurBytes: z.number(),
  DirtyBytes: z.number(),
  Enabled: z.number(),
  SafeMode: z.number(),
  PanicActive: z.number(),
  EffectiveCacheMB: z.number(),
  CacheReadLatencyUs: z.number(),
  DiskReadLatencyUs: z.number(),
  // [U1 2026-11-06] 读延迟分位数（消费字段落地即入强约束）
  CacheReadLatP50Us: z.number(),
  CacheReadLatP95Us: z.number(),
  CacheReadLatP99Us: z.number(),
  DiskReadLatP50Us: z.number(),
  DiskReadLatP95Us: z.number(),
  DiskReadLatP99Us: z.number(),
  // [F2 2026-11-06] TRIM 计数（状态页消费）
  TrimCount: z.number(),
  // [83 2026-11-07] 几何重建统计：旧驱动短回包无这些字段，default(0) 防白屏
  EngineBlockSize: z.number().default(0),
  GeometryRebuildCount: z.number().default(0),
  GeometryRebuildLastStatus: z.number().default(0),
  GeometryRebuildInProgress: z.number().default(0),
  LastGeometryRebuildUtc: z.number().default(0),
});

const eventListSchema = z.object({
  Cursor: z.number(),
  Count: z.number(),
  Reserved: z.number(),
  Items: z.array(
    z.object({
      Seq: z.number(),
      OpType: z.number(),
      Percent: z.number(),
      Cancelable: z.number(),
      Phase: z.number(),
      RecoveredBytes: z.number(),
      StageText: z.string(),
    }),
  ),
});

const logListSchema = z.object({
  Count: z.number(),
  Cursor: z.number(),
  Items: z.array(
    z.object({
      Ticks: z.number(),
      Level: z.number(),
      Src: z.number(),
      Status: z.number(),
      Text: z.string(),
    }),
  ),
});

const configSchema = z.looseObject({
  Enabled: z.number(),
  WriteBack: z.number(),
  CacheSizeMB: z.number(),
  BlockSize: z.number(),
  // [F1 2026-11-06 / 旧桥接兼容] TinyLFU 是 RB_CONFIG 末尾纯追加字段：新桥接的
  // decodeConfig 恒定回传（旧驱动回包按 0=OFF 兜底），但「新前端 + 旧桥接进程」混跑时
  // 旧 decodeConfig 不含该字段，必填 z.number() 会让整页报 DECODE 失败（本次 Bug 现象）。
  // 取 0=OFF 缺省，与驱动 RB_DEFAULT_TINYLFU=0 一致。取舍：字段真缺失时不再报错而是
  // 按 OFF 处理，故仅放宽这一个末尾纯追加字段，其余 4 个必填字段保持强校验。
  TinyLFU: z.number().default(0),
});

/* ---------------- 传输实现 ---------------- */

interface RpcBody {
  method: number;
  params?: unknown;
}

export class HttpTransport implements RbTransport {
  readonly timeoutMs: number;
  private readonly client: typeof ky;
  private readonly onTrace?: ((t: TraceEvent) => void) | undefined;

  constructor(baseUrl = '/rb', timeoutMs = 1500, onTrace?: (t: TraceEvent) => void) {
    this.timeoutMs = timeoutMs;
    this.onTrace = onTrace;
    this.client = ky.create({
      prefix: baseUrl, // [FIX] ky 2.x 由 prefixUrl 更名而来
      timeout: timeoutMs,
      retry: 0, // 控制通道不做自动重试，避免重复触发有副作用的 IOCTL
      headers: { 'content-type': 'application/json' },
    });
  }

  /* [PERF-UI 2026-11-04] 慢命令白名单：detach 强制前置 flush（脏多/盘慢时秒级）、
   * attach 走几何/挂栈、flush 同步回写——它们的内核 IRP 无 cancel 例程，桥接会
   * 等到真正完成；前端 1500ms 全局超时只会制造"控制通道无响应"假报警
   * （实测 detach 刷脏 8s）。命令完成语义不变，只是允许它把话说完。 */
  private static SLOW_MS: Record<number, number> = {
    3: 30000,   // FLUSH
    6: 30000,   // SET_VOLUME（切直写强制刷脏）
    8: 30000,   // ATTACH_VOLUME
  };

  /* [PERF-UI 2026-11-04] 只读查询的排队弹性：桥接是单线程服务，慢命令执行期间
   * （detach 刷脏可达数秒）所有轮询请求都在服务端排队——它们不是"失败"，只是
   * 还没轮到。只读方法无副作用，超时后重试是安全的：
   *   · 首次超时后重试一次，超时预算放宽到 8s（覆盖排队 + 执行）
   *   · 仍失败才报错 → 只有真故障（桥接死/驱动挂）才会弹"控制通道无响应"
   * 有副作用的命令（SET_CONFIG/PANIC/FLUSH 等）保持零重试，绝不复放。 */
  private static READONLY = new Set([1, 4, 7, 9, 10, 14]);

  private async call<T>(
    method: number,
    schema: z.ZodType<T>,
    params?: unknown,
  ): Promise<Result<T>> {
    const body: RpcBody = params === undefined ? { method } : { method, params };
    const t0 = performance.now();
    const readonly = HttpTransport.READONLY.has(method);

    let payload: unknown;
    try {
      try {
        payload = await this.client
          .post('rpc', { json: body, timeout: HttpTransport.SLOW_MS[method] ?? this.timeoutMs })
          .json();
      } catch (e) {
        if (!readonly) throw e;
        // 只读：超时通常只是排队，放宽预算重试一次
        this.onTrace?.({ method, ok: false, ms: performance.now() - t0 });
        payload = await this.client.post('rpc', { json: body, timeout: 8000 }).json();
      }
    } catch (e) {
      this.onTrace?.({ method, ok: false, ms: performance.now() - t0 });
      return err<T>(this.mapNetworkError(e, method));
    }
    const ms = performance.now() - t0;

    // 桥接层以统一信封返回：{ status, win32?, data }
    const envelope = payload as {
      status?: number;
      win32?: number;
      message?: string;
      data?: unknown;
    };

    if (envelope.status !== 0) {
      this.onTrace?.({ method, ok: false, ms });
      return err<T>({
        code: 'NT_STATUS',
        message: envelope.message ?? `IOCTL 0x${method.toString(16)} 应答非零状态`,
        ntStatus: envelope.status,
        win32: envelope.win32,
      });
    }

    const parsed = schema.safeParse(envelope.data);
    if (!parsed.success) {
      this.onTrace?.({ method, ok: false, ms });
      return err<T>({
        code: 'DECODE',
        message: `应答结构不符合协议：${parsed.error.issues[0]?.message ?? '未知字段错误'}`,
      });
    }

    this.onTrace?.({ method, ok: true, ms });
    return ok(parsed.data);
  }

  private mapNetworkError(e: unknown, method: number): BridgeError {
    if (e instanceof TimeoutError) {
      return {
        code: 'TIMEOUT',
        message: `控制通道 ${this.timeoutMs}ms 无响应`,
        ioctl: method,
      };
    }
    if (e instanceof HTTPError) {
      const status = e.response.status;
      if (status === 503) {
        return { code: 'DRIVER_ABSENT', message: '驱动未加载或桥接未以管理员运行' };
      }
      if (status === 401 || status === 403) {
        return { code: 'PERMISSION', message: '权限不足，需要管理员权限' };
      }
      return { code: 'IOCTL_FAILED', message: `桥接返回 HTTP ${status}` };
    }
    return { code: 'UNREACHABLE', message: '桥接进程不可达，请先启动 bridge 服务' };
  }

  getVersion() {
    return this.call(RB_MSG.GET_VERSION, versionSchema);
  }
  getStats() {
    // 部分字段强校验、其余按协议透传；返回类型按完整 RB_STATS 断言，
    // 未声明字段的可靠性由 bridge/server 的结构化解码保证。
    return this.call(RB_MSG.GET_STATS, statsSchema as unknown as z.ZodType<RbStats>);
  }
  getConfig() {
    return this.call(RB_MSG.GET_CONFIG, configSchema as unknown as z.ZodType<RbConfig>);
  }
  getVolumes() {
    // 与 stats/config 同一模式：schema 只校验 number 层，WriteMode 的字面量收窄由协议类型断言承接
    return this.call(RB_MSG.GET_VOLUMES, volumeListSchema as unknown as z.ZodType<RbVolumeList>);
  }
  getLog() {
    return this.call(RB_MSG.GET_LOG, logListSchema);
  }
  getEvents(query: RbEventQuery) {
    return this.call(RB_MSG.GET_EVENTS, eventListSchema, query);
  }

  private async command(method: number, params?: unknown): Promise<Result<void>> {
    const res = await this.call(method, z.unknown(), params);
    return res.ok ? ok(undefined) : err(res.error);
  }

  setConfig(input: RbConfig) {
    return this.command(RB_MSG.SET_CONFIG, input);
  }
  setVolume(input: RbVolumeSet) {
    return this.command(RB_MSG.SET_VOLUME, input);
  }
  attachVolume(input: RbVolumeAttach) {
    return this.command(RB_MSG.ATTACH_VOLUME, input);
  }
  invalidateRange(input: RbInvalidateRange) {
    return this.command(RB_MSG.INVALIDATE_RANGE, input);
  }
  flush() {
    return this.command(RB_MSG.FLUSH);
  }
  panic() {
    return this.command(RB_MSG.PANIC);
  }
  panicClear() {
    return this.command(RB_MSG.PANIC_CLEAR);
  }
  exitSafeMode() {
    return this.command(RB_MSG.EXIT_SAFE_MODE);
  }
}
