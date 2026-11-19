# v11 实施手册虚构性审计（`284_286_u3_v11.md` 全量核实）

> 日期：2026-11-18 20:1x ｜ 作者：主线程（编排）+ QA 严过关（独立复核，已并入 §8）
> 类型：**只读审计**（零代码改动、零内核、零部署、零重启、零 git 写）
> 审计对象：`C:\Users\陈\Desktop\284_286_u3_v11.md`（107,032 B / 2,868 行 / 无 BOM / 纯 LF）
> 上游事实源：`284`、`285`、`286`、`design/U3-layered-glass-wave-coupling-design-2026-11-18.md`
> QA 独立复核：`design/v11-audit-qa-verify-2026-11-18.md`（54,833 B / 734 行）

---

## 0. 一句话结论

**手册不能按原文落地。** 逐项核实 **17 个具名判定**：**🔴 7 项虚构（照抄即坏）+ 🟡 7 项语义/数字失真 + ✅ 3 项数字属实**；另有 **20 项「可保留」条目**经独立验证无误（§1.4、§5）。QA 独立复核一致 7 项、**纠正我 2 项**（V-07 特异性数值、V-14 口径），并**新增 7 项我漏掉的发现**（§8），其中 **N-1（`RB_STATS2` 128 B→48 B）比我判定的 V-03 破坏面更大**。

7 个最高危项会**改掉全部 IOCTL 数值、删掉 A4 卷数门、删掉 `PersistSkipReasonPrev` 成对判据、把主统计结构从 128 B 砍成 48 B、让测试文件直接编译失败、让倒角环白做**。

但**它的方向性结论是对的**（解析耦合、`WAVE_DAMPING` 锁 0.95、层级透色用 blur 不用 `url()`），且多处实测数据与我方独立复现一致——**问题不在思路，在于它把「真实结论」和「编造的接口细节」混装在一起**。

---

## 1. 判定总表（17 个具名判定 + 20 项可保留条目）

> **编号纪律**：本条用 `V-01..V-17`（v11 手册审计条目），**刻意避开 `F-xx`** —— 因为 `278` 已把 `F-01..F-09` 定义为「虚构的九种形态」，手册本身又用 `F23–F32` 表示缺陷编号。三套 F 编号共用会互相污染。
> 「形态」列按 `278 §2` 分类学标注（`F-03` 只钉声明不钉消费 / `F-07` 转述漂移 / `F-09` 部分覆盖说成全部 / `F-08` 沉默失败）。

标记：✅ 属实 ｜ 🟡 部分失真 ｜ 🔴 虚构/会导致故障

### 1.1 最高危：照抄即坏（7 项 🔴）

| # | `278` 形态 | 文档位置 | 文档声称 | 真实源码 | 落地后果 |
|---|---|---|---|---|---|
| **V-01** 🔴 | F-07 转述漂移 | `:2085` | `#define RB_DEVICE_TYPE 0x8000U` | `rb_common2.h:15-19` 用 `FILE_DEVICE_UNKNOWN`（0x22） | **全部 IOCTL 数值改变**，ctl/驱动/门禁三处同时失配 |
| **V-02** 🔴 | F-07 转述漂移 | `:2091-2098` | `0x902=FLUSH_ALL_SYNC`、`0x903=L2_SAVE_INDEX_SYNC`、`0x904=ATTACH_VOLUME`、`0x905=PANIC_CLEAR`、新增 `0x908/0x909` | `rb_common2.h:10-19`：`0x902=REFRESH`、`0x903=SHRINK`、`0x904=SNAPSHOT`、`0x905=VOLSTATUS`；**无 0x908/0x909** | 刷盘指令实际触发 **L2 REFRESH**；`0x907` 的 access 标志也被改（真实 `FILE_WRITE_ACCESS`，文档 `FILE_ANY_ACCESS`） |
| **V-03** 🔴 | F-07 + F-03 | `:2128-2138` | 24 B 结构含 `ShutdownPersistWhyCurr/LastCleanShutdown/PrevValidAndFlags/ActiveAttachedVolumes/PendingDirtyBlocks/SentinelMagic` | `rb_common2.h:115-123` 24 B 含 `ShutdownPersistWhyPrev(ULONG)/PersistSkipReasonPrev(ULONG)/BootFailCount(ULONG)/BootLoopGuard/Reserved[3]` | **同尺寸、字段全错位**。`off=8` 真实是 ULONG 而文档是 uint8；**`PersistSkipReasonPrev` 被删 ⇒ 275 号「成对判据」全部失效**，`rbfeatctl stats` 读到错位数据 |
| **V-04** 🔴 | F-07 转述漂移 | `:2498` | `NTSTATUS RbRegSetClean(UCHAR PersistWhy, UCHAR CleanFlag)` 双参、直接写两个值 | `rb_common.h:581` + `rbdriver.c:216`：`BOOLEAN RbRegSetClean(ULONG v)` **单参**，内含 `RbBootAttachedAny()` 卷数门（`:236-248`）+ `LastCleanWithheld` | **复活 A4 缺陷**；`check_a4_clean_gate.py` 的 I1–I10 判据失败（机器校验「调用点数==1」被破） |
| **V-05** 🔴 | F-07 转述漂移 | `:1226-1227` | 写 `--rbc-pointer-x` / `--rbc-pointer-y`，**百分比** | `useLiquidGlassPointer.ts:19-20` 写 `--rbc-lg-ptr-x` / `--rbc-lg-ptr-y`，**px**；被 `liquid-glass.css:108` 以 `280px circle at var(...)` 消费 | **指针光泽彻底失效**（变量名零命中）；且百分比与 `280px` 圆心坐标语义冲突 |
| **V-06** 🔴 | F-08 沉默失败 | `:1513` | 「追加至 `liquidWaveCore.test.ts`」，块首重写 `import { describe, expect, it } from 'vitest'` | 该文件 `:2` **已有同一 import** | **ESM 重复声明 ⇒ `SyntaxError`**，整个测试文件不执行 ⇒ 宣称「82 用例全绿」自我否定 |
| **V-07** 🔴 | F-01 无证据结论 | `:1468` | `.rbc-section .rbc-card::before { padding: 1.25px }` 得到「1.25px 菲涅尔倒角」 | `liquid-glass.css:365` 已有 `.rbc-card:not(...):not(...)::before { padding: 1px }`，特异性 **(0,3,1)** > 文档 **(0,2,1)** | **实测 computed `padding=1px`**，倒角环完全不生效；**且 `background-image` 也一并被覆盖 ⇒「碰撞边缘闪光」同时失效**（详见 §2；QA 已订正特异性数值，我原写 (0,4,1) 高估一级） |

