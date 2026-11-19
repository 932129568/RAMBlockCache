# 源码侧零风险清理（161 零风险清单第 1/2/3/5/6/7 项）

> 依据：`deliverables/161-source-architecture-plan-2026-11-12.md` 附「零风险清单」（第 4、8 项已在 `162-gate-hardening-2026-11-12.md` 完成）
> 执行日期：2026-11-12
> 执行方式：只改源码 + 只构建。**未部署、未重启、未 attach 卷、未跑真机探针、未 `git add/commit`**。
> 改动文件（7 个源码 + 3 个 deliverables 记录）：授权集 6 个 —— `src/driver/rbio_worker.c`、`src/driver/rbL2.c`、`src/driver/rbrebuild.c`、`src/core/cache.c`、`src/core/cache.h`、`src/core/read_plan.h`；**授权后追加** `src/driver/rbattach.c`（仅 §7 的 3 行移位）。
> 未改但被工具改写的文件：`deliverables/RB-sign-deploy-150.ps1`（pin 值）、`deliverables/.deploy-pin-state.json`（fingerprint），由 `update-deploy-pin.ps1` 生成，见 §3。
> 行号为**改后**工作树实测值（改前行号按 161 原文对照）。

## 结论摘要

| 项 | 状态 | 净行数 |
|---|---|---|
| 1 删不可达判空 | ✅ 已做，前提复核成立 | 与第 4 项同在 rbio_worker.c，合计 +58 / −19 |
| 2 `RbDiagRecord` 上移 | ✅ 已做（首轮因文件越权挂起，授权后执行，见 §7） | +11 / −3 |
| 3 三处 `(void)` | ✅ 已做（1 补 `(void)`，2 处改为显式接收 + 记录） | rbL2 +4/−1，rbrebuild +17/−2 |
| 4 派发 `default` | ✅ 已做，且发现审计漏了**第三处**派发点 | 与第 1 项同在 rbio_worker.c，合计 +58 / −19（见 §6） |
| 5 编译期断言 | ✅ 已做，但**不能用 `_Static_assert`**（实测 tests TU 编译失败），改用负数组长度 | +30 / 0 |
| 6 删死函数 | ✅ 已做，grep 复核零调用方 | cache.c +3/−2、cache.h +0/−1 |

构建：driver OK（185856 / `a49fad959c781cd999b0eecf19113aea28f378ced78331452d40bde118a5c023`，0 warning，两轮同 sha）、core `RUN_FAIL=0`、gates `ALL GATES PASS (0 fail, 0 warn)`、pin 已重同步（§7.4 为最终值）。

> 本报告有两轮：第一轮 6 个文件（§1-§5），第二轮补做 `rbattach.c`（§7）并留痕三条审计错漏（§8）。**最终产物与 pin 以 §7.3 / §7.4 为准。**

---

## §1 六条逐条

### 1.1（清单第 1 项）删不可达死码 —— `rbio_worker.c`

**核实的前提**：函数入口 `:413-417` 已把 `RbMapIrpBuffer` 的 NULL 结果筛掉并 `goto done`：

```c
    PVOID tgt = RbMapIrpBuffer(irp);          /* :405 唯一求值点 */
    ...
    if (!tgt) {                               /* :413 */
        RbNoteBypass(ext, RB_BYPASS_RESOURCE);
        s = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
```

而 `tgt` 在 `:430` 之前**没有任何再赋值**（`:483` 的 `tgt = (PUCHAR)tgt + chunk` 在 8 行之后，且不在该分支内）。因此 `:430` 的 `if (!tgt)` 恒假 —— **前提成立**。

**改前 → 改后**（`:428-441`，diff）：

```diff
     if (!ext->IsAttached || ext->Stream == NULL || ext->Removed ||
         g_Rb.Unloading || g_Rb.Recreating) {
-        if (!tgt) {
-            RbNoteBypass(ext, RB_BYPASS_RESOURCE);
-            s = STATUS_INSUFFICIENT_RESOURCES;
-        } else {
-            if (ext->Stream && !g_Rb.Unloading &&
-                KeGetCurrentIrql() == PASSIVE_LEVEL &&
-                rb_stream_has_dirty_range(ext->Stream, cur, remaining))
-                rb_stream_flush(ext->Stream, 1);
-            RbReadDirectFallback(ext, tgt, len, (LONGLONG)cur, &s, &info);
-        }
+        /* [165 / 161 §2.5] 此处原有 `if (!tgt)` 二次判空——函数入口已把 NULL 筛掉
+         * 并 goto done，该分支恒假（复制粘贴残留）。删除后直读路径唯一，不再
+         * 误导读者以为 tgt 可能在此为 NULL。 */
+        if (ext->Stream && !g_Rb.Unloading &&
+            KeGetCurrentIrql() == PASSIVE_LEVEL &&
+            rb_stream_has_dirty_range(ext->Stream, cur, remaining))
+            rb_stream_flush(ext->Stream, 1);
+        RbReadDirectFallback(ext, tgt, len, (LONGLONG)cur, &s, &info);
         goto done;
     }
```

行为等价性：被删分支的错误码（`STATUS_INSUFFICIENT_RESOURCES`）与旁路记账（`RB_BYPASS_RESOURCE`）与入口 `:413-417` 完全相同，只是执行点提前了 17 行。**零语义变化**。

### 1.2（清单第 2 项）`RbDiagRecord` 上移 —— 已执行（首轮挂起，授权后补做）

落点在 `src/driver/rbattach.c`（不是任务清单写的 `rbio_worker.c`），首轮因不在授权改动集而挂起；team-lead 授权后执行。**核实结论：审计条目成立**。完整核实过程、消费方排查、diff、重建与 pin 结果见 **§7**；首轮的挂起理由与待授权补丁保留在 §4.1 作为过程留痕。

### 1.3（清单第 3 项）`(void)` 误用三处

