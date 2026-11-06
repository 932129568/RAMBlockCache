/* [OPT-SPIN 2026-11-02] rb_read_try_cache 自旋快路径并发正确性：
 *  - 快读者 vs 写者（撕裂检测：每子块自描述代际模式，块内字代必须一致）
 *  - 快读者 vs 驱逐（小 max_bytes 逼持续 chunk_create/chunk_free → ref 协议
 *    防 UAF，ASan 构建下任何释放竞态即爆）
 *  - 快读者 vs invalidate（回滚窗口读旧代=合法，代 ≤ 当前即通过）
 *  - 快读者 vs flush（回写落盘后读回源，代仍自描述）
 * 写者只做整子块对齐写（保证"子块内单代"不变式，撕裂检测才精确）；
 * 部分覆盖/非对齐路径由 test_write_ex/edge_* 覆盖，不在本测试目标内。
 * 两相位：bs=4096（16MB 区/4096 子块）与 bs=65536（16MB 区/256 子块）。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cache.h"
#ifdef _MSC_VER
#include "pthread.h"
#else
#include <pthread.h>
#endif

#define REGION_BYTES  (16ull*1024*1024)   /* 两相位同区 16MB */
#ifdef _MSC_VER
static void ts_yield(void){ Sleep(0); }
#else
static void ts_yield(void){ usleep(200); }
#endif

static unsigned char *g_disk;        /* 盘面影子（回写/回源） */
static pthread_mutex_t g_disk_mx;
static volatile LONG *g_subgen;      /* 每子块当前代（自增） */
static rb_u32 g_sub_bytes;           /* 相位几何：子块=引擎 block_size */
static rb_u32 g_nsubs;               /* 子块总数 */
static rb_engine *g_eng;
static rb_stream *g_str;
static volatile long g_stop;
static volatile long g_hits, g_miss, g_tornfail;

static int wb(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t len){
    (void)u;(void)fid;
    pthread_mutex_lock(&g_disk_mx);
    memcpy(g_disk+off,d,len);
    pthread_mutex_unlock(&g_disk_mx);
    return 0;
}
static int rbk(void *u, rb_u64 fid, rb_u64 off, void *buf, size_t len){
    (void)u;(void)fid;
    pthread_mutex_lock(&g_disk_mx);
    memcpy(buf,g_disk+off,len);
    pthread_mutex_unlock(&g_disk_mx);
    return 0;
}
static void fill_gen(unsigned char *buf, rb_u64 gen){
    rb_u64 *w=(rb_u64 *)buf; rb_u32 i,n=g_sub_bytes/8;
    for(i=0;i<n;i++) w[i]=((rb_u64)gen<<32)|(rb_u64)i;
}
/* 校验 [off,off+len) 每子块内 8 字节字代一致且 ≤ 当前代；返回破坏数 */
static volatile long g_diag_n=0;
static void diag(const char *tag,rb_u64 off,rb_u32 sub,rb_u64 gen0,long cur,
                 const rb_u64 *w,rb_u32 n){
    long i=RB_AADD32((volatile LONG*)&g_diag_n,1);
    if(i>3)return;
    printf("  [DIAG %s] off=%llu sub=%u gen0=%llu cur=%ld w0=%016llx w1=%016llx w2=%016llx\n",
           tag,(unsigned long long)off,sub,(unsigned long long)gen0,cur,
           (unsigned long long)w[0],(unsigned long long)w[1],(unsigned long long)w[2]);
    fflush(stdout);
}
static long validate(const unsigned char *buf, rb_u64 off, size_t len){
    long bad=0;
    size_t p=0;
    while(p<len){
        const rb_u64 *w=(const rb_u64 *)(buf+p);
        rb_u64 gen0=w[0]>>32; rb_u32 k,n=g_sub_bytes/8;
        rb_u32 sub=(rb_u32)(((off+p)/g_sub_bytes)%g_nsubs);
        long cur=(long)RB_ALOAD32((volatile LONG *)&g_subgen[sub]);
        for(k=1;k<n;k++){
            if((w[k]>>32)!=gen0){ diag("TEAR",off+p,sub,gen0,cur,w,n); bad++; break; }
        }
        if(gen0>(rb_u64)(cur<0?0:cur)){ diag("FUTURE",off+p,sub,gen0,cur,w,n); bad++; }
        p+=g_sub_bytes;
    }
    return bad;
}