### 1.2 中危：语义失真（6 项 🟡）

| # | `278` 形态 | 文档位置 | 问题 | 真实情况 |
|---|---|---|---|---|
| **V-08** 🟡 | F-07 转述漂移 | 通篇（`:10/25/2868`） | 称「内核 `LastChanceShutdown` 轻量盖章」 | `IoRegisterLastChanceShutdownNotification` **全仓库 0 命中**；实际用 `IoRegisterShutdownNotification`（`rbdriver.c:1391`）。这是**另一个 API**（LastChance 是更晚、更受限的通知），不能混称 |
| **V-09** 🟡 | F-07 转述漂移 | `:31` | 把 F27–F30 归因为「单点注入波纹略显单薄且玻璃间孤立」 | F27–F30 在设计原文是**否决旧通量方案的证据**（`:83-116`），不是「波纹单薄」的描述。归因偷换，会误导后人以为要「加强波纹」 |
| **V-10** 🟡 | F-09 部分覆盖 | `:1226-1271` | 重写 `useLiquidGlassPointer`，删掉三处护栏 | 真实实现有 `prefers-reduced-motion` 早退（`:24`）、`document.visibilityState==='hidden'` 判定（`:24`）、`requestAnimationFrame` 节流（`:26-28`）。**全部被删** ⇒ 无障碍与后台节电双双倒退 |
| **V-11** 🟡 | F-01 无证据结论 | `:1520-1531` | V12 用 `stepWave(f1)` vs `stepWave(f2, WAVE_DAMPING)` 比较相等 | `stepWave` 的默认参数**就是** `WAVE_DAMPING`，两条调用必然等价 ⇒ **同义反复，零变异检测力**。把 0.95 改成任何值该断言都通过。真正有牙的只有 `expect(WAVE_DAMPING).toBe(0.95)` |
| **V-12** 🟡 | F-09 部分覆盖 | `:1506` | 称「V1–V14 完整自动化验收测试套件」 | **V10（休眠保持）无任何测试代码**，只在 `:615` 注释里被提及。其余 V 号均有 `describe`/`it`（V4 合并进 V1&V4）。**实测覆盖 = 13/14**，缺 1 项即不能称「完整」 |
| **V-13** 🟡 | F-04 量纲/计数 | `:3` | WPF 壳与安装器「96 文件 / 800,992 B」 | 实测 **100 文件 / 816,259 B**（controller 84 + installer 16） |

### 1.3 数字偏差（4 项：V-14 🟡 + V-15/V-16/V-17 ✅）

| # | 文档位置 | 声称 | 实测 |
|---|---|---|---|
| **V-14** 🟡 | `:3` | Web UI「9,868 行」 | **口径未声明，非虚构**（QA 订正）。实测总行 **10,642** − 空行 **774** = **9,868**，精确命中「非空行」口径。61 文件 / 409,447 B 本身正确。⇒ **降级为「口径未声明」，建议手册写明「非空行」** |
| **V-15** ✅ | `:3` | 总数「350 文件 / 3,910,725 B」 | **完全一致** |
| **V-16** ✅ | `:3` | Web「61 文件 / 409,447 B」、「12 测试文件 / 82 用例」 | **完全一致** |
| **V-17** ✅ | `:3` | 驱动「27 文件 / 804,091 B」 | **完全一致** |