#### (a) `rbL2.c:251`（改后 `:254`）—— 裸调用补 `(void)`

**核实**：`ZwSetInformationFile` 在该处连 `(void)` 都没有，同函数内 `:246` 的 `ZwWriteFile` 有 `(void)`。截断失败的后果按 161 §2.4 是 fail-safe：载入侧按 `h.Count` 取条目、CRC 只覆盖 entries，残尾不入索引。

```diff
         fe.EndOfFile.QuadPart = (LONGLONG)(sizeof(h) + (ULONG64)n * sizeof(RB2_L2IDX_E));
-        ZwSetInformationFile(s_IdxFile, &ib, &fe, sizeof(fe), FileEndOfFileInformation);
+        /* [165 / 161 §2.4] 显式 (void) 固化"有意丢弃状态"：截断失败只会留下旧尾部，
+         * 载入侧按 h.Count 取条目、CRC 覆盖 entries，残尾不入索引（fail-safe）。
+         * 与 :246 的 (void)ZwWriteFile 同一纪律（rbio_worker.c 立规：显式转型固化意图）。 */
+        (void)ZwSetInformationFile(s_IdxFile, &ib, &fe, sizeof(fe), FileEndOfFileInformation);
```

按任务要求**只补 `(void)`，未开始核查该返回值**（161 §7 明示 `rbL2.c:417` 的 `(void)ZwReadFile` 上下文未读，本次同样未动 `:417`）。

#### (b) `rbrebuild.c:612`（改后 `:615-619`）—— 返回值是**卷数**不是状态

**核实**：`RbRebuildReattachAll` 返回 `ULONG restored`（`rbrebuild.c:277`），真实失败明细经 `*pFailed` 出参。此处若只"去掉 `(void)`"会退化成**裸调用**——与 (a) 刚确立的"显式固化意图"纪律直接冲突，而且 `/W3` 下对"丢弃返回值"不报任何告警，等于什么都没解决。因此按任务允许的第二方案**显式接收并记录**：

```diff
     RbApplyEngineConfig(&g_Rb.Config);
-    (void)RbRebuildReattachAll(vols, volCount, &failed);
+    /* [165 / 161 §2.4] 原为 `(void)RbRebuildReattachAll(...)`——返回值是**成功重挂
+     * 卷数**（不是状态），真实失败明细在 failed 出参（逐卷盘符已由被调方记 ERROR）。
+     * 去掉误导性的 (void)，以返回值 + failed 共同判定是否留一条汇总 WARN，
+     * 返回值语义与 :587 主路径一致，不改任何恢复顺序。 */
+    failed = 0;
+    if (RbRebuildReattachAll(vols, volCount, &failed) != volCount && failed)
+        RbLog(1, __LINE__, (LONG)failed,
+              L"rebuild: rollback reattach incomplete (per-letter ERROR logged above)");
     RbL2LazyOpen();
```

`failed = 0` 是显式清零（被调方 `:281` 本来也会 `*pFailed = 0`，这里是防"两个条件共用未初始化值"的读隐患，不依赖被调方实现）。条件用 `!= volCount && failed`：`WasAttached==FALSE` 的卷本就不参与重挂，只看 `failed` 才可能误报。

#### (c) `rbrebuild.c:631`（改后 `:643-646`）—— 状态就在返回值里

**核实**：`RbReattachVolumeForRebuild` 返回 `NTSTATUS`（`rbattach.c:1219`、声明 `rb_common.h:558`），**没有**状态出参。这里的 `(void)` 是真误用。

```diff
         for (k = 0; k < volCount; k++) {
+            NTSTATUS rst;
             if (!vols[k].Detached) continue;
-            (void)RbReattachVolumeForRebuild(vols[k].Letter);
+            /* …(注释略)… */
+            rst = RbReattachVolumeForRebuild(vols[k].Letter);
+            if (!NT_SUCCESS(rst))
+                RbLog(2, __LINE__, (LONG)vols[k].Letter,
+                      L"rebuild: rollback reattach FAILED (left passthrough)");
             if (vols[k].FlagValid)
```

**行为影响**：恢复顺序、`FlagValid` 复原条件、函数返回码全部不变，只在失败时多一条 ERROR 日志（原状态下这一支的失败是**完全静默**的——`out_rollback` 循环里没有别的日志点）。选"记录"而不是"断言"：内核驱动没有可用的 `ASSERT` 生产语义，且该分支设计意图就是"尽力恢复、不回滚引擎"，失败可接受但必须留痕。

### 1.4（清单第 5 项）派发加 `default` —— 且发现第三处派发点

**核实**：审计说"两处"（`rbio_worker.c:932-940`），实际有**三处**同一模式的 `if/else if/else`，兜底系统队列路径（`RbRwWorker`）同样把未知 Major 交给 `RbWriteWorker`：

| # | 位置（改前） | 场景 |
|---|---|---|
| 1 | `:932-934` | worker 池：自己队列空 → 窃取到节点后派发 |
| 2 | `:938-940` | worker 池：自己队列取到节点 |
| 3 | `:1255-1257` | 兜底 `RbRwWorker`（池未启动，系统 DelayedWorkQueue）——**审计未列** |

**Major 取值面核实**（决定"拒绝"是否安全）：`ctx->Major` 只在三处赋值——`rbio.c:555`（`sp->MajorFunction`，该函数只挂在 `IRP_MJ_READ`/`IRP_MJ_WRITE` 两个分发表项）、`rbio.c:343`（显式 `IRP_MJ_WRITE`）、`rbio.c:608`（显式 `IRP_MJ_FLUSH_BUFFERS`）。因此现状下 `READ/WRITE/FLUSH` 之外**不存在**合法值，`default` 分支在今天是死路 —— 加它不会改变任何现有 IO 的行为，这正是"零风险"的依据。

**改后**（新增，`:906-950` 一带）：

