/* [P0-EVICT 2026-11-10] I47 判别风暴：极小上限 + 回写 + 并发读写 + flush +
 * 在线缩容 + trim/idle + 时钟跳变，全部同分片碰撞（纯内存 g_disk 模型，
 * 零磁盘 IO）。目标：在 ASan 下复现或排除 core 层内存错误（I47 候选根因）。
 *
 * 判据（全部满足才算排除 core）：
 *  - 全程 g_disk 逐字节 == fbyte（确定性 oracle：一切写入=fbyte，盘初值=fbyte）；
 *  - lost_dirty_bytes == 0；
 *  - EvictBytes > 0（触发证据：小上限必须真的打出驱逐）；
 *  - ASan 零报告；退出码 0。
 *
 * 组合（对照 I47 崩溃面：WB=1 + CacheSizeMB=8 + 驱逐/flush 并发）：
 *  写者 ×8  ：[满子块 / 跨 chunk 大写 / 512B 非对齐 RMW] 混合
 *  读者 ×4  ：随机读 + fbyte 逐字节校验
 *  flush ×2 ：rb_stream_flush(s,1) 周期强推（同分片自旋在途回写）
 *  配置 ×1  ：max_bytes [2MB,64MB] 骤缩/骤增（A3 语义：固定水位切换）+ idle 阈值抖动
 *  杂务 ×1  ：trim + idle_reclaim + 时钟大跳变
 * 终态：全量 flush 后磁盘逐字节 + dirty/lost 双零 + evict>0。
 */
#include "cache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>


#define FILESZ   (32ull * 1024 * 1024)   /* 64KB 整除 */
#define NW       8
#define NR       4
#define WOPS     3000
#define ROPS     3000
#define DROPS    900
#define MAXLEN   (256 * 1024)
#define MAXR     160

static unsigned char *g_disk;
static rb_engine     *g_eng;
static rb_stream     *g_st;
static pthread_mutex_t disk_mx;
static atomic_int     g_stop;
static atomic_long    g_bad;
static atomic_llong   g_clk;
static rb_stats       g_st0;

static unsigned nrand(unsigned *s){ *s = *s*1103515245u+12345u; return *s>>1; }
static unsigned char fbyte(rb_u64 off){ return (unsigned char)(off*131u+7u); }

static int wb_fn(void*u,rb_u64 fid,rb_u64 off,const void*b,size_t n){
    (void)u;(void)fid;
    pthread_mutex_lock(&disk_mx);
    memcpy(g_disk+off,b,n);
    pthread_mutex_unlock(&disk_mx);
    return 0;
}
static int rb_fn(void*u,rb_u64 fid,rb_u64 off,void*b,size_t n){
    (void)u;(void)fid;
    pthread_mutex_lock(&disk_mx);
    memcpy(b,g_disk+off,n);
    pthread_mutex_unlock(&disk_mx);
    return 0;
}

