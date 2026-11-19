# V11 File System Minifilter 架构蓝图（仅设计资产，不编译）

> 来源：`deliverables/240-full-stack-architecture-guidance-2026-11-14.md` 第四章。
> 文档建议路径为 `docs/v11-minifilter-blueprint/rb_minifilter_skeleton.c`。本仓库
> 内核源码受 **Phase 4 冻结**约束，且 `src/driver/**/*.c|*.h` 的任何改动都会破坏
> 三锚点签名链；把一份 `fltKernel.h` 骨架落成 `.c` 还可能被静态启发式门禁
> 或未来的通配编译清单误收。因此此处以 `.md` 内嵌同一份骨架代码保存，
> **不参与任何构建、不进 `build/sources.manifest.json`、不部署**。

## 1. 为什么要迁移

| 维度 | V10 WDM 卷块过滤（现状） | V11 Minifilter（目标） |
| :--- | :--- | :--- |
| 挂载层级 | `BOOT_START` + Volume `UpperFilters`（LBA 扇区层） | `FltRegisterFilter`（文件对象 / 流上下文层） |
| 语义感知 | 盲视扇区，需手工维护排除表、分页 I/O 旁路、TRIM 失效 | `PreCreate`/`PreRead`/`PreWrite` 可直接取 `FLT_FILE_NAME_INFORMATION`，天然排除 `$MFT`、`$LogFile`、页面文件 |
| 加载卸载 | 依赖引导启动，更新需重启 + 双重 UAC/重启验证 | `fltmc load` / `fltmc unload` 热插拔 |
| 核心复用 | `src/core/` 经 `RB_KERNEL_BUILD` 静态链接 | 100% 复用，键由 `VolumeId + LBA` 改为 `StreamFileId + FileOffset` |

## 2. 前置条件（当前均不满足，禁止立项实施）

1. Phase 4 判据满足、R2 例外窗口关闭、`VerifyRestore` 九项全 True。
2. J6 / J10 / J14 / J16 / J17 中至少 L2 主线有真机触发证据（R7）。
3. WDK + FltMgr 工具链引入，并重新设计 22 项门禁的签名/锚点/部署链路。
4. 用户明确拍板启动 V11 分支；本轮不改 `src/driver/**`。

## 3. 骨架参考实现（原文档代码，逐字保留，未编译验证）

```c
/*
 * rb_minifilter_skeleton.c
 * V11 File System Minifilter Architectural Blueprint (FltMgr)
 * Reuses src/core/cache.h engine at StreamContext (FileId + Offset) granularity.
 */

#include <fltKernel.h>
#include "../../src/core/cache.h"

static PFLT_FILTER g_RbFilterHandle = NULL;

FLT_PREOP_CALLBACK_STATUS
RbMiniPreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
)
{
    UNREFERENCED_PARAMETER(FltObjects);
    *CompletionContext = NULL;

    /* 1. Fast-path bypass for paging I/O, kernel-mode callers, or non-cached metadata */
    if (FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    /* 2. Query stream context -> lookup aligned blocks in src/core/cache.h */
    /* On full hit: copy from RAM cache, set Data->IoStatus.Status = STATUS_SUCCESS,
     * and return FLT_PREOP_COMPLETE (zero lower-driver IRP overhead). */
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

FLT_PREOP_CALLBACK_STATUS
RbMiniPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext
)
{
    UNREFERENCED_PARAMETER(FltObjects);
    *CompletionContext = NULL;

    if (FlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO | IRP_SYNCHRONOUS_PAGING_IO)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    /* Write-through default: invalidate/update cache lines and pass down;
     * Controlled write-back: absorb into RAM dirty blocks and schedule ParallelFlush. */
    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

static const FLT_OPERATION_REGISTRATION g_RbMiniCallbacks[] = {
    { IRP_MJ_READ,  0, RbMiniPreRead,  NULL },
    { IRP_MJ_WRITE, 0, RbMiniPreWrite, NULL },
    { IRP_MJ_OPERATION_END }
};

static const FLT_REGISTRATION g_RbMiniRegistration = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,                          /* Dynamic unload allowed (fltmc unload) */
    NULL,
    g_RbMiniCallbacks,
    NULL,                       /* FilterUnloadCallback */
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL
};

NTSTATUS
RbMiniDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    UNREFERENCED_PARAMETER(RegistryPath);

    status = FltRegisterFilter(DriverObject, &g_RbMiniRegistration, &g_RbFilterHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = FltStartFiltering(g_RbFilterHandle);
    if (!NT_SUCCESS(status)) {
        FltUnregisterFilter(g_RbFilterHandle);
        g_RbFilterHandle = NULL;
    }
    return status;
}
```

## 4. 已知风险（评审时必须回答）

- `FLT_REGISTRATION` 结构字段数量随 WDK 版本变化，上表按文档原样保留，未经编译验证。
- `FLT_PREOP_COMPLETE` 全命中路径必须自行填充 `Data->IoStatus.Information`，否则上层读到长度 0。
- 回写（WriteBack）在文件语义层需处理 `IRP_NOCACHE`、内存映射与稀疏文件，不能照搬卷块层 `ParallelFlush` 假设。
- 现有 22 项门禁中"签名链 / 三锚点 / BOOT_START 部署"全部针对 WDM 驱动，V11 需重新设计，不得复用。
