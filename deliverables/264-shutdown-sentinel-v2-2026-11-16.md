# 264 — 第二版诊断件：同步持久化哨兵 + 只读判读脚本（编译全绿，未签名未部署）

**日期**：2026-11-16 00:5x（本地）
**触发**：用户选路线 B —— 出第二版诊断件，用同步哨兵解决 `263` 发现的「日志非同步落盘」歧义。
**性质**：**源码已改、已编译、已回归、门禁全绿；未签名、未部署、未重启、未改 `dist/` 与三锚点。** git 未提交。

---

## 0. 结论

第二版诊断件 `20eafc1a…`（未签名，191,488 B）与只读判读脚本 `src/scripts/read_shutdown_sentinel.py` 已就位。它把「关机时是否派发 IRP」与「关机时文件 IO 是否可用」拆成**两个独立的同步持久标记**，形成三态判别器：

| 注册表 `DiagShutdownSeq` | 哨兵文件记录 | 结论 |
|---|---|---|
| 递增 | 条数匹配 | **IRP 派发 + 关机时 `Zw*File` 可用** ⇒ 生产补丁的 `RbL2IdxSave` 有可行性前提 |
| 递增 | 条数不足/无文件 | **IRP 派发了，但关机时文件 IO 不可用** ⇒ 生产补丁即使收到 IRP 也救不了 B2 |
| 不变 | — | **IRP 未派发到该控制设备** |

编译与门禁：`-Target driver` **BUILD OK**（30 源文件全 ok）、`-Target core` `RUN_FAIL=0`、`-Target gates` **ALL GATES PASS**、`qa_teeth killed=3/3 PASS`、`qa_w4check cache TU 6/6`、离线单测五种组合全过、部署前基线 `reg=0 / 0 B / rc=3`。

---

## 1. 为什么需要第二版（`263` 的观测缺陷）

`rblog.c:35-60` 的 `RbLog()` **只写内核内存环 `g_Log[512]`，全文无 `ZwWriteFile`**；磁盘日志由用户态经 `IOCTL_RB_LOGQ (0x809)`（`rbctl.c:1047`）旁路抽取。第一版两轮实验中，只有 `Power Action Reboot` 那次留下 `diag:` 行，`Power Action Shutdown Off` 那次没有 —— **无法区分「未派发」与「日志没落盘」**。

第二版不再依赖日志作判据：两个标记都在 `IRP_MJ_SHUTDOWN` 分派内**同步写**，`RbLog` 降级为 best-effort（其 `Status` 改放哨兵写入的 NTSTATUS）。

---

## 2. 实际改动（改前备份 `.workbuddy/patch-264-backup/`）

| 文件 | 增量 | 内容 |
|---|---|---|
| `src/driver/rbmsg.c` | **+76 / −22**（326 → 380 行） | 把 `RbDispatchShutdownDiag` 升级为双标记；`rbdriver.c` / `rb_common.h` **零改动**（哈希与备份完全一致） |
| `src/scripts/read_shutdown_sentinel.py` | 新增 | 只读判读脚本（三态结论 + 时间戳便于与事件日志对齐） |

编码：`rbmsg.c` UTF-8 无 BOM + CRLF 原样保留；脚本 UTF-8 无 BOM + LF。

### 2.1 两个标记

```c
/* marker 1: registry, monotonically increasing ACROSS BOOTS */
ULONG prev = 0, total;
(void)Rb2GetDW(L"Parameters", L"DiagShutdownSeq", 0, &prev);
total = prev + 1;  if (total == 0) total = 1;
RbRegSetDW(L"DiagShutdownSeq", total);       /* rbdriver.c:76, synchronous ZwSetValueKey */
rec[1] = total;
/* marker 2: synchronous append with FILE_WRITE_THROUGH */
fst = RbDiagWriteSentinel(rec);
```

`Rb2GetDW`（`rb_common2.h:121`）读同一个 `Parameters` 键做**读-改-写累计**——这是必需的：分派内的 `s_DiagShutdownSeen` 是每映像计数，冷启即归零，两轮实验会都写成 1，无法累计。

