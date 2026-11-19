# 194 · L2 拓扑审计（容器卷 vs 被加速卷）

- 日期：2026-11-12
- 对象：`src/driver/rbL2.c`、`rbfeat.c`、`rbattach.c`、`rbio.c`（辅：`rbfeatctl.c`、`rbmsg.c`）
- 方式：只读代码审计；未改 `src/`、未跑 `rbfeatctl`、未启停服务。
- 结论口径：下文“被加速卷”= `IsAttached=TRUE` 且有 `Stream` 的卷；“容器卷”= `Features\L2Letter` 指向的卷。

## §0 结论表（先读）

| 问题 | 结论 | 关键证据 |
|---|---|---|
| `l2 enable <X:>` 的 X: 是容器卷还是被加速卷？ | **容器卷**。X: 只用于拼容器路径，与“哪个卷被加速”完全解耦；被加速卷由 attach 状态决定 | `rbL2.c:246-259`（`RbL2Path` 用 `s_Letter` 拼 `\??\X:`）、`rbL2.c:605-614`（`s_Letter` 来自 `Features\L2Letter`）、`rbL2.c:689`（`RAMBoostL2.bin`）、`rbL2.c:265`（`RAMBoostL2.idx`） |
| 容器卷能否同时被加速？ | **设计上不能，代码上可残留**：重启后自动/手动 attach 都被 `RbFeatRefuseLetter` 拒绝；但“先 attach 再 enable”的卷本会话内不会被摘下，会一边被加速一边承载容器 | 拒绝：`rbattach.c:743`（自动 sweep）、`rbattach.c:1133`（手动 `RbAttachVolumeByLetter`）、`rbattach.c:1467`（运行时挂栈）。不摘：`rbfeat.c:717-718`（REFRESH 只重载 flags + `RbL2LazyOpen`），无 detach |
| 自缓存环有无防护？ | **无任何显式防护**（无 top-level IRP / 线程 / 文件对象 / 文件名判断）。只有两条**间接**条件在兜底：①容器卷未 attach ⇒ 过滤器纯透传；②容器 IO 是分页 IO 且 `CachePagingIo` 默认 0 ⇒ 直接旁路。两者都不是“递归守卫”，`CachePagingIo=1` 时存在递归/自缓存面 | 无守卫：`rbio.c:394-577` 全路径无 `IoGetTopLevelIrp`/`PsGetCurrentThread`/文件名判断；旁路门：`rbio.c:450`；透传门：`rbio.c:424-433` |
| 推荐拓扑 | **(b) 独立的不加速卷放容器，D: 只做被加速卷**；无独立卷时退 (a) 容器放 C:、加速 D:；**(c) 同卷不可用**（不可复现、且语义自指） | `rbfeat.c:807`、`rbattach.c:1133`、`rbL2.c:619`（单卷门只数卷数，不校验“容器盘 != 被加速盘”） |

**一句话**：夹具 `l2 enable D: 2` + 在 D: 上跑负载，只有在“D: 在 enable 之前已 attach”时才可能有 stores；这是残留态而非支持态，**重启后必然 stores=0**（D: 被拒 attach，无流）。方案结构应改为“容器卷 ≠ 负载卷”。

---

## §1 `l2 enable <X:>` 的 X: 语义与容器路径

1. `rbfeatctl l2 enable <X:> <GB>` 只做三件事：写 `Features\L2Letter`（REG_DWORD，值是盘符字符码）、写 `L2SizeGB`、置 `Flags|=4`（L2 位），然后发 `RB2_IOCTL_REFRESH`。见 `src/ctl/rbfeatctl.c:252-272`。
2. 驱动侧读取点：`Rb2GetDW(L"Features", L"L2Letter", 0, &v)` → 校验 `'A'..'Z'` → `s_Letter = (WCHAR)v`（`rbL2.c:605-614`）。同一值的另一处读取在开机索引作废路径 `rbL2.c:390-396`。
3. 路径拼接：`RbL2Path` 固定前缀 `\??\` + `s_Letter` + `':'` + `'\'` + 文件名（`rbL2.c:246-259`）。
   - 数据容器：`\??\X:\RAMBoostL2.bin`（`rbL2.c:689`）
   - 索引文件：`\??\X:\RAMBoostL2.idx`（`rbL2.c:265`、`rbL2.c:397`）
