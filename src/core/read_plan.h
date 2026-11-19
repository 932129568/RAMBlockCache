#ifndef RB_READ_PLAN_H
#define RB_READ_PLAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Platform-independent bounded read planning: 4K geometry, at most 32 touched
 * sub-blocks per slice. No WDK/WPF/driver dispatch dependency. */

/* [165 / 161 §3.2] 切片几何：本头为权威定义，read_plan.c 里的同名宏是同值镜像
 * （两处改动必须同步；下面的断言把不同步变成编译错误而不是静默漂移）。 */
#define RB_READ_PLAN_SUB_SIZE  4096ULL
#define RB_READ_PLAN_MAX_SUBS  32ULL
#define RB_READ_PLAN_MAX_BYTES (RB_READ_PLAN_SUB_SIZE * RB_READ_PLAN_MAX_SUBS)

/* 编译期断言：本头同时被 tests/（MSVC 旧 C 模式，无 /std:c11）包含，`_Static_assert`
 * 在那里不可用（C2143/C2091），故用从 C89 起就可靠的负数组长度写法；条件为假时
 * 编译器报 "array size is negative"，并在类型名里点出是哪条不变量。 */
#define RB_READ_PLAN_CT_ASSERT_(cond, tag) typedef char rb_read_plan_ct_##tag[(cond) ? 1 : -1]
#define RB_READ_PLAN_CT_ASSERT(cond, tag)  RB_READ_PLAN_CT_ASSERT_(cond, tag)

/* 不变量 ①（单 TU）：一次切片窗口恒为 4096 × 32 = 128KiB。驱动按此窗口分片
 * （rbio_worker.c 的读循环），放大窗口即同时放大单次 rb_read 的 miss 区间数
 * 与 staging 峰值。 */
RB_READ_PLAN_CT_ASSERT(RB_READ_PLAN_MAX_BYTES == 131072ULL, window_128k);

/* 不变量 ②（跨文件）：一次切片最多触 RB_READ_PLAN_MAX_SUBS 个 sub-block，而驱动
 * 用 miss[RB_MAX_RANGES] 承载它们（rb_read(..., RB_MAX_RANGES)），故必须
 * MAX_SUBS <= RB_MAX_RANGES。违反时 rb_read 返回 -2 → 每次读都
 * STATUS_INVALID_PARAMETER（161 §3.2 的原始隐患）。
 * RB_MAX_RANGES 定义在 src/driver/rb_common.h；驱动 TU 的包含顺序是
 * rb_common.h → read_plan.h，故此处可见。tests/（test_sparse_miss、bench_read_plan）
 * 单独包含本头、不消费区间数组容量，未定义时跳过该断言。 */
#if defined(RB_MAX_RANGES)
RB_READ_PLAN_CT_ASSERT(RB_READ_PLAN_MAX_SUBS <= (unsigned long long)RB_MAX_RANGES,
                       subs_fit_rb_max_ranges);
#endif

size_t rb_read_plan_chunk(uint64_t offset, size_t remaining);

#ifdef __cplusplus
}
#endif

#endif