> **V-13 的补充证据（QA 穷举）**：`src/ui/{controller,installer}` 的全部扩展名子集（2⁷=128 种）**没有任何组合等于 (96, 800,992)**；最接近 `.cs+.xaml+.manifest` = 96 文件 / **800,088 B**（差 904 B）。⇒ 「96 文件」对，**字节数在当前工作树不存在**。

### 1.4 可保留（20 项 ✅，逐项已验证）

| 类别 | 项目 | 证据 |
|---|---|---|
| **核心策略** | `WAVE_DAMPING=0.95` 锁死、`liquidWaveCore.ts` 零改动 | 与 `285`/`286` 结论一致，方向正确 |
| **数学内核** | `reach(baseAmp)=λ·ln(baseAmp·κ/minAmp)` | 独立验算：`reach(0.90)=0.7502`、`reach(0.135)=0.0292`，V13 区间与两条功能不等式**全部通过** |
| **单调性** | V3 `reach` 严格递增 | 独立验算 `0.0292 < 0.3327 < 0.5630 < 0.7502` ✅ |
| **Kelvin 几何** | `buildKelvinWakeSamples(0.5,0.5,1,0,0.8)` 两瓣对称、尾迹反向 | 独立复算：w1=(-0.0247,-0.0132)、w2=(-0.0247,+0.0132)，ny 和=1.000000 ✅ |
| **F24 真实** | canvas 固定 `zIndex:'3'` | `useLiquidWave.ts:110` 确认；card 为 auto ⇒ section 波盖 card 之上 |
| **F25 真实** | 嵌套 card `backdrop-filter:none` | `liquid-glass.css:409-410` 确认 |
| **嵌套模糊有效** | 父 blur10 + 子 blur20 生效 | 我方 `286` 已像素实测 `sd 127.5→1.39`，文档引用正确 |
| **26px 开启生效** | `:1453` 规则真能覆盖 `:409` 的 `none` | **实测 `blur(26px) saturate(1.8) brightness(1.04)`**（同特异性、后者胜） |
| **令牌纪律** | 只新增 10 个 `--rbc-layer-*`，不碰受断言三族 | 契约测试 `18/18/13` 硬断言不受影响；实测声明数确为 10 |
| **缺陷编号** | F23–F32 全部真实存在 | 设计原文逐条命中（F23–F32 共 22 个编号） |
| **D-04 归因** | `g_RbBootEpoch` 每次加载归零 | `rbdriver.c:33` 初值 0 + `:1188` `InterlockedIncrement64`；**无注册表持久化** |
| **介质 API** | `StorageDeviceSeekPenaltyProperty` + `IOCTL_STORAGE_QUERY_PROPERTY` | 合法 Windows API，用法正确（`:2681-2693`） |
| **依赖齐全** | `@testing-library/react ^16.3.3`、`jsdom ^30.1.3`、`vitest 5.0.5` | `package.json` 确认 |
| **Hook 签名兼容** | `useLiquidWave({root})` / `useLiquidGlassPointer(rootRef)` | `App.tsx:51` 调用方式不变，`root` 可选 |
| **孤儿 card 处理** | `src.slot ? projectIntoSlot(...) : edgeLocal` | 逻辑正确，孤儿退化为本地场不报错 |
| **第 2 跳防回声** | 排除 `pointerCardId` | `:2499` 确认 `childId === input.pointerCardId` 跳过 |
| **rect 回退** | `offsetParent !== section` 时用 rect | `:189-199`/`:220-228` 真实实现 |
| **ASan 开关** | `/W4 /Wx /fsanitize=address` | 与 `sources.manifest.json` 的 asan flags 一致 |
| **CSS 契约禁忌** | 文档 CSS 追加块（`:1419-1501`）内零出现 `display:` / `flex-direction:` / `gap:` / `contain:` / `url(` | 逐行 grep 零命中；`liquid-glass.contract.test.ts:41-46` 的四条禁令不受影响。宿主模式用 `background` 覆写而非 `display:none` 隐藏 Layer 0，符合 `286` §3.6 的决定 |
| **V 判据覆盖** | V1–V14 中 **13 项**有对应 `it`/`describe` 标题 | 精确正则核对 13/14，唯一缺 V10（见 V-12） |

---

## 2. 关键实测证据（V-07 特异性覆灭）

### 2.1 特异性对比（数值经 QA 订正）

| 规则 | 来源 | 特异性 |
|---|---|---|
| `.rbc-card:not(.rbc-card-accent):not(.rbc-card-danger)::before` | 既有 `liquid-glass.css:365` | **(0,3,1)** |
| `.rbc-section .rbc-card::before` | 文档 `:1468` | **(0,2,1)** |

> **订正**：我初版写 (0,4,1)，高估一级 —— `:not()` 伪类**本身不贡献特异性**，只贡献其参数（1 个 class）。正确计数：`.rbc-card`(1) + `:not(.rbc-card-accent)`(1) + `:not(.rbc-card-danger)`(1) + `::before`(1 元素) = **(0,3,1)**。QA 指出后用「复合 class 逐级二分」实测确认（见 §2.2）。

CSS 规则：特异性高者胜，**与书写顺序无关**。文档规则即使追加在文件末尾也无法生效。

### 2.2 三方独立实测（全部一致）