4. 因此 **X: 是容器/索引所在卷**，不是被加速卷。被加速卷完全由当前 `IsAttached && !Removed` 的卷决定：`RbL2CountAttached()`（`rbL2.c:906-916`）与 `RbL2AttachedVolId()`（`rbL2.c:919-929`）。
5. 关键缺口：打开容器时只校验“被加速卷数量 ≤ 1”（`rbL2.c:619-631`），**不校验容器盘符是否等于被加速卷盘符**。所以“容器盘 == 被加速盘”会被静默接受，`s_VolId` 就取该卷的 VolId（`rbL2.c:666-671`）。

## §2 容器卷能否被加速：各条拒绝闸门与 REFRESH 行为

### 2.1 `RbFeatRefuseLetter` 的作用面

`rbfeat.c:801-810`：

```c
if ((g_Rb2Flags & RB2F_L2) == 0) return FALSE;     // L2 关闭时一律放行
Rb2GetDW(L"Features", L"L2Letter", 0, &v);
return ((WCHAR)v == Letter);
```

它**不只作用于自动挂载 sweep**，而是被三个 attach 入口共同消费：

| 调用点 | 路径 | 失败结果 |
|---|---|---|
| `rbattach.c:743` | 开机/30s 巡检 `RbAutoAttachSweep` | `continue`（跳过，日志 `auto-attach skip: L2 storage volume`，`rbattach.c:947-948`） |
| `rbattach.c:1133` | 手动热开关 `RbAttachVolumeByLetter` | `STATUS_ACCESS_DENIED` |
| `rbattach.c:1467` | 运行时挂栈 `RbAttachVolumeRuntime`（TestMode） | `STATUS_ACCESS_DENIED` |

### 2.2 手动 `RbAttachVolumeByLetter` 的全部拒绝闸门（`RbAttachVolumeByLetterImpl`，`rbattach.c:1076-1216`）

1. 盘符非 A–Z：`1083`
2. 重建窗口 `g_Rb.Recreating`：`1090`（`forRebuild` 除外）
3. attach 锁未初始化：`1095`
4. `RbPanicActive()`：`1103`
5. `BootLoopGuard`（≥2 次非干净启动）：`1109`
6. `RbFindExtByLetter` 找不到卷：`1117`
7. `ext->Removed`：`1124`
8. **`RbFeatRefuseLetter(letter)`（L2 容器卷保护）：`1133`**
9. 同盘符存在多个 ext（B-9）：`1171-1175`
10. `RbL2AttachBlocked()`（L2 容器已开且已有别的被加速卷）：`1182-1186`
11. 几何无效且重测失败：`1189-1191`
12. `RbEnsureEngine` 失败：`1193-1194`

注意闸门 10 是“第二卷”门：它只在**已有**被加速卷时拦截新卷，不拦截“唯一卷恰好是容器卷”。这与 §1 第 5 点同源。

### 2.3 REFRESH 会不会 detach 已挂载的容器卷？

不会。`RB2_IOCTL_REFRESH` 的处理只有两步（`rbfeat.c:717-718`）：

```c
Rb2ReloadFlags(); RbL2LazyOpen();
```

`RbL2LazyOpenInt` 只做容器开关（flags 关闭时 `RbL2CloseInt`，`rbL2.c:560-574`）；**全函数不调用 `RbDetachVolumeByLetter`**。`RbDetachVolumeByLetter` 的唯一控制面调用点是 `rbctl.c:566` 的显式 detach 请求。

### 2.4 结论

- **重启后**：D: 不会自动 attach（`rbattach.c:743`），手动 attach 也被拒（`rbattach.c:1133`），D: 无 `Stream` ⇒ 在 D: 上的负载不产生 cache miss/eviction 路径 ⇒ `RbFeatOnEvict` 不触发 ⇒ `l2s` 恒 0。夹具原方案在此场景下结构性错误。
- **同会话内、若 D: 在 enable 前已 attach**：D: 仍是 attached，继续加速；`l2 enable D:` 也能成功打开容器（单卷门只数到 1 个卷）。这是“残留态可跑通”，但它同时是容器卷与被加速卷，属 §3 的自指形态，且**重启即失去**，不可作为验收拓扑。

