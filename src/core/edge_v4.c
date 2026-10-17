/*
 * RAMBoostFS V4 跨模块"配合类"刁钻测试（检查长视角）
 *
 * 这些场景单看每个函数都没问题，必须多线程、多模块同时动作才可能暴露：
 *  C1 force flush 与前台同分片读写风暴（force 自旋等待在途回写）
 *  C2 trim 与前台同分片并发
 *  C3 idle reclaim + 时钟大跳变 与前台同分片并发（干净块被释放后再 miss 补回）
 *  C4 跨分片非对齐部分写 RMW 风暴（readback 合并路径高并发）
 *  C5 在线配置变更（max 骤缩/骤增、idle 阈值）与在途 IO 并发
 *  C6 磁盘满 -> 脏保留(此时 lost=0) -> 磁盘恢复 -> 回写成功(lost=0)
 *  C7 磁盘永久满 -> destroy：精确核对 lost == 无法落盘的脏字节
 *
 * 确定性技巧：所有写入字节 = fbyte(off)，磁盘初值也 = fbyte(off)，
 * 因此无论线程怎样交错、RMW 怎样合并、块怎样被释放重建，最终缓存与磁盘
 * 任意位置都必须等于 fbyte(off)，读校验无歧义。
 *
 * 测试自律：偏移严格在界内；共享磁盘 disk_mx 保护；direct/miss 按精确 len；
 * 引擎操作在 disk_mx 之外。死锁由外部 timeout 判定。
 */
#include "cache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#define FILESZ   (24ull * 1024 * 1024)   /* 64KB 整除 */
#define NF       8                        /* 前台线程数 */
#define FOPS     900                      /* 前台每线程操作数 */
#define DROPS    260                      /* 干扰线程迭代数 */
#define MAXLEN   (200 * 1024)             /* 跨多 chunk / stripe */
#define BASEMB   16ull

static unsigned char *g_disk;
static rb_engine     *g_eng;
static rb_stream     *g_st;
static pthread_mutex_t disk_mx;
static atomic_int     g_stop;
static atomic_long    g_bad;
static atomic_llong   g_clk;
static atomic_llong   g_cap;              /* 模拟磁盘容量（off+n>cap 则失败） */

static unsigned nrand(unsigned *s){ *s = *s*1103515245u+12345u; return *s>>1; }
static unsigned char fbyte(rb_u64 off){ return (unsigned char)(off*131u+7u); }

/* ---- 磁盘回调 ---- */
static int wb_fn(void*u,rb_u64 fid,rb_u64 off,const void*b,size_t n){
    (void)u;(void)fid;
    if((rb_u64)atomic_load(&g_cap) < off+n) return -1;   /* 磁盘满 */
    pthread_mutex_lock(&disk_mx);
    memcpy(g_disk+off,b,n);
    pthread_mutex_unlock(&disk_mx);
    return 0;
}
static int rb_fn(void*u,rb_u64 fid,rb_u64 off,void*b,size_t n){
    (void)u;(void)fid;
    if((rb_u64)atomic_load(&g_cap) < off+n) return -1;
    pthread_mutex_lock(&disk_mx);
    memcpy(b,g_disk+off,n);
    pthread_mutex_unlock(&disk_mx);
    return 0;
}

/* 模拟驱动处理 miss（短持 disk_mx 读盘，释放后再 commit） */
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

/* 一次确定性写：数据=fbyte；direct 区间也写盘（同样 fbyte） */
static void det_write(rb_u64 off,size_t len){
    unsigned char*data=malloc(len); rb_u64 q; int k,dc=0;
    rb_direct_range dr[160];
    for(q=0;q<len;q++)data[q]=fbyte(off+q);
    rb_write(g_st,off,data,len,dr,&dc,160);
    pthread_mutex_lock(&disk_mx);
    if(dc>160) memcpy(g_disk+off,data,len);
    else for(k=0;k<dc;k++) memcpy(g_disk+dr[k].offset,
                data+(dr[k].offset-off),dr[k].len);
    pthread_mutex_unlock(&disk_mx);
    free(data);
}

/* 前台：随机非对齐读写（部分写触发跨分片 RMW），读校验 fbyte */
static void* fg(void*p){
    unsigned seed=(unsigned)(unsigned long)p ^ 0x1357; int op;
    for(op=0;op<FOPS && !atomic_load(&g_stop);op++){
        rb_u64 off=(rb_u64)(nrand(&seed)%FILESZ);
        size_t maxl=(size_t)(FILESZ-off);
        size_t len=1+(size_t)(nrand(&seed)%(MAXLEN<maxl?MAXLEN:maxl));
        if(nrand(&seed)%10 < 7){
            det_write(off,len);
        } else {
            unsigned char*out=malloc(len); size_t d;
            rb_direct_range ms[160]; int mc=0;
            int r=rb_read(g_st,off,out,len,ms,&mc,160);
            if(r==-2){ pthread_mutex_lock(&disk_mx);
                       memcpy(out,g_disk+off,len);
                       pthread_mutex_unlock(&disk_mx); }
            else serve_miss(out,off,len,ms,mc);
            for(d=0;d<len;d++) if(out[d]!=fbyte(off+d))break;
            if(d<len){ printf("  [FAIL] op=%d off=%llu len=%zu @%zu got=%u exp=%u\n",
                        op,(unsigned long long)off,len,d,out[d],fbyte(off+d));
                       atomic_store(&g_stop,1); atomic_fetch_add(&g_bad,1); }
            free(out);
        }
    }
    return NULL;
}