```c
static VOID RbRejectUnknownMajor(PRB_IOCTX ctx)
{
    PRB_DEV_EXT ext = ctx->Ext;
    PIRP irp = ctx->Irp;

    RbLog(2, __LINE__, (LONG)(ULONG)ctx->Major,
          L"unknown IRP major function reached io worker: rejected (STATUS_INVALID_DEVICE_REQUEST)");
    RbNoteBypass(ext, RB_BYPASS_RESOURCE);
    irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    RbCtxFree(ctx);          /* [FIX M10] 先还 ctx 后减引用 */
    RbIoCompleteMark();      /* [96 §2] 拒绝出口同样计入 completed */
    RbDerefExt(ext);
}

static VOID RbDispatchCtx(PRB_IOCTX ctx, PRB_IOWORKER w)
{
    switch ((ULONG)ctx->Major) {
    case IRP_MJ_READ:           RbReadWorker(ctx, w);      break;
    case IRP_MJ_FLUSH_BUFFERS:  RbFlushIrpWorker(ctx);     break;
    case IRP_MJ_WRITE:          RbWriteWorker(ctx, w);     break;
    default:                    RbRejectUnknownMajor(ctx); break;
    }
}
```

三个调用点统一为 `RbDispatchCtx(ctx, w)` / `RbDispatchCtx(ctx, NULL)`。

**四个取舍及其理由**（任务要求"跟随最接近的既有失败语义、不要发明"）：

1. **跟随哪一种**：读 worker 的资源失败出口（`RbNoteBypass(RB_BYPASS_RESOURCE)` + 置失败状态 + 单一完成点）。它是三个 worker 里唯一"没有下发任何盘 IO 就失败"的出口，语义最接近。
2. **错误码换成 `STATUS_INVALID_DEVICE_REQUEST`**：读 worker 那支用 `STATUS_INSUFFICIENT_RESOURCES` 是因为真的没拿到缓冲；未知 Major 不是资源问题，用 INVALID_DEVICE_REQUEST 才能让上层看到真实原因（也是 161 §2.2 原文的建议）。
3. **刻意不调 `RbNoteIoResult`**——这是本次唯一一个"看着该调、实际不能调"的点：`RbNoteIoResult`（`:129-140`）对失败调用会 `InterlockedIncrement(&ext->IoFailRun)`，累计 32 次触发 `RbSetPanic(L"device I/O failing persistently")` ⇒ **全局熔断透传**。那是"盘在坏"的记账。未知 Major 是上层/驱动的编程错误，与盘健康无关，记进去等于让一个 bug 把整个卷降级成裸盘。忽略该调用是有意的（注释里写明）。
4. **`IO_NO_INCREMENT`**：本次未做任何下层 IO（与 FLUSH 失败出口 `:1240` 同款），不给上层一个不存在的优先级提升。`RbIoCompleteMark` 必须调：issued/completed 配对是卸载排空的判据（`rbio_worker.c` 文件头 96 §2 的纪律），漏记会让一次拒绝把卸载永久卡住。

**未新增 bypass 原因码**：扩 `LastBypassReason` 的取值域会牵动诊断展示口径（`rb_common.h:88-100` 是连号常量），沿用 `RB_BYPASS_RESOURCE` 的"本次 IO 未经缓存"既有语义。

### 1.5（清单第 6 项）编译期断言 —— 实测 `_Static_assert` 不可用

**第一次实现用 `_Static_assert`（C11，驱动 manifest 是 `/std:c11`），驱动构建通过，但 `build.ps1 -Target core` 直接红**：

```
..\..\src\core\read_plan.h(23): error C2143: 语法错误: 缺少")"(在"("的前面)
..\..\src\core\read_plan.h(23): error C2091: 函数返回函数
[X] release build failed: test_sparse_miss
```

原因：`tests/core/*.c` 的编译 flag 是 `["/nologo","/O2","/W4","/utf-8"]`（`build/sources.manifest.json` `targets.core.compile`），**没有 `/std:c11`**，MSVC 旧 C 模式不认 `_Static_assert`。而 `read_plan.h` 被 `test_sparse_miss.c`、`bench_read_plan.c` 直接包含。仓库里 `_Static_assert` 也只在 `tests/core/offchk.c:4` 的注释中出现过，从未被真正编译过——这是个"看着有先例、实际不可用"的写法。

**改用负数组长度**（C89 起可靠、驱动与 tests 同构、`/W4` 下无告警）：

```c
#define RB_READ_PLAN_CT_ASSERT_(cond, tag) typedef char rb_read_plan_ct_##tag[(cond) ? 1 : -1]
#define RB_READ_PLAN_CT_ASSERT(cond, tag)  RB_READ_PLAN_CT_ASSERT_(cond, tag)

RB_READ_PLAN_CT_ASSERT(RB_READ_PLAN_MAX_BYTES == 131072ULL, window_128k);
#if defined(RB_MAX_RANGES)
RB_READ_PLAN_CT_ASSERT(RB_READ_PLAN_MAX_SUBS <= (unsigned long long)RB_MAX_RANGES,
                       subs_fit_rb_max_ranges);
#endif
```

配套改动：把 `read_plan.c:3-5` 的三个私有宏**原样**提升为头文件的权威定义（`read_plan.c` 里的同名定义保持一字不改，同值重定义合法、驱动与 tests 两侧都编译通过）。这样断言才不是"写死 32 对 32"的空转——改 `read_plan.c` 的窗口或改 `rb_common.h` 的 `RB_MAX_RANGES`，两个方向都会失败。

**负向验证**（临时目录，已删除）：

```
# RB_MAX_RANGES=32 → ok
--- ok.c (RB_MAX_RANGES=32) ---   cl /nologo /c /W4 /utf-8 ...
ok.c
exit=0
# RB_MAX_RANGES=16 → 命中
--- bad.c (RB_MAX_RANGES=16) ---
read_plan.h(39): error C2118: 负下标
read_plan.h(39): error C2148: 数组的总大小不得超过 0x7fffffff 字节
exit=2
```

