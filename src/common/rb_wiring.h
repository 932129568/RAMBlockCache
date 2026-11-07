/* common/rb_wiring.h — 接线总表（构建期事实，替代注释性"未接线"标注）
 * 0 = 已实现但未接入调用链（审计 RB-Rev-V10.3.4 WB-1 / 第10章确认的 OFF 状态）
 * 1 = 已接线。纪律：一格 commit 只允许翻转一个开关。 */
#ifndef RB_WIRING_H
#define RB_WIRING_H

#define RB_WIRED_WBCOAL        1   /* rbwback.c : RbWbInit/RbWbStop 接挂载/停机（[101] 2026-11-08 用户授权接线开放；运行期仍受 Features\Flags bit8 控制，默认 OFF） */
#define RB_WIRED_L2_ON_EVICT   1   /* rbfeat.c  : RbFeatOnEvict 接 core 淘汰路径（5 参：Ext=file_id, Lba=chunk 起始, Len=chunk_size, Src=chunk 数据, Hits=读命中；2026-10-15） */

/* [101] 原 "wire ONE feature per commit" 熔断已拆除：WBCOAL 与 L2_ON_EVICT 属
 * 不同调用链（回写节奏 vs 淘汰旁路），接线互不依赖。运行期互斥约束
 * 「L2 与 WBCOAL 的 Flags 位不可同为 1」由 rbfeatctl flags 命令强制（CLI 层）。 */

/* 消费点约定（门禁 check_repo.py 强制校验）：
 *   任何调用 RbWbInit/RbWbStop 的文件，必须含 "#if RB_WIRED_WBCOAL" 守卫；
 *   任何调用 RbFeatOnEvict 的文件，必须含 "#if RB_WIRED_L2_ON_EVICT" 守卫；
 *   RB_WIRED_* 只准在本文件定义，他处 #define 视为 FAIL。 */
#endif /* RB_WIRING_H */
