# 97 号 delta 独立复核（64K 解禁 + 驱动性能减负 + 部署链解冻）· 2026-11-08

> 复核者：qa-rebuild-spec 独立复核（作者≠验证者，只读）
> 对象：commit `97cedc1`（deliverables/97-64k-unban-perf-unfreeze-2026-11-08.md），基线 `fb8a54f`（96 号复核 SPEC OK）
> 方法：git diff fb8a54f..97cedc1 全量 + 全仓 grep + 门禁复跑 + 测试 .out 佐证 + 工作树比对（`git status` = 0 改动，工作树 == 97cedc1；早前 diff 全文件差异仅 CRLF）。

---

## 1. 64K 白名单八层一致性 —— PASS

| 层 | 位置 | 65536 | 131072 | 证据 |
|---|---|---|---|---|
| 1 驱动 clamp | src/driver/rbctl.c:175 `rb_legal_bs[]` | ✅ 含 | ✅ 不含 | `{4096, 8192, 16384, 32768, 65536}` |
| 2 core legal | src/core/cache.c:958 `legal[]` | ✅ 含 | ✅ 不含 | 同上五值；非法回落 4096（:960-961） |
| 3a 重建守卫① | src/driver/rbrebuild.c:425-428 | ✅ 含 | ✅ 不含 | `newBlockSize != 65536u`（[64K-UNBAN]） |
| 3b 重建守卫② | src/driver/rbrebuild.c:726-729 | ✅ 含 | ✅ 不含 | 同款（RbRequestGeometryRebuild） |
| 4 CLI | src/ctl/ramboostctl.c:1533 `ok_bytes[]` + :1535 循环上界 `j < 5` + :1536 警告文本 `{4/8/16/32/64 KB}` | ✅ | ✅ | 三件套齐；提交的 ramboostctl.exe（Nov 8 00:36，随 97cedc1）二进制内实测含 ASCII 字面量 `4/8/16/32/64`（已验） |
| 5 WPF 代码 | src/ui/wpf/Views/MemoryView.xaml.cs:17 `_blockSizes` | ✅ | ✅ | 五值；128K 回读 -1→落回 4K 不越界（:19） |
| 6 WPF UI | src/ui/wpf/Views/MemoryView.xaml（blockSizeBox 5 个 ComboBoxItem；全文件 ComboBoxItem 共 9 = 5 档位 + 4 刷新间隔，对齐 _flushMs） | ✅ | ✅ | 新增 `64K (65536 B)` 项 |
| 7 Web | web/src/features/settings/SettingsPage.tsx:16 `BLOCK_SIZES` + :202 提示文案 | ✅ | ✅ | 文案含 64 KB 及小随机建议 4K |
| 8 门禁 | check_blocksize_contract.py（LEGAL 五值、BANNED 仅 131072）/ check_geom_contract.py LEGAL 五值 | ✅ | ✅ | 两门禁复跑 PASS（见 §6） |

**漏网点 grep（65536 / 131072 全仓）**：
- configSchema/zod：web/src/bridge/http-transport.ts:129 `BlockSize: z.number()`——无 z.enum/z.literal/refine 锁死；SettingsPage.tsx:198 对旧配置越表值加「当前值」兜底项，优雅。✅
- rb_common2.h:164 仅注释提及 s_Cnt 槽位，无白名单。✅
- rbfeatctl.c：无块大小白名单（仅 "refused-blocksize" 状态名 :126）。✅
- C# 侧：RbProto.cs:112 仅协议字段注释列举 `…/65536/131072`（文档性，无校验逻辑）。LOW（见 L-5）。
- 非漏点的同名数字：RB_MAX_CACHE_IO_KB 系列（rb_common.h:47-52、rbio.c:1680、rbrebuild.c 相关 64MB/128MB IO 门）与块大小白名单无关。✅
- 残留旧四值：仅 qa 产物副本与 bench 工具（详见 L-5），不在发布链/门禁内。

## 2. cache.c 改动面 —— PASS

`git diff fb8a54f..97cedc1 -- src/core/cache.c`：13 行，全部为 legal[] 注释块重写（[F1]→[64K-UNBAN]）+ `legal[]` 数组 4→5 值。**无任何逻辑改动**（回落 4096 分支、几何派生、锁、flush 均未触碰）。

