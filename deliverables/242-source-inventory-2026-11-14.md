# 242 — RAMBlockCache 全量源码清单（当前时刻 / 附可下载源码包）

生成时间：2026-11-14 22:5x（本地）｜性质：**只读盘点 + 打包**，不改任何源码。
本清单是 `238-source-inventory-2026-11-14.md` 的当前快照，覆盖本轮 239/240/241 全部改动。
编译源的唯一事实源仍是 `build/sources.manifest.json`；本清单范围更宽（含配置/脚本文档）。

## §1 汇总

| 模块 | 文件数 | 总行数 | 总字节 |
|---|---:|---:|---:|
| 内核驱动 (src/driver) | 27 | 12842 | 720646 |
| 缓存引擎 (src/core) | 13 | 4175 | 228893 |
| 公共协议 (src/common) | 7 | 514 | 16168 |
| 控制面 CLI (src/ctl) | 16 | 3388 | 163304 |
| 静默安装引擎 (src/setup) | 5 | 1446 | 74151 |
| PowerShell 脚本 (src/scripts) | 83 | 13055 | 644101 |
| Web 控制台 (src/ui/web) | 77 | 17866 | 670161 |
| WPF 控制台/安装 UI (src/ui) | 108 | 16812 | 884261 |
| .NET 工具 (src/tools) | 9 | 1194 | 53020 |
| 构建系统 (build) | 4 | 1241 | 49391 |
| 测试 (tests) | 281 | 35985 | 1749799 |
| 静态审计工具 (tools) | 8 | 2376 | 99022 |
| 设计文档 (docs) | 17 | 1467 | 90292 |
| 证书 (certs) | 1 | 12 | 1358 |
| 本轮新增运维脚本 (extras) | 2 | 110 | 4237 |
| 仓库根文档 | 4 | 876 | 204136 |
| 其他 | 3 | 347 | 17929 |
| **合计** | **665** | **113706** | **5670869** |

排除项（刻意范围界定，非遗漏）：`node_modules/`、`.git/`、`.workbuddy*/`、`.mimosa/`、`.nuget/`、
`obj/`、`bin/`、`x64/Release/`、`target/`、`dist/`、`deliverables/`（历史交付物）、`archive/`、
`tests/core/build/`，以及 `.exe/.sys/.dll/.pdb/.obj/.lib/.zip/.png/.log/.rlib/.rmeta/.map` 等
二进制与构建产物；单文件 > 2 MB 亦排除（`src/ui/installer/.../payload/*.zip` 属发布载荷）。
`package-lock.json` 保留以保证依赖可复现。

## §2 物理源码包

`242-RAMBlockCache-source-bundle-2026-11-14.zip` 与下面清单**逐文件一一对应**（665 条目），
目录结构保持仓库相对路径；`extras/` 为本轮新增、位于交付物目录下的两个自提权运维脚本。
包体 1,980,422 B，sha256 `2c61e2a3fca807892c936cfca370d3ced0c982abd9808bce7d70e9d196706fd8`。

## §3 逐文件清单

### 内核驱动 (src/driver)（27 文件 / 12842 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `src/driver/RAMBlockCache.inf` | 26 | 1267 | `87e6cd85abb7` |
| `src/driver/RAMBlockCache.vcxproj` | 172 | 9447 | `332a501820bc` |
| `src/driver/rbL2.c` | 1243 | 76437 | `65b80950e7e5` |
| `src/driver/rb_common.h` | 692 | 45285 | `28f9a313d319` |
| `src/driver/rb_common2.h` | 206 | 11956 | `f15cf2e0f03d` |
| `src/driver/rb_stub.c` | 10 | 453 | `58ea22ecea67` |
| `src/driver/rbattach.c` | 1689 | 90767 | `9bdb8b189165` |
| `src/driver/rbconfig.c` | 130 | 8255 | `cdeacd07c524` |
| `src/driver/rbctl.c` | 1161 | 66069 | `1b0ce41fa483` |
| `src/driver/rbdev.c` | 270 | 13609 | `863871701fa2` |
| `src/driver/rbdriver.c` | 1261 | 65829 | `34c96114e729` |
| `src/driver/rbetw.c` | 81 | 3500 | `56f8fee169a7` |
| `src/driver/rbfeat.c` | 958 | 50774 | `5bd60fcb535f` |
| `src/driver/rbhot.c` | 87 | 4150 | `1ae145498e7e` |
| `src/driver/rbio.c` | 627 | 36722 | `ca2858569173` |
| `src/driver/rbio_budget.c` | 174 | 9607 | `d1a48ec73435` |
| `src/driver/rbio_flush.c` | 586 | 32287 | `c972c0bcd1b6` |
| `src/driver/rbio_ports.c` | 70 | 3442 | `1f5f53539ceb` |
| `src/driver/rbio_worker.c` | 1341 | 80922 | `070ebaefd918` |
| `src/driver/rblog.c` | 123 | 6358 | `c76d6fa66b2b` |
| `src/driver/rbmsg.c` | 264 | 13430 | `f49ca16decc4` |
| `src/driver/rbpools.c` | 212 | 11579 | `ae0b5c2b33a9` |
| `src/driver/rbpref.c` | 252 | 12843 | `1cbba1637db4` |
| `src/driver/rbquiesce.c` | 131 | 7260 | `6922e6bf43e1` |
| `src/driver/rbrebuild.c` | 765 | 42466 | `0edc2c99382b` |
| `src/driver/rbthermal.c` | 110 | 4978 | `0b3f23188650` |
| `src/driver/rbwback.c` | 201 | 10954 | `57f3aef45393` |

### 缓存引擎 (src/core)（13 文件 / 4175 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `src/core/build/qa4/cache_flush_mut.c` | 485 | 27733 | `f39da0b7f783` |
| `src/core/build/qa_geom_gate.json` | 8 | 575 | `e5e5695901cf` |
| `src/core/cache.c` | 634 | 30927 | `91d1009963a0` |
| `src/core/cache.h` | 312 | 19897 | `6fb7b9c63e9a` |
| `src/core/cache_evict.c` | 301 | 14578 | `ab8c7613bd19` |
| `src/core/cache_flush.c` | 513 | 28852 | `87c89512e1ae` |
| `src/core/cache_internal.h` | 732 | 40736 | `55e9ee54118f` |
| `src/core/cache_read.c` | 254 | 14553 | `174511a3a5a4` |
| `src/core/cache_stats.c` | 65 | 3882 | `87fad8a96d9e` |
| `src/core/cache_testhook.inc` | 105 | 4643 | `bf764000d752` |
| `src/core/cache_write.c` | 694 | 39403 | `db7c9447b3d3` |
| `src/core/read_plan.c` | 23 | 824 | `b548c59f39b4` |
| `src/core/read_plan.h` | 49 | 2290 | `0d50d8e2862a` |

### 公共协议 (src/common)（7 文件 / 514 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `src/common/rb_alloc.h` | 12 | 498 | `a3818076cc3f` |
| `src/common/rb_percpu.h` | 150 | 4111 | `84bb8b7cafcd` |
| `src/common/rb_shard.h` | 119 | 2880 | `c5ead930217a` |
| `src/common/rb_shardlock.h` | 65 | 1635 | `3d3c15cacef7` |
| `src/common/rb_threadsched.h` | 124 | 4341 | `782d1196d7d1` |
| `src/common/rb_wiring.h` | 18 | 1334 | `2c38659e827c` |
| `src/common/version.h` | 26 | 1369 | `44d33bcbab38` |

### 控制面 CLI (src/ctl)（16 文件 / 3388 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `src/ctl/bench_policy.c` | 25 | 928 | `f59c1a4f8522` |
| `src/ctl/bench_policy.h` | 16 | 551 | `463beddee0d3` |
| `src/ctl/ctl_bench.c` | 454 | 21472 | `da110d09c26a` |
| `src/ctl/ctl_conn.c` | 254 | 9214 | `fab54a08b14f` |
| `src/ctl/ctl_events.c` | 85 | 3933 | `006a1e8c42b8` |
| `src/ctl/ctl_internal.h` | 89 | 4338 | `1e11963a5473` |
| `src/ctl/ctl_license.c` | 201 | 8699 | `7d19de617312` |
| `src/ctl/ctl_preflight.c` | 157 | 7435 | `22d314875824` |
| `src/ctl/ctl_signature.c` | 264 | 12630 | `54c48a54b8c4` |
| `src/ctl/ramboostctl.c` | 769 | 41151 | `8205b1ce6b92` |
| `src/ctl/rb_bootcfg.h` | 38 | 1528 | `8f43b46fb44e` |
| `src/ctl/rb_excl.h` | 62 | 2879 | `c615329f0add` |
| `src/ctl/rb_logq.h` | 67 | 2917 | `10851f0ffe37` |
| `src/ctl/rb_proto.h` | 358 | 17873 | `3a56d9414d3f` |
| `src/ctl/rbfeatctl.c` | 489 | 25158 | `c4bd615fc0be` |
| `src/ctl/test_bench_policy.c` | 60 | 2598 | `77e6e4162264` |

