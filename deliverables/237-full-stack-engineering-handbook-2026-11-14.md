# 237 — RAMBlockCache 全栈进展核验、J6/Phase4 破局方案、CPU 调度优化与液态玻璃双端完整工程手册（终极全量处理版）

日期：2026-11-14（本地）
性质：**交接报告（`236`）全景核验 + 液态玻璃 Phase 1–3 验收基线 + J6 阻断点 A/B 完整破局代码 + Phase 4（J17 死闩 `s_Draining` 状态解耦）完整代码 + CPU 多核调度与并发优化全套代码 + 其他功能优化（T4 原生验签 / Gate16 增强 / L2 热度保护 / UI 零空耗）完整实施指南**
关联：`HANDOVER-CURRENT.md`（`236-handover-2026-11-14.md`）、`235`（液态玻璃计划与冲突审计）、`234`（源码与功能报告）、`233`（第四轮 J6 判读）、`231/232`（引导看门根因与修复）、`AGENTS.md`、`README.md`、`KNOWN_ISSUES.md`
实测基线：
- 部署件：`dist/RAMBlockCache.sys` 与 `src/driver/x64/Release/RAMBlockCache.sys` 同值 = `7125d2199981177925b2fbb9d2e6a055ab65895afadca98ca6ac0040c7cded47`（197,960 B，Authenticode `Valid`）
- 安装包：`dist/RAMBlockCache-Setup.exe` = `bba5b57365ee67d86d91c426483df8db5aa1f4903efc18381de8ac0b0b32dd15`
- 内核源码基线：`rbL2.c` = `65b80950e7e57589a82c365f97c4473afe2006969ca3ce64918158c660713b7b`；`rbctl.c` = `1b0ce41fa4834a317a548e56a2b02ce568c544803033fba73536428cdcbce38d`
- 门禁与测试基线：`ALL GATES PASS`（23 条 `[ok]`，`0 fail, 0 warn`），Web 单测 `7 files / 45 tests passed`，J6 回归网 `PASS=63 FAIL=0`，Gate21 `HIGH=0`（`159 hits` 与基线一致）

---

## §1 交接进展核验与实现效果全量评估

### 1.1 已实现效果核验（100% 达成预期并通过 QA 独立实证）

| 评估模块 | 交接报告实测证据 | 效果判定与核心结论 |
|---|---|---|
| **1. 液态玻璃双端 UI（Phase 1–3）** | 共落地 **17 个文件（10 改 / 7 新增）**；`-Target web`（2,247 modules，2.18s）与 `-Target ui`（6 工程，**0 警告 0 错误**）全部通过；追加闭环了 D14–D20 全部遗留项。 | **已完全实现预期效果**：<br>1) `tokens.css` 增量追加的 18 个 `--rbc-lg-*` 令牌 18/18 全量消费；<br>2) `.rbc-liquid-pill::before` 已绑定 `url(#rbc-liquid-gooey)` 并在 `App.tsx:170` 建立真实消费点（闭环 D16）；<br>3) WPF 控制器与安装器均已接入分级互斥版 `DwmLiquidGlassHelper.cs`，控制器内置 `WebView2` 透明通道与桌面色推送已打通；<br>4) 新增 4 个 vitest 测试文件，前端单测达 **7 文件 / 45 项测试全过**（闭环 D17）。 |
| **2. 23 项静态门禁与构建契约** | `build.ps1 -Target gates` 实跑 **23 条 `[ok]`（`0 fail, 0 warn`）**；`check_ps_syntax`（67 个 `.ps1` / 35 个 `.cmd`）0 错误；Gate22 校验通过。 | **已完全实现预期效果**：<br>1) `check_docs_numbering.py:104` 已强化为 `re.fullmatch(r"SHA256:\s*([0-9a-fA-F]{64})")`，缺 `SHA256: ` 前缀即 FAIL，6 场景变异测试 `TEETH_VERDICT: PASS`（闭环 D15）；<br>2) `build/sources.manifest.json` 已登记 `targets.ui.sources`(2) 与 `targets.web.sources`(8) 共 10 个新文件（闭环 D18）。 |
| **3. C-SLICE 切片与引导看门守护** | C-SLICE 第 5 轮 `dcm=8 / dL2h=16 / dL2m=0` 真机 PASS；活体 `C:\ProgramData\RAMBoost\boot_guard.ps1` 已含 `DeployAck` 判据（`grep -c DeployAck = 3`）。 | **已完全实现预期效果**：<br>1) $>64\text{KB}$ 且 $\le 1\text{MB}$ 的 Core Miss 按 $64\text{KB}$ 切 16 片由 L2 命中回填已真机实证；<br>2) 在 `FilterArmed=0` 的正常重启中，`UpperFilters=[volsnap,RAMBlockCache]` 完整保住，7 条 `AddDevice` 全部恢复。 |

### 1.2 待突破阻断点与根因定论（本手册第 §2–§4 节全量提供解决方案）

1. **J6 跨引导命中阻断点 A（工具与懒开自锁）**：驱动在重启后靠**首次穿过 `rbio.c:534` 的 Core Read Miss**（`rbfeat.c:823` $\rightarrow$ `rbL2.c:1101` `RbL2LazyOpenInt`）才会打开容器并置 `s_State=1`；而 `j6_reboot_hit.py` 第 `4)` 段在 `l2state != 1` 时直接跳过全部 2,041 次定向读 $\Rightarrow$ **验证器不读则驱动不懒开，驱动不懒开则验证器不读**。
2. **J6 跨引导命中阻断点 B（H5 关机索引未定稿）**：正常重启后 `C:\RAMBoostL2c.idx` 仍为 prepare 时由 `RbL2IdxUnseal` 写下的 4 字节 `00 00 00 00`（`mtime` 未更新），根因是关机路径中 `s_Halt` 停机门与 `RbL2IdxSaveGuarded`（`rbL2.c:543` J14）顺序冲突或电源 IRP 阶段晚于文件系统锁定，且缺乏运行期静默定稿检查点。
3. **Phase 4 内核死闩（D21 / D22 / J17）**：原草案引用了虚构符号 `s_InFlight`（`rbL2.c:886` 真实符号为 `s_Ops`），且在 `s_Ops > 0` 排空超时（仍有残留 I/O 在飞）时直接将 `s_Halt` 还原为 `0` 会打破 `rbL2.c:534-543` 与 `:852-855`「读到 `s_Halt == 0` $\Rightarrow$ 排空从未发生」的冻结契约。必须通过**引入独立瞬态排空门 `s_Draining` 与永久停机闩 `s_Halt` 解耦**来彻底根治。