**主线程 · 场景 A（既有规则 + 文档规则同时加载）**
```
cardBackdrop:      "blur(26px) saturate(1.8) brightness(1.04)"   ← :1453 开启规则 ✓ 生效
cardBgColor:       "rgba(0, 0, 0, 0)"                            ← transparent ✓ 生效
beforePadding:     "1px"                                          ← 应为 1.25px ✗ 被覆盖
beforeBgContainsBevel: false                                      ← 文档渐变 ✗ 被覆盖
```

**主线程 · 场景 B（复合 class 二分反推既有特异性）**
```
对手 (0,1,1) → 1px   既有 ≥ (0,1,1)
对手 (0,2,1) → 1px   既有 ≥ (0,2,1)
对手 (0,3,1) → 9px   既有 < (0,3,1)   ← 交叉点
⇒ 既有特异性 = (0,3,1)，与 QA 计数一致
```

**QA · 场景 C（四组情形，验证「与顺序无关」）**
```
基线（仅真实 css）          : padding = 1px
真实 css + 手册规则（手册在后）: padding = 1px
手册规则在前 + 真实 css 在后  : padding = 1px     ← 与顺序无关，纯特异性压制
移除既有 ::before，仅手册规则  : padding = 1.25px  ← 规则语法本身有效
```

**结论**：手册 `::before` 规则**语法没问题**（场景 C 第四组能拿到 1.25px），就是被 (0,3,1) 压死。且 `background-image` 同样被覆盖 ⇒ **「菲涅尔倒角环」与「碰撞边缘闪光」两个卖点同时失效，不只是 padding**。

### 2.3 复跑命令

```bash
cd "C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包/src/ui/web"
# playwright + Edge fallback；加载既有规则与文档规则，读 getComputedStyle(el,'::before').padding
# 用「复合 class 逐级二分」反推特异性，比读文档更可靠
```

---

## 3. 为什么这些虚构是「危险」而非「瑕疵」

这份手册的特殊风险在于**它混装了真实与虚构**：

1. **它引用的缺陷编号全是真的**（F23–F32、U-P0..U-P4、D-01..D-08），让人相信它做过核实；
2. **它的核心策略是对的**（解析耦合、锁 0.95、禁 `url()`），让人相信整份可落地；
3. **它的接口细节是编的**（IOCTL 号、结构体字段、函数签名、CSS 变量名）。

三者混在一起的后果：执行者按 §1 的策略判断「这份文档靠谱」→ 直接照抄 §2–§4 的代码 → 得到**改掉全部 IOCTL + 复活 A4 + 测试编译失败 + 倒角环白做**的产物。而每一处单看都「像是真的」。

### 3.1 最阴的一层：它的 `RB_STATIC_ASSERT` 会全部通过

文档给三个结构体都加了编译期断言（`:2175-2177`）。我按其自身字段定义手算（`pack(1)`，无编译器环境，逐字段累加）：

| 结构体 | 文档声明尺寸 | 按其字段手算 | 断言结果 |
|---|---|---|---|
| `RB2_SHUTDOWN_DIAG_OUT` | 24 B | **24 B** | ✅ 通过 |
| `RB2_CACHE_POLICY_IN` | 24 B | **24 B** | ✅ 通过 |
| `RB_STATS2` | 48 B | **48 B** | ✅ 通过 |

**尺寸全对，字段全错。** 静态断言只能钉「多大」，钉不住「第 8 字节是 `WhyPrev` 还是 `WhyCurr`」。真实 `rb_common2.h:127-128` 的 `RB2_SHUTDOWN_DIAG_OUT_IS_24_BYTES` 同样只查 24 B —— 它**也拦不住**这次错位。

⇒ 这正是我方 `check_a4_clean_gate.py`（10 条 I1–I10）、Gate24（`check_sup01_tables.py`）那类「钉消费方语义而非声明方尺寸」门禁存在的根本理由：**只钉尺寸不钉字段语义的门禁，会被这种「同样 24 字节」的伪造整体绕过。**

### 3.2 QA 独立实测证实了这一点（MSVC 探针）

QA 用本机 MSVC 14.51.36231 编译探针实测：

```
sizeof(DOC_DIAG)   = 24   off(WhyCurr)=8   off(ActiveVol)=12   off(Sentinel)=20
sizeof(REAL_DIAG)  = 24   off(WhyPrev)=8   off(SkipPrev)=12    off(BootFailCount)=16  off(Guard)=20
```

两侧都是 24 B ⇒ `rb_common2.h:127-128` 的 `sizeof(...) == 24 ? 1 : -1` **不会报警**。只有逐字段比对（Gate25 P5）能发现。

### 3.3 按手册落地后 `rbfeatctl shutdiag` 会打印什么（QA 逐值推演）