### 静默安装引擎 (src/setup)（5 文件 / 1446 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `src/setup/RamBoostSetup.exe.manifest` | 14 | 647 | `2613ad72fd3e` |
| `src/setup/ramboostsetup.c` | 1141 | 59649 | `a681600cffce` |
| `src/setup/ramboostsetup.ps1` | 258 | 11935 | `450bfcf6f39c` |
| `src/setup/shortpaths.txt` | 1 | 239 | `609071c712b7` |
| `src/setup/sign_driver.ps1` | 32 | 1681 | `0822e2a2de59` |

### PowerShell 脚本 (src/scripts)（83 文件 / 13055 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `src/scripts/analyze_bugcheck_dump.py` | 207 | 8883 | `c85d1af75a11` |
| `src/scripts/bench_proto.ps1` | 284 | 16257 | `4575131dbf88` |
| `src/scripts/benchmark.ps1` | 16 | 1224 | `72c37a29859f` |
| `src/scripts/boot_guard.ps1` | 48 | 2806 | `f687b4027f9c` |
| `src/scripts/bridge_keeper.ps1` | 82 | 3896 | `bffb2257b11d` |
| `src/scripts/bridge_watchdog.ps1` | 65 | 2478 | `93fc96184422` |
| `src/scripts/buffer_write_baseline.txt` | 104 | 4837 | `fa1e1206d3a3` |
| `src/scripts/build_setup.ps1` | 271 | 19496 | `5cec84137c5f` |
| `src/scripts/check_blocksize_contract.py` | 176 | 8036 | `3201acd71dd2` |
| `src/scripts/check_buffer_write_contract.py` | 182 | 9157 | `7229727acba0` |
| `src/scripts/check_build_manifest.py` | 390 | 17851 | `148084422656` |
| `src/scripts/check_coexist.ps1` | 84 | 4839 | `a96414457e20` |
| `src/scripts/check_config_decode_contract.py` | 386 | 18051 | `ffae61bd365e` |
| `src/scripts/check_ctl_freshness.py` | 243 | 9995 | `89850ad68480` |
| `src/scripts/check_docs_numbering.py` | 237 | 10073 | `817dedb3039f` |
| `src/scripts/check_geom_contract.py` | 137 | 6938 | `3bfb8d8b5170` |
| `src/scripts/check_installer_contract.py` | 201 | 9441 | `711bf3324e5a` |
| `src/scripts/check_ioctl_registry.py` | 206 | 8064 | `1431739e5ead` |
| `src/scripts/check_l1_accel_readiness.ps1` | 64 | 3265 | `4134edfdee78` |
| `src/scripts/check_l2_contract.py` | 199 | 9329 | `6164f3d10dd9` |
| `src/scripts/check_layout.py` | 127 | 6031 | `27afb75ca300` |
| `src/scripts/check_node_contract.py` | 82 | 3599 | `6dd8c6fe6d6a` |
| `src/scripts/check_poolcopy_contract.py` | 219 | 11206 | `72e1e51ab667` |
| `src/scripts/check_proto_codegen.py` | 92 | 3891 | `3922ae943e6d` |
| `src/scripts/check_proto_sync.py` | 1156 | 59053 | `2ece80553531` |
| `src/scripts/check_ps_syntax.ps1` | 197 | 10825 | `327a7eddae95` |
| `src/scripts/check_repo.py` | 273 | 13945 | `c6538480ad03` |
| `src/scripts/check_signature.ps1` | 153 | 7208 | `0d46fcf849dc` |
| `src/scripts/check_stats_contract.py` | 172 | 8085 | `df88cf362e72` |
| `src/scripts/check_ts_proto_sync.py` | 157 | 6801 | `d9822389f969` |
| `src/scripts/check_xaml_resources.py` | 61 | 2640 | `b9566b05de34` |
| `src/scripts/copy_dumps.ps1` | 30 | 1608 | `92ba22107118` |
| `src/scripts/copy_rb_file_readwrite_share.ps1` | 33 | 1063 | `7e7e9897ef73` |
| `src/scripts/ctl_safe.ps1` | 80 | 3613 | `c8d8f13df53e` |
| `src/scripts/deploy.cmd` | 15 | 741 | `7b9548159861` |
| `src/scripts/deploy.ps1` | 568 | 31482 | `7500660da53a` |
| `src/scripts/deploy_w15.ps1` | 62 | 4211 | `76fbeea8cc9a` |
| `src/scripts/devloop.ps1` | 147 | 8466 | `dbe41e1996ad` |
| `src/scripts/game_mode.cmd` | 19 | 1090 | `0d21c6911b23` |
| `src/scripts/game_mode.ps1` | 65 | 2959 | `cd4518eec525` |
| `src/scripts/gen_proto_ts.py` | 254 | 9754 | `a82b204358d3` |
| `src/scripts/gen_version.ps1` | 49 | 2323 | `4f445a8462bd` |
| `src/scripts/install.cmd` | 20 | 1032 | `150f1584f8a3` |
| `src/scripts/install.ps1` | 115 | 7480 | `b338d044af5d` |
| `src/scripts/install_auto.ps1` | 6 | 450 | `70fcf81ce6de` |
| `src/scripts/install_auto2.ps1` | 6 | 451 | `873cc6081994` |
| `src/scripts/j6_deploy_guard_fix.ps1` | 85 | 4925 | `b241cbde64b9` |
| `src/scripts/j6_rearm_upperfilters.ps1` | 64 | 3560 | `380b10415448` |
| `src/scripts/j6_reboot_hit.py` | 1211 | 57747 | `530a169a61ca` |
| `src/scripts/l1_perf_orchestrator.ps1` | 99 | 4636 | `bc785d0dc956` |
| `src/scripts/logon_disarm.ps1` | 12 | 539 | `8d4908b3a40c` |
| `src/scripts/offline_rollback.cmd` | 159 | 7194 | `c9aca29a1975` |
| `src/scripts/p5_verdict.py` | 223 | 9931 | `eeb05b1dfa17` |
| `src/scripts/p5b_directed_reread.py` | 472 | 20330 | `63fdf12d914f` |
| `src/scripts/parse_l2_idx.py` | 135 | 5546 | `1241dd53d6fe` |
| `src/scripts/pfro_pending.ps1` | 53 | 2873 | `b71db04956b6` |
| `src/scripts/phase4_close_race_probe.ps1` | 139 | 6285 | `cc14d4d3b5ee` |
| `src/scripts/qa_geom_gate.ps1` | 368 | 17643 | `fa455947f614` |
| `src/scripts/qa_paths.ps1` | 89 | 3738 | `07dc8ee38a77` |
| `src/scripts/qa_recovery_20261024.ps1` | 150 | 8904 | `8028363da159` |
| `src/scripts/qa_repeat_concurrency_20261020.ps1` | 51 | 2373 | `d21f7a090b48` |
| `src/scripts/qa_smoke_wsconc.ps1` | 41 | 1923 | `d11fd829b468` |
| `src/scripts/qa_sweep_wsconc.ps1` | 69 | 3590 | `cd86a0fd0dde` |
| `src/scripts/qa_teeth.ps1` | 173 | 10628 | `01750bd16ce5` |
| `src/scripts/qa_w4check_20261020.ps1` | 114 | 5354 | `f4ba783205f8` |
| `src/scripts/rb_common.ps1` | 200 | 10415 | `76f475246328` |
| `src/scripts/rbmemguard.ps1` | 45 | 2106 | `f696c8538563` |
| `src/scripts/restart_bridge.ps1` | 91 | 4120 | `b09d7555cc38` |
| `src/scripts/run_diskspd.cmd` | 19 | 1098 | `01f7942c96fb` |
| `src/scripts/run_diskspd.ps1` | 46 | 2238 | `88daf713ea54` |
| `src/scripts/set_node27_path.ps1` | 55 | 2612 | `20c52f59566d` |
| `src/scripts/stats2.schema.json` | 107 | 1083 | `083236f9cd3d` |
| `src/scripts/sync_anchors.ps1` | 87 | 4968 | `201b1113f2ea` |
| `src/scripts/sync_setup_hash.ps1` | 213 | 11902 | `2eb10e16d3b0` |
| `src/scripts/uninstall.cmd` | 9 | 405 | `66f9dc916e7b` |
| `src/scripts/uninstall.ps1` | 71 | 3937 | `4880104ffc3b` |
| `src/scripts/use_node27.ps1` | 16 | 1051 | `e018aa45003f` |
| `src/scripts/verifier_disable.cmd` | 21 | 696 | `f0ba3aef00bc` |
| `src/scripts/verifier_enable.cmd` | 38 | 1662 | `10fda2c0da22` |
| `src/scripts/verifier_harvest_check.ps1` | 87 | 4246 | `5d2f44944ace` |
| `src/scripts/verify_deploy_signature.ps1` | 166 | 7669 | `f3b4e4aaa7a7` |
| `src/scripts/verify_final_reboot.ps1` | 242 | 15594 | `b59c977b9efd` |
| `src/scripts/verify_install.ps1` | 95 | 5657 | `1b57e0a337d6` |

