# 97 · 64K 解禁 + 驱动性能减负 + 部署链解冻（2026-11-08）

> 授权：用户在本轮明确授权「64K 解禁 + 各模式性能/CPU 优化 + 其余未解项全部推进，缺什么自己搞」。
> 前置：96 号（94 号实施 SPEC OK）；59-64k-audit §5 前置清单（F1/F4/F5/F10/F13 已由 62 号批全部落地）。
> 新驱动构建：`src\driver\x64\Release\RAMBlockCache.sys` **未签名仅编译目录**，哈希见 §5；禁止提前签名/部署。

---

## 1. 64K 解禁（11-03 事故封禁 56 天后恢复 65536 档）

### 1.1 为什么现在可以解
| 59 号 §5 前置 | 状态 |
|---|---|
| F1 白名单封堵 | ✅（76 号证实四层已摘除——本批是**恢复**） |
| F4 slab 留池 2GB | ✅ 62 号批 `RB_SLAB_RETAIN_BYTES`=256MB（cache.c:385-406） |
| F5 64K 档单批 16MB 分配 | ✅ 62 号批批次自适应（test_f5_batch 断言） |
| F10 引擎几何 getter | ✅ 62 号批 |
| F13 卷尾越窗钳位（64K 最现实事故形态） | ✅ rbio.c LINK-3（本批 test_tail(65536) 回归绿） |
| §5.2 必补测试包 | ✅ **本批补齐**（§1.2） |
| WinDbg 复现 11-03 形态 | ⚠️ **不可满足**（无 VM/无 dump/无 PDB）；替代：本机 Verifier（待重启激活，§4）+ S4 五档矩阵真机验证 + 看门/回滚兜底。**残余风险如实登记**：11-03 的二进制（A39AF94C 自旋快路径+64K）从未归因；但该架构已被内联读快路径替代，静态审计 48R2/R3 + 59F1-F16 均无 P0/P1 |

### 1.2 测试包（59 §5.2 逐项）
- `run_one(65536)`：合法档断言（eff==bs、chunk=1MB、写后读命中一致、脏=0）✅
- `test_tail(65536)`：卷尾非对齐写 RMW 合并+读回一致 / truncate 清尾 / 越界 chunk 销毁 ✅
- `test_f4_slab` 加 65536：cap = 256 块（256MB/chunk 1MB）✅
- `test_rebuild` C16 非法值改 131072（65536 已合法）；SIM 三处镜像同步 ✅
- Release 25/25 + ASan 25/25 全绿

### 1.3 白名单同步面（8 层 + 门禁 + 发布链）
驱动 `rbctl.c rb_legal_bs[]` / `cache.c legal[]` / `rbrebuild.c` 两处 / CLI `ok_bytes[]` / WPF `MemoryView.xaml.cs`+`.xaml`（5 项）/ Web `SettingsPage.tsx`（含提示文案）/ **Gate10** `check_blocksize_contract.py`（LEGAL 五值、BANNED 仅剩 131072）/ **geom 契约** `check_geom_contract.py` / `test_flushpar legal_block_size` / 三条 finalize 链 payload 断言（`$rxBlockArray` 五值 + `$blockLabels` 加 '64 KB'）/ CI 注释。**131072 维持封禁**（slab 资源面放大 2×、无测试包、59 号同族 NO-GO 未撤销）。

## 2. 性能 / CPU（驱动侧三项减负；数字待真机 S 矩阵 A/B）

