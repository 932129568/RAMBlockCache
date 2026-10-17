/* common/rb_wiring.h — 接线总表（构建期事实，替代注释性"未接线"标注）
 * 0 = 已实现但未接入调用链（审计 RB-Rev-V10.3.4 WB-1 / 第10章确认的 OFF 状态）
 * 1 = 已接线。纪律：一格 commit 只允许翻转一个开关。 */
#ifndef RB_WIRING_H
#define RB_WIRING_H

#define RB_WIRED_WBCOAL        0   /* rbwback.c : RbWbInit/RbWbStop 接挂载/停机（M2 预留） */
#define RB_WIRED_L2_ON_EVICT   1   /* rbfeat.c  : RbFeatOnEvict 接 core 淘汰路径（5 参：Ext=file_id, Lba=chunk 起始, Len=chunk_size, Src=chunk 数据, Hits=读命中；2026-10-15） */

#if RB_WIRED_WBCOAL && RB_WIRED_L2_ON_EVICT
#  error "wire ONE feature per commit: finish WBCOAL wiring first"
#endif

/* 消费点约定（门禁 check_repo.py 强制校验）：
 *   任何调用 RbWbInit/RbWbStop 的文件，必须含 "#if RB_WIRED_WBCOAL" 守卫；
 *   任何调用 RbFeatOnEvict 的文件，必须含 "#if RB_WIRED_L2_ON_EVICT" 守卫；
 *   RB_WIRED_* 只准在本文件定义，他处 #define 视为 FAIL。 */
#endif /* RB_WIRING_H */
