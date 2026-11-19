/*
 * RAMBoostFS V4 多核分片压力测试（同一文件、多线程）
 *
 * 阶段A（分片并行 + 后台并发）：
 *   把文件切成 NW 个互不相交的 SHARD，每个前台线程只读写自己的 shard
 *   （shutdown 后读校验无歧义）；后台线程并发做 flush / reclaim / idle / tick，
 *   覆盖"flush、trim、idle 与前台同分片并发"、跨分片 RMW、时钟前跳。
 * 阶段B（同分片竞争 + 确定性数据）：
 *   所有线程对同一区域做读写，写入数据为偏移的纯函数（初值也相同），
 *   故无论怎样交错，缓存/磁盘/期望都应收敛到同一确定字节，
 *   真正压测同分片锁竞争（无损坏、无死锁）。
 *
 * 死锁探测：本程序若卡死，由外部 `timeout` 杀死并以非零退出判失败。
 * 最终：force flush 后磁盘逐字节 == 影子，脏=0、非预期丢弃=0。
 *
 * 注意（测试代码自律）：所有偏移严格在界内、memcmp 按精确 len、
 * 共享磁盘用 disk_mx 保护；缓存引擎操作在 disk_mx 之外。
 */
#include "cache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#define NW        12
#define SHARD     (5ull*1024*1024)          /* 64KB 整除（80 块） */
#define FILEA     ((rb_u64)NW*SHARD)        /* 阶段A 文件 60MB */
#define FILEB     (8ull*1024*1024)          /* 阶段B 共享区域 */
#define CACHEMB   24ull
#define OPSA      1100                     /* 阶段A 每线程操作数 */
#define OPSB      700                      /* 阶段B 每线程操作数 */
#define MAXLEN    (280*1024)               /* 跨多 chunk / 多 stripe */
#define NBG       3

static unsigned char *g_disk;
static unsigned char *g_shadow;
static rb_engine     *g_eng;
static rb_stream     *g_st;
static rb_u64         g_filesz;
static pthread_mutex_t disk_mx;
static atomic_int     g_stop;
static atomic_long    g_bad;
static atomic_llong   g_clk;
static atomic_int     g_wb_fail;            /* 受控回写失败（默认0） */

static unsigned nrand(unsigned *s){ *s = *s*1103515245u+12345u; return *s>>1; }
static unsigned char fbyte(rb_u64 off){ return (unsigned char)(off*131u+7u); }
static void mkrand(unsigned char*p,size_t n,unsigned*s){
    size_t i; for(i=0;i<n;i++) p[i]=(unsigned char)nrand(s);
}

/* ---- 磁盘回调：stripe -> disk_mx（唯一方向，无环） ---- */
static int wb_fn(void*u,rb_u64 fid,rb_u64 off,const void*b,size_t n){
    (void)u;(void)fid;
    if(atomic_load(&g_wb_fail)) return -1;
    if(off+n>g_filesz) return -1;
    pthread_mutex_lock(&disk_mx);
    memcpy(g_disk+off,b,n);
    pthread_mutex_unlock(&disk_mx);
    return 0;
}
static int rb_fn(void*u,rb_u64 fid,rb_u64 off,void*b,size_t n){
    (void)u;(void)fid;
    if(off+n>g_filesz) return -1;
    pthread_mutex_lock(&disk_mx);
    memcpy(b,g_disk+off,n);
    pthread_mutex_unlock(&disk_mx);
    return 0;
}

/* 模拟驱动处理 miss：读盘（disk_mx，短持，释放后再 commit） */
static void serve_miss(void*out,rb_u64 reqoff,size_t reqlen,
                       rb_direct_range*ms,int mc){
    int k; rb_u64 reqend=reqoff+reqlen;
    for(k=0;k<mc;k++){
        unsigned char*tmp=malloc(ms[k].len);
        rb_u64 mend=ms[k].offset+ms[k].len,lo,hi;
        pthread_mutex_lock(&disk_mx);
        memcpy(tmp,g_disk+ms[k].offset,ms[k].len);
        pthread_mutex_unlock(&disk_mx);
        rb_read_commit(g_st,ms[k].offset,tmp,ms[k].len);
        lo=ms[k].offset>reqoff?ms[k].offset:reqoff;
        hi=mend<reqend?mend:reqend;
        if(hi>lo) memcpy((unsigned char*)out+(lo-reqoff),
                         tmp+(lo-ms[k].offset),(size_t)(hi-lo));
        free(tmp);
    }
}