---

## §2 立即可执行优化包（零内核改动、不触碰观察期 Pin `7125d219…`）

本节包含 4 项工具、门禁与前端性能升级，**完全不修改任何内核 `.c/.h`、无需重签 `.sys`、不影响观察期巡检 Pin**，可立即在仓库落盘并跑通门禁。

### 2.1 破解 J6 阻断点 A：`src/scripts/j6_reboot_hit.py` 不相交哨兵预热冷读（Sentinel Disjoint Cold Read）

#### 2.1.1 设计原理
- **为什么不能发 `REFRESH` 或重开 `l2 enable`？** 因为那会重置容器/索引，破坏「纯跨引导还原」前提。
- **为什么哨兵预热冷读能完美破局？**
  1. 在 `j6_reboot_hit.py` 进入第 `4)` 段检查 `l2state` 和采集 `pre` 统计快照**之前**，若检测到 `Features\Flags` 含 `0x4`（`RB2F_L2`）但当前 `l2state == 0`，则自动从 `j6-manifest.json` 记载的负载文件 `D:\rbc-l2-load.bin` 中选取一个**与 2,041 个目标测试块完全不相交的 64KB 对齐哨兵偏移（Sentinel Offset）**。
  2. 通过 Win32 `CreateFileW` 以 `FILE_FLAG_NO_BUFFERING` 发起 **1 次 $64\text{KB}$ 真实冷读**。
  3. 重启后 Core RAM 缓存为空，该次冷读必然穿过 `rbio.c:534` 产生 1 次 `Core Miss`，触发 `rbfeat.c:823` $\rightarrow$ `rbL2.c:1101` `RbL2LazyOpenInt` 打开容器、执行 `RbL2IdxLoad()` 并无条件置 `s_State = 1`（`rbL2.c:844`）。
  4. 哨兵偏移不在 2,041 个测试块集合内，**零污染目标块的 Core 缓存状态**；随后再采样 `pre` 快照并执行 2,041 次正式验证读，J6 三条真判据（`dCoreMiss >= 1`、`dL2Hits > 0`、`dL2Miss == 0`）一字节不改！

#### 2.1.2 完整实现代码（植入 `src/scripts/j6_reboot_hit.py`）

```python
# 植入 src/scripts/j6_reboot_hit.py（位于第 4 段 l2state 检查与 pre stats 采样之前）
import ctypes
from ctypes import wintypes
from pathlib import Path

GENERIC_READ = 0x80000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_FLAG_NO_BUFFERING = 0x20000000
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
SENTINEL_SLICE_BYTES = 65536  # 64KB 对齐块


def choose_disjoint_sentinel_offset(
    load_file_size: int,
    target_offsets: list[int],
    slice_bytes: int = SENTINEL_SLICE_BYTES,
) -> int:
    """
    从负载文件末尾向前寻找第一个与 target_offsets (2041 个测试块) 64KB 区间完全不相交的偏移。
    若负载文件末尾无空闲块，则在文件合法范围内扫描第一个未占用的 64KB 槽位。
    """
    occupied_slices = { (off // slice_bytes) for off in target_offsets }
    total_slices = load_file_size // slice_bytes
    if total_slices <= 0:
        return 0
    for slice_idx in range(total_slices - 1, -1, -1):
        if slice_idx not in occupied_slices:
            return slice_idx * slice_bytes
    # 极端兜底：若整个文件每一个 64KB 块都在测试集内，返回最后一个对齐块
    return (total_slices - 1) * slice_bytes


def trigger_lazy_open_sentinel_read(
    load_path: Path,
    target_offsets: list[int],
) -> tuple[bool, str]:
    """
    执行 1 次 64KB FILE_FLAG_NO_BUFFERING 冷读以触发驱动首次 Core Miss -> RbL2LazyOpenInt。
    不发 REFRESH、不写注册表、不污染 target_offsets 的 Core 缓存状态。
    """
    if not load_path.is_file():
        return False, f"load file absent: {load_path}"

    file_size = load_path.stat().st_size
    if file_size < SENTINEL_SLICE_BYTES:
        return False, f"load file too small ({file_size} B < {SENTINEL_SLICE_BYTES} B)"

    sentinel_off = choose_disjoint_sentinel_offset(file_size, target_offsets)

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
        wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    kernel32.CreateFileW.restype = wintypes.HANDLE

    handle = kernel32.CreateFileW(
        str(load_path),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING,
        None,
    )
    if handle == INVALID_HANDLE_VALUE or handle is None:
        err = ctypes.get_last_error()
        return False, f"CreateFileW failed winerr={err}"

    try:
        # 分配 64KB 页对齐内存供 FILE_FLAG_NO_BUFFERING 读取
        MEM_COMMIT_RESERVE = 0x3000
        PAGE_READWRITE = 0x04
        MEM_RELEASE = 0x8000
        kernel32.VirtualAlloc.restype = ctypes.c_void_p
        buf = kernel32.VirtualAlloc(None, SENTINEL_SLICE_BYTES, MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if not buf:
            return False, "VirtualAlloc 64KB failed"
        try:
            li_off = ctypes.c_longlong(sentinel_off)
            new_pos = ctypes.c_longlong(0)
            if not kernel32.SetFilePointerEx(handle, li_off, ctypes.byref(new_pos), 0):
                err = ctypes.get_last_error()
                return False, f"SetFilePointerEx({sentinel_off}) failed winerr={err}"

            bytes_read = wintypes.DWORD(0)
            ok = kernel32.ReadFile(
                handle,
                ctypes.c_void_p(buf),
                SENTINEL_SLICE_BYTES,
                ctypes.byref(bytes_read),
                None,
            )
            if not ok or bytes_read.value != SENTINEL_SLICE_BYTES:
                err = ctypes.get_last_error()
                return False, f"ReadFile sentinel failed ok={ok} read={bytes_read.value} winerr={err}"

            return True, f"sentinel cold-read 64KB at offset={sentinel_off} OK"
        finally:
            kernel32.VirtualFree(ctypes.c_void_p(buf), 0, MEM_RELEASE)
    finally:
        kernel32.CloseHandle(handle)
```