| ctl 字段（真实偏移） | 实际取到 | 显示 |
|---|---|---|
| `ShutdownPersistWhyPrev`(@8 ULONG) | 打包 [WhyCurr,WhyPrev,LastClean,PrevFlags] | 值 ≫0xFF ⇒ `WhyText()` 落 default ⇒ **unknown** |
| `PersistSkipReasonPrev`(@12) | `ActiveAttachedVolumes` | 打印成**已挂卷数** |
| **`BootFailCount`(@16)** | **`PendingDirtyBlocks`** | **打印成脏块数** |
| `BootLoopGuard`(@20 u8) | `SentinelMagic` 最低字节 `0x52`('R') | **恒 TRUE ⇒ attach 被拒** |

> 推演前提已在 QA 报告中标注：假设「驱动按手册填充、ctl 保持真实」。这是**推演**而非实测输出。

---

## 4. 必须修正才能落地（按危险度排序，已并入 QA §11）

| 序 | 修正项 | 改法 |
|---|---|---|
| **1** | **N-1 `RB_STATS2` 128 B→48 B（QA 新增，最高危）** | **整节回滚**。真实 `RB_STATS2` = **128 B / 20 字段**（`rb_common2.h:30-43`，我按自然对齐逐字段累算确认 128 B），有 **10 处消费方**（driver `rbfeat.c`/`rbpools.c`、ctl `rbfeatctl.c:41-48` 手工副本、WPF `RbFeature.cs`/`DriverService.cs` 等 5 文件、TS 协议层）。手册改成 48 B / 10 字段且字段名全换（`TotalReadBytes` 等 7 个名字**全仓库 0 命中**） |
| 2 | **V-01/V-02 IOCTL 全表** | 删除 `rb_diag_abi.h`（该文件**不存在**，`src/common/` 只有 7 个头）或改为 `#include "rb_common2.h"` 复用真实码；`0x907` 用 `FILE_WRITE_ACCESS`；删除 0x908/0x909 |
| 3 | **V-03 DIAG 结构体** | 改为真实 7 字段（含 `PersistSkipReasonPrev`）；尺寸断言不足以自证，须加逐字段比对 |
| 4 | **V-04 `RbRegSetClean`** | 恢复单参 `BOOLEAN`，保留卷数门与 `LastCleanWithheld`；不得在别处新增裸 `ZwSetValueKey` 直写 |
| 5 | **V-06 重复 import** | 追加块删掉 `import { describe, expect, it }` 与 `createWaveField/injectWaveImpulse/stepWave/WAVE_DAMPING` 的重复导入 |
| 6 | **V-05 指针变量 + V-10 护栏** | 保留 `--rbc-lg-ptr-x/y` 与 **px**；只**追加**宿主消息分支，不重写整个 hook；**不要**加 `width<=0` 早退（jsdom 下恒为 0，会让 2 个测试文件必红） |
| 7 | **V-07 `::before`** | 选择器改为 `.rbc-section .rbc-card:not(.rbc-card-accent):not(.rbc-card-danger)::before`，改完**必须重测** `computed padding === '1.25px'` |
| 8 | **V-11 V12 断言** | 删除同义反复的两条 `stepWave` 比对，只保留 `expect(WAVE_DAMPING).toBe(0.95)` |
| 9 | **V-12 V10 缺失** | 补休眠保持测试，或改称「V1–V14 除 V10 外已实现」 |
| 10 | **V-08 `LastChanceShutdown`** | 全文改称 `IoRegisterShutdownNotification`（`rbdriver.c:1391-1392`） |
| 11 | **V-09 归因** | F27–F30 改回「否决旧方案的实测证据」，不写「波纹单薄」 |
| 12 | **V-13/V-14 数字** | WPF→100/816,259；Web 行数写明「非空行 9,868 / 总行 10,642」 |
| **13** | **N-3 12 个虚构符号（QA 新增）** | `RbIsEngineFrozen`/`RbIsFullOperationMode`/`RbGetTotalDirtyBlockCount`/`RbFlushAllDirtySync`/`RbGetActiveAttachedVolumeCount`/`RbWriteSentinelRbd4`/`RbBootEpochInit`/`RbCheckAndDisableFastStartup`/`g_RbPrevValidAndFlags`/`g_RbLastCleanShutdown`/`g_RbShutdownPersistWhyCurr`/`g_RbShutdownPersistWhyPrev` 全 0 命中；`RB_VOL_FILTER_EXT` 应作 `RB_DEV_EXT`（25 命中）、`RB_PERSIST_WHY_*` 非 `RB_SHUTDOWN_WHY_*` |
| **14** | **N-2 头文件路径（QA 新增）** | `#include "../common/rb_diag_abi.h"` 与真实布局不符（`rb_common2.h` 用同目录 `#include "rb_common.h"`） |
| **15** | **N-7 改动范围声明（QA 新增）** | 手册称「纯 CSS + 新增配置」，实际改了 4 处（`useLiquidWave.ts` canvas zIndex、`useLiquidGlassPointer.ts` 全文、`tokens.css`、`liquid-glass.css`）并删了 hook 两道运行时护栏 |

---

