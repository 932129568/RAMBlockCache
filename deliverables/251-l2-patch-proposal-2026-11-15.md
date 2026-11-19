# 251 — L2 根除补丁方案（B1 + B2 打点 + B3）：待红队评审

- 日期：2026-11-15 15:3x
- 性质：**方案 + 可 apply 的 diff 草案**。本轮**未改任何源码**、未重编、未签名、未部署、未提交 git
- 前置：判决依据见 `250`（B2-a 成立、B1 二次复现、B3 判据缺陷）
- 范围：`src/driver/rbL2.c`（2 处 + 1 处复检）、`src/driver/rbmsg.c`（只加打点）、`src/scripts/l2_store_warmup.py`（判据）

> 三段补丁彼此独立，可分别落地。**P2（打点）必须先于 P4（B2 行为修改）上线**，否则是用推断驱动内核改动（R7）。

---

## §1 P1 — B1：把卷身份门的顺序改正（2 处 + 1 处打开后复检）

### 1.1 为什么这是回归而不是设计

`rbL2.c:702-709` 的 `[FIX S7-06]` 注释写明设计意图：

> 拒绝开后**由首卷的 miss 探针带正确 VolId 重开（探针路径自带重试）**。

而 `:1093` 的门位于 `RbL2LazyOpenInt(FALSE)`（`:1101`）**之前**，`s_VolId` 只在打开路径 `:707` 写入 ⇒ 开机后 `s_VolId==0`，`Extp->VolId != 0` 恒成立 ⇒ **探针永远到不了重试点**。两条设计互斥，实现者按后写的那条走，把前一条废掉了。

### 1.2 diff 1 — `rbL2.c` `RbL2ProbeFill` 入口（:1091-1094）

```diff
--- a/src/driver/rbL2.c
+++ b/src/driver/rbL2.c
@@ -1088,11 +1088,25 @@ NTSTATUS RbL2ProbeFill(PVOID Extp, ULONG64 lba, ULONG len, PVOID dst)
     if (!dst || len == 0 || len > RB2_L2SLICE_MAX) return STATUS_NOT_FOUND;
 
     /* [FIX R6-01] 每次探测的卷身份闸（无状态滞后，不依赖节流/复检时序）：
-     * 请求卷的 VolId 必须匹配容器身份——第二卷/换卷的读一律 MISS。 */
-    if (Extp && ((PRB_DEV_EXT)Extp)->VolId != InterlockedOr64(&s_VolId, 0))
-        return STATUS_NOT_FOUND;
+     * 请求卷的 VolId 必须匹配容器身份——第二卷/换卷的读一律 MISS。
+     * [FIX B1 2026-11-15] s_VolId==0 表示本会话尚未打开过容器，此时**不能**
+     * 用 0 去比对：容器身份只在打开路径（:707）写入，而本门位于打开之前 ⇒
+     * 开机后首个 miss 被自己拒掉，数据路径永远开不了容器，只有控制面
+     * REFRESH 能开。这与 :702 的 [FIX S7-06]「由首卷的 miss 探针带正确
+     * VolId 重开」直接矛盾，属回归。
+     * 放行 s_VolId==0 不削弱跨卷防护：懒开内部按 RbL2AttachedVolId()（唯一
+     * 被加速卷）写入身份，紧接其后的二次复检（见下）对任何不匹配的请求
+     * 仍然拒绝；若懒开失败（无卷 attach）则 s_Idx==NULL，:1113 的守卫退出。 */
+    {
+        ULONG64 vid = (ULONG64)InterlockedOr64(&s_VolId, 0);
+        if (Extp && vid != 0 && (ULONG64)((PRB_DEV_EXT)Extp)->VolId != vid)
+            return STATUS_NOT_FOUND;
+    }
 
     /* [F1] 在飞计数：通过全部门之后、触碰 s_File/s_Idx 之前占位；
      * 单一出口 cleanup 保证与 RbL2Close 的排空等待严格配对（所有路径必达）。 */
     InterlockedIncrement(&s_Ops);
     rec = NULL; st = STATUS_NOT_FOUND;
     if (s_Halt) goto cleanup;                      /* [F1] 拒绝/关闭态：容器不可用（防跨卷污染） */
     RbL2LazyOpenInt(FALSE);                   /* [FIX S7-02] 数据路径：禁内联 close */
     if (s_Halt) goto cleanup;                      /* 本次开路径恰好判罪（含延迟闩停机） */
+    /* [FIX B1 2026-11-15] 打开后复检卷身份：懒开刚把 s_VolId 写成
+     * RbL2AttachedVolId()，若与本请求卷不一致，说明该卷不是本容器的加速
+     * 目标（多卷/换卷的边角），本次放行的目的已经达成（容器已开），
+     * 但本请求必须拒绝，否则会用别卷的 LBA 去读本卷的记录。
+     * 不计 L2Miss：该卷本就不应被加速，计入会污染命中率口径。 */
+    {
+        ULONG64 vid2 = (ULONG64)InterlockedOr64(&s_VolId, 0);
+        if (vid2 != 0 && Extp && (ULONG64)((PRB_DEV_EXT)Extp)->VolId != vid2)
+            goto cleanup;
+    }
```

