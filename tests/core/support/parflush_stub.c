/* parflush_stub.c — SHELL ONLY. The implementation moved to
 * src/core/cache_testhook.inc, which src/core/cache.c textually includes under
 * `#ifndef RB_KERNEL_BUILD` (it needs cache.c's static helpers and the opaque
 * rb_engine layout). Kept empty so historical scripts/docs referencing this
 * path do not break; do not compile it standalone. The declaration consumed by
 * test hosts remains in tests/core/support/parflush_stub.h. */