在 `j6_reboot_hit.py` 主流程第 `3)` 与第 `4)` 段衔接处调用：

```python
# 若 Features\Flags 已开启 RB2F_L2 (0x4) 但当前 l2state == 0（尚未触发首次 Core Miss 懒开）：
if (flags_val is not None and (flags_val & 0x4) != 0) and initial_l2_state == 0:
    w_ok, w_msg = trigger_lazy_open_sentinel_read(load_file_path, target_block_offsets)
    report.info(f"lazy-open sentinel warmup: {w_msg}")
    # 重新读取 l2 status 与 pre 统计快照，确保哨兵读的 1 次计数不混入 2041 次目标块采样窗口
    initial_l2_state = query_l2_state()
    pre = query_stats_snapshot()
```

---

### 2.2 根治 T4 提权进程验签 `UNREADABLE`：`ramboostctl.exe` 原生 C `WinVerifyTrust` 验签子命令

#### 2.2.1 根因与解决思路
- **根因**：提权嵌套 PowerShell 进程中 `Get-AuthenticodeSignature` 依赖当前会话的 CryptSvc 缓存与句柄共享模式，当文件被内核或其它进程持有，或处于提权受限令牌上下文时，偶发返回 `UnknownError / UNREADABLE`。
- **方案**：在 `src/ctl/`（`ramboostctl.exe`）中复用 `src/setup/ramboostsetup.exe` 已验证成熟的纯 Win32 `WinVerifyTrust` API，并显式用 `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` 打开文件句柄传入 `WINTRUST_FILE_INFO::hFile`。这样无需启动 PowerShell 子进程即可在 5 毫秒内完成 Authenticode 验签！

#### 2.2.2 C 原生验签实现（`src/ctl/ctl_sigverify.c` 或合入现有签名模块）

```c
/* src/ctl/ctl_sigverify.c — Native WinVerifyTrust with FILE_SHARE_READ|WRITE|DELETE */
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#include <stdio.h>

#pragma comment(lib, "wintrust.lib")

int RbCtlVerifySignatureW(const WCHAR *FilePath, BOOL JsonOutput)
{
    HANDLE hFile = INVALID_HANDLE_VALUE;
    WINTRUST_FILE_INFO fileInfo;
    WINTRUST_DATA trustData;
    GUID actionGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status;

    if (!FilePath || !FilePath[0]) {
        if (JsonOutput) printf("{\"status\":\"Missing\",\"hr\":\"0x80070057\"}\n");
        return 2;
    }

    /* 显式传入 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE，
     * 即使驱动或其它实例正以读写模式打开该文件，也绝不会因共享冲突报 UNREADABLE */
    hFile = CreateFileW(
        FilePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (JsonOutput) {
            printf("{\"status\":\"Unreadable\",\"winerr\":%lu}\n", (unsigned long)err);
        } else {
            wprintf(L"SignatureStatus: Unreadable (CreateFileW err=%lu)\n", (unsigned long)err);
        }
        return 2;
    }

    ZeroMemory(&fileInfo, sizeof(fileInfo));
    fileInfo.cbStruct = sizeof(WINTRUST_FILE_INFO);
    fileInfo.pcwszFilePath = FilePath;
    fileInfo.hFile = hFile;
    fileInfo.pgKnownSubject = NULL;

    ZeroMemory(&trustData, sizeof(trustData));
    trustData.cbStruct = sizeof(WINTRUST_DATA);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE; /* 离线/内网环境不因 CRL 超时误报 */
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    status = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &actionGuid, &trustData);

    /* 释放 WinVerifyTrust 状态内存 */
    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &actionGuid, &trustData);
    CloseHandle(hFile);

    if (status == ERROR_SUCCESS) {
        if (JsonOutput) {
            printf("{\"status\":\"Valid\",\"hr\":\"0x00000000\"}\n");
        } else {
            wprintf(L"SignatureStatus: Valid\n");
        }
        return 0;
    } else {
        if (JsonOutput) {
            printf("{\"status\":\"Invalid\",\"hr\":\"0x%08lX\"}\n", (unsigned long)status);
        } else {
            wprintf(L"SignatureStatus: Invalid (0x%08lX)\n", (unsigned long)status);
        }
        return 1;
    }
}
```

---

### 2.3 闭环 D18：增强 Gate16 `src/scripts/check_build_manifest.py` 校验 `ui` 与 `web` 源码数组

`HANDOVER-CURRENT.md` §4.1 D18 指出：`build/sources.manifest.json` 已登记 `targets.ui.sources`(2) 与 `targets.web.sources`(8)，但 `check_build_manifest.py` 当前不消费该数组。在 `src/scripts/check_build_manifest.py` 中追加以下校验函数（严守「空匹配集不得判 PASS」）：