## 3. 测试真实性 —— PASS（.out 佐证 + 树一致）

- **test_geom**（build 产物 src/core/build/rel_test_geom.out，Nov 8 00:38，commit 00:40）：`bs=65536 eff=65536 chunk=1048576`（1MB=64K×16，**真 64K 档运行，非回落**）；`bs=131072` → 摘档回落 `eff=4096 chunk=65536`；`F4 cap bs=65536 → 256 块（256MB/chunk 1024KB）`；`tail bs=65536 vol=5377285 nsz=5343740` 三断言（RMW 合并+truncate 清尾+越界 chunk 销毁）齐全；末行 PASS。test_geom.c:44 现源含 `||bs==65536u` 合法档断言、:333-338 main 序列与 .out 结构逐一对应（tail 65536 段仅存在于新源 → .out 确为本次新码跑出）。ASan 侧 asan_test_geom.out（00:32）同含真 64K 行，PASS。
- **test_rebuild C16**：test_rebuild.c:1003 `raw = 131072u`（非法值改用仍封禁 128K）→ sim_clamp_bs:473 与 sim_recreate 守卫:491-492 均已含 65536u（三处镜像同步属实）。逻辑自洽：131072 clamp→4096；旧判定 raw∉白名单→漏判（复现缺陷面）；修复后按 clamp 副本 4096 != 16384 → 判重建请求 → 以 4096 入队重建成功。rel_test_rebuild.out / asan_test_rebuild.out：`RESULT: PASS (FAIL=0)`。
- **test_flushpar**：legal_block_size 五值（src/core/test_flushpar.c:131）。✅
- **限制**：测试 exe 跑后已删（无 rel_test_*.exe），复核禁止重编译，无法复跑；佐证 = 272 个 Nov 8 00:31-00:38 新鲜产物（Release+ASan）+ 工作树与 commit 逐位一致（0 diff）+ 门禁复跑全 PASS。可接受。

## 4. 性能项语义 —— PASS

**PERF-1 RbSubmitIo（rbio.c）**：
- (a) 当前核亲和成功路径（:1418-1439）零全局原子：slot 命中即 `InsertTailList + KeSetEvent + return`（:1430-1433），不触 IoRR。✅
- (b) 兜底轮询语义不变：start 于 :1444 `InterlockedIncrement(&g_Rb.IoRR) - 1`（唯一自增点、唯一消费点前），:1445-1456 `for (i = 0; i < n; i++)` 从 `(start+i) % n` 全扫，全死落系统队列（:1457-1464）。起点单调轮转保持（仅兜底用户推进游标，无饥饿面）。✅
- (c) 两处 `ULONG start`：rbio.c:1405（本函数）赋值 :1444 先于唯一使用 :1446；rbio.c:1892（RbFlushSubmit，本批未改）赋值 :1894 先于使用 :1909。无未初始化使用；n==0 时循环不进入（同旧行为，无除零）。✅

**PERF-2 rbfeat.c RB2 垫槽**：
- C_ASSERT(sizeof(RB2_CNT_SLOT)==64)（rbfeat.c:46；8+56=64B）。s_Cnt 声明 :48。
- 全部 s_Cnt 消费者已改 `.v`：grep s_Cnt 全出现点 = rbfeat.c:13/36/48（声明与注释）、:52（Rb2Count）、:61（Rb2CntSum）、rb_common2.h:164（注释）。Rb2StatsFill 经 Rb2CntSum 间接读，无裸访问残留。✅

## 5. 解冻脚本 fail-closed —— PASS

- **RB-finalize-rebuild-fix.cmd**：`if not exist …94-SPEC-OK-LIFTED.ok → echo + exit /b 1`（:21-27）位于 `net session`/UAC（:29-33）**之前** → 文件缺失时不请求 UAC 直接退出。✅
- **finalize_rebuild_fix.ps1**：
  - 门：:125-135 查 `94-SPEC-OK-LIFTED.ok`（改名同步完成），位于日志初始化（:204）与一切写操作之前，缺失 `exit 1`。✅
  - marker LAST：新第 11 步 :502-513——前 10 步（sign/anchors/gate5×3/build_setup/payload 断言/deploy CHILD/post-gate5/ledger/scope 双回读）全部通过后才 `Set-Content` 写 finalize-utc.txt 并立即回读校验（失败 Abort）。marker 之后仅 L/Write-Host/exit 0（第 12 步 :515-524），无 Abort 豁口。✅
  - 无 verifier 操作：全脚本无 verifier.exe 调用（scope 文本 :250 仅声明 "no verifier /reset"）。✅
  - 纪律保留：Invoke-Step -999 哨兵（:152-166）、gate5 双判据（:195-202）、ledger FATAL+双回读（:424-446）、deploy.ps1 CHILD（:405-406）、scope 双回读（:448-500）、README hash-line-only（:291-296, :461-463）。✅
