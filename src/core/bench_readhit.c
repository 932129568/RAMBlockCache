/*
 * 4K 读命中微基准（MSVC 兼容）：量化纯读命中每 4K 子块的核心引擎开销。
 * 方法：写满一个 64KB chunk（16 个 4K 子块全脏后回写干净），随后对热 chunk
 * 做 N 次 4K 读命中（mc==0 全命中），用 QueryPerformanceCounter 测 ns/op。
 * 结果不受磁盘/内存带宽污染（命中路径纯 memcpy + 分片锁），用于前后对比
 * 读热路径的 ERESOURCE/原子操作开销。
 *
 * [D2/D3] 追加并发段（memlevel 专项 F8）：
 *   B) 并发写创建：8 线程各自对独占区间做 4KB 首写（chunk_create +
 *      chunk_mark_dirty —— chain 分片锁（D2）的目标路径），测 ns/op；
 *   C) 读命中受压：8 写者并发跑动期间测热 chunk 读命中 ns/op（D4：分片后
 *      读热路径不劣化）。
 * 口径：QueryPerformanceCounter，ns/op；仅用户态微基准，真机判据以
 * bench_proto.ps1 + CDM 为准（本数字只证明"无用户态回归"）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <pthread.h>
#include "cache.h"

static int wb(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t n){
    (void)u;(void)fid;(void)off;(void)d;(void)n; return 0; }
static int rb(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n){
    (void)u;(void)fid;(void)off; memset(b,0,n); return 0; }

static double now_ns(void){
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}

/* ---- 并发段状态（D2/D4） ---- */
#define BENCH_WRITERS   8
#define BENCH_CREATES   512                     /* 每写者首写次数（8×512×64KB=256MB 驻留） */
#define BENCH_WREGION   (32ull*1024*1024)       /* 每写者独占区间（跨多条纹） */
static rb_engine    *g_be;
static rb_stream    *g_bw;                      /* 写者流（与读热流分离） */
static unsigned char g_bhot[65536];
static volatile LONG g_bstop;
static volatile LONG g_loop_writers;   /* 1=循环到停止信号；0=单遍（B 段） */

typedef struct { int id; } BWArg;

/* 写者：对独占区间做 4KB 首写/覆写（chunk_create + mark_dirty 路径）；
 * loop=1 时循环到 g_bstop（供"读命中受压"段提供持续写负载）。 */
static void *bench_writer(void *p)
{
    BWArg *a=(BWArg*)p; int id=a->id;
    rb_u64 base=(rb_u64)id*BENCH_WREGION;
    unsigned char blk[4096];
    int op;
    memset(blk,(unsigned char)(0x40+id),sizeof(blk));
    for(op=0; !g_bstop; op++){
        rb_u64 off=base+(rb_u64)(op%BENCH_CREATES)*65536ull;
        rb_direct_range dr[4]; int dc=0;
        if(rb_write(g_bw,off,blk,sizeof(blk),dr,&dc,4)!=0){
            printf("bench writer %d fail @%llu\n",id,(unsigned long long)off);
            break;
        }
        if(!g_loop_writers) break;   /* 单遍模式：跑完 BENCH_CREATES 即退 */
    }
    return NULL;
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
    double rh_nsop=(t1-t0)/(double)N;
    printf("read-hit 4K: ops=%ld  hits=%ld  total_ms=%.1f  ns/op=%.2f\n",
           N, h, (t1-t0)/1e6, rh_nsop);

    /* ============ 并发段（D2/D4 验收） ============ */
    {
        pthread_t th[BENCH_WRITERS]; BWArg ba[BENCH_WRITERS];
        double b0,b1;
        g_be=e; g_bw=rb_stream_create(e,2);
        memset(g_bhot,0x5a,sizeof(g_bhot));
        if(!g_bw){ printf("bench: writer stream create fail\n"); }
        else{
            /* ---- B) 并发写创建：8 线程 × 512 次 4K 首写（chunk_create 路径） ---- */
            g_bstop=0; g_loop_writers=0;
            b0=now_ns();
            for(i=0;i<BENCH_WRITERS;i++){ ba[i].id=i; pthread_create(&th[i],NULL,bench_writer,&ba[i]); }
            for(i=0;i<BENCH_WRITERS;i++) pthread_join(th[i],NULL);
            b1=now_ns();
            { long ops=(long)BENCH_WRITERS*BENCH_CREATES;
              printf("write-create 4K (%d thr): ops=%ld  total_ms=%.1f  ns/op=%.2f\n",
                     BENCH_WRITERS, ops, (b1-b0)/1e6, (b1-b0)/(double)ops); }

            /* ---- C) 读命中受压：8 写者循环跑动期间测热 chunk 命中 ---- */
            g_bstop=0; g_loop_writers=1;
            for(i=0;i<BENCH_WRITERS;i++) pthread_create(&th[i],NULL,bench_writer,&ba[i]);
            { long M=N/4>1000000?1000000:N/4; long h2=0;
              b0=now_ns();
              for(i=0;i<M;i++){
                  rb_direct_range m[8]; int mm=0;
                  int r=rb_read(s,(rb_u64)(i%16)*4096,rd,4096,m,&mm,8);
                  if(r==1 && mm==0) h2++;
              }
              b1=now_ns();
              printf("read-hit 4K under %d writers: ops=%ld  hits=%ld  total_ms=%.1f  ns/op=%.2f\n",
                     BENCH_WRITERS, M, h2, (b1-b0)/1e6, (b1-b0)/(double)M); }
            g_bstop=1;
            for(i=0;i<BENCH_WRITERS;i++) pthread_join(th[i],NULL);
        }
        printf("== bench summary: read-hit=%.2f ns/op (baseline, single-thread) ==\n", rh_nsop);
        if(g_bw) rb_stream_destroy(g_bw);
    }
    rb_stream_destroy(s); rb_engine_destroy(e);
    return 0;
}