注意 `#if defined(RB_MAX_RANGES)` 这层条件：驱动 TU 的包含顺序是 `rb_common.h`（`:8`）→ `read_plan.h`（`:11`），断言在驱动侧**必然生效**；tests 不定义 `RB_MAX_RANGES`，跳过——但 tests 也不消费 `miss[RB_MAX_RANGES]`，跳过不影响覆盖。这是"条件生效"，不是"静默失效"：`read_plan.h` 与 `rb_common.h` 若调换包含顺序，断言会静默消失，这一点写在此处备查。

### 1.6（清单第 7 项）删死函数 `rb_readahead_note_hit`

**核实（全库 grep，排除 `node_modules`/`.git`/`archive`/构建产物）**：

| 命中 | 性质 |
|---|---|
| `src/core/cache.c:337-338` | 定义（本次删除） |
| `src/core/cache.h:306` | 声明（本次删除） |
| `deliverables/161-…md`、`deliverables/backup-driver-src-20261111/**` | 文档 / 备份快照 |
| `.mimosa/hook-state/**.source` | 编辑器 hook 的基线快照 |
| `*.obj` / `*.pdb` | 二进制产物里的符号 |

`src/driver/`、`src/ctl/`、`tests/`、`bench/`、`src/ui/` 全部**零命中** ⇒ 零调用方成立，删除安全。

```diff
-void rb_readahead_note_hit(rb_engine *eng, rb_u64 bytes)
-{ if(eng&&bytes)RB_AADD64(&eng->readahead_hit_bytes,bytes); }
+/* [165 / 161 §1.2] 原 rb_readahead_note_hit 死函数已删：全库零调用方
+ * （src/ tests/ bench/ 均无），使 eng->readahead_hit_bytes 永久为 0。
+ * 统计字段与 RB_STATS2 的 readahead_hit_bytes 保留（ABI，见 rb_common2.h）。 */
```

**配套计数器（按要求只说明、不删）**：删掉后仍有完整的一条"永远输出 0"的链路——

```
eng->readahead_hit_bytes      cache_internal.h:497   （再无写者）
  → out->readahead_hit_bytes  cache_stats.c:12      （恒读 0）
  → out->ReadaheadHitBytes    rbctl.c:128            （RB_STATS2 字段，ABI）
```

三个字段**全部保留**：`cache_internal.h:497` 与 `rb_common2.h` 的 `RB_STATS2` 布局一旦改动，协议三副本门禁必红（161 §5.1 的同一个 ABI 约束）。后果是"预取命中字节"KPI 继续恒为 0——这是**已知且刻意保留**的状态，与 161 §1.2 判断一致：要么接线要么删函数，本次选后者，字段留给将来的接线。

---

## §2 构建结果

> **注意**：本节是第一批（6 个文件）的构建快照。补做 §7 的 `rbattach.c` 之后驱动 sha 变为 `a49fad959c781cd999b0eecf19113aea28f378ced78331452d40bde118a5c023`，**最终值以 §7.3 为准**；core / gates 的结论两轮一致。

命令一律 `& "C:\Program Files\PowerShell\7\pwsh.exe" -NoProfile -File build\build.ps1 -Target <t>`，工作目录为项目根。

### 2.1 `-Target driver`

```
=== driver: RAMBlockCache.sys ===
  sources: 30 (manifest)  includes: src/core, src/common
  [ok] rbdriver.c … [ok] read_plan.c        (30/30 全 ok)
  BUILD OK
  RAMBlockCache.sys | size=185856 | sha256=501f0d5dea30edd2cb016f8e7eb143569e63f1bbc2ea7cc60763d23eaeecacd4
  NOTE: UNSIGNED. Sign before deploy; …
BUILD TARGET 'driver' OK
```

- **size = 185856**，**sha256 = `501f0d5dea30edd2cb016f8e7eb143569e63f1bbc2ea7cc60763d23eaeecacd4`**
- 确定性复核：同一源码连续两次 `-Target driver` 得到**同一个 sha256**（`/Brepro` + 构建前删 PDB 生效，与 `update-deploy-pin.ps1` 头注释所述一致）。
- **warning = 0、error = 0**（整份构建日志逐行 grep `warning|error` 零命中）。驱动 manifest 的告警等级是 `/W3`（不是 `/W4`——`/W4` 是 `targets.core.compile`），本项按实际配置记录，见 §5.2。

### 2.2 `-Target core`

```
  compiled=64 skipped=1
  RUN_FAIL=0
BUILD TARGET 'core' OK        (exit=0)
```

- `compiled=64 skipped=1`（Release + ASan 两种配置各编一次同一批宿主，故 64 是"配置 × 宿主"数，不是宿主数；`skipped=1` 是 `test_geom`，`skip` 原因见 161 §3.4 的 I52）。
- **`RUN_FAIL=0`**。
- 期间有一次中间态失败（`_Static_assert` → C2143，见 §1.5），已修复并重跑全绿。

### 2.3 `-Target gates`

```
== check_ps_syntax == files=57 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=17 …
== check_signature ==
  sys: …\dist\RAMBlockCache.sys
  signature status: Valid
  sha256: 75d02eba4ba4d67e9e85dc5559e944d3a9f4d5ae7365a897760e3c4ddb702242
  anchor deploy.ps1 / verify_deploy_signature.ps1 / dist sidecar: 三者同上
result: PASS (0 fail, 0 warn)
  ALL GATES PASS
BUILD TARGET 'gates' OK       (exit=0)
```

**0 fail、0 warn**。注意 `check_signature` 校验的是 `dist\RAMBlockCache.sys`（**已签名的旧件**，sha `75d02e…`），与本次新构建的 `src\driver\x64\Release\RAMBlockCache.sys`（`501f0d…`，UNSIGNED）是两份不同文件——本次不部署，dist 未动，门禁自然仍绿。这与 161 §5.1 所述"三锚点必须同批更新"一致：**签名的下一步是 `RB-sign-deploy-150.cmd` + `deploy.ps1` 的三锚点同步，本次刻意没做**。