哨兵文件 `C:\ProgramData\rb-deploy\shutdown-sentinel.bin`，`ZwCreateFile` 用 `FILE_APPEND_DATA | SYNCHRONIZE` + `FILE_OPEN_IF` + `FILE_WRITE_THROUGH`，`ZwWriteFile` 追加 24 字节定长记录（避开结构体填充歧义）：

```
ULONG[0] Magic 0x52424433 ('RBD3')
ULONG[1] total（与 DiagShutdownSeq 同值）
ULONG[2] KeGetCurrentIrql()
ULONG[3] 1 表示 dev == g_Rb.ControlDevice
ULONG[4..5] FILETIME 低/高 32 位（KeQuerySystemTimePrecise）
```

### 2.2 严格不变的边界

- **不调用** `RbFlushAllDirty` / `RbHotSnapshot` / `RbL2IdxSaveGuarded` / `RbRegSetClean`；**不引入一次性闩**；`RbDispatchPower` 原样保留。
- `DriverEntry` 的注册/注销、`MajorFunction[IRP_MJ_SHUTDOWN]` 分派、`Features\Flags`、回写/WBCOAL、L2 窗口全部未动。
- 唯一新增的「状态写入」是**诊断标记**：一个 `Parameters` 下的 DWORD 和一个 24 B/次的哨兵文件；不写用户数据、不碰缓存/L2 索引。
- `dist/` 与三锚点仍是第一版签名件 `2053c33b…`；System32 仍 `2053c33b…`。

---

## 3. 编译与门禁（本轮实跑）

| 检查 | 结果 |
|---|---|
| `-Target driver` | **BUILD OK**，30 源文件全 `[ok]`；`src/driver/x64/Release/RAMBlockCache.sys` = **`20eafc1a160950f739a7b314f80e429d514aaab2111d67e84cb4b4c54e9212f7`** / 191,488 B（未签名） |
| 二进制字符串 | `diag: IRP_MJ_SHUTDOWN seen (Status=sentinel st; …)` / `shutdown-sentinel.bin` / `DiagShutdownSeq` / `diag: IoRegisterShutdownNotification(control)` 四条 UTF-16LE **全部存在** |
| `-Target core` | `compiled=66 skipped=0`，**`RUN_FAIL=0`** |
| `-Target gates` | **ALL GATES PASS**（0 fail / 0 warn）；`check_signature` 仍校验 `dist` `2053c33b…` Valid |
| `qa_teeth` / `qa_w4check` | `killed=3/3 PASS` / cache TU `6/6 warnings=0`（驱动实际 `/W3`，见 `259` C4） |
| 判读脚本 | `py_compile` 无警告（`-W error::SyntaxWarning`）；离线单测 5 种 `reg/records` 组合结论正确 |
| **部署前基线** | `read_shutdown_sentinel.py` 实跑：`DiagShutdownSeq=0`、`sentinel 0 B / 0 records`、`VERDICT: NO DELIVERY EVIDENCE`、`rc=3` |

---

## 4. 两轮实验与判读协议（**尚未执行**）

现场 `HiberbootEnabled=0`，因此本轮目标不是测 hybrid，而是**分别覆盖 `Power Action Shutdown Off` 与 `Power Action Reboot` 两种完整关机**，重点回答第一版遗留的「ShutdownOff 到底派发没有」。

```
轮 A：shutdown /s /t 0   （关闭电源 / Power Action Shutdown Off）
      -> 关机后手动开机（等系统完全停止，不要长按电源）

轮 B：shutdown /r /t 0   （重启 / Power Action Reboot）
      -> 自动开机
```

两轮都完成后，**告诉我一声即可**（不用贴屏），我跑：

```
python src\scripts\read_shutdown_sentinel.py
```

预期与判读：

| `DiagShutdownSeq` | 哨兵记录数 | 结论 |
|---|---|---|
| `2` | `2` | **两种 action 都派发，且关机时 `Zw*File` 可用** ⇒ 可以按 `259` §6 写 v2 生产补丁 |
| `2` | `<2` | 派发了但某一轮文件 IO 失败 ⇒ 生产补丁的索引定稿路径不可靠，须先解决 |
| `1` | `1` | 只有一种 action 派发 ⇒ 用记录里的 UTC 时间戳对齐 System 日志 1074 / Kernel-Power 109，确定是哪一种；若缺的是 ShutdownOff，用户日常「关机」不受保护 |
| `0` | `0` | 两种都没派发（与第一版 Reboot 有 `diag:` 行矛盾，须复查部署是否生效） |

