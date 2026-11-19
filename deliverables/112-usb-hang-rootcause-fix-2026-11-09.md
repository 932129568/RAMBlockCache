# 112 · U 盘拔出整机冻结：根因确证与修复（2026-11-09 10:2x–10:4x）

> 用户明确指出「不插 U 盘本来就不会重启，不要逃避问题」。本文不再给观察建议，直接给出**静态可证的内核缺陷**、因果链、修复、验证与验收方法。前置：`111-usb-hang-forensics-2026-11-09.md`。

---

## 1. 结论

**根因是 `rbattach.c:RbSendSyncIoctl` 的无界死等。** U 盘拔出时，START_DEVICE 排在 PASSIVE 工作项里的几何探测正在向该卷发同步 IOCTL；下层 USB 存储栈的 IRP 既不完成也不响应 `IoCancelIrp`，而该函数在 `STATUS_PENDING` 后是 `for(;;)` 无限等待（注释原文「永不放弃在飞 IRP」）。工作项永久卡死，且持有 `ext` 引用不归还，连带使 `RbVolumeRemove` 的 `Refs<=1+SelfRefs` 排空与僵尸收割永不就绪。这解释了 10:07:03 两条 `AddDevice` 之后驱动日志与系统事件日志同时静默、整机冻结、且因硬断电没有任何 dump。

**已修复并重编**：`e8d433ea…`（未签名，177,664 B），18 源编译 **0 error / 0 warning**，三道门禁 PASS，8 个调用方逐一核对安全。待一次 UAC 部署 + 一次重启，随后可**用 U 盘直接复现验收**。

---

## 2. 缺陷代码（修复前）

```c
st = IoCallDriver(target, irp);
if (st == STATUS_PENDING) {
    for (;;) {
        to.QuadPart = -50000000LL;                 /* 5s 步进 */
        st = KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, &to);
        if (st == STATUS_SUCCESS) break;
        if (!cancelled) {
            IoCancelIrp(irp);                       /* 强制完成，杜绝永久挂起 */
            cancelled = TRUE;
            continue;
        }
        /* 已取消仍未完成则继续等（RbSyncIo 同款：永不放弃在飞 IRP） */
    }
}
```

「5s 有界等待」的注释与实现不符：预算只在**未取消时**生效一次，之后是真正的无限循环。

### 完整因果链

| 步 | 事实来源 |
|---|---|
| 1 | U 盘插入 → `RbAddDevice` 对所有卷无差别挂栈（`rbdriver.c:819-930`，默认透传） |
| 2 | START_DEVICE → `RbPnpStartComplete` 排队 `RbGeoWorkRoutine`（`rbattach.c:901`） |
| 3 | 10:07:03 日志两条 `AddDevice: filter on volume stack`，此后零日志 |
| 4 | `RbGeoWorkRoutine` → `RbRefreshVolumeGeometry` → 4 个 `RbSendSyncIoctl`（GET_DRIVE_GEOMETRY / STORAGE_QUERY_PROPERTY ×2 / GET_LENGTH_INFO） |
| 5 | 用户压缩文件后拔盘，下层 IRP 在飞且永不完成/不响应取消 → `for(;;)` 永久阻塞 |
| 6 | 工作项持有 `RbDerefExt(ext)` 引用（`rbattach.c:965`）不归还 → 拆除排空（60s）超时转 zombie，reaper 的 `Refs<=1` 恒定不成立 |
| 7 | PnP 移除流程与卷栈被阻塞 → 系统级冻结，事件日志服务亦停；硬断电无 dump |

> 为什么排除「仅第三方 360 过滤驱动」：无法完全排除其参与，但**我方确有一条自身即可致死的无限等待**，且时间精确落在 AddDevice 之后、日志静默之前；修掉它后该路径不再有无限阻塞点。

---

## 3. 修复内容

### 3.1 `RbSendSyncIoctl` — 有界等待 + 全池化上下文（根因）

- `KEVENT` / `IO_STATUS_BLOCK` / `in` / `out` 全部改为 `NonPagedPool` 分配，不再用调用者栈。
- 等待预算 **10s**（5s × 2），首次超时 `IoCancelIrp`，第二次超时立即返回 `STATUS_IO_TIMEOUT`。
- 超时路径按项目既有 **leak-by-design** 纪律**不释放** IRP 与池块（bounded：每个几何探测至多 4 个 IOCTL、合计 <4KB），并**不触碰调用者 `out`**。
- 正常完成路径：把池 `out` 拷回调用者、释放全部池块；IRP 沿用既有语义由 I/O 管理器回收（不额外 `IoFreeIrp`，避免 associated-IRP 双重释放）。