---

## §3 pin 同步结果

命令：`& $pwsh -File deliverables\update-deploy-pin.ps1 -AcceptSourceChange`（exit=0）。之所以必须带 `-AcceptSourceChange`：源码指纹确实变了（本次 6 个文件 + 上一轮门禁加固留下的 2 个构建输入）。

### 3.1 前后值

| 项 | 改前 | 改后 |
|---|---|---|
| `RB-sign-deploy-150.ps1:27` `-ExpectSha` | `048478c23760a41c753f32bb3dca7912872e0f5491c93dea3b2573c0a25c035d` | **`501f0d5dea30edd2cb016f8e7eb143569e63f1bbc2ea7cc60763d23eaeecacd4`** |
| `.deploy-pin-state.json` `artifactSha256` | `048478c23760a41c753f32bb3dca7912872e0f5491c93dea3b2573c0a25c035d` | **`501f0d5dea30edd2cb016f8e7eb143569e63f1bbc2ea7cc60763d23eaeecacd4`** |
| `artifactSize` | `185344` | **`185856`**（+512B） |
| `srcFingerprint` | `4f55c927858cf7f34b10db7c3478dc206e0dac58999818971cfd419945e9cdf3` | **`9aa0f6da577e26efcb447b0d9fc797612c23798ff8b739df1f27a738c2a9e3d9`** |
| `fileCount` | `38` | `38`（无文件增删） |
| `updatedUtc` | `2026-11-11T17:36:21Z` | `2026-11-11T22:36:59Z` |

### 3.2 `-AcceptSourceChange` 打印的 M/A/D 清单（原样）

```
[!] SOURCES CHANGED since the last pin registration
    M  build/build.ps1
    M  build/sources.manifest.json
    M  src/core/cache.c
    M  src/core/cache.h
    M  src/core/read_plan.h
    M  src/driver/rbio_worker.c
    M  src/driver/rbL2.c
    M  src/driver/rbrebuild.c
[!] -AcceptSourceChange given - blessing the new source set
[ok] pin updated in RB-sign-deploy-150.ps1
     old: 048478c23760a41c753f32bb3dca7912872e0f5491c93dea3b2573c0a25c035d
     new: 501f0d5dea30edd2cb016f8e7eb143569e63f1bbc2ea7cc60763d23eaeecacd4
[ok] state written: …\deliverables\.deploy-pin-state.json
```

只有 **M**，无 **A**/**D**（`fileCount` 38 → 38 也印证）。

**清单归因（重要）**：8 条 M 里只有 6 条是本次改动；`build/build.ps1` 与 `build/sources.manifest.json` **不是本次改的**（本次未触碰 `build/`），它们是上一轮 `162-gate-hardening` 落地后一直没有重跑 pin 造成的未登记变更。也就是说，**在本次之前 pin 与源码已经漂移**（旧 pin `048478c2…` 对应的源指纹 `4f55c9…` 里没有 162 的门禁改动），本次一次性把两轮变更一起批准。

---

## §4 我判断不该做（或做不了）的项目

### 4.1 清单第 2 项 `RbDiagRecord` 上移 —— 首轮挂起的过程留痕（**后续已授权执行，见 §7**）

> 本节保留首轮的原始判断，作为"为什么没有当场改"的记录。结论在 §7：授权后已按这里的补丁执行。

- **审计原文（161 §2.1）与任务清单不一致**：任务清单说"`rbio_worker.c` 里 `RbDiagRecord(...)` 的调用（约 `:868-870`）"，但全库 grep 显示 `rbio_worker.c` **没有任何 `RbDiagRecord` 调用**；161 §2.1 原文写的是 `rbattach.c:864-870`。唯一符合描述的调用在 **`src/driver/rbattach.c:868-870`**。
- **前提核实成立**（我读了 `rbattach.c:828-874`）：`:866` 的 `while (SelfRefs-- > 0) RbDerefExt(ext);` 之后，`:868-870` 仍在读 `ext` 与 `ext->LowerDevice`；`:864-865` 的注释明写"函数内不得再访问 ext"。修法就是把那 3 行整体移到 `:866` 之前，纯顺序。
- **为什么不改**：硬约束把可改文件限死在 6 个，`rbattach.c` 不在其中；且 `rbattach.c` 正是审计 §6「不要碰 C」列出的排空/僵尸协议所在文件（`RbVolumeRemove`，`:760-874`），同一文件里做未授权改动不合适。已向 team-lead 请示授权，未在本次窗口内得到答复。
- **待授权的补丁**（3 行移位，零语义变化，可直接应用）：

```diff
     /* 4) 先释放本调用方引用，再分离/释放 PDO/删设备。
      *    IoDeleteDevice 之后 ext 所在内存随设备扩展销毁，函数内不得再访问 ext。 */
-    while (SelfRefs-- > 0) RbDerefExt(ext);
-
     RbDiagRecord(RB_DIAG_KIND_ATTACH, RB_DIAG_PHASE_END, STATUS_SUCCESS,
                  RB_DIAG_FLAG_UNKNOWN, NULL, NULL, ext, NULL,
                  ext->LowerDevice, NULL);
+    /* [165 / 161 §2.1] 上移到 deref 之前：拆引用回路之后再读 ext 依赖三个互不相邻的
+     * 前提（未真正释放 / 队列未回收 / 无人复用），纯顺序调整即可消除这条隐性 UAF 依赖。 */
+    while (SelfRefs-- > 0) RbDerefExt(ext);
+
     if (ext->LowerDevice) { IoDetachDevice(ext->LowerDevice); ext->LowerDevice = NULL; }