记录里的 UTC 时间戳可直接与 `1074`/`109` 事件对齐，用来把每条记录归到 Reboot 或 Shutdown Off。`RbLog` 行仅作辅助（其 `Status` 是哨兵写入的 NTSTATUS，成功为 `0x00000000`）。

---

## 5. 签名 / 部署链路与基线同步（**尚未执行**）

```
1) 双击 deliverables\RB-sign-driver.cmd   -> UAC 点「是」，把输出贴回
   预期：dist 变为新签名件，三锚点自动同步
2) 我同步第四处 RB-postreboot-verify.ps1 的 $PIN（BOM+CRLF）与 README.md:17
   ⚠ 第五处 o9_fix_deploy_result.txt 仍指向 2053c33b…，按 255/262 同一纪律
     等新版 V1 验收通过后再追加新哈希（该记录代表「已真机验收基线」）
3) 双击 deliverables\RB-deploy-driver.cmd -> UAC 点「是」 -> exit 0
4) 开始菜单正常重启一次
5) 双击 deliverables\RB-postreboot-verify.cmd -> 期望 PASS=9 FAIL=0
   并确认日志有 diag: IoRegisterShutdownNotification(control)
6) 把 V1 结果贴回 -> 我追加第五处基线并回读（09:30 巡检前必须完成）
7) 按 §4 跑轮 A / 轮 B -> 我跑判读脚本给三态结论
```

**时限**：当前 00:5x，观察期巡检 09:30；第五处基线必须在巡检前追加，否则 System32 换新件后会假 drift（历史处置见 `255` / `262`）。

**回滚**：当前处于「已编译未签名」级 —— 丢弃 `src/driver/x64/Release/RAMBlockCache.sys`、从 `.workbuddy/patch-264-backup/` 还原 `rbmsg.c`、删除新增的判读脚本即可；`dist/` 与三锚点全程未动。若已签名/已部署，按 `261` §4 / `262` §5 的逆序步骤（含第四处改回、README 改回、第五处再追加一条旧哈希）。

---

## 6. 边界

- 诊断件**不改持久化行为**：`.idx` 仍 4 B、B2-a 仍存在，属预期；三态结论里的「文件 IO 可用」只说明 `Zw*File` 在关机时能写，不等于 B2 已修。
- `259` 的 F2（hiberfile 闩）、F3（关机时 `Rb2Tick` 未停）、F4（判据）、F7（`RbRegSetClean` 护栏）、F10（注销 UAF）仍未解；`258` 已被红队一致 REJECT 并标记作废。
- `263` 的 Q2（hybrid 是否派发）仍未回答，因为本机 `HiberbootEnabled=0`；要测须单独启用快速启动（改系统电源配置，需另行拍板）。
- 仍未验证：J6 跨引导命中、严格逐字节一致性、任何性能数字。

## 7. 落盘与状态

- 新增 `deliverables/264-shutdown-sentinel-v2-2026-11-16.md`（本文件）、`src/scripts/read_shutdown_sentinel.py`
- 改动 `src/driver/rbmsg.c`；备份 `.workbuddy/patch-264-backup/`（`rbmsg.c` `7cf4c6cc…` / `rbdriver.c` `12841825…` / `rb_common.h` `badab0b0…`）
- `AGENTS.md` / `HANDOVER-CURRENT.md` / `MEMORY.md` / `.workbuddy\memory\2026-11-16.md` 同步
- 现场：`live == dist == 2053c33b…`/Valid、`Flags=0x0`、容器/索引/负载三删、观察期 pin 与 live 一致、git 未提交；`dist/`、三锚点、System32、服务配置、第五处基线本轮**零改动**

**下一步等你拍板签名**：双击 `deliverables\RB-sign-driver.cmd`（一次 UAC），把输出贴回来。签名会改 `dist/` 与三锚点，此后不再是「随时可弃」状态，所以我停在这里。
