/**
 * 协议常量 —— 生成来源：src/ctl/rb_proto.h（V8，RB_PROTOCOL_VERSION=9）
 * 本文件为手工映射产物，任何修改必须同步回 rb_proto.h 并经 check_proto_sync 复核。
 * 冻结规则：消息编号不得复用；新字段只追加结构末尾。
 */

/** 控制设备路径（对应 RB_USER_PATH_W） */
export const RB_USER_PATH = '\\\\.\\RAMBlockCache' as const;

/** 单请求 IOCTL：CTL_CODE(0x22, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS) */
export const IOCTL_RB_REQUEST = 0x00222000;

/** RB_MSG 总字节数 = 16B 信封 + 8208B union（最大成员 RB_EVENT_LIST） */
export const RB_MSG_SIZE = 8224;

/** 信封固定 16 字节：Type / Size / Status / Reserved */
export const RB_MSG_HEADER_SIZE = 16;

export const RB_MSG_OFFSET = {
  TYPE: 0,
  SIZE: 4,
  STATUS: 8,
  RESERVED: 12,
} as const;

export const RB2_IOCTL = {
  STATS: 0x00222404,
  REFRESH: 0x00222408,
  SHRINK: 0x0022240c,
  SNAPSHOT: 0x00222410,
  VOLSTATUS: 0x00222414,
} as const;

export const RB_PROTOCOL_VERSION = 9;

/** 消息类型：编号冻结，新增只能追加 */
export const RB_MSG = {
  GET_STATS: 1,
  SET_CONFIG: 2,
  FLUSH: 3,
  GET_CONFIG: 4,
  EXIT_SAFE_MODE: 5,
  SET_VOLUME: 6,
  GET_VOLUMES: 7,
  ATTACH_VOLUME: 8,
  GET_LOG: 9,
  GET_VERSION: 10,
  PANIC: 11,
  PANIC_CLEAR: 12,
  INVALIDATE_RANGE: 13,
  GET_EVENTS: 14,
} as const;

export type RbMsgType = (typeof RB_MSG)[keyof typeof RB_MSG];

export const RB_MAX_VOLUMES = 8;
export const RB_STRIPES = 64;
export const RB_EVENT_MAX = 64;
export const RB_LOG_MAX = 64;

/** 每卷写模式 */
export const RB_WRITEMODE = {
  THROUGH: 0,
  BACK: 1,
  GLOBAL: 2,
} as const;
export type RbWriteMode = (typeof RB_WRITEMODE)[keyof typeof RB_WRITEMODE];

/** 操作进度事件类型 */
export const RB_OP = {
  BENCHMARK: 1,
  PRESET_APPLY: 2,
  UNMOUNT: 3,
  L2_REBUILD: 4,
} as const;

export const RB_PHASE = {
  START: 1,
  PROGRESS: 2,
  DONE: 3,
} as const;

/** 日志级别：0=Info 1=Warn 2=Error */
export const RB_LOG_LEVEL = {
  INFO: 0,
  WARN: 1,
  ERROR: 2,
} as const;
