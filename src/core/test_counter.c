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
static rb_engine *g_eng;
static rb_stream  *g_st;
static volatile LONG g_stop = 0, g_bad = 0;
static int wb_fn(void*u,rb_u64 f,rb_u64 o,const void*d,size_t n){
    (void)u;(void)f; if(o+n>DISKSZ) return -1; memcpy(g_disk+o,d,n); return 0; }
static int rb_fn(void*u,rb_u64 f,rb_u64 o,void*b,size_t n){
    (void)u;(void)f; if(o+n>DISKSZ) return -1; memcpy(b,g_disk+o,n); return 0; }
static unsigned nrand(unsigned*s){*s=*s*1103515245u+12345u;return *s>>1;}
static DWORD WINAPI writer(LPVOID p){
    unsigned seed=(unsigned)(unsigned long)p^0x5a5a; int op;
    for(op=0;op<OPS&&!g_bad;op++){
        rb_u64 off=(rb_u64)(nrand(&seed)%OMAX);
        size_t len=1+(size_t)(nrand(&seed)%(200*1024));
        if(off+len>OMAX) len=(size_t)(OMAX-off);
        if(len==0) continue;
        { unsigned char*b=(unsigned char*)malloc(len);
          rb_direct_range dr[64]; int dc=0,k;
          for(size_t i=0;i<len;i++) b[i]=(unsigned char)nrand(&seed);
          rb_write(g_st,off,b,len,dr,&dc,64);
          if(dc>64) memcpy(g_disk+off,b,len);
          else for(k=0;k<dc;k++) memcpy(g_disk+dr[k].offset,b+(dr[k].offset-off),dr[k].len);
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
    rb_stream_destroy(g_st); rb_engine_destroy(g_eng);
    printf(g_bad?"test_counter: FAIL\n":"test_counter: PASS\n");
    free(g_disk);
    return g_bad?1:0;
}
