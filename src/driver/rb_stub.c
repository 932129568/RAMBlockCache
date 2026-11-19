/* cache.c（只读）内核分支在 rb_stream_reconcile_dirty 里残留调试 printf，
 * 内核无 libc。本文件不 include 任何内核/CRT 头，仅提供链接符号（no-op）。
 * 例程 IRQL 契约：无——本文件不含任何内核 API 调用（纯用户态语义的空函数），
 * 任意调用上下文均安全；不得在本文件添加任何内核逻辑。
 */
int printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}