### 1.3 diff 2 — `rbL2.c` `RbL2Store` 入口（:1174-1181）

同样加 `vid != 0`，**不**加打开后复检（理由见 §4 R2）：

```diff
@@ -1171,9 +1185,14 @@ VOID RbL2Store(PVOID Extp, ULONG64 lba, ULONG len, PVOID src, ULONG64 Gen)
     }
-    /* [FIX R6-01] 存储的卷身份闸（同探测）：非本容器的卷不写（防覆写他卷记录） */
-    if (Extp && ((PRB_DEV_EXT)Extp)->VolId != InterlockedOr64(&s_VolId, 0)) {
+    /* [FIX R6-01] 存储的卷身份闸（同探测）：非本容器的卷不写（防覆写他卷记录）
+     * [FIX B1 2026-11-15] s_VolId==0 时放行，理由同 RbL2ProbeFill：本门与探测门
+     * 同构，若只修一处，「L2 从未打开 + 仅发生淘汰」这条次路径仍会被拒。 */
+    {
+        ULONG64 vid = (ULONG64)InterlockedOr64(&s_VolId, 0);
+        if (Extp && vid != 0 && (ULONG64)((PRB_DEV_EXT)Extp)->VolId != vid) {
         static volatile LONG s_RejVol = 0;
         if ((InterlockedIncrement(&s_RejVol) & 0xFFF) == 1)
             RbLog(1, __LINE__, 0, L"l2: store rejected (vol identity) [diag O9]");
         return;
+        }
     }
```

### 1.4 预期效果与判据

| 观测 | 修前（`250` 实测） | 修后预期 |
|---|---|---|
| 重启后 `pre state`（`flags=0x4`） | 0 | **1**（首个 miss 自行打开） |
| 同引导 `l2:` 日志 | 0 行 | 应有拒绝/打开相关行（取决于是否 attach） |
| `dL2Miss`（首次读） | +1037 且 `state` 原为 0 | 同样 +N，但 `state` 已为 1 |
| 人工 `refresh` 必要性 | 必须 | **不需要** |

**这不是 J6 PASS**：跨引导命中仍被 B2 阻断（`250` §6 C1 段）。P1 只让「L2 在每个引导内自己可用」成立。

---

## §2 P2 — B2 打点：只加日志，不改任何行为

### 2.1 待答的两个问题

1. `IRP_MN_SET_POWER && Type == SystemPowerState` 是否曾经到达过本驱动的卷过滤设备？（若从未到达 ⇒ `rbmsg.c:239-258` 是死代码，与 `250` §3 的推断一致）
2. 到达的 `DevicePowerState` SET_POWER 里 `ShutdownType` 是什么？（决定 P4 能否用它区分关机/睡眠/休眠）

### 2.2 diff — `rbmsg.c` `RbDispatchPower`（插在 `ext = ...` 之后、`PoStartNextPowerIrp` 之前）