/* 模拟驱动处理 miss（短持 disk_mx 读盘，释放后 commit） */
static void serve_miss(void*out,rb_u64 reqoff,size_t reqlen,
                       rb_direct_range*ms,int mc){
    int k; rb_u64 reqend=reqoff+reqlen;
    for(k=0;k<mc;k++){
        unsigned char*tmp=malloc(ms[k].len);
        rb_u64 mend=ms[k].offset+ms[k].len,lo,hi;
        if(!tmp){ atomic_fetch_add(&g_bad,1); continue; }
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

/* 确定性写：数据=fbyte；direct 区间同写盘（同值） */
static void det_write(rb_u64 off,size_t len){
    unsigned char*data=malloc(len); int k,dc=0;
    rb_direct_range dr[MAXR];
    if(!data){ atomic_fetch_add(&g_bad,1); return; }
    { rb_u64 q; for(q=0;q<len;q++) data[q]=fbyte(off+q); }
    rb_write(g_st,off,data,len,dr,&dc,MAXR);
    pthread_mutex_lock(&disk_mx);
    if(dc>MAXR) memcpy(g_disk+off,data,len);
    else for(k=0;k<dc;k++) memcpy(g_disk+dr[k].offset,
                data+(dr[k].offset-off),dr[k].len);
    pthread_mutex_unlock(&disk_mx);
    free(data);
}

static void* writer(void*p){
    unsigned seed=(unsigned)(unsigned long)p ^ 0x51ED; int op;
    for(op=0;op<WOPS && !atomic_load(&g_stop);op++){
        rb_u64 off=(rb_u64)(nrand(&seed)%FILESZ);
        size_t maxl=(size_t)(FILESZ-off);
        size_t len;
        switch(nrand(&seed)%3){
        case 0:  len = 512u + (nrand(&seed)%3)*512u; break;      /* 非对齐 RMW */
        case 1:  len = 4096u;                        break;      /* 满子块 */
        default: len = 64*1024u + (nrand(&seed)%3)*64*1024u; break; /* 跨 chunk */
        }
        if(len>MAXLEN) len=MAXLEN;
        if(len>maxl) len=maxl;
        det_write(off,len);
    }
    return NULL;
}

static void* reader(void*p){
    unsigned seed=(unsigned)(unsigned long)p ^ 0xA5A5; int op;
    for(op=0;op<ROPS && !atomic_load(&g_stop);op++){
        rb_u64 off=(rb_u64)(nrand(&seed)%FILESZ);
        size_t maxl=(size_t)(FILESZ-off);
        size_t len=1+(size_t)(nrand(&seed)%(MAXLEN<maxl?MAXLEN:maxl));
        unsigned char*out=malloc(len); size_t d;
        rb_direct_range ms[MAXR]; int mc=0;
        int r;
        if(!out) continue;
        r=rb_read(g_st,off,out,len,ms,&mc,MAXR);
        /* [FIX 器具 2026-11-10] rb_read 契约：返回 1=部分命中（hit 部分已由
         * core 拷入 out）+ miss 区间待调用方补盘；0=纯 miss；-2=异常/overflow。
         * 旧器具漏 r==1 分支 → out 的 miss 部分未初始化 → 假 FAIL（got=0/190）。 */
        if(r==-2){ pthread_mutex_lock(&disk_mx);
                   memcpy(out,g_disk+off,len);
                   pthread_mutex_unlock(&disk_mx); }
        else if(r==0 || r==1) serve_miss(out,off,len,ms,mc);
        else { free(out); atomic_fetch_add(&g_bad,1); return NULL; }
        for(d=0;d<len;d++) if(out[d]!=fbyte(off+d)) break;
        if(d<len){ printf("  [FAIL] rd op=%d off=%llu len=%zu @%zu got=%u exp=%u\n",
                    op,(unsigned long long)off,len,d,out[d],fbyte(off+d));
                   atomic_fetch_add(&g_bad,1); atomic_store(&g_stop,1); }
        free(out);
    }
    return NULL;
}

static void* flusher(void*p){
    (void)p;
    for(int i=0;i<DROPS && !atomic_load(&g_stop);i++){
        rb_stream_flush(g_st,1);
        { volatile int _s; for(_s=0;_s<20000;_s++); }
    }
    return NULL;
}

static int g_mode = 0;   /* 0=storm 1=fixed2m 2=fixed64m */
static void* config_churn(void*p){
    if(g_mode != 0) return NULL;   /* fixed 模式：无在线缩容 */
    { unsigned s=(unsigned)(unsigned long)p ^ 0xC0FF;
    for(int i=0;i<DROPS && !atomic_load(&g_stop);i++){
        rb_u64 mb = (nrand(&s)%2) ? 2ull : 64ull;   /* 2MB << FILESZ：驱逐风暴 */
        rb_engine_set_max_bytes(g_eng, mb*1024*1024);
        rb_engine_set_idle_reclaim(g_eng,(nrand(&s)&1)?1000:0);
        { volatile int _s; for(_s=0;_s<60000;_s++); }
    }
    rb_engine_set_max_bytes(g_eng, 64ull*1024*1024);
    rb_engine_set_idle_reclaim(g_eng, 0); }
    return NULL;
}

static void* ticktrim(void*p){
    unsigned s=(unsigned)(unsigned long)p ^ 0x7EE5;
    for(int i=0;i<DROPS && !atomic_load(&g_stop);i++){
        long j=20000+(nrand(&s)%40000);
        long c=atomic_fetch_add(&g_clk,j)+j;
        rb_engine_tick(g_eng,(rb_u64)c);
        rb_stream_trim(g_st);
        rb_stream_idle_reclaim(g_st);
        { volatile int _s; for(_s=0;_s<40000;_s++); }
    }
    return NULL;
}

int main(int argc, char**argv){
    if(argc>1 && argv[1][0]=='1') g_mode=1;
    else if(argc>1 && argv[1][0]=='2') g_mode=2;
    pthread_t w[NW],r[NR],f1,f2,cc,tt;
    rb_config cfg; int i; rb_stats st;
    memset(&cfg,0,sizeof(cfg));
    cfg.max_bytes = (g_mode==2) ? 64ull*1024*1024 : 2ull*1024*1024;   /* storm/fixed2m=2MB 驱逐风暴；fixed64m=无驱逐 */
    cfg.max_merge_len = 4*1024*1024;
    cfg.readback = rb_fn; cfg.writeback = wb_fn;
    g_disk = malloc(FILESZ);
    if(!g_disk){ printf("  [FAIL] oom disk\n"); return 2; }
    { rb_u64 q; for(q=0;q<FILESZ;q++) g_disk[q]=fbyte(q); }
    pthread_mutex_init(&disk_mx,NULL);
    g_eng = rb_engine_create(&cfg);
    if(!g_eng){ printf("  [FAIL] engine\n"); return 2; }
    g_st = rb_stream_create(g_eng,777);
    if(!g_st){ printf("  [FAIL] stream\n"); return 2; }
    rb_engine_get_stats(g_eng,&g_st0);

    for(i=0;i<NW;i++) pthread_create(&w[i],NULL,writer,(void*)(unsigned long)(i+1));
    for(i=0;i<NR;i++) pthread_create(&r[i],NULL,reader,(void*)(unsigned long)(i+101));
    pthread_create(&f1,NULL,flusher,NULL);
    pthread_create(&f2,NULL,flusher,NULL);
    pthread_create(&cc,NULL,config_churn,NULL);
    pthread_create(&tt,NULL,ticktrim,NULL);

    for(i=0;i<NW;i++) pthread_join(w[i],NULL);
    for(i=0;i<NR;i++) pthread_join(r[i],NULL);
    pthread_join(f1,NULL); pthread_join(f2,NULL);
    pthread_join(cc,NULL); pthread_join(tt,NULL);

    /* 终态硬判据 */
    rb_stream_flush(g_st,1);
    rb_engine_get_stats(g_eng,&st);
    printf("  evict=%llu (delta=%lld) dirty=%llu lost=%llu cur=%llu bad=%ld\n",
           (unsigned long long)st.evict_bytes,
           (long long)(st.evict_bytes - g_st0.evict_bytes),
           (unsigned long long)st.dirty_bytes,
           (unsigned long long)st.lost_dirty_bytes,
           (unsigned long long)st.cur_bytes,
           (long)atomic_load(&g_bad));
    { long bad=0; rb_u64 q;
      pthread_mutex_lock(&disk_mx);
      for(q=0;q<FILESZ;q++) if(g_disk[q]!=fbyte(q)){ bad++; if(bad==1)
          printf("  [FAIL] disk@%llu got=%u exp=%u\n",(unsigned long long)q,g_disk[q],fbyte(q)); }
      pthread_mutex_unlock(&disk_mx);
      if(bad){ atomic_fetch_add(&g_bad,bad); } }
    if(st.lost_dirty_bytes!=0 || st.dirty_bytes!=0)
        atomic_fetch_add(&g_bad,1);
    if(st.evict_bytes - g_st0.evict_bytes == 0){
        printf("  [FAIL] eviction never triggered (no trigger evidence)\n");
        atomic_fetch_add(&g_bad,1);
    }
    rb_stream_destroy(g_st); g_st=NULL;
    rb_engine_destroy(g_eng); g_eng=NULL;
    free(g_disk); g_disk=NULL;
    if(atomic_load(&g_bad)){ printf("  [FAIL] bad=%ld\n",(long)atomic_load(&g_bad)); return 1; }
    printf("  [ok] P0-EVICT storm clean (evict storm + concurrent RW + flush + churn)\n");
    return 0;
}