## §3 自缓存环：驱动自己的容器 IO 会不会被自己缓存/递归

### 3.1 代码里有没有显式递归防护？

**没有。** 全仓 `src/driver` 检索 `IoGetTopLevelIrp` / `TopLevelIrp` / `PsGetCurrentThread` / `KeGetCurrentThread` / `IRP_NOCACHE` / 文件名跳过，均无命中；`rbio.c` 的读写分发（`rbio.c:394-577`）也没有任何“这是本驱动发出的 IRP”判断。

### 3.2 容器卷上到底有没有本过滤器？

有。部署脚本把本驱动写进卷类 `UpperFilters`（`src/scripts/deploy.ps1:441-448`），`RbAddDevice` 对每个卷 PDO 都 `IoAttachDeviceToDeviceStack` 并建 `ext`，默认 `IsAttached=FALSE` 纯透传（`rbmsg.c:30-33`、`rbmsg.c:67-94`）。所以容器卷即使不被加速，`\??\D:\RAMBoostL2.bin` 的 IRP **仍然会经过本过滤器**，只是走透传。

### 3.3 现有两条“间接兜底”

1. **未 attach ⇒ 透传**：`rbio.c:424-433` 在任何缓存逻辑之前判 `!ext->IsAttached`，直接 `RbDispatchPassthrough`。容器卷被拒 attach 时，容器 IO 不可能进入 L2 探针/存储路径。这是当前唯一“硬”的结构性保护，但它保护的是“容器卷不被加速”，不是“自己发出的 IO”。
2. **分页 IO 旁路**：`rbio.c:450-453`，`!g_Rb.Config.CachePagingIo && (irp->Flags & IRP_PAGING_IO)` ⇒ 透传。容器文件以缓存方式打开（`rbL2.c:692-696` 无 `FILE_NO_INTERMEDIATE_BUFFERING`），`ZwWriteFile` 先落 cache，脏页由 lazy writer 以分页写下发；`ZwReadFile` 的缺页读也是分页读。配置默认 `CachePagingIo=0`（`rbdriver.c:1120` `RtlZeroMemory` 后未显式赋值，`rbdriver.c:180` 只是配置项名），所以这些 IO 默认不进缓存、不触发 `RbFeatOnMiss`/`RbL2ProbeFill`，**不会形成探针递归**。

### 3.4 但“无防护”意味着两个真实缺口

- **缺口 A：`CachePagingIo=1`（注册表可改）**。此时容器分页读会进入缓存路径：miss → `RbFeatOnMiss` → `RbL2ProbeFill`（`rbfeat.c:819-825`）→ 对容器文件再 `ZwReadFile`（`rbL2.c:982`）→ 再次分页读……虽然 `RbL2IdxProbe` 先查索引、未命中会立即返回（`rbL2.c:978`），但一旦容器自身的块被 L1 捕获并淘汰进 L2 索引，递归就有落点。没有任何深度/线程标志阻断。
- **缺口 B：容器卷“先 attach 再 enable”**。此时容器卷 `IsAttached=TRUE`，`RbDispatchReadWrite` 不会在 `rbio.c:424` 早退；保护只剩分页旁路。同时容器文件在扩容/索引截断时会走 `IRP_MJ_SET_INFORMATION`：`FileEndOfFileInformation` 属 `clusterAffecting`（`rbio.c:226-234`），在 attached 卷上会执行 `rb_stream_flush` + `rb_stream_discard_all` **并调用 `RbL2DropAll()`**（`rbio.c:241-273`）。也就是说：**每次 `RbL2IdxSave` 对 `.idx` 截断，都会把该卷 L1 视图整份丢弃、并把刚保存的 L2 索引整份作废**。这不是递归，但是同卷拓扑下的确定性自干扰。

### 3.5 确定结论