## 5. 可保留（已核实无误）
- `reach` 公式、V13 区间与功能不等式、V3 单调性 —— 数学自洽，独立验算通过；
- Kelvin 尾迹几何、Ricker 采样点数量与符号；
- `planCoupling` 的拓扑逻辑（第 1 跳 / 第 2 跳 / 防回声 / 冷却 / 孤儿退化）；
- 10 个 `--rbc-layer-*` 令牌设计与消费关系；
- Phase 1 的 `mapCardToSection` / `mapSectionToCard` / `refreshSlot` 映射与 rect 回退；
- Phase 4 的视觉采样协议（2px 内缩、对照基线、记录 sd 数值）—— 这条纪律正是我方 `286` 因一次假阴性而立的。
- 介质检测 API 用法、D-04 归因。

---

## 6. 未验证（禁止宣称）

- 手册 Phase 2–4 的**完整代码能否编译**（本轮只做符号/签名/结构体层面的比对，未装配编译环境；且 V-06 已证明至少一处必然编译失败）；
- WebView2 **真机宿主**内双层 `backdrop-filter` 帧率（U-P4 仍是未验证项，手册也承认）；
- `reg add ... HiberbootEnabled` 的**实际效果**（本机 `HiberbootEnabled=0`，改了也无从验证）；
- `0x906` 的 `EngineEpochLo32 == 1/2` 期望值 —— 在 V-03 结构体错位的前提下，这个验收判据**本身就是基于错位字段写的**，不能作为验收依据。

### 6.1 QA 独立复核状态（已完成，已并入 §8）

QA 严过关的独立复核**已完成并落盘**：`deliverables/design/v11-audit-qa-verify-2026-11-18.md`（54,833 B / 734 行 / 无 BOM / 全 LF）。

结果：**7 项一致 / 2 项纠正我 / 7 项我漏掉的发现**。逐条见 §8。按 `278 §3` 口径，本报告 §1 的判定现已达到「双人独立复核」等级，但**纠正项以 QA 实测为准**（已在 §1.1 V-07、§1.3 V-14、§2 就地订正）。

### 6.2 QA 明确标注为「未验证」的三项

- `IoRegisterLastChanceShutdownNotification` 在本项目 WDK 下**是否可用** —— 本机 WDK 只装了 `um/shared/ucrt/cppwinrt/winrt`，**无 `km/` 头**；只验证了「项目源码 0 命中」；
- 结构体错位对 `read_shutdown_sentinel.py` 的**直接影响** —— 判读器读的是**注册表**（`:315-316`）不是 0x906，所以不直接影响判读器；影响的是 `rbfeatctl shutdiag`。判读器是否另有 0x906 路径**未验证**；
- 手册驱动代码的**真实可编译性** —— 依赖 N-1/N-2/N-3，无法编译验证。

---

## 7. 本轮边界

- **零代码改动**：`src/` 下所有 `.c/.h/.ts/.tsx/.css/.py/.ps1` 均未修改；
- **零内核 / 零部署 / 零重启 / 零 git 写**；
- 唯一副作用：`src/ui/web` 下若干 `.tmp-*.mjs` 探测脚本，**用完即刻删除**（已确认无残留）；
- 签名基线 `2c1096d8…` 未动。
---

## 8. QA 独立复核（`design/v11-audit-qa-verify-2026-11-18.md`）

> QA 严过关，只读复核，未改任何源码/CSS/测试/门禁；临时探测脚本与变异副本已清理。全部输出为其本机实跑原文（playwright computed style / MSVC sizeof / 门禁 stdout / node 变异体）。

### 8.1 一致与分歧

| 判定 | 主线程 | QA | 结论 |
|---|---|---|---|
| A 基线数字 | 低危失真 | 部分属实 + **口径未声明** | ⚠ **QA 订正我**：`9,868` 是「非空行」而非错数 |
| B IOCTL 全错 | 高危 | 虚构成立 | ✅ 一致 |
| C 结构体错位 | 最高危 | 虚构成立（且门禁实测能抓） | ✅ 一致 |
| D `RbRegSetClean` | 最高危 | 虚构成立，门禁能抓 | ✅ 一致 |
| E 指针 hook | 高危 | 虚构成立（并实测 jsdom 必红 2 文件） | ✅ 一致 |
| F `::before` 特异性 | 功能性虚构 | 实测确认成立，**但特异性应为 (0,3,1)** | ⚠ **QA 订正我** |
| G `LastChanceShutdown` | 中危 | 虚构成立 | ✅ 一致 |
| H V12 同义反复 | 低危 | 成立，**已用 0.96 变异体实测证明** | ✅ 一致 |
| I 属实部分 | 属实 | **全部独立验算通过** | ✅ 一致 |

**7 项一致 / 2 项纠正 / 7 项新增发现。**

### 8.2 QA 新增发现（我漏掉的）