- **三条 finalize 链 `$rxBlockArray` 五值**：rebuild_fix.ps1:109 / distribution_fix.ps1:94 / setup_only.ps1:83 三处 regex 同款五值。✅（`$blockLabels` 见 M-1。）
- **check_ps_syntax 复跑**：`files=71 fail=0 (bom_miss=0 syntax_err=0 dup_sw=0) cmd_files=33 cmd_chr=0 cmd_bom=0`。✅
- **RB-arm-verifier.cmd**：独立可选脚本，不在部署链（finalize/deploy 无引用），UAC 自提权仅对 RAMBlockCache.sys `/standard`，文内自带二次蓝屏回滚提示（verifier /reset）。分箱正确。

## 6. 门禁复跑 —— 全 PASS

| 门禁 | 结果 |
|---|---|
| check_repo.py | PASS (0 fail, 0 warn) |
| check_proto_sync.py --strict | PASS（23 组镜像核对 + 版本五联动） |
| check_blocksize_contract.py (Gate10) | PASS（LEGAL 五值 / BANNED 131072） |
| check_geom_contract.py | PASS（stripe key 0 违例；白名单五值断言过） |
| check_installer_contract.py (Gate11) | PASS（13 payload 名） |
| check_stats_contract.py | PASS（sizeof=1304 / decode 守卫五值） |
| check_ctl_freshness.py (Gate9) | PASS（exe 含 5 statval keys） |
| check_ps_syntax.ps1 | files=71 fail=0 |

rel_test_geom/rel_test_rebuild exe 已删无法复跑（禁止重编译），以 §3 的 .out 佐证替代。

## 7. 64K 特有风险面抽查 —— 无 P0/P1

- **pre.max_bits=2048 × 64K 子块数学**：sub_shift=log2(block)（cache.c:964，64K→16，子块=块=64KB，chunk=1MB=16 子块）。切块 carve_bs 取引擎几何 getter（rbio.c:964，与 rb_write_ex 内 `offset & ~(eng->block_size-1)`（cache.c:1879）同源，无 config/engine 分裂）；`cend=(cur&~(carve_bs-1))+8MB`（rbio.c:982）⇒ 每块 nbits = 8MB/64K = **128 位 < 2048**（4K 档 = 2048 == max_bits，`nbits > max_bits` 为假，恰好贴合；越界即 bits=-1 走保守记账，安全侧）。✅
- **read_plan 128KiB 片跨 64K 块**：read_plan.c:3-5 片 ≤32×4K=128KiB、窗口 4K 对齐 ⇒ 一片至多跨 2 个 64K 块，miss 区间数 ≤3 << RB_MAX_RANGES=32（rb_common.h:43），且 rbio.c:808-813 有 mcnt 越界防御性失败。✅
- **L2 键控**：L2 键 = lba/64KB 与块几何无关；rbL2.c:307-313 几何门（block*16==64KB 才开 = 仅 4K 档）fail-closed 拒绝，64K 档 L2 关闭（与 8/16/32K 同），键空间分裂被门挡死。INFO-I-1。✅
- **zone flush**：zone_of = off>>28（256MB/zone，字节维度，块几何无关，cache.c:271-272）；`zone_of(c->start) % nzones` 已有 nzones<=0 防除零（cache.c:2346-2354）。✅
- **F4/F5 前置在 64K 的实效**：slab cap = 256MB/1MB=256 块（cache.c:386-397 注释与测试一致）；F5 单批 = (4MB-64KB)/1MB = 3 块 ≈3.1MB/次 grow（16MB 大额分配已消解）。✅

---

## 缺陷分级

**BLOCKER：无。 HIGH：无。**