**为什么必须全池化**：超时若直接返回，晚到的 IRP 完成时会写调用者栈上的 `KEVENT`/`IO_STATUS_BLOCK`/`out` → UAF / 池腐坏（正是 11-09 `0x7E` 的形态）。这是「不能简单加超时」的原因。

### 3.2 `RbRefreshVolumeGeometry` — 纵深防御

入口加 `if (ext->Removed) return;`：surprise removal 已置 `Removed=1` 时不再向正在消失的设备发同步 IOCTL，几何保持未知即透传。

### 3.3 调用方安全性核对（8 处，全部安全）

`rbattach.c:338/352/367/396/1714/1718` 与 `rbthermal.c:78` 全部以 `NT_SUCCESS(st)` 或 `ret`（超时路径不写 `returned`，保持 0）为门；超时时统一走「几何未知 → 透传 / 回落 VolId 哈希」的保守路径，不会误判几何或使用未初始化输出。

### 3.4 同族缺陷登记（本批未改，明确理由）

`rbdev.c:RbSyncIo:157-172` 也是 `for(;;)` + 「取消已请求仍未完成：继续等」。它属于**数据面**同步读写，`buf` 是缓存数据缓冲（最大 64KB+，且调用方在返回后可能释放）；超时放弃必须同时保证缓冲生命周期与回写失败时的数据一致性，需单独设计 + core 全序测试，**不能套用几何路径的池化方案**。U 盘卷默认透传（`IsAttached=FALSE`），本次事故不经过它。已在台账登记，待专门批次处理。

---

## 4. 验证（本批已完成部分）

| 项 | 结果 |
|---|---|
| 重编 18 源（`cl /O2 /kernel /utf-8` + link） | `compiled 18 sources OK; warning/error lines=0` |
| 产物 | `src\driver\x64\Release\RAMBlockCache.sys` = `e8d433ea71312c7e9ab6ad96aea914139d482bf939b13d6f62ec121bbbd44ae3` / 177,664 B / `NotSigned` |
| `check_repo.py` | `PASS (0 fail, 0 warn)` |
| `check_proto_sync.py --strict` | `PASS (0 fail, 0 warn)` |
| `refcount_model.py` | `5/5 场景，17/17 断言` |
| 静态论证 | 超时后 10s 返回 → 工作项继续 → `RbDerefExt` 归还引用 → 拆除排空/僵尸收割可就绪；无 UAF、无无限阻塞点 |

**未验证**：真机复现（需部署重启后主动插拔 U 盘）。静态修复不等于内核已验证，按项目红线如实标注。

---

## 5. 待你执行 + 验收方法（真正的闭环）

1. **再次双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`** → 一次 UAC。脚本自动：签名新 sys → 同步 `dist` / sidecar / 三锚点并回读 → `deploy.ps1` 部署 → `check_signature.ps1` 门禁⑤。日志 `o9_fix_deploy_result.txt` 出现 `RESULT: OK` 即可。（脚本会顺带重跑一次 dump/cdb 复核，无害。）
2. **重启一次**（新驱动 + Verifier 同时生效，看门狗 `DeploySeq` 递增后由登录任务追平）。
3. **主动复现验收**：重复「插 U 盘 → 压缩文件 → 拔 U 盘」，并留意是否还有整机冻结。按修复后的语义，最坏情况是几何探测 10s 超时、驱动日志出现 `sync ioctl timeout: device gone, IRP left in flight (bounded leak)`、U 盘卷保持透传，**系统不应卡死**。
4. 回来叫我做复核：读新开机段日志中的 timeout 记录、`o9_watch.ps1`（`VERIFIER_EVENT=0`、`FAIL=0`）、看门狗闭环、四层复核。

这一步不是「观察等待」，是用触发场景验证修复有效性。

---

## 6. 红线与限制

- A2 回写验证维持 **FROZEN**；`96:1394/:1452` 两门禁改；`Features\Flags=0`；默认直写；`WBCOAL ↔ L2` 互斥；Verifier **保留在岗**（修复后正好在 Verifier 下验证）；禁 `method=2` / Web 保存；禁再跑 `finalize_*`。
- 超时路径为 **bounded leak**：每次 surprise removal 最多泄漏 4 个 IOCTL 的 IRP + 池块（<4KB）。这是「不 UAF」与「不无限阻塞」之间的明确取舍，已记录在代码注释；长期累积需后续用完成例程 + 引用计数回收。
- 本批无真机验证；`RbSyncIo` 同族缺陷未修（§3.4）；本次挂起因硬断电无 dump，静态归因虽自洽但不能称为内核级证明。

---

*报告完。根因已定位并落码，剩下一次 UAC、一次重启、一次主动复现验收。*