```

  唯一的顺序差异：`RbDiagRecord` 里的 `ext->Stream` 参数在两处都是 `NULL`（`:869`），`ext->LowerDevice` 在 `:871` 才被置空，所以放在 deref 前后读到的值完全一样。

### 4.2 没有顺手做的事（明确放弃，避免扩大风险）

| 想法 | 为什么不做 |
|---|---|
| 删 `eng->readahead_hit_bytes` 字段 / `rb_stats.readahead_hit_bytes` / `RB_STATS2.ReadaheadHitBytes` | 动结构体布局 = 协议三副本门禁必红（161 §5.1）。字段留着只是"恒 0"，删了是 ABI 事故 |
| 新增 bypass 原因码（如 `RB_BYPASS_UNKNOWN_MAJOR 13`） | `LastBypassReason` 是诊断展示口径的连号常量集，扩号会牵动 UI/日志解释；未知 Major 本就不该出现，不值得为它扩枚举 |
| 给拒绝出口调 `RbNoteIoResult(ext, STATUS_INVALID_DEVICE_REQUEST)` | 会累计 `IoFailRun` 并在 32 次后 `RbSetPanic` 全局熔断 —— 把编程错误记成盘故障，见 §1.4 第 3 点 |
| 把三处派发合并成表驱动（161 §6 值得做 B 的雏形） | 本次只要"加 default"。表驱动牵动 `RbDispatchDeviceControl`，超出零风险清单 |
| 核查 `rbL2.c:417` 的 `(void)ZwReadFile` | 任务明确要求"只补 `(void)`，不要开始检查这个返回值"，161 §7 也标注该处后果未评估 |
| 重排列 `rbrebuild.c` 的 `out_restore`/`out_rollback` 出口顺序 | 与本次无关，动它就是在碰重建数据路径 |

---

## §5 未核实项（明确声明）

1. **真机与部署全链未验证**：未签名、未部署、未 attach 卷、未跑 `RB-sign-deploy-150.cmd`、未跑任何 QA/探针。因此"未知 Major 拒绝路径"只能静态论证（今天是死路），**没有实测触发**。
2. **`/W4` 口径**：`-Target driver` 实际用的是 manifest 里的 `/W3`（`targets.driver.compile.flags`），`/W4` 只出现在 `targets.core.compile`。本次记录的是"实际配置下 0 条 warning"，**没有**把驱动单独提到 `/W4` 重编译（那会偏离发布配置、并可能暴露大量既有告警）。若需要 `/W4` 口径的结论，得另跑一次非发布构建。
3. **改前的 warning 基线**：未在改动前用同一命令采集基线（不能在 450+ staged 文件的工作树上回滚源码）。结论依据是"改后日志里 warning 行数为 0"——没有告警就不可能新增告警。
4. **`compiled=64` 与 161 的"31 宿主 ×2 = 62"口径差异**未追查（162 之后 manifest 可能新增宿主）。本次只如实记录该数字。
5. **`RbLog` 的第三参数语义**：拒绝出口传的是 `(LONG)(ULONG)ctx->Major`，而 `RbLog(unsigned Level, unsigned Src, long Status, const WCHAR *Text)` 的第三参数在别处一律是 NTSTATUS/盘符（`:601` 传 `failed` 计数、`:288` 传盘符，所以"非状态整数"是既有用法）。日志消费侧如何解释这个值**未读**，若 UI 把它当 NTSTATUS 格式化，显示会是 `0xC0000010` 之类的怪值（仅日志可读性，无功能影响）。
6. **`read_plan.c` 与头文件的宏"同值镜像"没有门禁**：断言只覆盖数值一致性方向（`MAX_BYTES == 128KiB`、`MAX_SUBS <= RB_MAX_RANGES`），如果有人只改 `read_plan.c` 的 `RB_READ_PLAN_SUB_SIZE` 同时保持乘积 128KiB（例如 2048 × 64），不变量 ① 仍通过、不变量 ② 会因 64 > 32 拦住。但"2048 × 64 且 `RB_MAX_RANGES` 也改成 64"的组合无人拦。
7. **`__pycache__` 清理**：全库 `find -name __pycache__` 零命中（门禁脚本运行后未留缓存），无需清理。

---

## §6 改动清单汇总（最终态）

`git diff --stat`（工作树 vs index，**未 add/commit**）：

```
 src/core/cache.c         |  5 ++--
 src/core/cache.h         |  1 -
 src/core/read_plan.h     | 30 +++++++++++++++++++
 src/driver/rbL2.c        |  5 +++-
 src/driver/rbattach.c    | 14 +++++++--
 src/driver/rbio_worker.c | 77 ++++++++++++++++++++++++++++++++++++------------
 src/driver/rbrebuild.c   | 19 ++++++++++--
 7 files changed, 123 insertions(+), 28 deletions(-)
