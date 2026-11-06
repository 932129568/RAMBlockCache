/**
 * 传输层接口 —— 只依赖 protocol 类型，不依赖任何 UI、状态或框架。
 * 上层（state / features）只面向本接口编程，替换实现不需要改动调用方。
 *
 * 依赖方向：transport.ts  ←  http-transport.ts / native-transport.ts
 *           transport.ts  ←  state/driver-store.ts
 *           transport.ts  ←  features/*
 * 禁止本文件 import 任何上层模块。
 */

import type {
  RbConfig,
  RbEventList,
  RbEventQuery,
  RbInvalidateRange,
  RbLogList,
  RbStats,
  RbVersion,
  RbVolumeAttach,
  RbVolumeList,
  RbVolumeSet,
} from '@protocol/types';

/** 失败原因必须可分类，禁止把"请求发出去了"当作成功（对应 FUNC-03 假成功修复） */
export type BridgeErrorCode =
  | 'UNREACHABLE' // 桥接进程不可达（本机服务未启动）
  | 'DRIVER_ABSENT' // 驱动设备无法打开（未加载 / 非管理员）
  | 'PERMISSION' // 权限不足
  | 'PROTO_MISMATCH' // sizeof(RB_MSG) 或协议版本不一致
  | 'IOCTL_FAILED' // DeviceIoControl 返回失败
  | 'NT_STATUS' // 应答 Status 非 0
  | 'DECODE' // 应答结构不符合协议
  | 'TIMEOUT'; // 控制通道超时（对应 P1-6）

export interface BridgeError {
  code: BridgeErrorCode;
  message: string;
  /** exactOptionalPropertyTypes：允许调用方显式传 undefined，省去条件展开 */
  ioctl?: number | undefined;
  win32?: number | undefined;
  ntStatus?: number | undefined;
}

export type Result<T> =
  | { ok: true; value: T }
  | { ok: false; error: BridgeError };

export function ok<T>(value: T): Result<T> {
  return { ok: true, value };
}

export function err<T>(error: BridgeError): Result<T> {
  return { ok: false, error };
}

/** 通道健康埋点事件（IoctlMatrix/SLO 消费；bridge 层只暴露回调注入点，不知道上层存在） */
export interface TraceEvent {
  method: number;
  ok: boolean;
  ms: number;
}

export interface RbTransport {
  getVersion(): Promise<Result<RbVersion>>;
  getStats(): Promise<Result<RbStats>>;
  getConfig(): Promise<Result<RbConfig>>;
  getVolumes(): Promise<Result<RbVolumeList>>;
  getEvents(query: RbEventQuery): Promise<Result<RbEventList>>;
  getLog(): Promise<Result<RbLogList>>;

  setConfig(input: RbConfig): Promise<Result<void>>;
  setVolume(input: RbVolumeSet): Promise<Result<void>>;
  attachVolume(input: RbVolumeAttach): Promise<Result<void>>;
  invalidateRange(input: RbInvalidateRange): Promise<Result<void>>;

  flush(): Promise<Result<void>>;
  panic(): Promise<Result<void>>;
  panicClear(): Promise<Result<void>>;
  /** 退出安全模式（消息号 5，驱动早已实现；[P1-10] 同源缺口） */
  exitSafeMode(): Promise<Result<void>>;

  /** 控制通道超时预算（毫秒）。超时返回 TIMEOUT，不静默冻结（对应 P1-6） */
  readonly timeoutMs: number;
}