static void *writer_fn(void *arg){
    unsigned seed=(unsigned)(size_t)arg; unsigned char *buf=malloc(g_sub_bytes);
    while(!g_stop){
        rb_u32 sub=(rb_u32)((seed=(seed*1103515245u+12345u)>>16)%g_nsubs);
        long g=(long)RB_AADD32((volatile LONG *)&g_subgen[sub],1);
        fill_gen(buf,(rb_u64)g);
        { rb_direct_range dr[8]; int dc=0,q;
          if(rb_write(g_str,(rb_u64)sub*g_sub_bytes,buf,g_sub_bytes,dr,&dc,8)!=0){ diag("WBAD",(rb_u64)sub*g_sub_bytes,sub,0,0,(const rb_u64*)buf,g_sub_bytes/8); g_tornfail++; }
          /* 直写/降级区间落盘影子：保盘面自描述 */
          pthread_mutex_lock(&g_disk_mx);
          for(q=0;q<dc&&q<8;q++)
              if(dr[q].len)
                  memcpy(g_disk+dr[q].offset,buf,dr[q].len<(size_t)g_sub_bytes?dr[q].len:(size_t)g_sub_bytes);
          pthread_mutex_unlock(&g_disk_mx); }
        ts_yield();
    }
    free(buf);
    return NULL;
}
static void *reader_fn(void *arg){
    unsigned seed=(unsigned)(size_t)arg*7919u+1u;
    unsigned char *buf=malloc(1024*1024);
    while(!g_stop){
        rb_u32 sub; size_t len;
        /* 80% 读热窗口（前 1/8 区）提高快路径命中覆盖，20% 全区随机 */
        if((seed & 0x07u)!=0) sub=(rb_u32)(((seed=(seed*1103515245u+12345u)>>16))%(g_nsubs/8+1));
        else                  sub=(rb_u32)(((seed=(seed*1103515245u+12345u)>>16))%g_nsubs);
        len=g_sub_bytes*(size_t)(1u+((seed>>8)&0x0Fu));   /* 1-16 子块读 */
        if((rb_u64)sub*g_sub_bytes+len>REGION_BYTES) len=g_sub_bytes;
        if(rb_read_try_cache(g_str,(rb_u64)sub*g_sub_bytes,buf,len)==1){
            RB_AADD32((volatile LONG *)&g_hits,1);
            if(validate(buf,(rb_u64)sub*g_sub_bytes,len)) g_tornfail++;
        }else{
            /* worker 路径：rb_read 只填命中段，miss 区间必须回源（盘面影子）
             * 并 commit——与驱动 worker 流程一致 */
            RB_AADD32((volatile LONG *)&g_miss,1);
            { rb_direct_range miss[64]; int mc=0,q; int bad=0;
              rb_u64 rs=(rb_u64)sub*g_sub_bytes, re=rs+len;
              if(rb_read(g_str,rs,buf,len,miss,&mc,64)<0) bad=1;   /* rb_read 契约：0=纯miss 1=有命中 <0=错误 */
              for(q=0;!bad&&q<mc&&q<64;q++){
                  /* [W1.5] miss 区间按整子块对齐返回，两端可超出请求窗口——
                   * 调用方必须钳位到 [rs,re) 再回填（否则缓冲区越界） */
                  rb_u64 lo=miss[q].offset, hi=miss[q].offset+miss[q].len;
                  if(lo<rs)lo=rs; if(hi>re)hi=re;
                  if(lo<hi){
                      pthread_mutex_lock(&g_disk_mx);
                      memcpy(buf+(size_t)(lo-rs), g_disk+lo, (size_t)(hi-lo));
                      pthread_mutex_unlock(&g_disk_mx);
                      rb_read_commit(g_str,lo,buf+(size_t)(lo-rs),(size_t)(hi-lo));
                  }
              }
              if(!bad && mc>64){ diag("MOVF",rs,sub,0,(long)mc,(const rb_u64*)buf,4); bad=1; }
              if(!bad && validate(buf,rs,len)) g_tornfail++;
              if(bad){ diag("RBAD",rs,sub,0,(long)mc,(const rb_u64*)buf,4); g_tornfail++; } }
        }
        ts_yield();
    }
    free(buf);
    return NULL;
}
static void *inval_fn(void *arg){
    unsigned seed=(unsigned)(size_t)arg*31u+7u;
    rb_u32 span=g_nsubs/8; if(!span)span=1;
    while(!g_stop){
        rb_u32 sub=(rb_u32)(((seed=(seed*1103515245u+12345u)>>16))%(g_nsubs-span));
        rb_stream_invalidate_range(g_str,(rb_u64)sub*g_sub_bytes,(rb_u64)span*g_sub_bytes);
        ts_yield();
    }
    return NULL;
}
static void *flush_fn(void *arg){
    (void)arg;
    while(!g_stop){ rb_stream_flush(g_str,0); ts_yield(); }
    return NULL;
}