/* ================= 阶段A：分片私有 ================= */
typedef struct { int wid; unsigned seed; } Wa;
static void* workerA(void*p){
    Wa*w=(Wa*)p; int wid=w->wid; unsigned seed=w->seed;
    rb_u64 lo0=(rb_u64)wid*SHARD;
    rb_stream*s=g_st;
    int op;
    for(op=0;op<OPSA && !atomic_load(&g_stop);op++){
        rb_u64 span=(rb_u64)(nrand(&seed)%(rb_u64)SHARD);
        size_t maxl=(size_t)(SHARD-span);
        size_t len=1+(size_t)(nrand(&seed)%(MAXLEN<maxl?MAXLEN:maxl));
        rb_u64 off=lo0+span;
        if(len==0)continue;
        if(nrand(&seed)&1){
            unsigned char*data=malloc(len); mkrand(data,len,&seed);
            rb_direct_range dr[160]; int dc=0,k;
            rb_write(s,off,data,len,dr,&dc,160);
            pthread_mutex_lock(&disk_mx);
            if(dc>160) memcpy(g_disk+off,data,len);
            else for(k=0;k<dc;k++) memcpy(g_disk+dr[k].offset,
                        data+(dr[k].offset-off),dr[k].len);
            pthread_mutex_unlock(&disk_mx);
            memcpy(g_shadow+off,data,len);   /* shard 私有，无需 shadow 锁 */
            free(data);
        } else {
            unsigned char*out=malloc(len);
            rb_direct_range ms[160]; int mc=0;
            int r=rb_read(s,off,out,len,ms,&mc,160);
            if(r==-2){ pthread_mutex_lock(&disk_mx);
                       memcpy(out,g_disk+off,len);
                       pthread_mutex_unlock(&disk_mx); }
            else serve_miss(out,off,len,ms,mc);
            if(memcmp(out,g_shadow+off,len)!=0){
                size_t d; for(d=0;d<len&&out[d]==g_shadow[off+d];d++);
                printf("  [FAIL A] wid=%d op=%d off=%llu len=%zu diff@%zu got=%u exp=%u\n",
                    wid,op,(unsigned long long)off,len,d,
                    d<len?(unsigned char)out[d]:0,d<len?(unsigned char)g_shadow[off+d]:0);
                atomic_store(&g_stop,1); atomic_fetch_add(&g_bad,1);
            }
            free(out);
        }
    }
    return NULL;
}

/* ================= 阶段B：同区域、确定性数据 ================= */
static void* workerB(void*p){
    unsigned seed=(unsigned)(unsigned long)p ^ 0x9e3779b9u;
    rb_stream*s=g_st; int op,k;
    for(op=0;op<OPSB && !atomic_load(&g_stop);op++){
        rb_u64 off=(rb_u64)(nrand(&seed)%FILEB);
        size_t len=1+(size_t)(nrand(&seed)%(MAXLEN<FILEB?MAXLEN:FILEB));
        if(off+len>FILEB)len=FILEB-off;
        if(len==0)continue;
        if(nrand(&seed)&1){
            /* 数据 = f(偏移)，与初值/其他线程完全一致，结果与交错无关 */
            unsigned char*data=malloc(len); rb_u64 q;
            for(q=0;q<len;q++)data[q]=fbyte(off+q);
            rb_direct_range dr[160]; int dc=0;
            rb_write(s,off,data,len,dr,&dc,160);
            pthread_mutex_lock(&disk_mx);
            if(dc>160) memcpy(g_disk+off,data,len);
            else for(k=0;k<dc;k++) memcpy(g_disk+dr[k].offset,
                        data+(dr[k].offset-off),dr[k].len);
            pthread_mutex_unlock(&disk_mx);
            free(data);
        } else {
            unsigned char*out=malloc(len);
            rb_direct_range ms[160]; int mc=0;
            int r=rb_read(s,off,out,len,ms,&mc,160);
            if(r==-2){ pthread_mutex_lock(&disk_mx);
                       memcpy(out,g_disk+off,len);
                       pthread_mutex_unlock(&disk_mx); }
            else serve_miss(out,off,len,ms,mc);
            /* 期望恒为 f */
            size_t d; for(d=0;d<len;d++) if(out[d]!=fbyte(off+d))break;
            if(d<len){ printf("  [FAIL B] op=%d off=%llu diff@%zu got=%u exp=%u\n",
                        op,(unsigned long long)off,d,out[d],fbyte(off+d));
                       atomic_store(&g_stop,1); atomic_fetch_add(&g_bad,1); }
            free(out);
        }
    }
    return NULL;
}