### Web 控制台 (src/ui/web)（77 文件 / 17866 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `src/ui/web/README.md` | 54 | 2573 | `abdbfc373bd1` |
| `src/ui/web/bridge/bridge-env.d.ts` | 14 | 532 | `6f12de2841d7` |
| `src/ui/web/bridge/native/.cargo/config.toml` | 9 | 252 | `789c488de288` |
| `src/ui/web/bridge/native/Cargo.lock` | 284 | 7497 | `ae60be35040c` |
| `src/ui/web/bridge/native/Cargo.toml` | 19 | 386 | `0b85e1cb54ce` |
| `src/ui/web/bridge/native/README.md` | 87 | 5046 | `8303324ef160` |
| `src/ui/web/bridge/native/build.rs` | 3 | 39 | `fcbd1ff3f627` |
| `src/ui/web/bridge/native/src/lib.rs` | 343 | 12596 | `95669bfcc341` |
| `src/ui/web/bridge/server/index.ts` | 609 | 26321 | `9af14c055279` |
| `src/ui/web/bridge/server/log-sink.ts` | 159 | 5240 | `d64f766e804a` |
| `src/ui/web/bridge/server/proto-decode.ts` | 202 | 8645 | `3a5468fe0bef` |
| `src/ui/web/flotest.cjs` | 21 | 1307 | `2bc0b08f1f8e` |
| `src/ui/web/index.html` | 35 | 1389 | `1d3550063fe1` |
| `src/ui/web/package-lock.json` | 6481 | 227316 | `09594e26e8d1` |
| `src/ui/web/package.json` | 66 | 1932 | `c4bf6862d21a` |
| `src/ui/web/postcss.config.mjs` | 5 | 70 | `4cae941be9bc` |
| `src/ui/web/selftest.cjs` | 78 | 3738 | `896c71332e9f` |
| `src/ui/web/src/app/App.tsx` | 327 | 14275 | `68e33537580e` |
| `src/ui/web/src/app/StatsPoller.tsx` | 56 | 2051 | `49aa1002e66c` |
| `src/ui/web/src/app/router.tsx` | 40 | 2062 | `89ff594d9c90` |
| `src/ui/web/src/bridge/config-orchestrator.test.ts` | 97 | 3950 | `c02c4abe7494` |
| `src/ui/web/src/bridge/config-orchestrator.ts` | 200 | 6942 | `cb05da2cf1c0` |
| `src/ui/web/src/bridge/http-transport.ts` | 323 | 11441 | `bfda18cf72b6` |
| `src/ui/web/src/bridge/transport.ts` | 84 | 2844 | `2cd51f58132c` |
| `src/ui/web/src/bridge/volume-attach-service.test.ts` | 63 | 2607 | `22a4fb512aa6` |
| `src/ui/web/src/bridge/volume-attach-service.ts` | 80 | 2685 | `fd644b5a96ea` |
| `src/ui/web/src/components/ConfirmDialog.tsx` | 78 | 3051 | `43593cf06304` |
| `src/ui/web/src/components/GlobalAlertBanner.tsx` | 82 | 3944 | `8953a9041e6f` |
| `src/ui/web/src/components/IoctlMatrix.tsx` | 226 | 9058 | `35c24a57f90f` |
| `src/ui/web/src/components/LiquidGlassFilter.test.tsx` | 60 | 2457 | `966fc074bdbc` |
| `src/ui/web/src/components/LiquidGlassFilter.tsx` | 128 | 4554 | `a4176d1a60db` |
| `src/ui/web/src/components/StateBar.tsx` | 118 | 4497 | `aaf4bb281393` |
| `src/ui/web/src/components/StripeMatrix.tsx` | 154 | 6099 | `39d177ebfe8b` |
| `src/ui/web/src/components/ToastHost.tsx` | 69 | 2635 | `41e35b8d6698` |
| `src/ui/web/src/components/ui.tsx` | 11 | 491 | `11a8dba30213` |
| `src/ui/web/src/components/ui/brand.tsx` | 19 | 809 | `0de6c3174e77` |
| `src/ui/web/src/components/ui/controls.tsx` | 161 | 5602 | `bda379a2488c` |
| `src/ui/web/src/components/ui/icons.tsx` | 52 | 1792 | `646a45929139` |
| `src/ui/web/src/components/ui/index.ts` | 15 | 652 | `51529b86634c` |
| `src/ui/web/src/components/ui/surface.tsx` | 144 | 4126 | `fbff90b71327` |
| `src/ui/web/src/components/ui/value.tsx` | 170 | 5424 | `efbf6da384e5` |
| `src/ui/web/src/env.d.ts` | 5 | 182 | `a141885f5b45` |
| `src/ui/web/src/features/benefit/BenefitPage.tsx` | 508 | 21105 | `12f5bcf13450` |
| `src/ui/web/src/features/diagnostics/DiagnosticsPage.tsx` | 748 | 32702 | `a450aa7d9031` |
| `src/ui/web/src/features/emergency/EmergencyPage.tsx` | 247 | 10837 | `76f90a40acac` |
| `src/ui/web/src/features/settings/SettingsPage.tsx` | 939 | 40618 | `0fac2d9a38b9` |
| `src/ui/web/src/features/settings/config-diff.test.ts` | 77 | 2801 | `5ba527c771c6` |
| `src/ui/web/src/features/settings/config-diff.ts` | 92 | 4157 | `c07f7133298c` |
| `src/ui/web/src/features/status/StatusPage.tsx` | 344 | 15698 | `21c8e8994267` |
| `src/ui/web/src/features/volumes/VolumesPage.tsx` | 343 | 15645 | `2ed39691ce0f` |
| `src/ui/web/src/hooks/useLiquidGlassPointer.test.ts` | 114 | 4018 | `29e5c87dc088` |
| `src/ui/web/src/hooks/useLiquidGlassPointer.ts` | 54 | 2264 | `db2f329150ca` |
| `src/ui/web/src/hooks/useVisibilityAwarePoll.test.ts` | 78 | 2318 | `3a23aa03888f` |
| `src/ui/web/src/hooks/useVisibilityAwarePoll.ts` | 72 | 2247 | `586a82dc5b50` |
| `src/ui/web/src/lib/cn.ts` | 7 | 274 | `7182c5922d5d` |
| `src/ui/web/src/lib/echarts-theme.ts` | 51 | 1724 | `9a2ddba5f71b` |
| `src/ui/web/src/lib/motion.ts` | 186 | 6704 | `51877371fb91` |
| `src/ui/web/src/lib/time.ts` | 26 | 832 | `c120fec3bb87` |
| `src/ui/web/src/main.tsx` | 29 | 980 | `c073f5ba3a16` |
| `src/ui/web/src/protocol/constants.ts` | 88 | 2077 | `b668f900f3cd` |
| `src/ui/web/src/protocol/rb-config.generated.ts` | 101 | 4427 | `2037cd06c7f3` |
| `src/ui/web/src/protocol/types.ts` | 248 | 7809 | `5af4cec2dd1c` |
| `src/ui/web/src/state/confirm-store.ts` | 51 | 1419 | `e20d2f5f831e` |
| `src/ui/web/src/state/driver-store.ts` | 82 | 2586 | `d2680c82b8c7` |
| `src/ui/web/src/state/stats-history.test.ts` | 77 | 3968 | `a474e35c4cb9` |
| `src/ui/web/src/state/stats-history.ts` | 107 | 4582 | `8b662321d596` |
| `src/ui/web/src/state/toast-store.ts` | 38 | 1122 | `b50bffc44415` |
| `src/ui/web/src/state/trace-store.test.ts` | 28 | 1220 | `4549ecb3e47e` |
| `src/ui/web/src/state/trace-store.ts` | 180 | 5877 | `9f283148c301` |
| `src/ui/web/src/styles/liquid-glass.contract.test.ts` | 74 | 2987 | `ac740b7d97fc` |
| `src/ui/web/src/styles/liquid-glass.css` | 284 | 9712 | `2538ae724b98` |
| `src/ui/web/src/styles/tokens.css` | 1005 | 34382 | `cc2e9f34816d` |
| `src/ui/web/src/utils/l2Diagnostics.test.ts` | 92 | 3236 | `9dd7179eb438` |
| `src/ui/web/src/utils/l2Diagnostics.ts` | 143 | 6006 | `711a945833aa` |
| `src/ui/web/tsconfig.bridge.json` | 25 | 655 | `2532a32d2e6b` |
| `src/ui/web/tsconfig.json` | 32 | 938 | `95666c16fd2a` |
| `src/ui/web/vite.config.ts` | 65 | 3125 | `3749a3fbe2d9` |

