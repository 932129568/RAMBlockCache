/**
 * 驱动连接状态 —— 全局唯一的状态持有者。
 * 只依赖 transport 接口与 protocol 类型，不 import 任何组件或页面。
 *
 * 设计约束：
 *  - 轮询与缓存交给 @tanstack/react-query，本 store 不做数据缓存，避免两套真相。
 *  - 连接阶段与最近一次错误单独存放，因为它们是跨页共享的"会话级"状态。
 */

import { create } from 'zustand';
import { HttpTransport } from '@bridge/http-transport';
import type { BridgeError, RbTransport } from '@bridge/transport';
import type { RbVersion } from '@protocol/types';
import { RB_MSG } from '@protocol/constants';
import { bumpTraceVersion, recordTrace } from './trace-store';

export type ConnectionPhase =
  | 'idle' // 尚未发起首次握手
  | 'connecting' // 握手中
  | 'online' // 已握手，正常
  | 'offline' // 桥接不可达或驱动未加载
  | 'degraded'; // 已连接但存在协议/权限降级

interface DriverState {
  readonly transport: RbTransport;
  phase: ConnectionPhase;
  version: RbVersion | null;
  lastError: BridgeError | null;

  /** 发起握手：GET_VERSION 兼作连通性自检 */
  connect(): Promise<void>;
  /** 供轮询层上报失败，用于把"持续失败"与"偶发失败"区分开 */
  reportError(error: BridgeError): void;
  reportSuccess(): void;
}

export const useDriverStore = create<DriverState>((set, get) => ({
  // [适配 2026-10-31] 通道健康埋点经回调注入（bridge 不 import state，依赖方向不变）
  transport: new HttpTransport('/rb', 1500, (t) => {
    recordTrace(t);
    bumpTraceVersion();
  }),
  phase: 'idle',
  version: null,
  lastError: null,

  async connect() {
    set({ phase: 'connecting', lastError: null });
    const result = await get().transport.getVersion();

    if (!result.ok) {
      set({
        phase: result.error.code === 'PROTO_MISMATCH' ? 'degraded' : 'offline',
        lastError: result.error,
      });
      return;
    }

    set({ phase: 'online', version: result.value, lastError: null });
  },

  reportError(error) {
    set({
      lastError: error,
      phase: error.code === 'UNREACHABLE' || error.code === 'DRIVER_ABSENT'
        ? 'offline'
        : get().phase,
    });
  },

  reportSuccess() {
    if (get().phase !== 'online') {
      set({ phase: 'online' });
    }
    if (get().lastError !== null) {
      set({ lastError: null });
    }
  },
}));

/** 供调试与日志使用：把当前握手载荷的消息号暴露出来，避免误读为其他命令 */
export const HANDSHAKE_MESSAGE = RB_MSG.GET_VERSION;
