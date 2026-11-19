/*
 * rb_bootcfg.h -- [USER-REQ 2026-11-05] 开机行为配置的控制面 ABI（驱动 + 桥接共用）
 *
 * 背景：用户要求"开机不默认加速任何卷，只手动开启"，同时"可选开机恢复上次
 * 加速过的盘"。这两项是开机行为策略，不属于冻结的 RB_CONFIG（Protocol 9），
 * 因此走独立 IOCTL 0x80A，读写 Parameters 下的 AutoAttach / ResumeOnBoot。
 *
 * 语义：
 *   Get=1 读当前策略；Get=0 写入（AutoAttach/ResumeOnBoot 为 0xFF 表示不改）。
 *   驱动在每次 attach/detach 时自动维护 ResumeLetters（上次加速卷列表）。
 */
#ifndef RB_BOOTCFG_H
#define RB_BOOTCFG_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#define RB_BOOTCFG_FUNC       0x80Au
#define IOCTL_RB_BOOTCFG      CTL_CODE(FILE_DEVICE_UNKNOWN, RB_BOOTCFG_FUNC, \
                                       METHOD_BUFFERED, FILE_ANY_ACCESS)

#define RB_BOOTCFG_UNCHANGED  0xFFu   /* 写入时表示"保持原值" */

#pragma pack(push, 8)
typedef struct _RB_BOOTCFG {
    uint32_t Get;              /* 1=只读查询（忽略其余输入字段） */
    uint32_t AutoAttach;       /* 1=开机自动挂载所有数据卷；0=不自动（默认） */
    uint32_t ResumeOnBoot;     /* 1=开机恢复上次加速卷（默认）；0=完全手动 */
    uint32_t Reserved;
    wchar_t  ResumeLetters[32];/* OUT（Get 时回填）：上次加速卷字母，如 "DE" */
} RB_BOOTCFG;
#pragma pack(pop)

#endif /* RB_BOOTCFG_H */