### WPF 控制台/安装 UI (src/ui)（108 文件 / 16812 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `src/ui/controller/RAMBlockCache.Controller.Tests/AggregationRed.il` | 280 | 12662 | `689a4fe26465` |
| `src/ui/controller/RAMBlockCache.Controller.Tests/Program.cs` | 413 | 22107 | `b485ce1a7359` |
| `src/ui/controller/RAMBlockCache.Controller.Tests/RAMBlockCache.Controller.Tests.csproj` | 13 | 462 | `1766525adf68` |
| `src/ui/controller/RAMBlockCache.Controller.Tests/WpfRenderCapture.cs` | 237 | 11946 | `9a749c7ce7c8` |
| `src/ui/controller/RAMBlockCache.Controller.Tests/WpfStateMatrixCapture.cs` | 323 | 18538 | `9e4ca208838e` |
| `src/ui/controller/RAMBlockCache.Controller/App.xaml` | 246 | 17100 | `d2e040c8dfdc` |
| `src/ui/controller/RAMBlockCache.Controller/App.xaml.cs` | 48 | 2657 | `172aa3381a49` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/CardHover.cs` | 108 | 5632 | `bb783e9c66e8` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/ChartShapes.cs` | 129 | 5851 | `5a83df99d480` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/DialogHost.xaml` | 26 | 1520 | `06053281dc2d` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/DialogHost.xaml.cs` | 142 | 5641 | `d258b1957252` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/DpiX.cs` | 24 | 1114 | `1b04e60a45ac` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/DualBarCompare.xaml` | 41 | 2616 | `bdbc61b7c22e` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/DualBarCompare.xaml.cs` | 63 | 2590 | `666be1f710c2` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/HitRateChart.cs` | 122 | 5822 | `7e3c2cfde21e` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/IoctlMatrix.cs` | 169 | 8118 | `4b09fa176b5e` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/LinkChip.cs` | 55 | 2382 | `729d36334bf6` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/Motion.cs` | 190 | 10256 | `d526a0144e4f` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/NumberRoller.cs` | 74 | 3612 | `4cd9f385b8c3` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/OverlayPop.cs` | 48 | 2506 | `bedc5a6a1414` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/SegmentedBar.cs` | 117 | 5422 | `111eb6000f69` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/SmoothProgressBar.cs` | 46 | 1604 | `a0ecd01d62e3` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/Sparkline.cs` | 110 | 5264 | `eeea92b812da` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/StateBar.xaml` | 68 | 5189 | `43aeacef6c43` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/StateBar.xaml.cs` | 229 | 11879 | `65dded375ace` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/StripeHeatmap.cs` | 148 | 8176 | `8bbc2553dcc2` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/ThroughputChart.cs` | 131 | 6650 | `d6bb274ead4d` |
| `src/ui/controller/RAMBlockCache.Controller/Controls/UsageRing.cs` | 177 | 9284 | `08ef8e2da82f` |
| `src/ui/controller/RAMBlockCache.Controller/Helpers/BenchStateVisConverter.cs` | 25 | 1108 | `10faaef1ae4f` |
| `src/ui/controller/RAMBlockCache.Controller/Helpers/InvertBoolConverter.cs` | 16 | 655 | `a2cb2f6bf95d` |
| `src/ui/controller/RAMBlockCache.Controller/Helpers/ProgressRatioConverter.cs` | 25 | 916 | `ee1dc4e08e60` |
| `src/ui/controller/RAMBlockCache.Controller/Helpers/RelayCommand.cs` | 87 | 4068 | `e6c52ffded9e` |
| `src/ui/controller/RAMBlockCache.Controller/Interop/DwmLiquidGlassHelper.cs` | 237 | 9319 | `cb5fb2e8ce7c` |
| `src/ui/controller/RAMBlockCache.Controller/MainWindow.xaml` | 33 | 2219 | `29edae1854ef` |
| `src/ui/controller/RAMBlockCache.Controller/MainWindow.xaml.cs` | 256 | 9557 | `9e1013b77261` |
| `src/ui/controller/RAMBlockCache.Controller/Models/DriverException.cs` | 26 | 958 | `b0e702105bce` |
| `src/ui/controller/RAMBlockCache.Controller/Models/Models.cs` | 251 | 10996 | `89ee5e9c5b84` |
| `src/ui/controller/RAMBlockCache.Controller/Models/OperationResult.cs` | 25 | 933 | `30fdff9c719b` |
| `src/ui/controller/RAMBlockCache.Controller/Models/RbErrorCode.cs` | 28 | 1499 | `24debfc67396` |
| `src/ui/controller/RAMBlockCache.Controller/Models/RbFeature.cs` | 143 | 6489 | `775648858924` |
| `src/ui/controller/RAMBlockCache.Controller/Models/RbFeatureException.cs` | 27 | 886 | `79fdbd8725f3` |
| `src/ui/controller/RAMBlockCache.Controller/Models/V106Contracts.cs` | 206 | 12925 | `5022068452b0` |
| `src/ui/controller/RAMBlockCache.Controller/RAMBlockCache.Controller.csproj` | 26 | 1283 | `cb1fe192388e` |
| `src/ui/controller/RAMBlockCache.Controller/Services/Abstractions/IBenchmarkService.cs` | 70 | 3783 | `a4442963ee2a` |
| `src/ui/controller/RAMBlockCache.Controller/Services/Abstractions/IErrorMessageProvider.cs` | 61 | 3371 | `58b6519ceb78` |
| `src/ui/controller/RAMBlockCache.Controller/Services/Abstractions/IFirstRunExperience.cs` | 28 | 1185 | `4c446aa37720` |
| `src/ui/controller/RAMBlockCache.Controller/Services/Abstractions/ILicenseService.cs` | 97 | 4584 | `1e08f63f4ca4` |
| `src/ui/controller/RAMBlockCache.Controller/Services/Abstractions/IOperationProgress.cs` | 114 | 4566 | `90514da4674a` |
| `src/ui/controller/RAMBlockCache.Controller/Services/Abstractions/IPrefetchGate.cs` | 85 | 4152 | `47d4e348ac16` |
| `src/ui/controller/RAMBlockCache.Controller/Services/Abstractions/IPresetService.cs` | 166 | 8722 | `c16a3e816101` |
| `src/ui/controller/RAMBlockCache.Controller/Services/Abstractions/ITelemetryClient.cs` | 53 | 1750 | `b80f961a4997` |
| `src/ui/controller/RAMBlockCache.Controller/Services/AnticheatInfo.cs` | 51 | 2377 | `e127d876e783` |
| `src/ui/controller/RAMBlockCache.Controller/Services/CliBenchmarkService.cs` | 243 | 11937 | `92351a1b8395` |
| `src/ui/controller/RAMBlockCache.Controller/Services/DriverEventPoller.cs` | 153 | 7017 | `4b87ec936067` |
| `src/ui/controller/RAMBlockCache.Controller/Services/DriverService.cs` | 851 | 43633 | `e9aee16efa3b` |
| `src/ui/controller/RAMBlockCache.Controller/Services/DriverSignatureProbe.cs` | 86 | 4796 | `b7891d94f89b` |
| `src/ui/controller/RAMBlockCache.Controller/Services/FeatureConfigService.cs` | 249 | 12126 | `90bf251f0b9b` |
| `src/ui/controller/RAMBlockCache.Controller/Services/FeatureRegistry.cs` | 63 | 3222 | `3a8da39f1a93` |
| `src/ui/controller/RAMBlockCache.Controller/Services/FirstRunExperience.cs` | 51 | 2173 | `8403553a6822` |
| `src/ui/controller/RAMBlockCache.Controller/Services/IoctlTrace.cs` | 93 | 4230 | `3aaa89708aaf` |
| `src/ui/controller/RAMBlockCache.Controller/Services/LogService.cs` | 81 | 3983 | `d9edbaed4f30` |
| `src/ui/controller/RAMBlockCache.Controller/Services/PowerInfo.cs` | 53 | 2501 | `1d76f5fc2853` |
| `src/ui/controller/RAMBlockCache.Controller/Services/PresetService.cs` | 132 | 6995 | `1f52633701bd` |
| `src/ui/controller/RAMBlockCache.Controller/Services/SettingsService.cs` | 118 | 6781 | `d7559156727d` |
| `src/ui/controller/RAMBlockCache.Controller/Services/StatsService.cs` | 215 | 7930 | `1f3b323a2121` |
| `src/ui/controller/RAMBlockCache.Controller/Services/SystemService.cs` | 74 | 3202 | `3ee678880051` |
| `src/ui/controller/RAMBlockCache.Controller/Services/ThinHostModeGuard.cs` | 47 | 1765 | `ca5ecd8f6333` |
| `src/ui/controller/RAMBlockCache.Controller/Services/ToastService.cs` | 132 | 5860 | `417bafbdd6ce` |
| `src/ui/controller/RAMBlockCache.Controller/Services/TrayService.cs` | 269 | 11817 | `040131d977c2` |
| `src/ui/controller/RAMBlockCache.Controller/Services/UiState.cs` | 161 | 9138 | `3b96270639ee` |
| `src/ui/controller/RAMBlockCache.Controller/Services/VolumeGuard.cs` | 54 | 2530 | `2975807cc96e` |
| `src/ui/controller/RAMBlockCache.Controller/Theme/Dark.xaml` | 538 | 33684 | `95eb91cd30fb` |
| `src/ui/controller/RAMBlockCache.Controller/Version.cs` | 11 | 442 | `2b5a0288f912` |
| `src/ui/controller/RAMBlockCache.Controller/ViewModels/DriveItemViewModel.cs` | 512 | 26155 | `1abe2a04a372` |
| `src/ui/controller/RAMBlockCache.Controller/ViewModels/FeaturesViewModel.cs` | 900 | 47636 | `56d5efbbbd81` |
| `src/ui/controller/RAMBlockCache.Controller/ViewModels/FirstRunWizardViewModel.cs` | 332 | 16899 | `e45120212f75` |
| `src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.Diag.cs` | 199 | 12392 | `0181fa2099d1` |
| `src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.Drives.cs` | 146 | 7797 | `43710850ca4a` |
| `src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.Sampling.cs` | 444 | 26360 | `dfbbb9572655` |
| `src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.Settings.cs` | 450 | 20759 | `4972c9ca93c7` |
| `src/ui/controller/RAMBlockCache.Controller/ViewModels/MainViewModel.cs` | 620 | 37563 | `08cc81568249` |
| `src/ui/controller/RAMBlockCache.Controller/Views/FirstRunWizard.xaml` | 200 | 14683 | `3dd1abcc3140` |
| `src/ui/controller/RAMBlockCache.Controller/Views/FirstRunWizard.xaml.cs` | 29 | 1038 | `061ab9258b96` |
| `src/ui/controller/RAMBlockCache.Controller/app.manifest` | 15 | 599 | `df1805e8923a` |
| `src/ui/installer/RAMBlockCache.Setup/App.xaml` | 18 | 939 | `993324878525` |
| `src/ui/installer/RAMBlockCache.Setup/App.xaml.cs` | 25 | 1112 | `d5339a6d646c` |
| `src/ui/installer/RAMBlockCache.Setup/Assets/README.payload.template.txt` | 380 | 27802 | `5a28cf89e813` |
| `src/ui/installer/RAMBlockCache.Setup/Assets/payload/RAMBlockCache.inf` | 26 | 1267 | `87e6cd85abb7` |
| `src/ui/installer/RAMBlockCache.Setup/Assets/payload/RAMBoostFS-Root.cer` | 12 | 1358 | `dc8ad78c8dd8` |
| `src/ui/installer/RAMBlockCache.Setup/Assets/payload/README.txt` | 380 | 27849 | `913484745167` |
| `src/ui/installer/RAMBlockCache.Setup/Assets/payload/boot_guard.ps1` | 48 | 2806 | `f687b4027f9c` |
| `src/ui/installer/RAMBlockCache.Setup/Assets/payload/logon_disarm.ps1` | 12 | 539 | `8d4908b3a40c` |
| `src/ui/installer/RAMBlockCache.Setup/Assets/payload/offline_rollback.cmd` | 159 | 7194 | `c9aca29a1975` |
| `src/ui/installer/RAMBlockCache.Setup/Assets/payload/start-webconsole.cmd` | 8 | 281 | `367e5342af96` |
| `src/ui/installer/RAMBlockCache.Setup/Controls/Motion.cs` | 65 | 3253 | `6c7a48e13a80` |
| `src/ui/installer/RAMBlockCache.Setup/Helpers/Converters.cs` | 46 | 2249 | `85100deeba0e` |
| `src/ui/installer/RAMBlockCache.Setup/Helpers/ObservableObject.cs` | 35 | 1407 | `f7ad32410eb3` |
| `src/ui/installer/RAMBlockCache.Setup/Interop/DwmLiquidGlassHelper.cs` | 237 | 9319 | `cb5fb2e8ce7c` |
| `src/ui/installer/RAMBlockCache.Setup/MainWindow.xaml` | 205 | 15192 | `5a96403426ff` |
| `src/ui/installer/RAMBlockCache.Setup/MainWindow.xaml.cs` | 47 | 2061 | `403dc800afb9` |
| `src/ui/installer/RAMBlockCache.Setup/RAMBlockCache.Setup.csproj` | 34 | 1764 | `20fd68cdc45f` |
| `src/ui/installer/RAMBlockCache.Setup/Services/ErrorTranslator.cs` | 117 | 6121 | `afe4832f106d` |
| `src/ui/installer/RAMBlockCache.Setup/Services/Payload.cs` | 94 | 4351 | `f1b1ff387ae3` |
| `src/ui/installer/RAMBlockCache.Setup/Services/SetupEngine.cs` | 825 | 45252 | `2bc883593675` |
| `src/ui/installer/RAMBlockCache.Setup/Theme/Dark.xaml` | 129 | 7056 | `7e2fe23bd577` |
| `src/ui/installer/RAMBlockCache.Setup/Version.cs` | 11 | 437 | `4260b414d6a5` |
| `src/ui/installer/RAMBlockCache.Setup/ViewModels/InstallerViewModel.cs` | 450 | 21509 | `2a5204cb7b28` |
| `src/ui/installer/RAMBlockCache.Setup/app.manifest` | 15 | 599 | `df1805e8923a` |