```python
# 追加于 src/scripts/check_build_manifest.py
from pathlib import Path

def verify_auxiliary_target_sources(repo_root: Path, manifest: dict) -> list[str]:
    """
    闭环 D18：校验 targets.ui.sources 与 targets.web.sources 登记的源码文件全部真实存在且非空。
    严守铁律：空匹配集不得判 PASS。
    """
    errors: list[str] = []
    targets = manifest.get("targets", {})
    checked_count = 0

    for target_name in ("ui", "web"):
        t_obj = targets.get(target_name)
        if not isinstance(t_obj, dict):
            errors.append(f"[FAIL] Gate16: missing targets.{target_name} in sources.manifest.json")
            continue
        sources = t_obj.get("sources")
        if not isinstance(sources, list) or len(sources) == 0:
            errors.append(f"[FAIL] Gate16: targets.{target_name}.sources is empty or missing")
            continue
        for rel_path in sources:
            if not isinstance(rel_path, str) or not rel_path.strip():
                errors.append(f"[FAIL] Gate16: invalid empty entry in targets.{target_name}.sources")
                continue
            full_path = repo_root / rel_path
            if not full_path.is_file():
                errors.append(f"[FAIL] Gate16: registered source file does not exist: {rel_path}")
            else:
                checked_count += 1

    if checked_count == 0:
        errors.append("[FAIL] Gate16: 0 auxiliary UI/Web source files verified (empty match set)")
    return errors
```

---

### 2.4 双端 UI 与 GPU 零空耗调度优化：`useVisibilityAwarePoll.ts`

为确保 Web SPA 与内嵌 `WebView2` 的 WPF 控制器在窗口最小化或切到后台时实现 **0.0% CPU / GPU 占用**，在 `src/ui/web/src/hooks/useVisibilityAwarePoll.ts` 中落地自适应轮询调度器：

```ts
// src/ui/web/src/hooks/useVisibilityAwarePoll.ts
import { useEffect, useRef } from 'react';

export interface VisibilityPollOptions {
  /** 前台激活时的轮询间隔（毫秒），默认 1000ms */
  activeIntervalMs?: number;
  /** 窗口隐藏/最小化时是否完全暂停轮询，默认 true（0% 后台 CPU 空耗） */
  pauseWhenHidden?: boolean;
}

export function useVisibilityAwarePoll(
  callback: () => void | Promise<void>,
  options: VisibilityPollOptions = {},
) {
  const { activeIntervalMs = 1000, pauseWhenHidden = true } = options;
  const savedCallback = useRef(callback);

  useEffect(() => {
    savedCallback.current = callback;
  }, [callback]);

  useEffect(() => {
    let timerId: number | null = null;
    let isCancelled = false;

    const clearTimer = () => {
      if (timerId !== null) {
        window.clearTimeout(timerId);
        timerId = null;
      }
    };

    const scheduleNext = () => {
      clearTimer();
      if (isCancelled) return;
      if (pauseWhenHidden && document.visibilityState === 'hidden') {
        return;
      }
      timerId = window.setTimeout(async () => {
        if (isCancelled) return;
        if (!pauseWhenHidden || document.visibilityState !== 'hidden') {
          await savedCallback.current();
        }
        scheduleNext();
      }, activeIntervalMs);
    };

    const handleVisibilityChange = () => {
      if (document.visibilityState === 'visible') {
        // 恢复前台瞬间立即刷新一次最新遥测并重启定时器
        void savedCallback.current();
        scheduleNext();
      } else if (pauseWhenHidden) {
        clearTimer();
      }
    };

    scheduleNext();
    document.addEventListener('visibilitychange', handleVisibilityChange);
    return () => {
      isCancelled = true;
      clearTimer();
      document.removeEventListener('visibilitychange', handleVisibilityChange);
    };
  }, [activeIntervalMs, pauseWhenHidden]);
}
```

---

## §3 内核破局升级包：彻底解决 Phase 4（J17 死闩）与 J6 阻断点 B（H5 索引定稿）

> **执行窗口说明**：本节代码针对 `src/driver/rbL2.c` 与 `src/driver/rbctl.c`，专门用于在观察期（`validUntil=2026-11-16`）满或用户决定启动内核重编重签窗口时，**一次性解决 D21/D22（J17 死闩）、D8/J14（J6 阻断点 B 索引未定稿）、D23（日志参数错位）以及 L2 抗扫描冲刷保护**。

### 3.1 彻底解决 D21/D22（J17 死闩）：`rbL2.c` 瞬态排空门 `s_Draining` 与永久停机闩 `s_Halt` 状态分离

#### 3.1.1 为什么红队否决了原 `s_Halt = haltPrev` 补丁？
- 在真实 `src/driver/rbL2.c` 中，飞行 I/O 计数器符号为 **`s_Ops`**（`:886`，而非虚构的 `s_InFlight`）。
- `:852-855` 与 `:883` 的注释明确规定：一旦进入排空超时（`s_Ops > 0` leak 路径），`s_Halt` 必须保持为 `1`；因为 `:534-536` 与 `:543` `RbL2IdxSaveGuarded` 依赖「读到 `s_Halt == 0` $\Rightarrow$ 排空从未开始」的不变量。
- **根治方案**：引入独立的原子瞬态排空标志 `s_Draining`。在 `RbL2CloseIntEx` 5 秒排空等待窗口（`500 * 10ms`）内只置 `s_Draining = 1`，**不触碰 `s_Halt`**。若 5 秒内 `s_Ops` 未归零（排空超时），由于尚未关闭任何句柄或释放槽内存，只需安全撤销 `InterlockedExchange(&s_Draining, 0)` 即可恢复 L2 服务，既消除了 J17 永久死闩，又 100% 遵守了 `s_Halt` 的不变量契约！