- **不存在“自缓存环递归”的传统形态**（在默认 `CachePagingIo=0` 且容器卷不 attach 时）：容器 IO 走分页旁路/透传，L2 探针与存储在物理上不会互相再次进入。
- **但也不存在任何显式防护**：安全性完全依赖“容器卷不 attach”＋“`CachePagingIo=0`”这两个**配置/部署不变量**，代码没有把它们锁死。一旦同卷 attach（夹具当前做法），至少会发生 §3.4-B 的 flush/discard/`RbL2DropAll` 自干扰；若再改 `CachePagingIo=1`，则存在真实的递归与自缓存面。
- 因此**不能**把它写成“已防护”；应登记为“拓扑不变量依赖”，并在 §6 的夹具设计里从结构上排除同卷。

## §4 正确测试拓扑

### (b) 独立的不加速卷放容器（首选）

- 容器路径由 `L2Letter` 指向该卷；被加速卷是另一个卷（D:）。被加速卷 attach 正常，容器卷不被 attach（不 attach 就不加速，无需依赖 RefuseLetter）。
- 优点：`RbFeatRefuseLetter` 只是第二道保险；容器 IO 走透传，不会与 L1/L2 数据面交叉；重启后拓扑不变（容器卷本来就不在 `ResumeLetters`）。
- 风险：若容器卷与被加速卷在**同一块物理盘**上，L2 落盘与负载盘 IO 争同一设备，收益/延迟会被污染，测试结论不可外推。所以优先“不同物理盘”。

### (a) 容器放 C:，负载放 D:（无独立卷时的退路）

- 可行性：`RbFeatRefuseLetter` 只拒绝 `L2Letter`，与系统卷无关；系统卷在**自动**路径本来就被跳过（`rbattach.c:716`、`rbattach.c:944` `RbIsBootCriticalPdo`）。容器放 C: 不会因此触发 0x7B——0x7B 的成因是“把引导卷纳入缓存”，不是“在引导卷上放一个文件”。
- 必须核实：C: **没有** `IsAttached=TRUE`。手动 attach 现在允许系统卷（`rbattach.c:1153-1156` 仅记日志），一旦有人手动加速 C:，就退回 §3.4-B 的同卷缺陷。
- 风险：容器文件最大 128GB 占用系统盘；C: 常有 pagefile/休眠文件，L2 的 64KB 记录写与系统分页写同盘竞争；若 C: 盘满，`ZwCreateFile`/扩容失败 ⇒ `L2 state=4`（`rbL2.c:697-701`、`742-755`），L2 静默拒绝。

### (c) 同卷（容器与负载都在 D:）——不可用

- 支持面为零：`l2 enable D:` 之后，重启即无法再 attach D:（`rbattach.c:743`/`1133`/`1467`），拓扑不可复现。
- 语义自指：把 D: 的热块写到 D: 上的文件，命中收益被落盘放大抵消；且 §3.4-B 的 `RbL2DropAll` 会在索引保存时把 L2 自身清空。
- 唯一“能跑”的窗口是“D: 在 enable 前已 attach”的残留态，不能作为验收依据。

## §5 容器卷被拒 attach 的副作用与 `l2 disable` 的可恢复性

1. **“卷不出现”是误读**。`RbFeatRefuseLetter` 只挡 `RbAttachVolumeByLetter`（缓存挂载/热开关），**不参与卷枚举、盘符分配、卷挂载**。卷仍然会出现、可读写，只是 `attached=0`、不加速。`rbattach.c:947-948` 的日志 `auto-attach skip: L2 storage volume` 是“跳过缓存挂载”，不是“跳过卷挂载”。
2. **真实副作用**：重启后该卷静默地永不被加速；且手动 attach 也被拒（`rbattach.c:1133`），用户在 UI 上点“加速”会得到 `STATUS_ACCESS_DENIED`。`rbfeat.c:804-805` 的注释也把它描述为“卷静默两晚无法恢复”，指的是缓存无法恢复，不是卷消失。
3. **`l2 disable` 能恢复**：
   - `rbfeatctl.c:275` 清 `Flags` 的 L2 位；`rbfeatctl.c:279-285` 删除 `L2Letter`/`L2SizeGB` 残值；
   - `rbFeatRefuseLetter` 首先判 `(g_Rb2Flags & RB2F_L2)==0 ⇒ FALSE`（`rbfeat.c:807`），因此 disable 之后**即使注册表残值还在也不再拒绝**；
   - 同一 REFRESH 里 `Rb2ReloadFlags()` 先落新 flags，再 `RbL2LazyOpen()` 关闭容器（`rbfeat.c:718` → `rbL2.c:560-574`），`RbL2AttachBlocked()` 随之解除；
   - 下一次 30s 巡检（`rbfeat.c:524` → `rbattach.c:670`）会按 `ResumeLetters` 重新 attach（如果该卷在恢复列表里）。
