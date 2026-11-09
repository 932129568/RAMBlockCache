# 130 · 主动激活取证：00:32 池腐坏根因点名与源码修复（2026-11-09 20:1x–20:2x）

> 用户指令：**「想办法深度排除，可以主动激活bug找原因」**。
> 结论：**根因找到了，不是「不可考」** —— 是当前运行镜像**仍然存在**的确定性内核池越界写；已在源码层修复、编译验证、加防回归门禁，但**尚未部署**。00:32 的 `rbx=0x0075006F00280020` 已被模型精确复现。

---

## 1. 对 129 号结论的更正（必须如实）

| 129 号判定 | 本批复核 |
|---|---|
| 「被覆盖池块的具体 `ExAllocatePool2` 分配点」物理不可达 | ✅ 仍然成立（triage dump 无完整内存、该代 PDB 丢失、栈上无我方帧） |
| 「写入源/越界点无法确证，只推断释放后重用」 | ❌ **错误**。`RbLogSnapshot` 的越界写可直接静态确证并用模型复现 |
| 「00:11 代缺陷族在当前代码库已全部消除」 | ❌ **错误**。129 号只审计了 `git diff c6d36c4c..HEAD` 的**改动**，漏审了**未改动代码**中的契约；`RbLogSnapshot` 两代逐字相同 |
| I10 状态 ✅ 已闭环 | ⏳ **撤回**，改见 §7：源码已修、待 UAC 部署 + 重启真机复测 |

方法论教训：差分审计只能发现「改过的地方」，不能证明「没改过的地方正确」。本批改为按**数据流契约**（谁写、写多少、目标容量多少）逐条审计才命中。

---

## 2. 根因（精确到代码行与字节）

`src/driver/rbdriver.c` 的 `RbLogSnapshot`（被 `RbMsgGetLog` → `RB_MSG_GET_LOG` 调用）：

```c
valid = (g_LogHead < RB_LOG_CAP) ? g_LogHead : RB_LOG_CAP;   /* <= 512 */
for (i = 0; i < valid; i++) {          /* n 无任何上界钳制 */
    src = &g_Log[idx]; dst = &out->Items[n];
    RtlStringCchCopyW(dst->Text, RB_LOG_TEXT_MAX, src->Text);
    n++;
}
```

| 几何量 | 值 |
|---|---|
| `RB_LOG_CAP` = `RB_LOGQ_MAX`（日志环容量） | **512** 条 |
| `RB_LOG_REC`（Ticks8+Level4+Src4+Status4+pad4+Text[48]×2，pack=8） | **120 B** |
| `RB_LOG_LIST` = 8 + `Items[64]`×120 | **7,688 B** |
| `RB_MSG` = 16 + union 最大成员 8,208 B | **8,224 B**（`msg->Data` 可用 8,208 B） |
| 最坏写入 = 8 + 512×120 | **61,448 B** |
| **写到相邻池块的越界量** | **53,240 B**（n ≥ 69 起；n=65..68 仍落在 union padding 内） |
| 相对 `RB_LOG_LIST` 声明容量的溢出 | 53,760 B |

写入内容是**日志文本（Unicode）**，其中 `src/driver/rbL2.c:650` 的
`L"l2: store rejected (out of window) [diag O9]"` 含 `" (ou"` 四个 WCHAR：
`20 00 28 00 6f 00 75 00` 按小端读成 64 位即 **`0x0075006F00280020`** —— 与 00:32 dump 的 `rbx=rax` **逐字节一致**。堆压缩线程 `ExpHpCompactionRoutine` 遍历空闲块链表时把这些文本当成指针解引用，于 `nt!RtlpHpVsSlotCompactChunks+0x210` 触发 `0x7E/0xC0000005`。

`git diff c6d36c4c..HEAD -- src/driver/rbdriver.c` 对 `RbLogSnapshot` **无任何差异** ⇒ 00:11 代与当前 B 批镜像都含此缺陷。

