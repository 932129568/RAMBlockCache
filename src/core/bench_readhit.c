/*
 * 4K 读命中微基准（MSVC 兼容）：量化纯读命中每 4K 子块的核心引擎开销。
 * 方法：写满一个 64KB chunk（16 个 4K 子块全脏后回写干净），随后对热 chunk
 * 做 N 次 4K 读命中（mc==0 全命中），用 QueryPerformanceCounter 测 ns/op。
 * 结果不受磁盘/内存带宽污染（命中路径纯 memcpy + 分片锁），用于前后对比
 * 读热路径的 ERESOURCE/原子操作开销。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "cache.h"

static int wb(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t n){
    (void)u;(void)fid;(void)off;(void)d;(void)n; return 0; }
static int rb(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n){
    (void)u;(void)fid;(void)off; memset(b,0,n); return 0; }

static double now_ns(void){
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}

int main(int argc, char **argv){
    rb_config cfg; memset(&cfg,0,sizeof(cfg));
    cfg.max_bytes=256ull*1024*1024; cfg.writeback=wb; cfg.readback=rb; cfg.block_size=4096;
    rb_engine *e=rb_engine_create(&cfg);
    rb_stream *s=rb_stream_create(e,1);
    unsigned char wr[65536]; memset(wr,0x5a,sizeof(wr));
    rb_direct_range dr[8]; int dc=0;
    if(rb_write(s,0,wr,sizeof(wr),dr,&dc,8)!=0){ printf("write fail\n"); return 1; }
    rb_engine_flush(e,1);   /* 落盘干净，chunk 变干净缓存块 */

    unsigned char rd[4096];
    rb_direct_range miss[8]; int mc=0;
    /* warmup */
    int i;
    for(i=0;i<100000;i++){ rb_direct_range m[8]; int mm=0;
        rb_read(s,(rb_u64)(i%16)*4096,rd,4096,m,&mm,8); }

    long N = (argc>=2)? strtol(argv[1],NULL,0) : 20000000;
    double t0=now_ns();
    long h=0;
    for(i=0;i<N;i++){
        rb_direct_range m[8]; int mm=0;
        int r=rb_read(s,(rb_u64)(i%16)*4096,rd,4096,m,&mm,8);
        if(r==1 && mm==0) h++;
    }
    double t1=now_ns();
    printf("read-hit 4K: ops=%ld  hits=%ld  total_ms=%.1f  ns/op=%.2f\n",
           N, h, (t1-t0)/1e6, (t1-t0)/(double)N);
    rb_stream_destroy(s); rb_engine_destroy(e);
    return 0;
}