```diff
@@ -225,6 +225,30 @@ NTSTATUS RbDispatchPower(PDEVICE_OBJECT dev, PIRP irp)
     }
     ext = (PRB_DEV_EXT)dev->DeviceExtension;
 
+    /* [DIAG B2 2026-11-15] 电源 IRP 可见性打点：只读，不改任何行为。
+     * 待答：本设备栈是否曾收到 Type==SystemPowerState 的 SET_POWER（若否，
+     * :239-258 即死代码）；以及 DevicePowerState 侧的 ShutdownType 能否
+     * 区分关机/睡眠/休眠。编解码：RbLog 的 Status 参数携带 pack
+     *   pack = (MinorFunction&0xF)<<28 | (ShutdownType&0xFFF)<<16 | State&0xFFFF
+     * 日志侧按 0x%08x 解码。节流：每个 (Minor,Type) 组合前 2 次各一行。 */
+    {
+        static volatile LONG s_PwrSeen[16];
+        ULONG isSys = (sp->Parameters.Power.Type == SystemPowerState) ? 1u : 0u;
+        ULONG slot  = (((ULONG)sp->MinorFunction & 0x7u) << 1) | isSys;
+        ULONG stt   = isSys ? (ULONG)sp->Parameters.Power.State.SystemState
+                            : (ULONG)sp->Parameters.Power.State.DeviceState;
+        ULONG pack  = ((((ULONG)sp->MinorFunction) & 0xFu) << 28) |
+                      ((((ULONG)sp->Parameters.Power.ShutdownType) & 0xFFFu) << 16) |
+                      (stt & 0xFFFFu);
+        if (InterlockedIncrement(&s_PwrSeen[slot]) <= 2)
+            RbLog(1, __LINE__, (long)pack,
+                  isSys ? L"pwr: SYSTEM power irp (pack=minor<<28|sdtype<<16|sysstate)"
+                        : L"pwr: DEVICE power irp (pack=minor<<28|sdtype<<16|devstate)");
+    }
+
     PoStartNextPowerIrp(irp);
```

### 2.3 解码表

`pack` 十六进制 `0xM T T T S S S S`：

| 位 | 字段 | 取值含义 |
|---|---|---|
| 31-28 | `MinorFunction` | `4=IRP_MN_SET_POWER`，`3=IRP_MN_QUERY_POWER`，`0=IRP_MN_WAIT_WAKE`，`2`… |
| 27-16 | `ShutdownType` | `0=PowerActionNone`，`1=Sleep`，`2=Hibernate`，`3=Shutdown`，`4=ShutdownReset`，`5=ShutdownOff` |
| 15-0 | `State` | `SystemState` 或 `DEVICE_POWER_STATE` |

### 2.4 判读规则（关机一次即可定论）

| 日志里出现的组合 | 结论 |
|---|---|
| 只有 `DEVICE power irp`，`minor=4`，`sdtype=3/4/5` | `rbmsg.c:232` 恒不成立**得到证实**；P4 必须改判据（用 `ShutdownType`），**不能**只改成 `DevicePowerState` |
| 只有 `DEVICE`，且 `sdtype=0`（None） | 更糟：连 `ShutdownType` 也没透传 ⇒ P4 需走 `IoRegisterShutdownNotification` 或别的信号源 |
| 出现 `SYSTEM power irp`，`minor=4` | `250` §3 的推断**被推翻** ⇒ 死代码假设错误，要回头查"为什么走到却不写"（例如 `IRQL != PASSIVE` 导致 `RbL2IdxSave` 直接 FALSE，但那样会留日志，与实测矛盾） |
| 完全没有 `pwr:` 行 | `IRP_MJ_POWER` 根本没到本设备对象 ⇒ 比死代码更严重，需查 `AddDevice` 是否挂到正确位置 |

### 2.5 成本

一处 `if` + 一行日志，零行为变化。唯一副作用 = 日志多 2–4 行/次电源事件。**可与其他改动同批部署**（一次 UAC、一次重启，符合 R8）。

---

## §3 P3 — B3：`l2_store_warmup.py` 判据改三态

### 3.1 现状问题（`250` §5）

`--expect hits` 在「空索引 + 顺序首读」下必然 `NOT MET`，而脚本第 231-233 行还会打印「L2 not usable in this state; capture the driver log」，把工况不成立导向「驱动坏了」。

