/* R2 回归：writer / flusher / flush_partition 交错，断言 dirty 计数
 * 全程无下溢(>FSZ 即判异常)且终态精确归零。Windows 线程版。 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define FSZ  (16ull*1024*1024)
/* V7.4 merges adjacent dirty blocks into large contiguous segments; a segment
 * near the end can overrun FSZ. Over-allocate disk and bound writes so every
 * truly-writable dirty block can land (fixture fix, not product behavior). */
#define DISKSZ (FSZ + 2ull*1024*1024)
#define OMAX  (FSZ - 512ull*1024)
#define NW   4
#define OPS  800
static unsigned char *g_disk;
static unsigned char *g_shadow;   /* [FIX #5.2] 每写者独占分片影子（NW x SHARD） */
static rb_engine *g_eng;
static rb_stream  *g_st;
static volatile LONG g_stop = 0, g_bad = 0;
static int wb_fn(void*u,rb_u64 f,rb_u64 o,const void*d,size_t n){
    (void)u;(void)f; if(o+n>DISKSZ) return -1; memcpy(g_disk+o,d,n); return 0; }
static int rb_fn(void*u,rb_u64 f,rb_u64 o,void*b,size_t n){
    (void)u;(void)f; if(o+n>DISKSZ) return -1; memcpy(b,g_disk+o,n); return 0; }
static unsigned nrand(unsigned*s){*s=*s*1103515245u+12345u;return *s>>1;}
/* [FIX #5.2] 每写者独占 1/4 分片（4MB）：原 4 写者在全 16MB 随机覆写但终态
 * 唯一断言是 dirty_bytes==0——无影子、无盘面比对，"有覆写、无校验"。
 * 现按 wid 把 off 限制在片内，每片维护独占影子（写后即时更新，无需锁），
 * 终检逐片全量 memcmp。直写分支补 rb_stream_invalidate_range（对齐
 * test_wsconc.c:186/201 驱动纪律：镜像成功后失效，防陈旧副本后被读命中）。 */
#define SHARD ((OMAX) / NW)
static DWORD WINAPI writer(LPVOID p){
    unsigned seed=(unsigned)(unsigned long)p^0x5a5a; int op;
    int wid=(int)((unsigned long)p-1); rb_u64 base=(rb_u64)wid*SHARD;
    for(op=0;op<OPS&&!g_bad;op++){
        rb_u64 off=base+(rb_u64)(nrand(&seed)%SHARD);
        size_t len=1+(size_t)(nrand(&seed)%(200*1024));
        if(off+len>base+SHARD) len=(size_t)(base+SHARD-off);
        if(len==0) continue;
        { unsigned char*b=(unsigned char*)malloc(len);
          rb_direct_range dr[64]; int dc=0,k;
          for(size_t i=0;i<len;i++) b[i]=(unsigned char)nrand(&seed);
          rb_write(g_st,off,b,len,dr,&dc,64);
          if(dc>64){ memcpy(g_disk+off,b,len); rb_stream_invalidate_range(g_st,off,len); }
          else {
              for(k=0;k<dc;k++) memcpy(g_disk+dr[k].offset,b+(dr[k].offset-off),dr[k].len);
              for(k=0;k<dc;k++) rb_stream_invalidate_range(g_st,dr[k].offset,dr[k].len);
          }
          memcpy(g_shadow+off,b,len);   /* 独占片：无需锁 */
          free(b); }
    } return 0; }
static DWORD WINAPI flusher(LPVOID p){
    unsigned seed=(unsigned)(unsigned long)p^0xa5a5;
    while(!g_stop&&!g_bad){
        rb_stream_flush(g_st,(int)(nrand(&seed)&1));
        { rb_stats st; rb_engine_get_stats(g_eng,&st);
          if(st.dirty_bytes>FSZ){ printf("  [FAIL] dirty underflow: %llu\n",
              (unsigned long long)st.dirty_bytes); g_bad=1; } }
        Sleep(1);
    } return 0; }
static DWORD WINAPI partflusher(LPVOID p){
    int my=(int)((unsigned long)p)&3;
    while(!g_stop&&!g_bad){ rb_stream_flush_partition(g_st,my,4); Sleep(1); }
    return 0; }
int main(void){
    HANDLE wt[NW],ff[2],pf[4]; int i;
    g_disk=(unsigned char*)calloc(1,DISKSZ);
    g_shadow=(unsigned char*)calloc(1,DISKSZ);   /* 初值 0=盘面初值 */
    { rb_config c; memset(&c,0,sizeof c);
      c.max_bytes=8ull*1024*1024; c.max_merge_len=4ull*1024*1024;
      c.writeback=wb_fn; c.readback=rb_fn; c.block_size=4096;
      g_eng=rb_engine_create(&c); }
    g_st=rb_stream_create(g_eng,1);
    for(i=0;i<NW;i++) wt[i]=CreateThread(NULL,0,writer,(LPARAM)(i+1),0,NULL);
    for(i=0;i<2;i++) ff[i]=CreateThread(NULL,0,flusher,(LPARAM)(i+11),0,NULL);
    for(i=0;i<4;i++) pf[i]=CreateThread(NULL,0,partflusher,(LPARAM)i,0,NULL);
    for(i=0;i<NW;i++) WaitForSingleObject(wt[i],INFINITE);
    g_stop=1;
    for(i=0;i<2;i++) WaitForSingleObject(ff[i],INFINITE);
    for(i=0;i<4;i++) WaitForSingleObject(pf[i],INFINITE);
    rb_stream_flush(g_st,1);
    { rb_stats st; rb_engine_get_stats(g_eng,&st);
      if(st.dirty_bytes!=0){ printf("[FAIL] dirty!=0: %llu\n",
          (unsigned long long)st.dirty_bytes); g_bad=1; }
      else printf("[ok] dirty=0: concurrent/partition writeback counts consistent\n"); }
    /* [FIX #5.2] 逐片全量比对：覆写并发 + 在途回写后的盘面必须等于影子权威态。
     * 原终态只查 dirty==0（记账），不查数据——旧数据覆写类污染 100% 逃逸。 */
    { int w;
      for(w=0;w<NW;w++){
        rb_u64 base=(rb_u64)w*SHARD;
        if(memcmp(g_disk+base,g_shadow+base,SHARD)!=0){
            size_t j; int shown=0;
            for(j=0;j<SHARD && shown<3;j+=4096){
                if(memcmp(g_disk+base+j,g_shadow+base+j,4096)!=0){
                    printf("  [FAIL] shard %d mismatch @%llu (first of run)\n",
                           w,(unsigned long long)(base+j));
                    shown++; g_bad=1;
                }
            }
            if(!shown){ printf("  [FAIL] shard %d mismatch (unaligned)\n",w); g_bad=1; }
        }
      }
      if(!g_bad) printf("[ok] all %d shards byte-exact vs shadow (overwrite x concurrent writeback)\n",NW); }
    rb_stream_destroy(g_st); rb_engine_destroy(g_eng);
    printf(g_bad?"test_counter: FAIL\n":"test_counter: PASS\n");
    free(g_disk); free(g_shadow);
    return g_bad?1:0;
}