### .NET 工具 (src/tools)（9 文件 / 1194 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `src/tools/rbbench/Program.cs` | 232 | 9971 | `5ffe488018aa` |
| `src/tools/rbbench/rbbench.csproj` | 19 | 635 | `6a687945f034` |
| `src/tools/rbprobe/Bench.cs` | 159 | 7589 | `d49943f86163` |
| `src/tools/rbprobe/Program.cs` | 205 | 10066 | `eeab5c040a97` |
| `src/tools/rbprobe/rbprobe.csproj` | 18 | 587 | `33a4a89a0fdb` |
| `src/tools/rbtempguard/DriverClient.cs` | 79 | 3310 | `afc28daaf7c1` |
| `src/tools/rbtempguard/ExtentResolver.cs` | 138 | 6283 | `c513893a874a` |
| `src/tools/rbtempguard/Program.cs` | 324 | 13855 | `7877a6d683d8` |
| `src/tools/rbtempguard/rbtempguard.csproj` | 20 | 724 | `1fd241f790a4` |

### 构建系统 (build)（4 文件 / 1241 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `build/RAMBlockCache.sln` | 104 | 7493 | `b2b0898e8458` |
| `build/build.ps1` | 458 | 21646 | `e116afeadfc2` |
| `build/paths.ps1` | 107 | 4195 | `945bd3d76fc5` |
| `build/sources.manifest.json` | 572 | 16057 | `903a71d415fa` |