**MEDIUM**
- **M-1** 交付物 §1.3 声称「三条 finalize 链 … `$blockLabels` 加 '64 KB'」，实际仅 rebuild_fix.ps1:108 扩为五标签；finalize_distribution_fix.ps1:93、finalize_setup_only.ps1:82 仍四标签，且两链日志（distribution_fix:301 / setup_only:243）宣称 "carries 4/8/16/32/64 KB" 而实检仅 4 标签。功能风险低：`$rxBlockArray` 五值断言三条链齐全（rebuild_fix:109 / distribution_fix:94 / setup_only:83），SettingsPage chunk 的新鲜度仍被数值数组钉死；'64 KB' 文字标签断言缺口属强度不对称 + 交付物超述。建议下批补齐两处 `$blockLabels`。
- **M-2** finalize_rebuild_fix.ps1 文件头纪律注释与本批行为相悖：:16 "NO marker write"、:26-27 "finalize-utc.txt is READ ONLY … writing it would break boot_utc > finalize_utc"、:44 序列表 "11) ALL OK"——实际第 11 步已改为写 marker（:502-513，94 §5.2/B-1+H-7，运行期 scope 声明 :250 已如实更新）。行为正确且优于旧模型（marker LAST 使 X2 boot>marker 真证本次重启），但本链是 fail-closed 纪律链，93 号事故根因正是「文件名/叙述漂移未同步」同类；头部陈旧叙述必须下批清理，防后来者按头注释回改。

**LOW（不拦批，登记）**
- L-1 .github/workflows/ci.yml:122 Gate10 注释残留句「三者均不含 64K/128K（76 号 NO-GO 封禁）」与上句更新后的五值表述自相矛盾（仅注释；门禁脚本本身已更新且 PASS）。
- L-2 残留 [F1] 旧注释与新 [64K-UNBAN] 注释同点矛盾：rbctl.c:168-171（"65536/131072 自白名单摘除"紧邻 "65536 恢复"）、test_geom.c:40-41（注释称 65536 白名单外，代码 :44 已含）、test_flushpar.c:126 头注释。
- L-3 三链 Abort 消息 "missing the 4-value block-size array"（rebuild_fix:369 / distribution_fix:300 / setup_only:242）陈旧。
- L-4 check_geom_contract.py:27 `LEGAL_STR` 仍为四值展示串（仅影响输出文案，:80/:123；实判用五值 LEGAL，PASS 已证）。
- L-5 qa 产物副本残留旧四值白名单（不在发布链/门禁内）：src/core/build/fix_verify_prefix_cache.c:955、src/core/build/qa4/cache_mut.c:955（qa_teeth.ps1 编译的冻结变异副本）、src/core/build/qa_geom_diag.c:131；bench_readhit.c:71 拒绝 65536（bench 工具）；RbProto.cs:112 注释把 131072 列入协议文档。均无功能面。

**INFO**
- I-1 64K 档 L2 关闭（rbL2.c:309 几何门，与 8/16/32K 同）——S4 五档矩阵性能预期需知：64K 无 L2 叠加，非缺陷（fail-closed 拒绝语义正确）。
- I-2 测试 exe 已删，测试真实性依赖 272 个 Nov 8 00:31-00:38 新鲜 .out/.log + 工作树与 97cedc1 逐位一致 + 门禁复跑；无法（也不允许）重编译复跑。
- I-3 push 未完成（main ahead 5），与交付物 §4「代理 502 维持本地」一致。
- I-4 src/driver/x64/Release/RAMBlockCache.sys（Nov 8 00:31）在仓未签名——与交付物「仅编译目录、禁止提前签名/部署」一致，未发现越权签名/部署痕迹。

## 结论

八层白名单一致、cache.c 改动面干净、性能两项语义正确且无未初始化/漏改、解冻链 fail-closed 与 marker-LAST 落实、八项门禁复跑全 PASS、64K 特有风险面（切块数学/读片/L2/zone flush/slab 前置）无 P0/P1。缺陷面全部为文档/断言强度级（MEDIUM×2、LOW×5），不构成拦批项；M-1/M-2 应随下批 docs 清理。64K 解禁风险残余按交付物如实登记（WinDbg 复现不可满足 → Verifier 可选兜底 + S4 真机矩阵），路径成立。

SPEC OK