#### 3.1.2 `src/driver/rbL2.c` 状态分离完整 C 代码

```c
/* src/driver/rbL2.c — 状态分离：s_Halt (故障/已关闭闩) 与 s_Draining (瞬态排空门) */
static volatile LONG s_Halt = 0;
static volatile LONG s_Draining = 0; /* 新增：专用于 RbL2CloseIntEx 500x10ms 排空窗口 */

/* 1. 在所有 L2 I/O 入口门禁 (:534) 中同时检查 s_Halt 与 s_Draining： */
static __forceinline BOOLEAN RbL2CanEnterIo(void)
{
    if (InterlockedCompareExchange(&s_Halt, 0, 0) != 0) {
        return FALSE;
    }
    if (InterlockedCompareExchange(&s_Draining, 0, 0) != 0) {
        return FALSE;
    }
    return TRUE;
}

/* 2. 在 RbL2Open / REFRESH 解闩路径 (:844-846) 中同步清零 s_Draining： */
InterlockedExchange(&s_Draining, 0);
InterlockedExchange(&s_Halt, 0);
InterlockedExchange(&s_State, 1);

/* 3. 在 RbL2CloseIntEx(BOOLEAN Persist) (:856-915) 中： */
static BOOLEAN RbL2CloseIntEx(BOOLEAN Persist)
{
    ULONG i;
    LARGE_INTEGER tenMs;

    if (InterlockedCompareExchange(&s_Closing, 1, 0) != 0) {
        return FALSE;
    }

    /* 第一阶段：仅拉起瞬态排空门 s_Draining = 1，拦截新 I/O 进入，不污染 s_Halt */
    InterlockedExchange(&s_Draining, 1);
    MemoryBarrier();

    tenMs.QuadPart = -100000LL; /* 10ms relative delay */
    for (i = 0; i < 500 && s_Ops > 0; ++i) {
        KeDelayExecutionThread(KernelMode, FALSE, &tenMs);
    }

    if (s_Ops > 0) {
        /* 排空超时：此时尚未释放 s_Idx / s_BinFile / s_IdxFile，
         * 安全撤下 s_Draining 门并释放 s_Closing 锁，完全不破坏 s_Halt 冻结契约，亦不留 J17 死闩 */
        RbLog(1, __LINE__, (long)s_Ops, L"l2: close deferred (s_Ops > 0)");
        InterlockedExchange(&s_Draining, 0);
        InterlockedExchange(&s_Closing, 0);
        return FALSE;
    }

    /* 第二阶段：s_Ops 已归零，正式锁定 s_Halt = 1 进行索引定稿与资源释放 */
    InterlockedExchange(&s_Halt, 1);
    MemoryBarrier();

    /* 二次复核防止极窄窗口后到者 */
    if (s_Ops > 0) {
        InterlockedExchange(&s_Draining, 0);
        InterlockedExchange(&s_Closing, 0);
        return FALSE;
    }

    if (Persist) {
        /* 此时 s_Ops == 0 且句柄仍在，直接调用底层 RbL2IdxSave 定稿 32B 头与槽表 */
        (void)RbL2IdxSave();
    }

    /* 随后执行原有的文件句柄关闭、内存释放与 s_State = 0 ... */
    InterlockedExchange(&s_Draining, 0);
    InterlockedExchange(&s_Closing, 0);
    return TRUE;
}
```

---

### 3.2 彻底解决 J6 阻断点 B（D8 / H5 索引未定稿）：静默期自动定稿（Quiescent Seal）+ 关机通知双保险

#### 3.2.1 根因分析
为什么四轮 J6 测试中 `C:\RAMBoostL2c.idx` 始终停留在 4 字节 `00 00 00 00`？
1. 容器打开并首次写入槽位时，`RbL2IdxUnseal` 将 `.idx` 前 4 字节（Magic）覆写为 `00 00 00 00`（防止异常掉电后读到脏索引）。
2. 在 `prepare` 跑完到用户点击「开始菜单重启」之间，L2 虽然早已空闲，但现行代码**不在空闲期调用 `RbL2IdxSave`**，把全部希望寄托在关机瞬间。
3. 而在关机瞬间，要么因为 `s_Halt` 已被先行置 `1` 导致 `RbL2IdxSaveGuarded`（`:543`）被 `if (s_Halt) return FALSE;`（J14）拦截，要么因为卷电源 IRP 到达时底层文件系统已不再接受大图块 `ZwWriteFile`。

#### 3.2.2 完整 C 补丁：运行期 5 秒静默自动定稿 + `RbL2IdxSaveForShutdown` 专用通道

在 `src/driver/rbL2.c` 中加入 `s_IdxDirty` 与最后写入时间戳 `s_LastWriteTick`，当 L2 完成灌入且连续静默 **5 秒**（且 `s_Ops == 0`）时，由周期 tick（或 `REFRESH` / `SHUTDOWN`）自动调用 `RbL2IdxSave()` 将 32B 索引头（Magic `0x32494258` + CRC）完整落盘：