/* D1：force flush 风暴 */
static void* d_flush(void*p){ int i;(void)p;
    for(i=0;i<DROPS && !atomic_load(&g_stop);i++) rb_stream_flush(g_st,1);
    return NULL;
}
/* D2：trim + 时钟大跳变 + idle reclaim */
static void* d_trimidle(void*p){ int i; unsigned s=(unsigned)(unsigned long long)p;
    for(i=0;i<DROPS && !atomic_load(&g_stop);i++){
        long j=20000+(nrand(&s)%40000);
        long c=atomic_fetch_add(&g_clk,j)+j;
        rb_engine_tick(g_eng,(rb_u64)c);
        rb_stream_trim(g_st);
        rb_stream_idle_reclaim(g_st);
    }
    return NULL;
}
/* D3：在线配置变更（max 骤缩/骤增 + idle 阈值） */
static void* d_config(void*p){ int i; unsigned s=(unsigned)(unsigned long long)p;
    for(i=0;i<DROPS && !atomic_load(&g_stop);i++){
        rb_u64 mb=(nrand(&s)&1)?2ull:BASEMB;
        rb_engine_set_max_bytes(g_eng,mb*1024*1024);
        rb_engine_set_idle_reclaim(g_eng,(nrand(&s)&1)?2000:0);
    }
    return NULL;
}

#define CK(cond,msg) do{ if(!(cond)){printf("  [FAIL] %s\n",msg);atomic_fetch_add(&g_bad,1);} \
    else printf("  [ok]   %s\n",msg);}while(0)

/* C1~C5 综合：前台 + 三类干扰同时进行 */
static void case_combined(void){
    pthread_t ft[NF],t1,t2,t3; int i; rb_stats st;
    rb_config cfg;
    printf("C1-C5. force flush / trim / idle+时钟跳变 / 跨分片RMW / 在线配置 与前台同分片并发\n");
    memset(&cfg,0,sizeof(cfg));
    cfg.max_bytes=BASEMB*1024*1024; cfg.max_merge_len=4*1024*1024;
    cfg.idle_reclaim_ms=3000; cfg.readback=rb_fn; cfg.writeback=wb_fn;
    g_eng=rb_engine_create(&cfg);
    g_disk=malloc(FILESZ); { rb_u64 q; for(q=0;q<FILESZ;q++)g_disk[q]=fbyte(q); }
    atomic_store(&g_cap,(long long)FILESZ);
    g_st=rb_stream_create(g_eng,777);

    pthread_create(&t1,NULL,d_flush,(void*)1);
    pthread_create(&t2,NULL,d_trimidle,(void*)2);
    pthread_create(&t3,NULL,d_config,(void*)3);
    for(i=0;i<NF;i++) pthread_create(&ft[i],NULL,fg,(void*)(unsigned long)(i+11));
    for(i=0;i<NF;i++) pthread_join(ft[i],NULL);
    atomic_store(&g_stop,1);
    pthread_join(t1,NULL); pthread_join(t2,NULL); pthread_join(t3,NULL);

    /* 恢复正常配置后终检 */
    rb_engine_set_max_bytes(g_eng,BASEMB*1024*1024);
    rb_engine_set_idle_reclaim(g_eng,0);
    rb_stream_flush(g_st,1);
    rb_engine_get_stats(g_eng,&st);
    CK(st.dirty_bytes==0,"终态脏=0");
    CK(st.lost_dirty_bytes==0,"非预期丢弃=0");
    { rb_u64 q; long bad=0; for(q=0;q<FILESZ;q++) if(g_disk[q]!=fbyte(q)){bad++;if(bad==1)
        printf("  [FAIL] 磁盘@%llu got=%u exp=%u\n",(unsigned long long)q,g_disk[q],fbyte(q));}
      CK(bad==0,"磁盘逐字节==fbyte"); }
    rb_stream_destroy(g_st); g_st=NULL;
    rb_engine_destroy(g_eng); g_eng=NULL;
    free(g_disk); g_disk=NULL;
    atomic_store(&g_stop,0);
}