```

7 个 `.c/.h`（6 个授权文件 + 授权后追加的 `rbattach.c`），加 `deliverables/` 下 3 个非源码文件：本报告（新增）、`RB-sign-deploy-150.ps1`（pin 值）、`.deploy-pin-state.json`（指纹，gitignore 内）。**没有触碰** `src/ctl/`、`src/ui/`、`build/`、`tests/`、`dist/`，也没有动任何结构体布局。

---

## §7 补做：`RB_DIAG_PHASE_END` 上移（`rbattach.c`）

首轮因文件越权挂起（见 §4.1），team-lead 授权后执行。授权范围：**仅此一处 3 行移位，其余字节一个不动**。

### 7.1 先决核实：这个 phase 有没有被当成"时长"消费

指令要求先 grep `RB_DIAG_PHASE_*` 的消费方，判断"END 记录的时刻提前一次 deref 循环"是否会破坏某个判据。查证结果——**不存在任何时长/SLO 判据**，四条证据：

1. **事件结构体里没有时间戳**。`RB_DIAG_EVENT`（`rb_common.h:420-436`）的字段是 `Sequence / Kind / Phase / Status / Flags / Reserved / Object / Aux / Ext / Stream / LowerDevice / Fn`，`C_ASSERT(sizeof(RB_DIAG_EVENT) == 80)` 钉死。没有 `KeQuerySystemTime`/TSC/`InterruptTime` 任何时间量 ⇒ **物理上无法计算任何阶段的持续时间**，能表达的只有环内先后顺序。
2. **`RB_DIAG_PHASE_*` 只有写者，没有读者**。全库 `src/ tests/ bench/ tools/` 命中 12 处，全部是 `RbDiagRecord(...)` 的实参（`rbattach.c` 6 处、`rbdev.c` 2 处、`rbpools.c` 3 处等），没有任何 `switch (Phase)` / 比较 / 阈值判断。
3. **事件只有一个消费方，且只用于崩溃取证**。`RbDiagSnapshot`（`rbdriver.c:487`）被 `rbdriver.c:583` 独占调用，结果填进 `RB_DIAG_CRASH_BLOB` 写 secondary dump——事后人读，不是运行时判据。
4. **`RbDiagRecord` 本身只在 `g_Rb.TestMode` 下生效**（`rbdriver.c:468` 第一行 `if (!g_Rb.TestMode) return;`）。生产路径上这条记录根本不落环。

补充两点顺序影响分析：

- `while (SelfRefs-- > 0) RbDerefExt(ext);` **不产生任何 `RbDiagRecord`**（`RbDerefExt` 只在删设备/僵尸回收时 `RbLog`），所以 END 事件与其它事件的**相对顺序不变**，只是它在环里的 `Sequence` 值小了 0~N（N = `SelfRefs`）。
- 唯一可见的差异：`Phase=END` 记录的 `Ext`/`LowerDevice` 字段现在是**引用仍有效时**的值（这正是修复目的）；`LowerDevice` 要到 `:879` 才被置 NULL，所以取到的值与改前逐字节相同。

**结论：只用于诊断展示（且仅 TestMode 展示、且无时间轴）⇒ 直接做。**

### 7.2 改前 → 改后（`rbattach.c`，diff）

```diff
     ext->Magic = 0;                                     /* [V10.1] 哨兵失活 */
 
+    /* [165 / 161 §2.1] PHASE_END 上移到拆引用回路**之前**：这条记录的实参
+     * （ext / ext->LowerDevice）是活引用下的字段。原位置之所以"没出事"，依赖三个
+     * 互不相邻的前提——deref 后 ext 未被真正释放、拆除队列还没来得及回收、
+     * 期间无人复用该内存——任一条失效即 UAF。纯顺序调整，参数与调用形态不变。
+     * 展示侧影响：RB_DIAG_EVENT 无时间戳字段（C_ASSERT 钉死 80 字节布局），
+     * 任何消费方都算不出"阶段时长"，此改动只让 END 事件在环里的序号早一次
+     * deref 循环的距离。 */
+    RbDiagRecord(RB_DIAG_KIND_ATTACH, RB_DIAG_PHASE_END, STATUS_SUCCESS,
+                 RB_DIAG_FLAG_UNKNOWN, NULL, NULL, ext, NULL,
+                 ext->LowerDevice, NULL);
+
     /* 4) 先释放本调用方引用，再分离/释放 PDO/删设备。
      *    IoDeleteDevice 之后 ext 所在内存随设备扩展销毁，函数内不得再访问 ext。 */
     while (SelfRefs-- > 0) RbDerefExt(ext);
 
-    RbDiagRecord(RB_DIAG_KIND_ATTACH, RB_DIAG_PHASE_END, STATUS_SUCCESS,
-                 RB_DIAG_FLAG_UNKNOWN, NULL, NULL, ext, NULL,
-                 ext->LowerDevice, NULL);
     if (ext->LowerDevice) { IoDetachDevice(ext->LowerDevice); ext->LowerDevice = NULL; }
```

改后行号：`RbDiagRecord` 在 `rbattach.c:871-873`，`while (SelfRefs...)` 在 `:877`。参数、调用形态、`RB_DIAG_FLAG_UNKNOWN`、`NULL` 占位一字未改；`:864-865`（现 `:875-876`）的原注释按指令**原样保留**（它陈述的事实"deref 之后不得再访问 ext"仍成立，只是现在它后面没有再访问）。

### 7.3 全套重跑（最终结果，覆盖 §2 的 driver 数字）

| 步骤 | 结果 |
|---|---|
| `-Target driver`（第 1 次） | `BUILD OK`，exit=0，**0 条 warning/error** |
| `-Target driver`（第 2 次） | 同 sha ⇒ **可复现**：`a49fad959c781cd999b0eecf19113aea28f378ced78331452d40bde118a5c023` |
| `-Target core` | `compiled=64 skipped=1`，**`RUN_FAIL=0`**，exit=0 |
| `-Target gates` | `result: PASS (0 fail, 0 warn)`，`ALL GATES PASS`，exit=0 |

- **size = 185856**（与上一轮相同：只调换了指令顺序，没增删函数/数据）
- **sha256 = `a49fad959c781cd999b0eecf19113aea28f378ced78331452d40bde118a5c023`**（上一轮 `501f0d5d…`）

### 7.4 pin 重同步

`& $pwsh -File deliverables\update-deploy-pin.ps1 -AcceptSourceChange`（exit=0）：

```
[!] SOURCES CHANGED since the last pin registration
    M  src/driver/rbattach.c
[!] -AcceptSourceChange given - blessing the new source set
[ok] pin updated in RB-sign-deploy-150.ps1
     old: 501f0d5dea30edd2cb016f8e7eb143569e63f1bbc2ea7cc60763d23eaeecacd4
     new: a49fad959c781cd999b0eecf19113aea28f378ced78331452d40bde118a5c023