### 3.2 改动点

1. 新增 `parse_idx_count(path)`：读 `.idx` 前 32 B，`Count = u32@offset12`；文件 <32 B ⇒ 返回 `None`。
2. 判据改三态：

```python
# 原
if a.expect == "stores":
    ok = d["L2Stores"] > 0
elif a.expect == "hits":
    ok = d["L2Hits"] > 0
print("  RESULT: %s" % ("PASS" if ok else "NOT MET"))

# 新
verdict = "NOT MET"
if a.expect == "stores":
    if d["L2Stores"] > 0:
        verdict = "PASS"
    elif d["coreMiss"] == 0:
        verdict = "INCONCLUSIVE (reads never reached the filter)"
    elif idx_count == 0 and d["L2Miss"] > 0:
        verdict = "INCONCLUSIVE (COLD-START: no eviction of hot blocks yet; store is triggered only by RbFeatOnEvict with Hits>=2)"
elif a.expect == "hits":
    if d["L2Hits"] > 0:
        verdict = "PASS"
    elif idx_count == 0:
        verdict = "INCONCLUSIVE (COLD-START: index Count==0, nothing to hit by construction)"
    elif d["coreMiss"] == 0:
        verdict = "INCONCLUSIVE (reads never reached the filter)"
print("  RESULT: %s" % verdict)
```

3. `stores` 模式改「二次读 + 触发淘汰」：先读一遍全量（让 chunk 进 core 且 `Hits` 累计），再读第二遍（每块 `Hits>=2`），再等待/驱动一次核心回收。**该模式本轮无法验证**（容器已删，需重新备料），故只写设计不写实现，列为 P3-b。
4. 退出码：`PASS=0` / `NOT MET=1` / `INCONCLUSIVE=5`（与夹具 `UNCOVERED=5` 口径一致）。

### 3.3 影响面

只影响取证工具的判读文案与退出码。**不改驱动、不影响门禁**。`check_ps_syntax.ps1` 不覆盖 `.py`，但 `tools/audit_sweep.py` 会扫。

---

## §4 红队审查点（逐条给"为什么可能被否"和"我的回答"）

| # | 质疑 | 回答 | 状态 |
|---|---|---|---|
| R1 | 放行 `s_VolId==0` 会不会让第二卷抢到容器身份？ | 不会。身份取自 `RbL2AttachedVolId()`（`:707`，语义 = **唯一**被加速卷；`[D4]` 单卷门在 >1 时拒绝打开），不是取自请求卷。打开后 `s_VolId!=0`，所有后续请求（含第二卷）恢复严格比对。 | 待审 |
| R2 | 为什么 `RbL2Store` 不加打开后复检？ | Store 不承担首次打开职责：它只在 `RbFeatOnEvict`（`rbfeat.c:867`）被调用，而该回调要求 core 有 chunk ⇒ 必然先发生过读 ⇒ `RbL2ProbeFill` 已把容器打开、`s_VolId` 已就位。Store 的懒开（`:1189`）只是保底。**若红队要求对称**，加 4 行即可，成本可接受。 | 待审 |
| R3 | 打开后复检用 `goto cleanup` 会不会漏 `Rb2Count`？ | 有意为之：该卷本就不该被加速，计入 `L2Miss` 会污染命中率口径（与语义不符）。若红队坚持要可观测，改用独立计数器（新增 slot 会动 `rb_common2.h` 的 ABI，不建议本轮做）。 | 待审 |
| R4 | `InterlockedOr64(&s_VolId, 0)` 的返回值是有符号 `LONG64`，直接比 `ULONG64` 安全吗？ | 安全。`VolId` 定义在 `rb_common.h:152` 为 `ULONG64`；`Or 0` 是纯读，不会产生负值语义；比较前显式 `(ULONG64)` 转换，避免隐式整型提升告警（`/W4` 门禁）。 | 待审 |
| R5 | 打点用 `RbLog` 的 `Status` 参数传 `pack`，会不会影响既有日志消费者？ | `RbLog(unsigned Level, unsigned Src, long Status, const WCHAR *Text)`（`rb_common.h:469`）。既有消费者按 `Text` 匹配，新增 `pwr:` 前缀不会被误判；`Status` 只是被打印成 `0x%08x`。**先例**：`rbctl.c:269` 已把布尔塞进该参数。 | 待审 |
| R6 | 打点会不会在关机时洪泛？ | 有节流：每 `(Minor,Type)` 组合前 2 次。最坏 16×2=32 行/引导。 | 待审 |
| R7 | P1 会不会让「多卷」场景行为改变？ | 现在只有 `D` 加速（`volumes: 7`、`D: accelerated`），单卷门下多卷本不可用。P1 不改变单卷门，只是把「首次打开」的入口从控制面扩展到数据面。**多卷支持是未启用能力**，若将来启用需重新设计这两道门。 | 待审 |
| R8 | 三段一起部署是否符合 R8（一次 UAC、一次重启）？ | 是。P1/P2/P3 合并为一次重编重签部署；P3 是脚本，无需部署。 | 待审 |