### 测试 (tests)（281 文件 / 35985 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `tests/core/bench_5g.c` | 257 | 9198 | `ac2d4a84eb32` |
| `tests/core/bench_read_plan.c` | 317 | 11073 | `5504ecc9f503` |
| `tests/core/bench_readhit.c` | 142 | 6663 | `7bafa47d0826` |
| `tests/core/bench_write_absorb.c` | 342 | 13169 | `0b87ad68e2d0` |
| `tests/core/build/asan_bench_readhit.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_bench_readhit.out` | 4 | 308 | `66c7f8675376` |
| `tests/core/build/asan_edge_v3.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_edge_v3.out` | 19 | 741 | `21741de1f7cf` |
| `tests/core/build/asan_edge_v4.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_edge_v4.out` | 16 | 668 | `ad80a4f39d2c` |
| `tests/core/build/asan_race_timing.out` | 17 | 1227 | `60ee5d29b1ac` |
| `tests/core/build/asan_stress_v3.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_stress_v3.out` | 2 | 116 | `ffe746a7eabd` |
| `tests/core/build/asan_stress_v4.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_stress_v4.out` | 4 | 221 | `65f85a9d224d` |
| `tests/core/build/asan_test_a2flush.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_a2flush.out` | 5 | 230 | `d4ac05a42f98` |
| `tests/core/build/asan_test_auditfix.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_auditfix.out` | 36 | 1791 | `c5c6a3b45409` |
| `tests/core/build/asan_test_bigread.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_bigread.out` | 2 | 87 | `370b7813875f` |
| `tests/core/build/asan_test_cache.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_cache.out` | 2083 | 97827 | `c1ceea3698f3` |
| `tests/core/build/asan_test_counter.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_counter.out` | 3 | 159 | `a349aa8d7d5a` |
| `tests/core/build/asan_test_directwrite_stale.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_directwrite_stale.out` | 441 | 30243 | `ba7160a81ce8` |
| `tests/core/build/asan_test_discard_clean.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_discard_clean.out` | 21 | 1064 | `41f2f8c8a1d4` |
| `tests/core/build/asan_test_evict_wire.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_evict_wire.out` | 23 | 1229 | `268e3b19640a` |
| `tests/core/build/asan_test_exclude.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_exclude.out` | 43 | 1912 | `2eba1b984e99` |
| `tests/core/build/asan_test_flush_many.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_flush_many.out` | 2 | 66 | `27af09562206` |
| `tests/core/build/asan_test_flushpar.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_flushpar.out` | 775 | 52591 | `21b84825e3b1` |
| `tests/core/build/asan_test_geom.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_geom.out` | 44 | 2338 | `75027ca8019c` |
| `tests/core/build/asan_test_keep_vs_discard.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_keep_vs_discard.out` | 24 | 812 | `b96369b3ad5d` |
| `tests/core/build/asan_test_mixed.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_mixed.out` | 4 | 178 | `1ca8f24f023f` |
| `tests/core/build/asan_test_p0evict.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_p0evict.out` | 2 | 144 | `aea3381a2a25` |
| `tests/core/build/asan_test_p0v2.err` | 842 | 29361 | `4f19db88523c` |
| `tests/core/build/asan_test_p0v2.out` | 2 | 84 | `ed65db1713d4` |
| `tests/core/build/asan_test_parflush.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_parflush.out` | 3 | 132 | `d2e2676a0e01` |
| `tests/core/build/asan_test_race1106.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_race1106.out` | 17 | 1354 | `76131f6c304f` |
| `tests/core/build/asan_test_rebuild.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_rebuild.out` | 237 | 12309 | `7e1e6075f040` |
| `tests/core/build/asan_test_regress.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_regress.out` | 2 | 118 | `51b5400c1408` |
| `tests/core/build/asan_test_release.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_release.out` | 4 | 193 | `986225f3d6ac` |
| `tests/core/build/asan_test_sparse_miss.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_sparse_miss.out` | 44 | 2009 | `96c486443ccd` |
| `tests/core/build/asan_test_spinfast.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_spinfast.out` | 7 | 192 | `a8b8f7e3fa91` |
| `tests/core/build/asan_test_tinylfu.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_tinylfu.out` | 13 | 703 | `e45135da92b5` |
| `tests/core/build/asan_test_v5.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_v5.out` | 21 | 927 | `5189d27728bc` |
| `tests/core/build/asan_test_v6.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_v6.out` | 33 | 1451 | `bde54ac87a77` |
| `tests/core/build/asan_test_write_ex.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_write_ex.out` | 66 | 3695 | `150069631920` |
| `tests/core/build/asan_test_wsconc.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/asan_test_wsconc.out` | 52 | 2983 | `caa2f2b2123e` |
| `tests/core/build/manual_test_geom.out` | 44 | 2766 | `eb245fb0bfde` |
| `tests/core/build/mut_mut_new_src_drain.out` | 18 | 1405 | `c2c3d5c67263` |
| `tests/core/build/mut_mut_new_src_final.out` | 19 | 1428 | `73ed54f90f27` |
| `tests/core/build/mut_mut_old_src_drain.out` | 17 | 1227 | `8894a3d452d0` |
| `tests/core/build/mut_mut_old_src_final.out` | 18 | 1253 | `279b6f393586` |
| `tests/core/build/mut_new_src.c` | 591 | 29463 | `621e41325ff1` |
| `tests/core/build/mut_old_src.c` | 542 | 25921 | `9f7bc407c9e4` |
| `tests/core/build/qa_geom_gate.json` | 292 | 6450 | `2601a82ab562` |
| `tests/core/build/qa_geom_gate_4k_l16.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_4k_l16.out` | 235 | 18273 | `d4318999c526` |
| `tests/core/build/qa_geom_gate_4k_noarg.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_4k_noarg.out` | 775 | 52841 | `fb555bd6fa4e` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs16384_l16_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs16384_l16_r1.out` | 244 | 18746 | `3ee2f90f0623` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs16384_l4_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs16384_l4_r1.out` | 148 | 9749 | `31b3661dd59c` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs32768_l16_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs32768_l16_r1.out` | 244 | 18846 | `6e7d5294adc7` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs32768_l4_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs32768_l4_r1.out` | 148 | 9755 | `b6780626ac6c` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs8192_l16_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs8192_l16_r1.out` | 244 | 18687 | `ae1b6324fd44` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs8192_l4_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_asan_bs8192_l4_r1.out` | 148 | 9748 | `ab6e07945e0e` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_0.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_0.out` | 2 | 194 | `617bc3ca2d92` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_131072.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_131072.out` | 2 | 199 | `0be76489afce` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_4095.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_4095.out` | 2 | 197 | `4fc8d5ed25b4` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_65536.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_65536.out` | 244 | 19005 | `80eaf90133f0` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_8191.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_8191.out` | 2 | 197 | `e136d5122f99` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_abc.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_illegal_abc.out` | 2 | 196 | `a6a014cb3875` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_16384.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_16384.out` | 124 | 7547 | `b42e5f272f3c` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_32768.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_32768.out` | 124 | 7539 | `9e06141c5d84` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_4096.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_4096.out` | 124 | 7499 | `840eb2cdd877` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_65536.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_65536.out` | 124 | 7537 | `8f89a16186de` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_8192.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_legal_8192.out` | 124 | 7515 | `0670f07303d2` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs16384_l16_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs16384_l16_r1.out` | 244 | 19078 | `b70863be2c8f` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs16384_l4_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs16384_l4_r1.out` | 148 | 9838 | `f3cce1fb008b` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs32768_l16_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs32768_l16_r1.out` | 244 | 18890 | `600df3605e14` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs32768_l4_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs32768_l4_r1.out` | 148 | 9754 | `c31d74a3cd77` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs8192_l16_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs8192_l16_r1.out` | 244 | 19075 | `2e503b6c5ac8` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs8192_l4_r1.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/qa_geom_gate_runs/gate_release_bs8192_l4_r1.out` | 148 | 9744 | `8f4427a2e9ae` |
| `tests/core/build/race_new_mode0.out` | 17 | 1350 | `1a885fa2c2ac` |
| `tests/core/build/rel_bench_readhit.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_bench_readhit.out` | 4 | 302 | `a04eef57b219` |
| `tests/core/build/rel_edge_v3.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_edge_v3.out` | 19 | 741 | `21741de1f7cf` |
| `tests/core/build/rel_edge_v4.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_edge_v4.out` | 16 | 668 | `ad80a4f39d2c` |
| `tests/core/build/rel_race_timing.out` | 17 | 1227 | `5a03552b4c82` |
| `tests/core/build/rel_stress_v3.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_stress_v3.out` | 2 | 116 | `099e0e377821` |
| `tests/core/build/rel_stress_v4.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_stress_v4.out` | 4 | 221 | `65f85a9d224d` |
| `tests/core/build/rel_test_a2flush.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_a2flush.out` | 5 | 230 | `d4ac05a42f98` |
| `tests/core/build/rel_test_auditfix.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_auditfix.out` | 36 | 1791 | `c5c6a3b45409` |
| `tests/core/build/rel_test_bigread.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_bigread.out` | 2 | 87 | `370b7813875f` |
| `tests/core/build/rel_test_cache.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_cache.out` | 2083 | 97827 | `c1ceea3698f3` |
| `tests/core/build/rel_test_counter.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_counter.out` | 3 | 159 | `a349aa8d7d5a` |
| `tests/core/build/rel_test_directwrite_stale.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_directwrite_stale.out` | 441 | 30243 | `ba7160a81ce8` |
| `tests/core/build/rel_test_discard_clean.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_discard_clean.out` | 21 | 1064 | `41f2f8c8a1d4` |
| `tests/core/build/rel_test_evict_wire.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_evict_wire.out` | 23 | 1229 | `268e3b19640a` |
| `tests/core/build/rel_test_exclude.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_exclude.out` | 43 | 1912 | `2eba1b984e99` |
| `tests/core/build/rel_test_flush_many.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_flush_many.out` | 2 | 66 | `27af09562206` |
| `tests/core/build/rel_test_flushpar.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_flushpar.out` | 775 | 52549 | `5405e0177c10` |
| `tests/core/build/rel_test_geom.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_geom.out` | 44 | 2338 | `75027ca8019c` |
| `tests/core/build/rel_test_keep_vs_discard.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_keep_vs_discard.out` | 24 | 812 | `b96369b3ad5d` |
| `tests/core/build/rel_test_mixed.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_mixed.out` | 4 | 178 | `1ca8f24f023f` |
| `tests/core/build/rel_test_p0evict.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_p0evict.out` | 2 | 144 | `8a899f54c697` |
| `tests/core/build/rel_test_p0v2.err` | 816 | 27865 | `372228666cd8` |
| `tests/core/build/rel_test_p0v2.out` | 2 | 84 | `0ce73c787a0b` |
| `tests/core/build/rel_test_parflush.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_parflush.out` | 3 | 132 | `d2e2676a0e01` |
| `tests/core/build/rel_test_race1106.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_race1106.out` | 17 | 1354 | `bf6e2da54cd0` |
| `tests/core/build/rel_test_rebuild.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_rebuild.out` | 237 | 12309 | `7e1e6075f040` |
| `tests/core/build/rel_test_regress.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_regress.out` | 2 | 118 | `51b5400c1408` |
| `tests/core/build/rel_test_release.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_release.out` | 4 | 193 | `986225f3d6ac` |
| `tests/core/build/rel_test_sparse_miss.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_sparse_miss.out` | 44 | 2009 | `96c486443ccd` |
| `tests/core/build/rel_test_spinfast.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_spinfast.out` | 7 | 193 | `e3467543577b` |
| `tests/core/build/rel_test_tinylfu.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_tinylfu.out` | 13 | 703 | `e45135da92b5` |
| `tests/core/build/rel_test_v5.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_v5.out` | 21 | 927 | `5189d27728bc` |
| `tests/core/build/rel_test_v6.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_v6.out` | 33 | 1451 | `bde54ac87a77` |
| `tests/core/build/rel_test_write_ex.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_write_ex.out` | 66 | 3695 | `150069631920` |
| `tests/core/build/rel_test_wsconc.err` | 0 | 0 | `e3b0c44298fc` |
| `tests/core/build/rel_test_wsconc.out` | 52 | 2983 | `caa2f2b2123e` |
| `tests/core/edge_v3.c` | 204 | 7898 | `ac7006cd8505` |
| `tests/core/edge_v4.c` | 272 | 12221 | `8726229a8beb` |
| `tests/core/offchk.c` | 30 | 1496 | `8cc2f22cba81` |
| `tests/core/pthread.h` | 58 | 2476 | `07ff9be404de` |
| `tests/core/rbc_e2e_verify.c` | 656 | 27599 | `2b6b88b94c93` |
| `tests/core/stdatomic.h` | 21 | 904 | `9412d2927b6d` |
| `tests/core/stress_v3.c` | 116 | 4347 | `8aa151d3f8d3` |
| `tests/core/stress_v4.c` | 268 | 11337 | `d146b0a36f54` |
| `tests/core/support/parflush_stub.c` | 6 | 443 | `534eadc67240` |
| `tests/core/support/parflush_stub.h` | 22 | 1218 | `27365b4eb6eb` |
| `tests/core/support/pthread.h` | 58 | 2476 | `07ff9be404de` |
| `tests/core/test_a2flush.c` | 201 | 8046 | `25521f9a9e2f` |
| `tests/core/test_auditfix.c` | 540 | 22079 | `6a0957a62828` |
| `tests/core/test_bigread.c` | 61 | 2457 | `e7b8cea48fd2` |
| `tests/core/test_cache.c` | 744 | 32966 | `7e34c673f4d7` |
| `tests/core/test_counter.c` | 108 | 5572 | `dcf7c5300045` |
| `tests/core/test_counter_seed.c` | 76 | 3474 | `7a3ea3fc4e4a` |
| `tests/core/test_directwrite_stale.c` | 407 | 21190 | `c7e1b5700c80` |
| `tests/core/test_discard_clean.c` | 186 | 7399 | `56e1e77ca08b` |
| `tests/core/test_evict_wire.c` | 282 | 11359 | `18a713c8b7ed` |
| `tests/core/test_exclude.c` | 264 | 9910 | `7cc3ea644508` |
| `tests/core/test_flush_many.c` | 33 | 1422 | `689b655a3143` |
| `tests/core/test_flushpar.c` | 754 | 39290 | `a13854e045b0` |
| `tests/core/test_geom.c` | 349 | 18292 | `26bc042fac38` |
| `tests/core/test_keep_vs_discard.c` | 289 | 11207 | `18e38b82a515` |
| `tests/core/test_mixed.c` | 91 | 3986 | `2398f628f6de` |
| `tests/core/test_p0evict.c` | 240 | 10070 | `c99f122e437f` |
| `tests/core/test_p0v2.c` | 333 | 14638 | `e5aedbe2149f` |
| `tests/core/test_parflush.c` | 89 | 3948 | `c70e7c65d1dc` |
| `tests/core/test_race1106.c` | 589 | 29034 | `76785ba78f1e` |
| `tests/core/test_rebuild.c` | 1339 | 64203 | `65fc4d556bad` |
| `tests/core/test_regress.c` | 74 | 3879 | `50475d5e8179` |
| `tests/core/test_release.c` | 104 | 4500 | `631c96873b0c` |
| `tests/core/test_sparse_miss.c` | 220 | 8624 | `08ad1ea50331` |
| `tests/core/test_spinfast.c` | 217 | 9736 | `35c5a96a2dbb` |
| `tests/core/test_tinylfu.c` | 196 | 7635 | `a26323814600` |
| `tests/core/test_v5.c` | 188 | 7675 | `8f7d5a755c0f` |
| `tests/core/test_v6.c` | 246 | 9332 | `be163607b172` |
| `tests/core/test_write_ex.c` | 563 | 25567 | `a869c95f7ce7` |
| `tests/core/test_wsconc.c` | 468 | 25147 | `b70930733754` |
| `tests/core/unistd.h` | 7 | 255 | `f0bde23e4bf9` |
| `tests/e2e/README.md` | 20 | 1088 | `353e38585ea8` |
| `tests/e2e/a2_concurrency_sweep.py` | 201 | 7603 | `b99799f00835` |
| `tests/e2e/a2_flush_model.py` | 144 | 5535 | `8a61bd7ed80d` |
| `tests/e2e/a2_repro_ab.py` | 165 | 6472 | `1286611c9a7f` |
| `tests/e2e/a2_reverify_after_reboot.py` | 243 | 10120 | `e21cee6bd847` |
| `tests/e2e/a2_writeback_stress.py` | 449 | 18021 | `e144a972dba3` |
| `tests/e2e/b_gate_mutation.py` | 125 | 4746 | `3090ddf732a6` |
| `tests/e2e/b_reverify_after_reboot.py` | 285 | 11975 | `aece95bafbed` |
| `tests/e2e/c2_pool_corruption_model.py` | 194 | 9617 | `9effb044bd00` |
| `tests/e2e/c3_single_machine_stress.py` | 259 | 10964 | `567d89547489` |
| `tests/e2e/c3_worker.py` | 49 | 1576 | `c97f108c465c` |
| `tests/e2e/c3b_warmup_ab.py` | 101 | 4499 | `10a2fa99d2f0` |
| `tests/e2e/c5_gate13_mutation.py` | 121 | 5231 | `be08ab79638e` |
| `tests/e2e/c5b_gate13_i48_mutation.py` | 124 | 4968 | `5c9b37bc057b` |
| `tests/e2e/d2_logsnapshot_overflow.py` | 159 | 7155 | `ded351585184` |
| `tests/e2e/d3_verify_after_reboot.py` | 288 | 12798 | `d9c57d71a842` |
| `tests/e2e/d4_verify_cache_takeover.py` | 507 | 22399 | `8de5f0f2d404` |
| `tests/e2e/d5_gate14_mutation.py` | 96 | 3840 | `aadb5dc06fc1` |
| `tests/e2e/e1_abatch_discriminative.py` | 424 | 17636 | `43b0e4547e1e` |
| `tests/e2e/e2_concurrent_eviction.py` | 320 | 13420 | `823aae26755d` |
| `tests/e2e/e4_i47_repro.py` | 205 | 9459 | `830d4dde5b7d` |
| `tests/e2e/e5_flush_probe.py` | 93 | 3654 | `0e5b373e1916` |
| `tests/e2e/j6_verifier_prereq_regression.py` | 325 | 14633 | `989e1c97b95e` |
| `tests/e2e/o9_probe_diag.py` | 34 | 1335 | `2046894ffa07` |
| `tests/e2e/o9_stress.py` | 155 | 6170 | `342c3fc628f4` |
| `tests/e2e/o9_stress_e.py` | 88 | 3592 | `c9872eaa5d40` |
| `tests/e2e/patch_gate13_i48.py` | 49 | 1985 | `45112787cc0a` |
| `tests/e2e/probe_dev.py` | 68 | 2900 | `2255f4f665cd` |
| `tests/e2e/t1_trigger_evidence.py` | 156 | 5657 | `f1769526bfeb` |
| `tests/e2e/t2_admit_discrimination.py` | 202 | 8550 | `96c99f28e5ec` |
| `tests/e2e/t3_exclude_rootcause.py` | 107 | 4364 | `a9eba9ea36e1` |
| `tests/e2e/t4_exclude_three_way.py` | 149 | 5741 | `04f4b0f2c0d1` |
| `tests/e2e/t5_syncio_interleave.py` | 248 | 8933 | `a1502d80b2a1` |
| `tests/e2e/t6_bugcheck_harness.py` | 164 | 6926 | `1519a49c8618` |
| `tests/e2e/t6_ioctl_fuzz.py` | 182 | 8329 | `cd6ff99af531` |
| `tests/e2e/t7_gate_mutation.py` | 184 | 7901 | `2e1a5f4c87ad` |
| `tests/e2e/t7b_gate_mutation.py` | 195 | 7838 | `66f6cdc6375b` |
| `tests/e2e/t7c_gate_mutation.py` | 113 | 4502 | `59126c4f643f` |
| `tests/e2e/t8_closure_batch.py` | 202 | 8635 | `483f0a0938c5` |
| `tests/e2e/test_e2e_exitcode_contract.py` | 969 | 35675 | `e872b7c3a7ea` |
| `tests/support/rbc_lba_probe.c` | 47 | 2423 | `589b40d91160` |
| `tests/support/rbc_pooltags.c` | 67 | 2682 | `063dc36dc173` |
| `tests/support/rbc_uncached_read.c` | 88 | 3547 | `92d00069338c` |

### 静态审计工具 (tools)（8 文件 / 2376 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `tools/audit_probe_failopen.py` | 634 | 26223 | `ac35a9ea561b` |
| `tools/audit_sweep.py` | 666 | 26412 | `a88ac2272e57` |
| `tools/probe_aligned_write.py` | 135 | 5827 | `e9ac172da203` |
| `tools/probe_disk_truth.py` | 185 | 8715 | `84fc4bbc11d1` |
| `tools/probe_stale_attribution.py` | 193 | 9070 | `d4f0c00fccf5` |
| `tools/probe_through_staleness.py` | 194 | 7046 | `4a17cc25d9cc` |
| `tools/probe_unbuffered_read.py` | 149 | 5565 | `ca0dd62f13fd` |
| `tools/probe_write_readback_integrity.py` | 220 | 10164 | `4c0124194841` |

### 设计文档 (docs)（17 文件 / 1467 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `docs/00-总览.md` | 111 | 11149 | `02088ca1908f` |
| `docs/01-工具链安装.md` | 52 | 2070 | `445facf18445` |
| `docs/02-构建.md` | 55 | 1785 | `1e0e081b0fb5` |
| `docs/03-部署与使用.md` | 85 | 4218 | `9228f44ae5a4` |
| `docs/04-数据安全与防蓝屏.md` | 78 | 6115 | `7a18ab446a07` |
| `docs/05-签名与安全启动.md` | 108 | 5481 | `abf67e92e1ae` |
| `docs/06-Win11-26H2-适配说明.md` | 54 | 2914 | `7cc707943524` |
| `docs/07-常见问题.md` | 118 | 6888 | `b9293e7f6783` |
| `docs/08-V3-多线程与空闲释放.md` | 72 | 4077 | `625c20a65d3a` |
| `docs/09-V4-分片锁多核与游戏模式.md` | 139 | 8820 | `42a2cc6faf14` |
| `docs/10-竞品对比与优化.md` | 121 | 11220 | `009dd71affd3` |
| `docs/11-实机验证清单.md` | 76 | 5653 | `f96fac2a427d` |
| `docs/12-V7架构定稿-颗粒度与部署.md` | 140 | 10088 | `e37e5df1f0c1` |
| `docs/ARCHITECTURE.md` | 18 | 767 | `2cb5eb106021` |
| `docs/class-diagram.mermaid` | 85 | 2658 | `c1bc7bb0a3da` |
| `docs/sequence-diagram.mermaid` | 30 | 1260 | `949aa01b2d33` |
| `docs/v11-minifilter-blueprint/README.md` | 125 | 5129 | `7def7e7254cf` |

### 证书 (certs)（1 文件 / 12 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `certs/RAMBoostFS-Root.cer` | 12 | 1358 | `dc8ad78c8dd8` |

### 本轮新增运维脚本 (extras)（2 文件 / 110 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `extras/RB-r2-restore-step1.cmd` | 59 | 2311 | `6d2771e8cce3` |
| `extras/RB-r2-restore-step2.cmd` | 51 | 1926 | `1bcaab44b646` |

### 仓库根文档（4 文件 / 876 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `.gitignore` | 104 | 3108 | `c688e6c055f1` |
| `AGENTS.md` | 156 | 10202 | `29e9197ff812` |
| `KNOWN_ISSUES.md` | 424 | 178689 | `c766fafbfeec` |
| `README.md` | 192 | 12137 | `95f0d74eff0a` |

### 其他（3 文件 / 347 行）

| 路径 | 行数 | 字节 | sha256 前 12 |
|---|---:|---:|---|
| `.github/workflows/ci.yml` | 227 | 12939 | `a36583083f08` |
| `src/sign_all.cmd` | 102 | 3945 | `a36733670c37` |
| `src/sign_wpf_single.cmd` | 18 | 1045 | `9b06be44880d` |