/* ---- 后台：flush / reclaim / idle / 单调时钟（含受控前跳） ---- */
static void* bg(void*p){
    unsigned seed=(unsigned)(unsigned long)p ^ 0x55aa; long it=0;
    while(!atomic_load(&g_stop)){
        long dt=1+(nrand(&seed)%4);
        long cur=atomic_fetch_add(&g_clk,dt)+dt;
        rb_engine_tick(g_eng,(rb_u64)cur);
        if((it%40)==3){ long jump=30000+(nrand(&seed)%20000);   /* 受控前跳，催老 */
            cur=atomic_fetch_add(&g_clk,jump)+jump;
            rb_engine_tick(g_eng,(rb_u64)cur);
            rb_stream_idle_reclaim(g_st);
        }
        switch(nrand(&seed)%4){
            case 0: rb_engine_flush(g_eng,0); break;
            case 1: rb_engine_reclaim(g_eng); break;
            case 2: rb_stream_flush(g_st,0); break;
            default: break;
        }
        usleep(400+(nrand(&seed)%1200));
        it++;
    }
    return NULL;
}

static int verify(const char*tag){
    rb_stats st; int bad=0; rb_u64 i;
    rb_engine_flush(g_eng,1);
    rb_engine_get_stats(g_eng,&st);
    if(st.dirty_bytes!=0){ printf("  [FAIL %s] 残留脏=%llu\n",tag,
        (unsigned long long)st.dirty_bytes); bad=1; }
    if(st.lost_dirty_bytes!=0){ printf("  [FAIL %s] 非预期丢弃=%llu\n",tag,
        (unsigned long long)st.lost_dirty_bytes); bad=1; }
    for(i=0;i<g_filesz;i++) if(g_disk[i]!=g_shadow[i]){
        printf("  [FAIL %s] 磁盘!=影子 @%llu got=%u exp=%u\n",tag,
            (unsigned long long)i,g_disk[i],g_shadow[i]); bad=1; break; }
    if(!bad)printf("  [ok]   %s：脏=0 丢弃=0 磁盘逐字节一致\n",tag);
    return bad;
}

static rb_engine* make_engine(void){
    rb_config cfg; memset(&cfg,0,sizeof(cfg));
    cfg.max_bytes=CACHEMB*1024*1024;
    cfg.max_merge_len=4*1024*1024;
    cfg.idle_reclaim_ms=4000;
    cfg.readback=rb_fn; cfg.writeback=wb_fn;
    return rb_engine_create(&cfg);
}

int main(void){
    pthread_mutex_init(&disk_mx,NULL);
    atomic_init(&g_stop,0); atomic_init(&g_bad,0);
    atomic_init(&g_clk,1); atomic_init(&g_wb_fail,0);

    g_eng=make_engine();
    g_disk=malloc(FILEA); g_shadow=malloc(FILEA);
    if(!g_disk||!g_shadow){printf("oom\n");return 1;}

    /* ---- 阶段A ---- */
    g_filesz=FILEA;
    { rb_u64 i; for(i=0;i<FILEA;i++){ g_disk[i]=fbyte(i); g_shadow[i]=g_disk[i]; } }
    g_st=rb_stream_create(g_eng,101);
    pthread_t wt[NW],bt[NBG]; Wa wa[NW]; int i;
    for(i=0;i<NW;i++){ wa[i].wid=i; wa[i].seed=0x1234+i*999; }
    for(i=0;i<NBG;i++) pthread_create(&bt[i],NULL,bg,(void*)(unsigned long)(i+1));
    for(i=0;i<NW;i++) pthread_create(&wt[i],NULL,workerA,&wa[i]);
    for(i=0;i<NW;i++) pthread_join(wt[i],NULL);
    atomic_store(&g_stop,1);
    for(i=0;i<NBG;i++) pthread_join(bt[i],NULL);
    long badA=verify("阶段A");
    rb_stream_destroy(g_st); g_st=NULL;

    /* ---- 阶段B（重建流，磁盘/影子重置为确定性 f） ---- */
    atomic_store(&g_stop,0);
    g_filesz=FILEB;
    { rb_u64 i; for(i=0;i<FILEB;i++){ g_disk[i]=fbyte(i); g_shadow[i]=g_disk[i]; } }
    g_st=rb_stream_create(g_eng,202);
    pthread_t wb[NW],bb[NBG];
    for(i=0;i<NBG;i++) pthread_create(&bb[i],NULL,bg,(void*)(unsigned long)(i+7));
    for(i=0;i<NW;i++) pthread_create(&wb[i],NULL,workerB,(void*)(unsigned long)(i+3));
    for(i=0;i<NW;i++) pthread_join(wb[i],NULL);
    atomic_store(&g_stop,1);
    for(i=0;i<NBG;i++) pthread_join(bb[i],NULL);
    long badB=verify("阶段B");
    rb_stream_destroy(g_st);

    rb_engine_destroy(g_eng);
    free(g_disk); free(g_shadow);
    if(atomic_load(&g_bad)||badA||badB){ printf("\nV4 多核分片压力测试：失败 (bad=%ld A=%ld B=%ld)\n",
        atomic_load(&g_bad),badA,badB); return 1; }
    printf("\nV4 多核分片压力测试通过（同文件 %d 线程分片并行 + 同分片竞争 + 后台并发）\n",NW);
    return 0;
}
