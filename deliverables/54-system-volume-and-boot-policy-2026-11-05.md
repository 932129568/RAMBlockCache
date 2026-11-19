# 54 号：系统卷开放 + 开机行为策略 + testsigning 硬保证（2026-11-05）

- **前序**：52（日志系统重构）→ 53（诊断页日志工作区 + 桥接容错）
- **本轮四项用户需求**：①C 盘可手动加速（含回写）②默认不自动挂载 ③设置页开机策略开关 ④安装器强制开启 testsigning

---

## 1. 四项需求实现

### 1.1 系统卷（C 盘）可手动加速【用户需求 ①】

**改动（driver/rbattach.c）**：移除 B-10 硬拒绝闸门（`IsSystem && !SystemVolumeCached → ACCESS_DENIED`），改为：允许手动 attach，**仍不进开机自动挂载候选集**（只有显式 attach 才缓存 C:）。

**改动（driver/rbio.c）**：`RbIsWriteBack` 移除 `if (ext->IsSystem) return FALSE` —— **系统卷现在也支持回写**（用户明确要求"C 盘也同意回写"）。

**保留的安全边界**：
| 边界 | 作用 |
|---|---|
| `SafeMode` 强制直写 | 上次异常退出时，所有卷（含 C:）强制直写，防二次数据损失 |
| 簇重排闸门（DATA-12/13/14/15） | 删除/改名/TRIM/卸载/置零前刷脏+丢缓存视图——**系统卷同样生效** |
| 开机自动挂载跳过系统卷 | 只有手工 attach 才会加速 C:（防"开机即 C 盘回写"的意外） |

**风险提示**（UI 文案）：系统卷回写会扩大掉电丢脏窗口——设置页 SystemVolumeCached 的说明已更新为"系统卷已可手动开启（含回写，风险自负）"。

### 1.2 默认不自动挂载任何卷【用户需求 ②】

**改动（driver/rbdriver.c）**：`AutoAttach` 默认值 **1 → 0**。

现在开机三档行为（优先级从高到低）：
1. `AutoAttach=1` → 全自动挂载所有数据卷（旧行为，需显式开启）
2. `AutoAttach=0` + `ResumeOnBoot=1`（**默认**）→ 只恢复用户上次手动加速过的卷
3. `AutoAttach=0` + `ResumeOnBoot=0` → 完全不自动挂载（纯手动）

### 1.3 设置页「开机行为」开关【用户需求 ③】

**新增 IOCTL 0x80A**（`ctl/rb_bootcfg.h`，独立 ABI，不影响 Protocol 9）：
```
RB_BOOTCFG { Get, AutoAttach, ResumeOnBoot, Reserved, ResumeLetters[32] }
```

**驱动侧自动维护 ResumeLetters**（`RbResumeTrack`）：attach 成功时加入字母、软分离 detach 时移除，写回 `Parameters\ResumeLetters`（如 `"DE"`）——**无论操作来自 GUI / CLI / 自动恢复**都一致更新。

**设置页 UI**（新增「开机行为」组）：
- 开机自动加速所有数据卷 `AutoAttach`（默认关）
- 开机恢复上次加速的卷 `ResumeOnBoot`（默认开）
- 实时显示上次加速记录（"D、E 盘"或"暂无"）

**桥接路由**：`GET/POST /rb/bootcfg`（原生插件新增 `rb_boot_cfg`）。

### 1.4 安装器强制 testsigning【用户需求 ④】

**问题（0x7B 真实成因）**：此前 `bcdedit /set testsigning on` 失败**只警告继续**——于是"驱动装好了但开机加载不了"，正是对方机器 0x7B 蓝屏循环的现场。

**改动（src/setup/ramboostsetup.c）**：
- `EnableTestSigning` 改为返回 `BOOL`
- bcdedit 失败时**重试 3 次**并显示拦截软件的**具体拒绝文本**（读 bcdedit stdout）
- 3 次仍失败 → **中止安装**（`return 1`），并给出明确指引（关闭 360 → 手动 bcdedit → 重启后重装）
- `/NoTestSigning` 保险丝保留（供将来正式签名分发包）

**效果**：绝不会留下"驱动已注册但开机拒载"的半成品，从源头消灭 0x7B 安装场景。

---

## 2. 兼容性

| 面 | 结论 |
|---|---|
| RB_CONFIG（Protocol 9） | **未改动** —— 开机策略走独立 IOCTL 0x80A |
| check_proto_sync | **PASS (0 fail, 0 warn)** |
| core 套件 | buildfail=0 |
| 旧 AutoAttach=1 配置 | 仍有效（全自动挂载，向后兼容） |
| 系统卷旧 opt-in 语义 | `SystemVolumeCached` 字段保留（仅日志用途）；手动 attach 不再需要它 |

---

## 3. 构建与产物

| 项 | 值 |
|---|---|
| 驱动 | `8CF4E862`（签名 Valid） |
| 桌面安装包 | **291.5MB · 14:26 · 签名 Valid** |
| Web | 构建 ✓ / selftest 18/18 ✓ |
| 原生插件 | cargo 构建成功（含 rb_log_page + rb_boot_cfg） |
| 静默引擎 | 已重编（testsigning 硬失败） |

---

## 4. 验证清单（部署后）

1. 设置页 → 「开机行为」组应显示两个开关（AutoAttach 关 / ResumeOnBoot 开）
2. 卷页 → **C 盘应可点「热开启」**（此前被拒）
3. 手动开启 D 盘 → 设置页"上次加速记录"应显示 "D 盘"
4. 重启 → 只恢复 D 盘（不恢复其它卷，C 盘不自动加速）
5. 关闭 ResumeOnBoot → 重启 → 无任何卷自动加速

## 5. 部署状态

| 项 | 状态 |
|---|---|
| 驱动文件 | 已签名，需部署到 System32（文件映射中，需重启或排程） |
| 桌面包 | 已就绪（内嵌全部改动） |
| 桥接/UI | 需重启桥接生效 |