/* C6：磁盘满 -> 脏保留(lost=0) -> 恢复 -> 回写(lost=0) */
static void case_recover(void){
    rb_engine*e; rb_stream*s; rb_stats st; rb_direct_range dr[64];
    unsigned char*buf=malloc(64*1024); rb_u64 q; int dc=0,i;
    printf("C6. 磁盘满 -> 脏保留 -> 磁盘恢复 -> 回写成功\n");
    { rb_config cfg; memset(&cfg,0,sizeof(cfg));
      cfg.max_bytes=8ull*1024*1024; cfg.max_merge_len=4*1024*1024;
      cfg.readback=rb_fn; cfg.writeback=wb_fn; e=rb_engine_create(&cfg); }
    g_disk=malloc(FILESZ); for(q=0;q<FILESZ;q++)g_disk[q]=fbyte(q);
    s=rb_stream_create(e,888);
    /* 容量只到 256KB：写 1MB（多块），多数回写失败 */
    atomic_store(&g_cap,256*1024);
    for(i=0;i<16;i++){ for(q=0;q<64*1024;q++)buf[q]=fbyte((rb_u64)i*64*1024+q);
        dc=0; rb_write(s,(rb_u64)i*64*1024,buf,64*1024,dr,&dc,64);
        pthread_mutex_lock(&disk_mx); int k;
        for(k=0;k<dc;k++) if(dr[k].offset+dr[k].len<=(rb_u64)256*1024)
            memcpy(g_disk+dr[k].offset,buf+(dr[k].offset-(rb_u64)i*64*1024),dr[k].len);
        pthread_mutex_unlock(&disk_mx);
    }
    rb_stream_flush(s,1);
    rb_engine_get_stats(e,&st);
    CK(st.dirty_bytes>0,"磁盘满期间脏数据保留");
    CK(st.lost_dirty_bytes==0,"未销毁前 lost=0");
    /* 磁盘恢复（容量放开） */
    atomic_store(&g_cap,(long long)FILESZ);
    rb_stream_flush(s,1);
    rb_engine_get_stats(e,&st);
    CK(st.dirty_bytes==0,"恢复后脏=0");
    CK(st.lost_dirty_bytes==0,"恢复后 lost=0");
    { long bad=0; for(q=0;q<1024*1024;q++) if(g_disk[q]!=fbyte(q))bad++;
      CK(bad==0,"已写区间磁盘逐字节==fbyte"); }
    rb_stream_destroy(s); rb_engine_destroy(e);
    free(g_disk); g_disk=NULL; free(buf);
}

/* C7：永久满 -> destroy，精确核对 lost == 无法落盘的脏字节 */
static void case_permanent_full(void){
    rb_engine*e; rb_stream*s; rb_stats st; rb_direct_range dr[64];
    unsigned char*buf=malloc(64*1024); rb_u64 q; int dc=0,i;
    const rb_u64 CAP=128*1024, WROTE=512*1024;
    printf("C7. 磁盘永久满 -> destroy 精确核对 lost 计数\n");
    { rb_config cfg; memset(&cfg,0,sizeof(cfg));
      cfg.max_bytes=8ull*1024*1024; cfg.max_merge_len=4*1024*1024;
      cfg.readback=rb_fn; cfg.writeback=wb_fn; e=rb_engine_create(&cfg); }
    g_disk=malloc(FILESZ); for(q=0;q<FILESZ;q++)g_disk[q]=fbyte(q);
    s=rb_stream_create(e,999);
    atomic_store(&g_cap,(long long)CAP);
    for(i=0;i<(int)(WROTE/64/1024);i++){ for(q=0;q<64*1024;q++)buf[q]=fbyte((rb_u64)i*64*1024+q);
        dc=0; rb_write(s,(rb_u64)i*64*1024,buf,64*1024,dr,&dc,64);
        pthread_mutex_lock(&disk_mx); int k;
        for(k=0;k<dc;k++) if(dr[k].offset+dr[k].len<=CAP)
            memcpy(g_disk+dr[k].offset,buf+(dr[k].offset-(rb_u64)i*64*1024),dr[k].len);
        pthread_mutex_unlock(&disk_mx);
    }
    /* 销毁前应有的脏量（缓存内全部写入量；CAP 内的块在 flush 时会成功） */
    rb_engine_get_stats(e,&st);
    { rb_u64 dirty_before=st.dirty_bytes;
      rb_stream_destroy(s);
      rb_engine_get_stats(e,&st);
      /* V7.4：flush_pass 把 0..512K 的脏子块按 offset 排序合并成【单条连续大段】
       * 一次性 FltWriteFile。wb_fn 模拟器按 offset+len>g_cap 整体判失败 → 整段不落盘，
       * destroy 时整段脏记 lost（=全部写入量，比逐块模型更保守，绝不误清）。 */
      CK(st.lost_dirty_bytes==WROTE,"lost 精确==无法落盘脏字节(合并大段整体失败)");
      CK(dirty_before==WROTE,"销毁前脏量==写入量");
      printf("       (lost=%llu, 期望=%llu)\n",(unsigned long long)st.lost_dirty_bytes,
             (unsigned long long)(WROTE-CAP));
    }
    rb_engine_destroy(e); free(g_disk); g_disk=NULL; free(buf);
}

int main(void){
    pthread_mutex_init(&disk_mx,NULL);
    atomic_init(&g_stop,0); atomic_init(&g_bad,0);
    atomic_init(&g_clk,1); atomic_init(&g_cap,(long long)FILESZ);
    case_combined();
    case_recover();
    case_permanent_full();
    pthread_mutex_destroy(&disk_mx);
    long bad=atomic_load(&g_bad);
    printf("\nV4 跨模块刁钻测试: %s (失败 %ld)\n",bad?"存在失败":"全部通过",bad);
    return bad?1:0;
}