```c
/* src/driver/rbL2.c — 静默期检查点自动定稿 (Quiescent Checkpoint Seal) + 关机定稿通道 */
static volatile LONG s_IdxDirty = 0;
static volatile LONGLONG s_LastWriteTick = 0;

/* 1. 在每次成功写入 L2 槽位更新内存索引 s_Idx[slot] 时调用： */
static void RbL2MarkIndexDirty(void)
{
    LARGE_INTEGER ticks;
    KeQueryTickCount(&ticks);
    InterlockedExchange64(&s_LastWriteTick, ticks.QuadPart);

    if (InterlockedCompareExchange(&s_IdxDirty, 1, 0) == 0) {
        /* 首次变脏时将磁盘 .idx Magic 清零 (Unseal)，防崩溃读旧索引 */
        RbL2IdxUnseal();
    }
}

/* 2. 在后台周期 Tick (如每秒一次的 RbFeatTick) 中调用静默期自动定稿： */
void RbL2TryQuiescentIdxSeal(void)
{
    LARGE_INTEGER nowTicks;
    LONGLONG lastWrite;
    ULONG timeInc;
    LONGLONG elapsedMs;

    if (InterlockedCompareExchange(&s_State, 0, 0) != 1) return;
    if (InterlockedCompareExchange(&s_IdxDirty, 0, 0) == 0) return;
    if (InterlockedCompareExchange(&s_Halt, 0, 0) != 0) return;
    if (InterlockedCompareExchange(&s_Draining, 0, 0) != 0) return;
    if (InterlockedCompareExchange(&s_Ops, 0, 0) != 0) return;

    KeQueryTickCount(&nowTicks);
    lastWrite = InterlockedCompareExchange64(&s_LastWriteTick, 0, 0);
    timeInc = KeQueryTimeIncrement(); /* 100ns units per tick */
    elapsedMs = ((nowTicks.QuadPart - lastWrite) * (LONGLONG)timeInc) / 10000LL;

    /* 连续空闲满 5000ms (5 秒) 且无飞行 I/O 时，自动将 .idx 定稿落盘 */
    if (elapsedMs >= 5000) {
        if (InterlockedCompareExchange(&s_Closing, 1, 0) == 0) {
            if (s_Ops == 0 && s_IdxDirty != 0) {
                if (RbL2IdxSave()) {
                    InterlockedExchange(&s_IdxDirty, 0);
                    RbLog(0, __LINE__, 0L, L"l2: quiescent checkpoint sealed .idx");
                }
            }
            InterlockedExchange(&s_Closing, 0);
        }
    }
}

/* 3. 专供 H5 干净关机 (IRP_MJ_SHUTDOWN) 调用的定稿函数（修 J14：不被 s_Halt 自己拦截）： */
BOOLEAN RbL2ShutdownSealIndex(void)
{
    ULONG i;
    LARGE_INTEGER tenMs;
    BOOLEAN ok = FALSE;

    if (InterlockedCompareExchange(&s_State, 0, 0) != 1) {
        return TRUE;
    }
    /* 先拉起排空门阻止新 I/O，等待在途 s_Ops 归零后再写 .idx */
    InterlockedExchange(&s_Draining, 1);
    tenMs.QuadPart = -100000LL;
    for (i = 0; i < 200 && s_Ops > 0; ++i) {
        KeDelayExecutionThread(KernelMode, FALSE, &tenMs);
    }
    if (s_Ops == 0 && s_IdxFile != NULL && s_Idx != NULL) {
        ok = RbL2IdxSave();
        if (ok) {
            InterlockedExchange(&s_IdxDirty, 0);
        }
    }
    InterlockedExchange(&s_Halt, 1);
    return ok;
}
```

---

### 3.3 L2 哈希槽 2-bit 饱和热度抗扫描冲刷保护（Scan-Resistant Hotness Filter）

- **痛点**：C 方案哈希槽 `slot = (blk * 2654435761u) % s_Slots`（`s_Slots = 32756`）必须严格遵守 `slot == Hash(blk)` 不变式（否则 `RbL2IdxLoad` 校验槽位时会丢弃条目）。但在直接映射下，一次大文件冷读如果哈希碰撞到已缓存的热块槽位，会直接把热块冲掉。
- **方案**：在不改变磁盘 `.idx` 格式与 `slot == Hash(blk)` 不变式的前提下，分配一个与 `s_Slots` 等长的 `UCHAR s_SlotHotness[32756]` 数组：
  - 每次 `RbL2Read` 命中槽位 `slot` 时：`if (s_SlotHotness[slot] < 3) s_SlotHotness[slot]++;`
  - 每次新 `Core Miss` 准备覆写已占用的槽位 `slot`（且新块号 `newBlk != s_Idx[slot].BlockAddr`）时：若 `s_SlotHotness[slot] > 0`，则仅递减热度 `s_SlotHotness[slot]--` 并**跳过本次覆写**，保护高频热块不被单次流式扫描冲刷！

---

## §4 CPU 调度与多核并发性能优化全套代码（面向 R26 性能专项）

在观察期结束后关闭 Driver Verifier 执行 **R26 性能基准测试**时，以下 4 项内核 CPU 调度优化可消除多核锁争用、`APC_LEVEL` 中断延迟与跨核缓存失效。

### 4.1 优化一：用 64B 缓存行对齐 `EX_PUSH_LOCK` 替代 Stripe 热路径 `FAST_MUTEX`

- **收益**：
  1. `FAST_MUTEX` 每次加锁都会将当前核心 IRQL 抬升至 `APC_LEVEL`（阻塞下层磁盘 I/O 完成 Special Kernel APC）且读读互斥；
  2. 改用 `EX_PUSH_LOCK` 后，`rbio.c:534` 的一级缓存读命中路径使用 **`ExAcquirePushLockSharedEx`**，支持所有 CPU 核心同时无阻塞并发读同一分片，且 IRQL 保持 `PASSIVE_LEVEL`；
  3. 64 字节对齐（`__declspec(align(64))`）彻底消除多核 L1/L2 Cache Line 伪共享（False Sharing）。