| 项 | 改动 | 依据 |
|---|---|---|
| PERF-1 | `RbSubmitIo` 的 IoRR 轮询游标全局 LOCK 自增**移入轮询兜底分支**——当前核亲和路径（绝大多数 IO）从不消费它，原实现纯付一次跨核乒乓 | 热路径审计 |
| PERF-2 | `rbfeat.c` RB2 遥测分片 32 路由相邻 8B 计数器共享 cache line → **64B 独占槽**（C_ASSERT 64）；命中读每 IO 两次 `InterlockedAdd64` 的跨核行乒乓归零 | 49 号 §3.1c 同域 |
| PERF-3 | **64K 档本身 = 49 号 §3.1d 的最大化**：per-chunk 开销 ÷8（同数据量块数 1/8，桶锁/引用/统计往返同比例下降） | 49 号蓝图 |
| 缓办登记 | 49 号 §3.1a（try_cache 两阶段）涉 cache.c 锁协议重构、§3.1b（遥测合并）快路径实测仅 2 次 volatile 读收益有限、§3.2（大文件驻留）需先做 per-volume 观测——**归入下一批**（待真机 S 矩阵基线定位最痛路径） | — |

**CPU 调用情况**：96 号已把全局原子从每 IO 2 次（原案 Interlocked 对）降到 0（per-CPU 槽 + DISPATCH 短窗 CR8 写）；本批再移除 1 次（IoRR）+ 命中读 2 次行乒乓（RB2）。热路径 LOCK 前缀清单（每 IO）：dispatch `Refs` inc/dec + `IoFailRun` + 命中遥测（本核行）——已是无跨核行的最小集。

## 3. 部署链解冻（用户「点一下就关」根因修复）

- **根因**：93 号冻结把 `RB-finalize-rebuild-fix.cmd` 砍成**无条件** `exit /b 1`（用户已正确创建 `94-SPEC-OK-LIFTED.ok`，但 cmd 根本不查）；且 `finalize_rebuild_fix.ps1` 检查的是旧文件名 `93-NOGO-LIFTED.ok`（94 号改名未同步）。
- **解冻改造**：cmd 护栏改**条件**（解除文件存在才请求 UAC，缺失即 fail-closed 退出）；ps1 门改查 `94-SPEC-OK-LIFTED.ok` + 叙述更新为 96 号 SPEC OK 历史；按 94 号 §5.2/B-1+H-7 补 **marker LAST**（新第 11 步：全部检查通过后才写 `finalize-utc.txt` 并回读校验，使 X2 `boot>marker` 真正证明本次 UAC 重启）；scope 声明同步。
- 链其余纪律逐字保留（-999 哨兵 / gate5 双判据 / Invoke-Step / scope 双回读 / ledger FATAL+read-back / deploy.ps1 CHILD / payload 五值断言）。

## 4. 验证与环境项

- 驱动 BUILD OK（未签名）；CLI 重编（`87aee00c…`/`eae22787…`，Gate9 PASS）；Web `tsc --noEmit` 0 错 + vite build 绿；Release/ASan 25/25；门禁全套 PASS（repo/proto/ctl/blocksize/geom/stats/installer/ps-syntax 71 文件）。
- **Verifier（O-8 部分闭环）**：`RB-arm-verifier.cmd`（新）在**验收通过后**由用户选择激活（标准档：Special Pool+IRQL+资源注入，仅 RAMBlockCache.sys），第二次重启生效；不在部署链内（风险分箱）。
- push：代理 502 未恢复则维持本地 ahead，网络恢复后 `git push`。

## 5. 现场与下一步

```
下一步（用户）：
  1. 双击 RB-finalize-rebuild-fix.cmd（一次 UAC）→ 等待 ALL OK
  2. 重启一次 + 交互登录
下一步（主理人）：sync_setup_hash.ps1 -RequireSetupChanged → verify_final_reboot.ps1（FAIL=0）
  → S4 五档矩阵 4K→8K→16K→32K→64K→4K（canary/卷集合/DirtyBytes=0 逐档复核）
  → 验收通过后可选：RB-arm-verifier.cmd（Verifier 激活）+ 第二次重启
```
冻结解除证据：`94-SPEC-OK-LIFTED.ok` 已由用户创建（主理人人工，符合 94 号 §5.1）；本机 System32 仍 `10fb7009…`（部署由链完成）。