---

## 3. 为什么偶发、为什么之后没复现（判别性解释）

| 条件 | 说明 |
|---|---|
| 触发门槛 | 环内日志 **n ≥ 69** 且调用 `RB_MSG_GET_LOG`（method 9） |
| 安全路径 | 桥接 `log-sink.ts` 用 `IOCTL_RB_LOGQ`(0x809) → `RbLogQueryPair`，该函数有 `for (i = 0; i < valid && n < maxCount; i++)` 且 `maxCount` 钳到 32 ⇒ **不会越界** |
| 危险入口 | `web/src/bridge/http-transport.ts:274`（`getLog()` 用 `RB_MSG.GET_LOG`）、`src/ui/controller/.../DriverService.cs:199`（`MSG_GET_LOG = 9`）、桥接 `index.ts:107` 仍注册 `9: decodeLogList` |
| 重启归零 | 18:39 重启后 `g_LogHead` 归零；**真机实测 `method=9` 返回 `Count=25 / Cursor=25`**，未达阈值 ⇒ 当前调用安全 |
| 00:32 当天 | 开机后日志累积越过 69 条，某个 UI/CLI 调用 method=9 → 越界 53 KB → 池腐坏 → 堆压缩线程延迟发现；之后重启使环归零，再没有调用方在环满时触发 method=9 |

这条因果同时解释了 128 号 C3（200 PID / 10 万次 churn）为何没复现：压测根本**没有调用 method=9**。

---

## 4. 主动激活：零风险用户态模型 D2（6/6 判据 PASS）

`C:\Users\Public\o9\stress\d2_logsnapshot_overflow.py` 按真实结构布局模拟 `RbLogSnapshot` 写满 512 条：

| 判据 | 观测 |
|---|---|
| J1 越界量 | **53,240 B**（与静态几何一致） |
| J2 越界区含 UTF-16 `" (ou"` | 命中，位于越界区偏移 300 |
| J3 **复现 dump 的 `rbx`** | `rbx_value_present = true`（`0x0075006F00280020`） |
| J4 会被当指针的 UTF-16 值 | **19,136** 个 8 字节窗口每个 uint16 均为可打印 ASCII |
| J5 修复版（钳位） | 512 条 → `n=64`、**0 越界** |
| J6 短环（40 条）当前实现 | **0 越界**（潜伏性对照） |

**为什么没有在真机上直接激活越界**：真机复现需要先人为制造 ≥69 条日志再调 `method=9`，届时驱动会向池写 53 KB 越界数据 —— 破坏范围与落点不可控（可能不是干净 bugcheck，可能破坏关键内核结构）。根因已由静态契约 + 模型确定性证明，且 `rbx` 值被精确复现，真机复现的边际证据价值低于风险。激活入口与门槛已在本报告记录，若需真机确认可由用户决定执行。

---

## 5. 修复（本批真正改了驱动源码）

| 文件 | 改动 |
|---|---|
| `src/driver/rbdriver.c` | `RbLogSnapshot` 循环改为 **`for (i = 0; i < valid && n < RB_LOG_MAX; i++)`**，并写入契约注释（几何、根因、对外行为不变）。对外仍返回「最近 64 条」；完整 96 字符文本由 0x809 LOGQ 分页提供 |
| `src/scripts/check_poolcopy_contract.py`（Gate13） | 新增 **A8** `RbLogSnapshot` 钳位断言、**A8b** `RbLogQueryPair` 保持 `n < maxCount`、**A8c** `RB_LOG_MAX=64` ABI 校验 |
| `c5_gate13_mutation.py` | 新增 **V6**（去掉钳位）变异，骨架补入 `rbdriver.c` / `rb_proto.h` |

**验证**：