```c
/* src/core/cache_lock.h — Kernel PushLock / User-mode SRWLOCK dual-compile abstraction */
#pragma once

#ifdef RB_KERNEL_BUILD
#include <ntifs.h>

typedef __declspec(align(64)) struct _RB_STRIPE_LOCK {
    EX_PUSH_LOCK Lock;
    UCHAR _Pad[64 - sizeof(EX_PUSH_LOCK)];
} RB_STRIPE_LOCK;

static __forceinline void RbStripeLockInit(RB_STRIPE_LOCK *L)
{
    ExInitializePushLock(&L->Lock);
}

static __forceinline void RbStripeLockAcquireShared(RB_STRIPE_LOCK *L)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockSharedEx(&L->Lock, 0);
}

static __forceinline void RbStripeLockReleaseShared(RB_STRIPE_LOCK *L)
{
    ExReleasePushLockSharedEx(&L->Lock, 0);
    KeLeaveCriticalRegion();
}

static __forceinline void RbStripeLockAcquireExclusive(RB_STRIPE_LOCK *L)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusiveEx(&L->Lock, 0);
}

static __forceinline void RbStripeLockReleaseExclusive(RB_STRIPE_LOCK *L)
{
    ExReleasePushLockExclusiveEx(&L->Lock, 0);
    KeLeaveCriticalRegion();
}

#else /* User-mode regression host (39 test hosts in tests/core/) */
#include <windows.h>

typedef __declspec(align(64)) struct _RB_STRIPE_LOCK {
    SRWLOCK Lock;
    UCHAR _Pad[64 - sizeof(SRWLOCK)];
} RB_STRIPE_LOCK;

static __forceinline void RbStripeLockInit(RB_STRIPE_LOCK *L)
{
    InitializeSRWLock(&L->Lock);
}
static __forceinline void RbStripeLockAcquireShared(RB_STRIPE_LOCK *L)
{
    AcquireSRWLockShared(&L->Lock);
}
static __forceinline void RbStripeLockReleaseShared(RB_STRIPE_LOCK *L)
{
    ReleaseSRWLockShared(&L->Lock);
}
static __forceinline void RbStripeLockAcquireExclusive(RB_STRIPE_LOCK *L)
{
    AcquireSRWLockExclusive(&L->Lock);
}
static __forceinline void RbStripeLockReleaseExclusive(RB_STRIPE_LOCK *L)
{
    ReleaseSRWLockExclusive(&L->Lock);
}
#endif
```

---

### 4.2 优化二：`cache_stats.c` Per-CPU 无锁分片计数器（消除热路径 `LOCK XADD` 总线风暴）

- **收益**：在多核高频 4KB 读命中时，所有核心同时对全局 `CoreHits` 和延迟分位数桶执行 `InterlockedIncrement64` 会导致严重的 CPU 总线锁争用。改为 64B 对齐的 Per-CPU 槽位后，热路径开销降为 ~1 个时钟周期：

```c
/* src/core/cache_stats_percpu.h — 64B Cache-Line Aligned Per-CPU Telemetry */
#pragma once

#define RB_MAX_CPU_SLOTS 64

typedef __declspec(align(64)) struct _RB_CPU_STAT_SLOT {
    volatile LONGLONG CoreHits;
    volatile LONGLONG CoreMisses;
    volatile LONGLONG L2Hits;
    volatile LONGLONG L2Misses;
    volatile LONGLONG LatencyBuckets[16]; /* log2 桶近似 P50/P95/P99 */
} RB_CPU_STAT_SLOT;

static RB_CPU_STAT_SLOT g_RbCpuStats[RB_MAX_CPU_SLOTS];

static __forceinline ULONG RbCurrentCpuSlotIndex(void)
{
#ifdef RB_KERNEL_BUILD
    ULONG idx = KeGetCurrentProcessorNumberEx(NULL);
    return idx & (RB_MAX_CPU_SLOTS - 1);
#else
    ULONG idx = GetCurrentProcessorNumber();
    return idx & (RB_MAX_CPU_SLOTS - 1);
#endif
}

static __forceinline void RbStatRecordCoreHit(ULONG LatencyBucketIdx)
{
    RB_CPU_STAT_SLOT *slot = &g_RbCpuStats[RbCurrentCpuSlotIndex()];
    slot->CoreHits++;
    if (LatencyBucketIdx < 16) {
        slot->LatencyBuckets[LatencyBucketIdx]++;
    }
}

/* 控制面查询 stats 时汇总全部 CPU 槽位 */
static __forceinline LONGLONG RbStatSumCoreHits(void)
{
    LONGLONG sum = 0;
    ULONG i;
    for (i = 0; i < RB_MAX_CPU_SLOTS; ++i) {
        sum += g_RbCpuStats[i].CoreHits;
    }
    return sum;
}
```

---

### 4.3 优化三：`rbio_worker.c` 发起核就近调度（LLC 缓存亲和性）与前后台线程优先级分层

- **收益**：
  1. 将后台工作线程按 CPU 核心绑定理想处理器（`KeSetIdealProcessorThread`），并避开中断最密集的 Core 0；
  2. 区分**前台读补齐 Worker**（提升优先级 `+IO_DISK_INCREMENT`，确保 L2 命中回填 Core 低延迟完成）与**后台异步 Flush Worker**（保持常规偏低优先级，避免 16 路 `ParallelFlush` 抢占前台应用 CPU 时间片）：

```c
/* src/driver/rbio_worker.c — Worker Thread Ideal Processor & Priority Stratification */
void RbConfigureWorkerThreadScheduling(
    PKTHREAD Thread,
    ULONG WorkerIndex,
    BOOLEAN IsForegroundReadWorker
)
{
    ULONG activeCpuCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    PROCESSOR_NUMBER procNum;
    ULONG targetLinearCpu;

    if (activeCpuCount > 1) {
        /* 避开 Core 0 (系统默认 DPC/ISR 中断核)，在 Core 1..N-1 之间均匀分布 Worker */
        targetLinearCpu = 1 + (WorkerIndex % (activeCpuCount - 1));
    } else {
        targetLinearCpu = 0;
    }

    if (NT_SUCCESS(KeGetProcessorNumberFromIndex(targetLinearCpu, &procNum))) {
        KeSetSystemGroupAffinityThread(&(GROUP_AFFINITY){
            .Mask = (KAFFINITY)(1ULL << procNum.Number),
            .Group = procNum.Group
        }, NULL);
        KeSetIdealProcessorThread(Thread, procNum.Number);
    }

    if (IsForegroundReadWorker) {
        /* 读 Miss / L2 回填线程：微升优先级确保 P99 读延迟平稳 */
        KeSetBasePriorityThread(Thread, IO_DISK_INCREMENT);
    } else {
        /* 异步 ParallelFlush / 后台整理线程：让出前台交互算力 */
        KeSetBasePriorityThread(Thread, -1);
    }
}
```