**红队必须否决的情况（我主动列出）**：

- 若认为「`s_VolId==0` 放行」在任何情形下都不可接受 ⇒ 备选方案是**把身份门移到懒开之后**（而不是加 `vid!=0` 条件），语义更干净但 diff 更大（要把 `:1093` 整块下移到 `:1103` 之后），需重新评估 `s_Ops` 计数窗口。
- 若认为 P2 打点属于「改动内核」必须单独一轮 ⇒ P2 单独部署，P1 推迟。

---

## §5 回滚预案

| 阶段 | 回滚动作 | 耗时 | 风险 |
|---|---|---|---|
| 仅出方案（当前） | 无需动作（源码零改动） | — | 零 |
| 已改源码未编译 | `git checkout -- src/driver/rbL2.c src/driver/rbmsg.c` | 秒级 | 零（R1：不提交，工作区可弃） |
| 已编译未签名 | 删 `src/driver/x64/Release/*.sys` | 秒级 | 零（`dist/` 与锚点未被触碰） |
| 已签名未部署 | 重签回 `7125d219…` 对应源码并重编，或直接 `deploy.ps1` 用 `dist/` 旧件 | 分钟级 | 低 |
| **已部署** | `deploy.ps1` 重放 `dist/RAMBlockCache.sys`（`7125d219…`）+ `sync_anchors.ps1` + 重启一次 | 一次 UAC + 一次重启 | **中**：部署件已在 `UpperFilters` 中，替换失败会进 0x7B 风险区，必须验签通过后才重启 |
| 部署后蓝屏 | 安全模式 → `sc config` 摘 `UpperFilters` → 正常启动 → 重新部署 | 30+ 分钟 | 高（但**本期不涉及**：P1/P2 不碰 flush/IO 语义，蓝屏概率低） |

**关键约束**：任何「已部署」回滚都必须先 `verify_deploy_signature.ps1` 通过再重启（R2）。

---

## §6 门禁与验证矩阵

### 6.1 门禁（每次改源码后必须全绿）

```powershell
& $pwsh -File build\build.ps1 -Target driver     # 编译（W4，0 警告）
& $pwsh -File build\build.ps1 -Target core       # 回归网（PASS=86 FAIL=0）
& $pwsh -File build\build.ps1 -Target gates      # ALL GATES PASS（21 条）
& $pwsh -File src\scripts\qa_w4check_20261020.ps1
& $pwsh -File src\scripts\qa_teeth.ps1
```

- Gate22 编号：本文件落地后 `max=251 next=252`（`AGENTS.md:32` / `:131` + `HANDOVER-CURRENT.md` 三处同步）
- `tools/audit_sweep.py --all --fail-on-high` 必须 `HIGH=0`
- `check_proto_sync`：P1/P2 **不碰** `RB_CONFIG` / `RB2_L2HDR` / IOCTL 编号 ⇒ 不受影响

### 6.2 真机验证矩阵

