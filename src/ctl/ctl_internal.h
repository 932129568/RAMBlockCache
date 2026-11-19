// ctl_internal.h —— ramboostctl 各 TU 的内部共享声明（142 拆分 2026-11-10）。
// 仅 ramboostctl*.c 可包含；对外契约仍是 rb_proto.h（冻结）。纯搬运拆分，零语义改动。
#ifndef RBC_CTL_INTERNAL_H
#define RBC_CTL_INTERNAL_H

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <winioctl.h>     /* FSCTL_GET_RETRIEVAL_POINTERS（B-5 flushfile extents） */
#include <wintrust.h>     /* WinVerifyTrust（B-6 signature） */
#include <softpub.h>      /* WINTRUST_ACTION_GENERIC_VERIFY_V2 / DRIVER_ACTION_VERIFY */
#include <wincrypt.h>     /* 证书链/签名者名（B-6 signature） */
#include <bcrypt.h>       /* BCrypt SHA-256（B-14 license HMAC 占位） */

#include "rb_proto.h"
#include "bench_policy.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

/* set flush=infinite：驱动真实哨兵是 0=关闭周期回写（BUG2 契约；[FIX D6] 原发
 * 0xFFFFFFFF 被驱动钳成 600000ms，用户显式"关闭"被静默改写） */
#define RB_FLUSH_INFINITE_MS 0u

/* g_dev = 驱动控制设备句柄（CreateFileW 打开 \.\RAMBlockCache）。 */
extern HANDLE g_dev;
extern SRWLOCK g_devLock;

/* [FIX 2026-11-13] 最近一次通信失败的原因，在失败现场捕获（见 ctl_conn.c）。
 * 提示文案与诊断输出都必须读它们，不能事后调 GetLastError()。 */
extern DWORD   g_lastWin32;
extern int32_t g_lastNt;

/* ---------------- 驱动通信（控制设备 + DeviceIoControl） ---------------- */

int  rb_connect(void);
const char *cli_conn_hint(void);
int  rb_send(const RB_MSG *msg, RB_MSG *reply);
int  rb_get_stats(RB_STATS *stats);
int  rb_get_config(RB_CONFIG *cfg);
int  rb_get_events(uint64_t cursor, RB_EVENT_LIST *out);
int  rb_set_config(const RB_CONFIG *cfg);
int  rb_flush(void);
int  rb_set_volume(wchar_t letter, int readOn, int writeOn, uint32_t writeMode);
int  rb_get_version(RB_VERSION *v);
int  rb_exit_safe_mode(void);
int  rb_get_volumes(RB_VOLUME_LIST *list);
int  rb_attach_volume(wchar_t letter, int attach, int readOn, int writeOn,
                      uint32_t writeMode, int32_t *pStatus);

/* ---------------- 命令行打印 / 词汇表 ---------------- */
/* [V10.6 契约执行体波次] CLI 错误码词汇表
 * 数值与前端 Models/RbErrorCode.cs 对齐（双栏日志对照纪律）：
 *   1=NotImplemented  2=NotLicensed  3=DisabledByPolicy  4=PreconditionFailed
 *   5=DriverOffline   6=AlreadyInitialized  7=DuplicateVolume  8=FlushRangeUnsupported
 * [FIX F-1] 宽字符安全输出（CliPrintWide）：控制台附着→WriteConsoleW；重定向→CP_ACP。 */
void CliPrintWide(const WCHAR *s);

/* ---------- [契约 B-5] RB_MSG_INVALIDATE_RANGE 下发（协议消息 13） ---------- */
int rb_invalidate_range(wchar_t letter, uint64_t offset, uint64_t length);

/* NTSTATUS → CLI errorCode（与前端 RbErrorCode 词汇表对齐的常用项） */
#define NT_STA_INVALID_PARAMETER   ((int)0xC000000DL)
#define NT_STA_NOT_FOUND           ((int)0xC0000225L)
#define NT_STA_DEVICE_NOT_READY    ((int)0xC00000A3L)
#define NT_STA_ACCESS_DENIED       ((int)0xC0000022L)
#define NT_STA_NAME_COLLISION      ((int)0xC0000035L)

/* ---------------- verb 命令实现（各 TU） ---------------- */
int cli_bench(int argc, char **argv);          /* [契约 B-4] 二遍读基准 */
int cli_flushfile(int argc, char **argv);      /* [契约 B-5] 缓存范围刷新 */
int cli_signature(int argc, char **argv);      /* [契约 B-6] 驱动签名态 */
int cli_sigverify(int argc, char **argv);      /* [237] 只读验签（T4 取证） */
int cli_events(int argc, char **argv);         /* [契约 B-7] 事件轮询 */
int cli_budget(void);                          /* [契约 B-13] 预算四元组 */
int cli_attach_precheck(wchar_t letter);       /* [契约 B-9] attach 前置校验 */
int cli_optin(int argc, char **argv);          /* [契约 B-10] 系统卷 opt-in */
int cli_prefetch_check(int argc, char **argv); /* [契约 B-11] 预取门后端校验 */
int cli_license(int argc, char **argv);        /* [契约 B-14] 许可 */

#endif /* RBC_CTL_INTERNAL_H */