| 编号 | 危险度 | 内容 |
|---|---|---|
| **N-1** | 🔴 **阻断（高于 V-03）** | 手册把 **`RB_STATS2` 从 128 B 改成 48 B**、字段名全换。MSVC 实测 `sizeof(REAL_STATS2)=128`；我按其 20 字段自然对齐逐项累算亦得 **128 B**。有 **10 处消费方**（driver `rbfeat.c`/`rbpools.c`；ctl `rbfeatctl.c:41-48` 手工副本 + `:476/:616` 两处 printf；WPF `RbFeature.cs`（23 字段）/`DriverService.cs`/`FeatureConfigService.cs`/`IoctlTrace.cs`/`IPrefetchGate.cs`；TS `protocol/`）。手册新字段名（`TotalReadBytes`/`L1HitBytes`/`CoalescedWriteBytes`/`DirtyBlockCount`/`IncursSeekPenalty`/`ActiveMediaMode`/`SelfDisabledPassthrough`）**全仓库 0 命中** |
| **N-2** | 🔴 阻断 | `#include "../common/rb_diag_abi.h"` 路径与真实布局不符（`rb_common2.h` 用同目录 `#include "rb_common.h"`），且该头**文件不存在**（`src/common/` 只有 7 个头） |
| **N-3** | 🟠 中高 | 手册新增 **12 个函数/变量在真实源码 0 命中**：`RbIsEngineFrozen`/`RbIsFullOperationMode`/`RbGetTotalDirtyBlockCount`/`RbFlushAllDirtySync`/`RbGetActiveAttachedVolumeCount`/`RbWriteSentinelRbd4`/`RbBootEpochInit`/`RbCheckAndDisableFastStartup`/`g_RbPrevValidAndFlags`/`g_RbLastCleanShutdown`/`g_RbShutdownPersistWhyCurr`/`g_RbShutdownPersistWhyPrev`；另有 `RB_VOL_FILTER_EXT`/`g_RbSelfDisabledFlag`/`CacheStreamActive`/`RB_MEDIA_CACHE_MODE`/`RB_SENTINEL_MAGIC`/`RB_MAX_DEFER_WRITE_MS`/`RB_L2_READ_ONLY_ENFORCED`/`RB_SHUTDOWN_WHY_*` 全 0 命中（真实名 `RB_DEV_EXT` 25 命中 / `RB_PERSIST_WHY_*`） |
| **N-4** | 🟡 低（**我方门禁缺陷**） | I3 只匹配 `RbRegSetDW`/`RbRegSetValue` 两种字面形态，**抓不住裸 `ZwOpenKey`+`ZwSetValueKey` 直写同名键**。实测：把 `RbRegSetDW(L"LastCleanShutdown", v)` 换成裸 Zw 序列、其余不变 ⇒ `[OK] I3` + `PASS (0 fail)`。手册版之所以被抓，是因为**改了签名**，不是因为「绕原语」被检测 |
| **N-5** | 🟡 低 | `RB_STATS2` 同名冲突（手册 `rb_diag_abi.h` 与真实 `rb_common2.h` 同名不同布局）→ `#include` 顺序埋雷。**未验证** ODR 后果 |
| **N-6** | 🟡 低 | `286` 报告 §1.1 表格行合计 335/3,649,998 ≠ 声称合计 350/3,910,725（缺 15 文件/260,727 B：`src` 根 2 文件 4,990 B + `web` 根 9 文件 240,470 B + `core/build` 2 文件 28,308 B）→ **这是我方 286 的瑕疵，非手册问题** |
| **N-7** | 🟡 低 | 手册改动范围声明不一致：称「纯 CSS + 新增配置」，实际改了 4 处并删了 hook 两道运行时护栏 |

### 8.3 QA 的门禁实测（关键：我们的门禁能拦多少）

**B（DeviceType→0x8000 且 0x907→ANY，两侧同改）**
```
[FAIL] P2: IOCTL_RB2_SHUTDOWN_DIAG ABI = (dt=0x8000, func=0x906, method=0, access=0)，期望 (0x22, 0x906, 0, 0)
[FAIL] P2: IOCTL_RB2_HEALTH_COMMIT ABI = (dt=0x8000, func=0x907, method=0, access=0)，期望 (0x22, 0x907, 0, 2)
result: FAIL (4 fail)
```
⇒ Gate25 **能拦住 B**。

**另一个 B 的结构性缺陷（QA 独家，我没提）**：手册 `:2086-2087` 的宏 `RB_CTL_CODE(Function) = CTL_CODE(RB_DEVICE_TYPE, (Function), METHOD_BUFFERED, FILE_ANY_ACCESS)` —— **access 被硬编码进宏，没有 access 参数**。所以手册写 `IOCTL_RB_HEALTH_COMMIT = RB_CTL_CODE(0x907)` 时，**结构上不可能表达 `FILE_WRITE_ACCESS`**。

**C（结构体）——注意这个结果**
- 仅驱动侧改：`[FAIL] P5: 字段数不一致 驱动=9 ctl=7` ⇒ FAIL ✅
- **两侧一起改成同一套手册字段：`result: PASS (0 fail)`** ⚠
  ⇒ **P5 只比对「两侧是否一致」，不比「是否等于权威布局」。手册正是同时给出两侧代码 → 会放过。** 这是 Gate25 的真实盲区，与我 §3.1 的论断一致。

**D（`RbRegSetClean`，4 个隔离变体）**