4. **两个残留**：`l2 disable` 不删除 `RAMBoostL2.bin/.idx`（文件留在容器卷，最大 128GB）；也不 detach 已 attach 的卷（若 D: 是 enable 前就挂上的，disable 后仍在加速）。

## §6 对夹具的修正建议

1. **`-Letter` 语义改为容器卷**：`-Letter` 必须是**不参与加速**的卷（记为 `L:`），负载卷单独指定为 `D:`。不要让 `-Letter` 与负载卷相同。
2. **负载卷必须先 attach**：夹具在 `l2 enable` 之前先 `attach D:`，并确认 `volstatus` 里 D: `State=1`（`rbfeat.c:727-791` 的 VOLSTATUS）。否则没有 `Stream`，`RbFeatOnEvict` 不会触发（`rbfeat.c:867-928`），stores 必然为 0。
3. **数据集必须大于 L1 缓存**：L2 存储只发生在淘汰路径，且要求 `Hits>=2`（`rbfeat.c:886-892`）。128MB 很可能整份驻留 L1、无淘汰 ⇒ stores=0。应把写入量提高到明显超过 `CacheSizeMB`，并做多轮“读热→写冷”制造淘汰。
4. **断言口径**：`stores` 看 `STATS2.L2Stores`（槽 8，`rbfeat.c:76`），`hits` 看槽 7（`rbfeat.c:75`）；同时看 `L2State`（`rbL2.c:901`，1=opened）。若 `L2State=2` 且日志有 `refused (multi-volume attached...)`，说明测试机上存在第二个被加速卷，单卷门会拒绝（`rbL2.c:619-631`）。若 `L2State=4`，先查容器创建/扩容失败（`rbL2.c:697-701/742-755`）。
5. **干净复位**：每轮测试前 `l2 disable`（清 flags + 残值）→ 确认容器卷不在 attach 列表 → `detach` 负载卷 → 再按新拓扑开启。容器文件如需回收，手工删除 `RAMBoostL2.bin/.idx`（驱动不会自动删）。
6. **不要为了“让 D: 能 attach”去改 `L2Letter` 为 C: 后又在 C: 上跑负载**——那只是把同卷缺陷搬到系统卷。

## §7 必须真机才能定的点

1. 实际部署是否真的把 `RAMBlockCache` 写进了卷类 `UpperFilters`（`deploy.ps1:441-448`）——决定容器卷上是否始终有本过滤器。若某机器是“运行时定向挂栈”且容器卷未 attach，则过滤器根本不在路径上（§3.2 的最强保护）。
2. 现场 `CachePagingIo` 的实际值（注册表 `Parameters\CachePagingIo`）。默认 0 是 §3 结论的前提；被改成 1 后必须重新评估递归面。
3. `ZwWriteFile`（缓存句柄）在本机过滤栈上是否 100% 表现为 `IRP_PAGING_IO`。lazy writer/`FILE_WRITE_THROUGH` 的路径由 cache manager 决定，代码只能推断，需用 ETW/Verifier 或日志在真机确认“容器 IO 从不进入 `RbFeatOnMiss`”。
4. 容器卷若在 enable 前已 attach：真机确认 `RbL2IdxSave` 的 `FileEndOfFileInformation` 是否确实触发 `RbL2DropAll`（`rbio.c:273`），以及由此导致的 L1 冷启动/`l2s` 抖动幅度。
5. 被加速卷与容器卷是否同一物理盘（`IOCTL_STORAGE_QUERY_PROPERTY` 的 device id / 总线号）。同盘会让 L2 的收益与延迟完全失真。
6. `l2 disable` 后 30s 内 `ResumeLetters` 的实际内容与 sweep 日志，确认“disable → 可恢复 attach”在目标机上闭环（依赖 `Parameters\ResumeLetters` 非空，`rbattach.c:714-715`）。