static int run_phase(rb_u32 bs, int secs){
    pthread_t th[8]; int i, fail=0;
    long hf, mf, tf;
    printf("--- spinfast bs=%u %ds ---\n",bs,secs); fflush(stdout);
    g_sub_bytes=bs; g_nsubs=(rb_u32)(REGION_BYTES/bs);
    g_disk=calloc((size_t)REGION_BYTES,1);
    g_subgen=calloc(g_nsubs,sizeof(LONG));
    g_hits=g_miss=g_tornfail=0; g_stop=0;
    pthread_mutex_init(&g_disk_mx,NULL);
    { rb_config c; memset(&c,0,sizeof(c));
      c.max_bytes=4ull*1024*1024;   /* 驱逐压力：缓存远小于区 */
      c.writeback=wb; c.readback=rbk; c.block_size=bs;
      g_eng=rb_engine_create(&c);
      if(!g_eng){ printf("[FAIL] create\n"); return 1; } }
    g_str=rb_stream_create(g_eng,1);
    /* 初始盘面=代0 全量（供 invalidate 后回源）+ 引擎预热 */
    { unsigned char *z=malloc(g_sub_bytes);
      rb_u32 k;
      for(k=0;k<g_nsubs;k++){
          fill_gen(z,0);
          pthread_mutex_lock(&g_disk_mx);
          memcpy(g_disk+(size_t)k*g_sub_bytes,z,g_sub_bytes);
          pthread_mutex_unlock(&g_disk_mx);
          { rb_direct_range dr[8]; int dc=0;
            rb_write(g_str,(rb_u64)k*g_sub_bytes,z,g_sub_bytes,dr,&dc,8); }
      }
      free(z); }
    pthread_create(&th[0],NULL,writer_fn,(void*)1u);
    pthread_create(&th[1],NULL,writer_fn,(void*)2u);
    pthread_create(&th[2],NULL,reader_fn,(void*)1u);
    pthread_create(&th[3],NULL,reader_fn,(void*)2u);
    pthread_create(&th[4],NULL,reader_fn,(void*)3u);
    pthread_create(&th[5],NULL,inval_fn,(void*)1u);
    pthread_create(&th[6],NULL,flush_fn,NULL);
    { int ms=secs*1000; while(ms>0){ Sleep(20); ms-=20; } }
    g_stop=1;
    for(i=0;i<7;i++) pthread_join(th[i],NULL);
    hf=(long)RB_ALOAD32((volatile LONG *)&g_hits);
    mf=(long)RB_ALOAD32((volatile LONG *)&g_miss);
    tf=(long)RB_ALOAD32((volatile LONG *)&g_tornfail);
    printf("  hit=%ld miss=%ld torn/fail=%ld\n",hf,mf,tf); fflush(stdout);
    if(tf){ printf("[FAIL] torn/fail>0\n"); fail=1; }
    if(hf==0){ printf("[FAIL] zero fast-path hits (path dead?)\n"); fail=1; }
    rb_stream_destroy(g_str);
    rb_engine_destroy(g_eng);
    pthread_mutex_destroy(&g_disk_mx);
    free(g_disk); free((void*)g_subgen);
    if(fail) return 1;
    printf("[ok] spinfast bs=%u\n",bs);
    return 0;
}

int main(void){
    int fail=0;
    fail|=run_phase(4096,3);
    fail|=run_phase(65536,3);
    printf(fail? "SPINFAST FAILED\n" : "SPINFAST PASS\n");
    return fail;
}