- Gate13 baseline **PASS**（A1–A8 全绿）；变异 **6/6 KILLED**（含 V6 去钳位）+ 还原 PASS，真实仓库零改动。
- **10/10 Python 门禁**全绿；`analyze_bugcheck_dump.py --all` 仍 `insufficient=10`、fail-closed 不假绿。
- 驱动 `_build_manual.ps1` **BUILD OK**：新 sys `cb16a2cdaa25238557c90948cc7290a384e43f66e1c7ebaa2f706b8871a7787f` / **179,712 B / 未签名，禁止部署**。
- `d2` 模型复跑 PASS；`d3_verify_after_reboot.py` fail-closed 预演正确输出 `is_pre_fix=true → VERDICT: FAIL`。

---

## 6. 当前状态与即时风险

- 运行镜像与 `dist` 仍是 **`29527798…`（B 批，含本缺陷）**，签名 Valid；三锚点未变，`check_signature` PASS；`o9_watch` `FAIL=0 WARN=0 VERIFIER_EVENT=0`。
- 真机 `method=9` 当前 `Count=25`，**安全**；但只需再产生 44 条日志并打开 WPF/Web 日志页即可复现 00:32 的越界。
- **部署修复前**：不要打开 WPF 控制台/Web 的日志页、不要调用 `GET_LOG`(method 9)；其余正常使用无影响。
- 全程未重启、未部署、未改配置、未制造崩溃。

---

## 7. 台账状态更正

- I10：由 129 号的「✅ 已闭环」改回 **「🔧 根因已点名 + 源码已修，⏳ 待 UAC 部署 + 重启真机复测」**；「被覆盖块的分配点」仍登记为不可考，但「写入源」已确证。
- 新增 **I43** 记录本缺陷、修复与部署要求；口径纪律段注明 **129 号闭环结论被 130 号部分推翻，以 130 号为准**。

---

## 8. 下一步（用户 2 步，顺序不可颠倒）

1. 双击 `C:\Users\Public\o9\RB-fix-deploy.cmd`（一次 UAC）→ 等 `RESULT: OK`。脚本会签名 `cb16a2cd…`、按哈希归档 PDB、同步 `deploy.ps1` / `verify_deploy_signature.ps1` / `README.txt` 三锚点、部署、跑门禁⑤。
2. **重启一次**并登录（Verifier 保持在岗）→ 告诉我，我立即跑 `python C:\Users\Public\o9\stress\d3_verify_after_reboot.py`。

复测判据：镜像守卫（≠ `29527798…`）；**20 次 `method=9`（含 8 并发）每次 `status=0` 且 `Count ≤ 64`**；池标签配对；零新增 dump / ERROR；canary 与配置未变；D2 模型复跑 PASS。可选再点一次 `RB-setup-repack.cmd` 同步分发载荷。

---

## 9. 证据索引

| 类别 | 路径 |
|---|---|
| 根因模型 | `C:\Users\Public\o9\stress\d2_logsnapshot_overflow.py` / `d2_result.json` / `d2_raw.txt` |
| 变异测试 | `c5_gate13_mutation.py` / `c5_result.json`（6/6 KILLED） |
| 部署后复测 | `d3_verify_after_reboot.py`（fail-closed 预演已通过） |
| 源码修复 | `src/driver/rbdriver.c`（`RbLogSnapshot` 钳位）、`src/scripts/check_poolcopy_contract.py`（A8） |
| 编译产物 | `src/driver/x64/Release/RAMBlockCache.sys`（`cb16a2cd…`，未签名） |
| 真机取证 | `method=9` 实测 `Count=25 / Cursor=25`；`web/src/bridge/http-transport.ts:274`、`DriverService.cs:199` 为危险入口 |
| 前序 | `deliverables/128`、`129`（结论按 §1 更正） |

> 纪律：本批「已修」仅指**源码 + 编译 + 门禁 + 模型**落地；内核真机结论以部署后 `d3` 复测为准。未签名 sys 禁止部署。