| 变体 | 改动 | 门禁结果 |
|---|---|---|
| 基线 | — | PASS，call site = `rbmsg.c:732` |
| V1 | 仅定义换双参 `NTSTATUS` | **FAIL 2**（定义体定位 0 处 / I5 空转） |
| **V2 手册原样** | 定义+调用点都改 | **FAIL 3**（`RbRegSetClean(1)` 命中 0 / I5 / I6） |
| V3 | 保留单参 `BOOLEAN`，只删卷数门 | **FAIL 4**（无 `if(v)` / 未用 `RbCountAttachedVolumes` / 未调 `RbBootAttachedAny` / I5） |
| V4 | 单参 + `if(v)`，无门 + 裸 Zw | FAIL 4 |

⇒ 手册原样让 **3 条**失败；只删门让 **4 条**失败。**Gate25 能拦住 D。**

**E（jsdom 实测）**
```
jsdom getBoundingClientRect = {"width":0,"height":0,...}
手册新增 if (rect.width <= 0 || rect.height <= 0) return;
→ early-return = YES（变量永不写入）
--rbc-pointer-x = ""     现有测试期望 --rbc-lg-ptr-x = '12px'
```
⇒ `useLiquidGlassPointer.test.ts:53-54` 与 `liquid-glass.contract.test.ts:71` **2 个文件必红**。三处护栏真实行号：`prefers-reduced-motion` = `:9`+`:24`；`visibilityState==='hidden'` = `:24`；rAF 节流 = `:14`+`:26-28`+`:51`。

**H（0.96 变异体）**
```
变异体 WAVE_DAMPING = 0.96
expect(f1.cur).toEqual(f2.cur)   -> PASS（未侦测）
expect(f1.prev).toEqual(f2.prev) -> PASS（未侦测）
expect(WAVE_DAMPING).toBe(0.95)  -> FAIL（侦测到）
```
另证伪「精度不足」辩解：`stepWave(0.95)` vs `stepWave(0.96)` 逐元素可分辨，首个差异在 index 6800（`0.16489285…` vs `0.16662856…`）⇒ 是**传了同一个值**，不是精度问题。

### 8.4 QA 确认可保留的 15 项（与我 §1.4 互补）

1. `WAVE_DAMPING = 0.95`（`liquidWaveCore.ts:17`）
2. `reach` 公式与常量：`reach(0.135)=0.029245`、`reach(0.90)=0.750151`（与我 0.0292/0.7502 吻合）
3. V13 三条不等式全过，`coupledAmp(0.90,0,1,1)=0.432 ≤ 0.45`
4. Kelvin：`ny` 和精确 = 1、两瓣 `nx` **完全相等**（`0.47527746739995`）、反方向、`amp=0.272`
5. Ricker：`len=5`、`[0].amp=1`、`[1].amp=-0.28<0`
6. F24：`useLiquidWave.ts:110` `zIndex:'3'`
7. F25：`liquid-glass.css:409-410` `backdrop-filter:none`
8. **F25 修复规则实际有效**：基线 `none` → 手册规则 `blur(26px) saturate(1.8) brightness(1.04)` + `z-index:1`。因该规则与既有规则**同特异性 (0,4,1)** 靠后置取胜 ⇒ **说明作者懂特异性，`::before` 那条是疏漏而非无知**
9. 测试依赖：`@testing-library/react ^16.3.3`、`jsdom ^30.1.3`（`package.json:49,57`）
10. 令牌纪律：`--rbc-lg-*`=18 / `--rbc-glass-*`=18 / `--rbc-ios-*`=13，手册只新增 `--rbc-layer-*` ⇒ 三契约断言不受影响
11. 10 个 `--rbc-layer-*` **零死链**：声明 10 / 消费 10 / 双向差集为空
12. CSS 追加块不违规：`display:`/`flex-direction:`/`gap:`/`contain: paint`/`url(` 全 0 命中
13. 82 用例 / 12 测试文件基线复算一致
14. 现有 4 个门禁基线全绿：`check_a4_clean_gate` / `check_ctl_codes` / `check_ioctl_registry` / `check_proto_sync`
15. 275 成对判据语义描述属实（`read_shutdown_sentinel.py:181-193,313-316`）

---

## 9. 结论修订（并入 QA 后）

**原结论不变，但危害排序变了：**

1. **N-1（`RB_STATS2` 128 B→48 B）现在是第一高危** —— 它破坏的是一天到晚在用的主统计通道，10 处消费方、WPF 23 字段镜像、TS 协议层全部错位。我原判的 V-03（DIAG 24 B 错位）只影响关机诊断这一条冷路径；
2. **我们的门禁能拦住 B、D，但拦不住 C**（P5 只比对两侧一致性，不比权威布局）—— 手册同时给两侧代码即可绕过；
3. **I3 有裸 Zw 盲区（N-4）** —— 这是**我方门禁的真实缺陷**，与手册无关，应单独开单修；
4. 手册作者的**方向判断和数学是对的**（F25 修复规则使用了正确的 (0,4,1) 选择器并通过实测；`reach`/V13/Kelvin/Ricker 全部经两方独立验算）。**唯独 `::before` 那条漏了 `:not()` 对** —— 这是一处疏漏，不是系统性无知。