```

**M 清单只有 `rbattach.c` 一条**，与预期完全一致（`build/build.ps1`、`build/sources.manifest.json` 已在上一轮批准，本轮不再出现；无 A/D）。

| 项 | 本轮改前 | 本轮改后（**最终值**） |
|---|---|---|
| `RB-sign-deploy-150.ps1:27` `-ExpectSha` | `501f0d5dea30edd2cb016f8e7eb143569e63f1bbc2ea7cc60763d23eaeecacd4` | **`a49fad959c781cd999b0eecf19113aea28f378ced78331452d40bde118a5c023`** |
| `.deploy-pin-state.json` `artifactSha256` | `501f0d5d…` | **`a49fad959c781cd999b0eecf19113aea28f378ced78331452d40bde118a5c023`** |
| `artifactSize` | 185856 | 185856 |
| `srcFingerprint` | `9aa0f6da577e26efcb447b0d9fc797612c23798ff8b739df1f27a738c2a9e3d9` | **`2a5cfa09d0ddc5385463b589812e70c7bd73c493b255476196d73af475106742`** |
| `fileCount` | 38 | 38 |
| `updatedUtc` | `2026-11-11T22:36:59Z` | `2026-11-11T22:48:23Z` |

---

## §8 审计原文的三处错漏（只记录，必做留痕）

这三条是执行过程中发现的**161 报告本身的问题**，不是代码问题。后续任何人拿 161 当行动清单时都会撞上，所以在这里钉住。

### 8.1 漏了第三处派发点：`rbio_worker.c:1255`（`RbRwWorker` 系统队列兜底）

161 §2.2 只说"`rbio_worker.c:932-940` 的派发没有 default，两处 `if/else`"。实际是**三处**，第三处是 `RbRwWorker`（`rbio_worker.c:1249-1259`，池未启动时的 `ExQueueWorkItem(DelayedWorkQueue)` 兜底例程）：

```c
    if (ctx->Major == IRP_MJ_READ)                RbReadWorker(ctx, NULL);
    else if (ctx->Major == IRP_MJ_FLUSH_BUFFERS)  RbFlushIrpWorker(ctx);
    else                                          RbWriteWorker(ctx, NULL);
```

**为什么危险**：兜底路径是"worker 池没起来"时的唯一出路——池启动失败、卸载/重启窗口、`IoWorkerCount==0` 的分支都会落到这里。也就是说**审计点名的两处（池内）在池没起来时根本不会执行**，唯一真正跑的是审计没点的那一处。只改前两处 = 加固了不存在的路径，真正的兜底口仍然把任意未知 Major 当写处理；而 `RbWriteWorker` 会按写语义读 `Parameters.Write.ByteOffset` / 长度并对不匹配的 IRP 下发盘 IO，那是类型混淆 + 潜在的越界/脏写。本次三处统一收敛到 `RbDispatchCtx`（§1.4）。

### 8.2 `rbrebuild.c:612` 的 `(void)` 定性偏差：返回值是**卷数**，不是状态

161 §2.4 原文：「`rbrebuild.c:612,631` 的 `(void)` 是误用，真实状态在 out 参数里」。**前半句成立，后半句对 `:612` 不准确**：

- `RbRebuildReattachAll`（`rbrebuild.c:277`）的签名是 `static ULONG RbRebuildReattachAll(...)`，返回 **`restored` 成功重挂卷数**——既不是 `NTSTATUS` 也不是 `BOOLEAN`。真实的失败明细在 `*pFailed`（出参）。
- 所以 `:612` 的 `(void)` 不是"掩盖了一个被消费的状态"，而是"**丢弃了一个计数**"。按审计字面执行（只删 `(void)`）会得到一个裸调用：`/W3`、`/W4` 都不会报未使用返回值，问题从"误导"退化成"沉默"，等于没修。
- 本报告的处理（§1.3b）：**显式接收返回值 + 与 `failed` 联合判定 + 一条汇总 WARN**，语义与 `:587` 主路径（`restored != volCount` 判部分失败）对齐。与审计建议的偏差在此备案。
- 对照 `:631`：`RbReattachVolumeForRebuild` 确实返回 `NTSTATUS`（无出参），审计对那一处的定性**完全正确**，按"显式接收 + 失败记 ERROR"处理。

### 8.3 `tests/core` 没有 `/std:c11` ⇒ `_Static_assert` 在 core 侧不可用

161 §3.2 建议"加一个 C11 `_Static_assert`"。**驱动侧可行、core/tests 侧不可行**，这是本仓库的一个硬约束：

- `build/sources.manifest.json`：`targets.driver.compile` 是 `standard: "/std:c11"`；`targets.core.compile` **只有** `release: ["/nologo","/O2","/W4","/utf-8"]`，**没有任何 `/std:` 开关**（MSVC 旧 C 模式）。
- `read_plan.h` 被 `tests/core/test_sparse_miss.c` 与 `tests/core/bench_read_plan.c` 直接 `#include`。实测第一版 `_Static_assert` 的后果：

```
..\..\src\core\read_plan.h(23): error C2143: 语法错误: 缺少")"(在"("的前面)
..\..\src\core\read_plan.h(23): error C2091: 函数返回函数
[X] release build failed: test_sparse_miss
```

- 仓库里 `_Static_assert` 的唯一"先例"是 `tests/core/offchk.c:4` 的**注释**（"`_Static_assert` 只能断言本镜像自身…"），从未被真正编译过——照抄这条先例会直接红。
- **硬约束**：任何放在 `src/core/*.h` 且被 tests 包含的头文件，**不能**用 `_Static_assert`/`static_assert`。本仓库可用的等价写法有两条：驱动侧 `C_ASSERT`（`winnt.h`，仅驱动 TU 可见）、跨侧负数组长度 `typedef char name[(cond)?1:-1]`（本报告 §1.5 采用）。要为 core 侧启用 `_Static_assert`，得先给 `targets.core.compile` 加 `/std:c11`——那是构建口径变更，不在零风险清单范围内。
