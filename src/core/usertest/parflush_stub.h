/* ============================================================================
 * parflush_stub.h — 测试专用的并行 flush 声明（测试侧持有）
 *
 * 这里声明的 rb_engine_flush_parallel 【不是】内核生产路径。生产路径是
 * cache.c 的 flush_pass（由 test_counter.c 的 partflusher 线程覆盖）。
 * 本测试版并行实现只用于压力/正确性场景，分歧在此显性化，不再藏进生产
 * 源文件 cache.c。
 *
 * 实现体见 usertest/parflush_stub.c；该文件被 cache.c 在
 * `#ifndef RB_KERNEL_BUILD` 区内 #include 进同一编译单元，以访问 cache.c
 * 内 static 助手与不透明的 rb_engine 布局。内核/生产构建不包含它。
 * ==========================================================================*/
#ifndef PARFLUSH_STUB_H
#define PARFLUSH_STUB_H

#include "../cache.h"

/* 原 rb_engine_flush_parallel 逐字迁移，签名不变。仅用户态测试使用。 */
size_t rb_engine_flush_parallel(rb_engine *eng, int force);

#endif /* PARFLUSH_STUB_H */