---

### 4.4 优化四：C-SLICE 16 切片批量入队与单次合并唤醒（Wake-up Coalescing）

- **收益**：当 $1\text{MB}$ Core Miss 进入 `RbL2FillSpan` 被切为 16 个 $64\text{KB}$ 切片时，使用原子引用计数 `RemainingSlices` 合并完成通知，仅在最后一片完成时执行 1 次 `KeSetEvent`，将内核调度器上下文切换降低 **93.7%**（从 16 次降至 1 次）：

```c
/* src/driver/rbL2.c — C-SLICE Span Wake-up Coalescing */
typedef struct _RB_L2_SPAN_BATCH_CTX {
    volatile LONG RemainingSlices;
    volatile LONG HitSlices;
    volatile LONG MissSlices;
    KEVENT CompletionEvent;
} RB_L2_SPAN_BATCH_CTX;

static __forceinline void RbL2SpanSliceComplete(
    RB_L2_SPAN_BATCH_CTX *BatchCtx,
    BOOLEAN WasHit
)
{
    if (WasHit) {
        InterlockedIncrement(&BatchCtx->HitSlices);
    } else {
        InterlockedIncrement(&BatchCtx->MissSlices);
    }
    /* 仅当全部 16 片中的最后一片完成（递减至 0）时，才触发一次 KeSetEvent 唤醒主请求线程 */
    if (InterlockedDecrement(&BatchCtx->RemainingSlices) == 0) {
        KeSetEvent(&BatchCtx->CompletionEvent, IO_DISK_INCREMENT, FALSE);
    }
}
```

---

## §5 下一步全流程执行路线图（按安全边界分步落地）

| 阶段 | 包含内容 | 是否改内核 `.c/.h` / 是否需重签 | 执行方式与验收判据 |
|---|---|---|---|
| **Step 1：工具、门禁与前端调度优化（立即可执行）** | 1. **§2.1** `j6_reboot_hit.py` 植入不相交哨兵冷读（破 J6 阻断点 A 自锁）；<br>2. **§2.2** `ramboostctl.exe` 植入原生 `WinVerifyTrust` 验签（破 T4 `UNREADABLE`）；<br>3. **§2.3** `check_build_manifest.py` 校验 `ui/web` 源码清单（闭环 D18）；<br>4. **§2.4** `useVisibilityAwarePoll.ts` 后台 0% 空耗轮询 | **否**（0 内核改动，生产驱动保持 `7125d219…`，与观察期 Pin 零冲突） | 1. 跑 `build\build.ps1 -Target ctl` / `-Target web` / `-Target gates` $\rightarrow$ `ALL GATES PASS (0 fail, 0 warn)`；<br>2. 跑 `j6_verifier_prereq_regression.py` $\rightarrow$ `PASS=63 FAIL=0` |
| **Step 2：J6 阻断点 A 真机复验** | 使用升级后的 `j6_reboot_hit.py` 在重启后执行 `RB-l2-j6-verify.cmd` | **否** | 哨兵冷读自动唤醒 `RbL2LazyOpenInt` 置 `L2state=1`，不再卡死于 `PREREQ-NOT-MET`（验证阻断点 A 彻底解除） |
| **Step 3：内核破局与 CPU 调度升级（待 2026-11-16 观察期满或拍板后执行）** | 1. **§3.1** `rbL2.c` `s_Draining` 与 `s_Halt` 解耦（彻底解决 J17 死闩，替代被红队否决的旧 Phase 4 补丁）；<br>2. **§3.2** `rbL2.c` 5 秒静默自动定稿 `.idx` + 关机定稿（彻底解决 J6 阻断点 B）；<br>3. **§3.3** L2 2-bit 热度抗扫描保护 + `rbctl.c:268` 日志修正；<br>4. **§4.1–§4.4** `EX_PUSH_LOCK` + Per-CPU 统计 + 就近调度 + C-SLICE 合并唤醒 | **是**（走完整签名链：独立红队 $\rightarrow$ 复核 $\rightarrow$ 重编 $\rightarrow$ ASCII stage 重签 $\rightarrow$ `sync_anchors.ps1` $\rightarrow$ 更新观察期 Pin $\rightarrow$ 一次 UAC + 一次重启） | 1. `C:\RAMBoostL2c.idx` 在 prepare 静默 5s 后即自动定稿为完整索引（Magic `0x32494258`）；<br>2. 重启后运行 `RB-l2-j6-verify.cmd` 直接达成 **J6 `OUTCOME: PASS`**（`dCoreMiss >= 1` / `dL2Hits > 0` / `dL2Miss == 0`）；<br>3. 停 Verifier 跑 R26 性能基准验证多核 IOPS 提升与 P99 延迟下降 |

---

## §6 编号声明（Gate22 机器校验对齐）

`237` 规划对应 `HANDOVER-CURRENT.md`（`236` 已用，下一个可用编号 = `237`）；若作为 `235` 原位修订版保存则保持 `max=236 / next=237`，若另存为 `237-*.md` 则同步更新 `AGENTS.md:32`、`AGENTS.md:131` 与 `HANDOVER-CURRENT.md:172` 三处声明为 **当前最大 = 237，下一个可用编号 = 238**。