| # | 动作 | 判据 | 证明什么 |
|---|---|---|---|
| V1 | 部署 + 重启一次 | `PASS=8 FAIL=0`（`RB-postreboot-verify.cmd`） | 部署链未破损 |
| V2 | `Features\Flags=0x4`（`l2 enable`），**不发 refresh** | 读 `D:\` 上任意文件后 `state` 由 0→1 | **P1 生效** |
| V3 | 关机一次（正常重启），看日志 | 出现 `pwr:` 行且能解码 | **P2 判读**（决定 P4 走向） |
| V4 | 再次重启后 `state` | 有 `.idx` 则应为 1 且可载入 | P4 之前仍应冷启动（B2 未修） |
| V5 | `RB-l2-p0c-rollback.cmd` 收尾 | `Flags=0`、`PASS=6 FAIL=0` | 现场复原 |

**注意 V2 的窗口**：`l2 enable` 会写 `Flags|=4`，11-16 09:30 巡检窗口内必须先回滚。**建议 V1–V5 安排在巡检之后或当场收尾**。

---

## §7 实施顺序与工作量

| 批次 | 内容 | 代码量 | 编译/签名 | 真机 | 风险 |
|---|---|---|---|---|---|
| A | P1 + P2 + P3 | ~40 行（含注释） | 1 次 | V1–V3、V5 | 低 |
| B | P4（B2 行为） | ~80 行（延迟完成 IRP + 工作项） | 1 次 | 需 2 次干净重启 + 1 次非干净 | **中高** |
| C | J6 复测 | 0（复用 `RB-l2-j6-*.cmd`） | 0 | ≥2 次重启 | 中 |

**P4 的前置条件（缺一不可）**：P2 日志已取到、解码结论明确、独立红队 + 独立复核通过、用户拍板（R10）。

---

## §8 本方案未覆盖（不得据此宣称）

- J11 陈旧索引、哈希冲突、逐字节一致性（J3 仅 checksum）、任何性能数字
- P4 的具体实现（延迟完成 power IRP 的工作项骨架、超时收尾）—— 待 P2 判读后再出第二份设计
- `RbRegSetClean(1)` 首次真正生效后，`rbdriver.c:302-331` 的 BootFailCount/SafeMode/self-disable 安全网行为变化 —— **这是 A/B 批次里最容易被忽略的回归点**，必须在 P4 验证矩阵中单列一项（连续 2 次干净重启 + 1 次非干净重启，观察 `BootFailCount` 与是否误进安全模式）

---

## §9 证据来源

| 项 | 位置 |
|---|---|
| B1 决策依据 | `250` §4；`rbL2.c:702-709`（S7-06 注释）、`:1093`、`:1101`、`:707`、`:1175` |
| B2 打点依据 | `250` §3；`rbmsg.c:225-259`（`:232-233` 条件）、`rb_common.h:469`（`RbLog` 签名） |
| B3 判据依据 | `250` §5；`rbfeat.c:819/867/886`、`rbL2.c:1153`；`l2_store_warmup.py:238-257` |
| VolId 类型 | `rb_common.h:152`（`ULONG64`） |

---

## §10 落地回执（15:3x–15:4x，A 批次已实施并验证）

**范围**：P1 + P2 + P3 已全部写入源码并编译、跑门禁。**未签名、未部署、未改 `dist/`、未改三锚点、未改观察期 pin、未提交 git。**

### 10.1 实际改动（相对改前备份，非相对 git HEAD）

| 文件 | 行数 | 内容 |
|---|---|---|
| `src/driver/rbL2.c` | +37 / −8 | P1：`RbL2ProbeFill` 入口放行 + 打开后复检（新增 `vid`/`vid2` 两个 12 行注释块）；`RbL2Store` 入口放行 |
| `src/driver/rbmsg.c` | +21 / −0 | P2：`RbDispatchPower` 插入 20 行只读打点 + 1 行空行 |
| `src/scripts/l2_store_warmup.py` | +59 / −7 | P3：新增 `idx_count()`；判据改三态；`RESULT`/退出码/`OBS` 文案更新 |

> `git diff --stat` 显示的 `rbL2.c 753 行` 是相对 **git HEAD** 的差异（含 11-14 那批历史未提交改动，R1 一直未提交），不是本轮增量。上表数字来自 `diff -u` 对比 `.workbuddy/patch-251-backup/`。

### 10.2 构建与门禁结果（全部实跑）

| 检查 | 命令 | 结果 |
|---|---|---|
| 驱动编译 | `build.ps1 -Target driver` | **BUILD OK**，30 个源文件全 `[ok]`（含 `rbmsg.c`、`rbL2.c`），未签名件 **190,464 B / `641a0a13b7819b822c49331f8ba88226446bfec3e2ad2260f225da1038f9d5ab`** |
| core 回归 | `build.ps1 -Target core` | `compiled=66 skipped=0`、**`RUN_FAIL=0`** |
| 全门禁 | `build.ps1 -Target gates` | **ALL GATES PASS**（`result: PASS (0 fail, 0 warn)`） |
| PS 语法门 | gates 内 `check_ps_syntax` | `files=73 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=41 cmd_chr=0 cmd_bom=0` |
| 签名链 | gates 内 `check_signature` | `Valid`；`dist/RAMBlockCache.sys` = **`7125d219…`**，三锚点全同 ⇒ **`dist/` 未被本次编译触碰** |
| /W4 | `qa_w4check_20261020.ps1` | cache TU `6/6`，`rc=0 warnings=0`，`PASS` |
| 门禁有牙 | `qa_teeth.ps1` | `PASS`；gate series `killed=3/3`；`survived-expected=5/6` |

**关于 `teeth` 里那条 `[KILLED!] wsconc-rewrite-p21-slowwb : unexpected kill`**：本次改动不涉及 `cache_flush.c` 与 `wsconc` 侧代码，该条的 `ok`/`KILLED!` 分类属既有基线状态，`qa_teeth` 整体仍 `PASS`。**不用它证明或否证本次补丁**，仅记录。

**关于未签名件比线上件小 7,496 B**：`190,464`（未签名）vs `197,960`（`7125d219…`，Authenticode 签名件）。差额 = Authenticode 证书 + PKCS#7 结构，属预期，**不是代码被删**。

### 10.3 现场状态

- `dist/RAMBlockCache.sys` = `7125d219…`（`Valid`）—— 线上部署件未变
- `src/driver/x64/Release/RAMBlockCache.sys` = `641a0a13…`（未签名，仅编译产物）
- 服务、`UpperFilters`、`Features\Flags`（当前 `0x0`）、容器/索引/负载（已删）**全部未动**
- 因此**不需要**任何回滚动作即可回到「已验证的部署态」

### 10.4 回滚（若要放弃本轮改动）

```bash
cp .workbuddy/patch-251-backup/rbL2.c          src/driver/rbL2.c
cp .workbuddy/patch-251-backup/rbmsg.c         src/driver/rbmsg.c
cp .workbuddy/patch-251-backup/l2_store_warmup.py src/scripts/l2_store_warmup.py
```

改前哈希：`rbL2.c = 65b80950e7e57589a82c365f97c4473afe2006969ca3ce64918158c660713b7b`、`rbmsg.c = f49ca16decc4bbc6f1262059892069d875c17843bdf6078037432193f236d026`、`l2_store_warmup.py = 3f84c81e4d86312c9c9100225ed4b5c6261035e72aedb0100f9f9f64ac9a342d`。

### 10.5 下一步（需用户拍板才执行）

1. **签名**：对 `641a0a13…` 签名（ASCII stage `C:\ProgramData\rb-deploy` 内签完再拷回）
2. **同步锚点**：`sync_anchors.ps1`（三锚点 + pin 一起换）
3. **部署**：`deploy.ps1` → **一次 UAC、一次重启**（R8）
4. **验证**：V1（`RB-postreboot-verify.cmd` `PASS=8 FAIL=0`）→ V2（`l2 enable` 后**不发 refresh**，读 `D:\` 文件看 `state` 0→1，这是 P1 生效的干净判据）→ V3（关机一次看 `pwr:` 行，定 P4 走向）→ V5（回滚 `Flags=0`）

**注意 V2–V3 的时间窗口**：`l2 enable` 会写 `Flags|=4`，观察期 11-16 09:30 巡检前必须回滚（`RB-l2-p0c-rollback.cmd`）。
