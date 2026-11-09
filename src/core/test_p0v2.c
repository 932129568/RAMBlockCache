/* [P0-V2 2026-11-10] 多流生产形态碰撞（I47 归因批 · 纯内存零磁盘 IO）：
 *   3 常驻流 ×（2 写者 + 1 读者）+ 每流 flush + churn 流高频建毁（卷插拔形态）
 *   + 引擎级 4MB 小上限的跨流驱逐 + stats 并发读 + 看门狗（stderr 里程碑）。
 * 判据：盘区逐字节==fbyte；dirty/lost 双零；驱逐触发；ASan 零报告。
 * 诊断：所有里程碑走 stderr（unbuffered）；watchdog 每 ~2s 打点。
 * [FIX 器具] g_disk 必须 (NFID+1)*REGION（file_id∈[1,NFID] → fbase 最高 NFID*REGION）。
 * [FIX 器具 2] rb_read 返回 1=部分命中（hit 已拷入 out，miss 区间待补盘）——
 *              与 -2/0 一样必须处理，否则 out 未初始化 → 假 FAIL。
 */
#include "cache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

/* [守卫] 单 core 调用 >10s = 活锁：报告调用点后终止（自诊断模式） */
static double now_s(void){ return (double)clock() / CLOCKS_PER_SEC; }
static rb_stream *g_guard_st; static rb_u64 g_guard_off; static size_t g_guard_len;
static const char *g_guard_what; static double g_guard_t0; static int g_guard_fid;
#define GUARD_BEGIN(what, st, fid, off, len) do { g_guard_what=(what); g_guard_st=(st); g_guard_fid=(int)(fid); g_guard_off=(off); g_guard_len=(len); g_guard_t0=now_s(); } while(0)
#define GUARD_CHECK() do { double _e = now_s() - g_guard_t0; if(_e > 10.0){ fprintf(stderr, "[LIVELOCK] %s fid=%d off=%llu len=%zu took %.1fs\n", g_guard_what, g_guard_fid, (unsigned long long)g_guard_off, g_guard_len, _e); fflush(stderr); _exit(70); } } while(0)

#define REGION  (32ull * 1024 * 1024)
#define NFID    3
#define WOPS    1500
#define ROPS    1500
#define CHURN   240
#define MAXR    160
#define MAXLEN  (192 * 1024)

static unsigned char *g_disk;
static rb_engine     *g_eng;
static rb_stream     *g_st[NFID];
static pthread_mutex_t disk_mx;
static atomic_int     g_stop;
static atomic_long    g_bad;
static atomic_long    g_churn_seq;
static atomic_long    g_milestone[24];

static unsigned nrand(unsigned *s){ *s = *s*1103515245u+12345u; return *s>>1; }
static unsigned char fbyte(rb_u64 off){ return (unsigned char)(off*131u+7u); }
static rb_u64 fbase(rb_u64 fid){ return fid * REGION; }
static void mark(int slot, int op){ if((op & 63)==0) atomic_store(&g_milestone[slot], op); }

static int wb_fn(void*u,rb_u64 fid,rb_u64 off,const void*b,size_t n){
    (void)u;
    /* [FIX 器具 4] churn 流后备=region0：直写路径（can_create 失败）的数据
     * 必须可读回，否则写后读=零 → 假 FAIL。region0 与常驻流(1..NFID)不重叠。 */
    pthread_mutex_lock(&disk_mx);
    memcpy(g_disk+((fid > (rb_u64)NFID) ? 0 : fbase(fid))+off, b, n);
    pthread_mutex_unlock(&disk_mx);
    return 0;
}
static int rb_fn(void*u,rb_u64 fid,rb_u64 off,void*b,size_t n){
    (void)u;
    pthread_mutex_lock(&disk_mx);
    memcpy(b, g_disk+((fid > (rb_u64)NFID) ? 0 : fbase(fid))+off, n);
    pthread_mutex_unlock(&disk_mx);
    return 0;
}

static void serve_miss(rb_stream *s, rb_u64 fid, void*out, rb_u64 reqoff, size_t reqlen,
                       rb_direct_range*ms, int mc){
    int k; rb_u64 reqend = reqoff + reqlen;
    for(k=0;k<mc;k++){
        unsigned char*tmp = malloc(ms[k].len);
        rb_u64 mend = ms[k].offset+ms[k].len, lo, hi;
        if(!tmp){ atomic_fetch_add(&g_bad,1); continue; }
        pthread_mutex_lock(&disk_mx);
        memcpy(tmp, g_disk+((fid > (rb_u64)NFID) ? 0 : fbase(fid))+ms[k].offset, ms[k].len);
        pthread_mutex_unlock(&disk_mx);
        GUARD_BEGIN("rb_read_commit", s, 0, ms[k].offset, ms[k].len);
        rb_read_commit(s, ms[k].offset, tmp, ms[k].len);
        GUARD_CHECK();
        lo = ms[k].offset>reqoff ? ms[k].offset : reqoff;
        hi = mend<reqend ? mend : reqend;
        if(hi>lo) memcpy((unsigned char*)out+(lo-reqoff),
                         tmp+(lo-ms[k].offset), (size_t)(hi-lo));
        free(tmp);
    }
}

static void* writer(void*p){
    unsigned long id = (unsigned long)p;
    rb_u64 fid = id % NFID;
    const int slot = (int)(id % 8);
    unsigned s = (unsigned)(id*7919+13);
    for(int op=0; op<WOPS && !atomic_load(&g_stop); op++){
        rb_u64 off; size_t len, maxl;
        mark(slot, op);
        off = (rb_u64)(nrand(&s)%(REGION-1024));
        maxl = (size_t)(REGION-off);
        switch(nrand(&s)%3){
        case 0:  len = 512u + (nrand(&s)%3)*512u; break;
        case 1:  len = 4096u; break;
        default: len = 64*1024u + (nrand(&s)%2)*64*1024u; break;
        }
        if(len>MAXLEN) len=MAXLEN;
        if(len>maxl) len=maxl;
        {
            unsigned char*data = malloc(len); int k, dc=0;
            rb_direct_range dr[MAXR];
            if(!data) continue;
            { rb_u64 q; for(q=0;q<len;q++) data[q]=fbyte(off+q); }
            GUARD_BEGIN("rb_write", g_st[fid], fid, off, len);
            rb_write(g_st[fid], off, data, len, dr, &dc, MAXR);
            GUARD_CHECK();
            if(fid < (rb_u64)NFID){
                pthread_mutex_lock(&disk_mx);
                if(dc>MAXR) memcpy(g_disk+fbase(fid)+off, data, len);
                else for(k=0;k<dc;k++) memcpy(g_disk+fbase(fid)+dr[k].offset,
                            data+(dr[k].offset-off), dr[k].len);
                pthread_mutex_unlock(&disk_mx);
            }
            free(data);
        }
    }
    return NULL;
}

static void* reader(void*p){
    unsigned long id = (unsigned long)p;
    rb_u64 fid = id % NFID;
    const int slot = (int)(8 + id % 8);
    unsigned s = (unsigned)(id*104729+7);
    for(int op=0; op<ROPS && !atomic_load(&g_stop); op++){
        rb_u64 off; size_t maxl, len; size_t d;
        unsigned char*out; rb_direct_range ms[MAXR]; int mc=0; int r;
        mark(slot, op);
        off = (rb_u64)(nrand(&s)%(REGION-1024));
        maxl = (size_t)(REGION-off);
        len = 1+(size_t)(nrand(&s)%(MAXLEN<maxl?MAXLEN:maxl));
        out = malloc(len);
        if(!out) continue;
        GUARD_BEGIN("rb_read", g_st[fid], fid, off, len);
        r = rb_read(g_st[fid], off, out, len, ms, &mc, MAXR);
        GUARD_CHECK();
        if(r==-2){
            if(fid < (rb_u64)NFID){ pthread_mutex_lock(&disk_mx);
                     memcpy(out, g_disk+fbase(fid)+off, len);
                     pthread_mutex_unlock(&disk_mx); }
            else memset(out, 0, len);
        } else if(r==0 || r==1){
            serve_miss(g_st[fid], fid, out, off, len, ms, mc);
        } else { free(out); atomic_fetch_add(&g_bad,1); return NULL; }
        for(d=0; d<len; d++){
            unsigned char exp = (fid<(rb_u64)NFID) ? fbyte(off+d) : 0;
            if(out[d]!=exp) break;
        }
        if(d<len){
            fprintf(stderr, "  [FAIL] rd fid=%llu op=%d off=%llu len=%zu @%zu got=%u exp=%u\n",
                   (unsigned long long)fid, op, (unsigned long long)off, len,
                   d, out[d], (fid<(rb_u64)NFID)?fbyte(off+d):0);
            atomic_fetch_add(&g_bad,1); atomic_store(&g_stop,1);
        }
        free(out);
    }
    return NULL;
}

static void* flusher(void*p){
    unsigned long id = (unsigned long)p;
    rb_u64 fid = id % NFID;
    const int slot = (int)(8 + id % 8);
    for(int i=0;i<700 && !atomic_load(&g_stop);i++){
        mark(slot, i);
        GUARD_BEGIN("rb_stream_flush", g_st[fid], fid, 0, 0);
        rb_stream_flush(g_st[fid], 1);
        GUARD_CHECK();
        { volatile int _s; for(_s=0;_s<30000;_s++); }
    }
    return NULL;
}

static void* churner(void*p){
    unsigned s = (unsigned)(unsigned long)p ^ 0xC0DE;
    for(int i=0;i<CHURN && !atomic_load(&g_stop);i++){
        rb_u64 fid = 100 + (unsigned long)atomic_fetch_add(&g_churn_seq,1);
        rb_stream *st;
        mark(16, i);
        fprintf(stderr, "  [churn] %d create enter\n", i);
        st = rb_stream_create(g_eng, fid);
        if(!st){ atomic_fetch_add(&g_bad,1); continue; }
        {
            unsigned char buf[8192], out[8192];
            rb_u64 off = (rb_u64)(nrand(&s)%((REGION)-8192));
            rb_u64 q; rb_direct_range dr[MAXR]; rb_direct_range ms[MAXR];
            int dc=0, mc=0, r;
            for(q=0;q<sizeof(buf);q++) buf[q]=fbyte(off+q);
            GUARD_BEGIN("churn.rb_write", st, (int)fid, off, sizeof(buf));
            rb_write(st, off, buf, sizeof(buf), dr, &dc, MAXR);
            GUARD_CHECK();
            GUARD_BEGIN("churn.rb_read", st, (int)fid, off, sizeof(buf));
            r = rb_read(st, off, out, sizeof(buf), ms, &mc, MAXR);
            GUARD_CHECK();
            if(r==0||r==1) serve_miss(st, fid, out, off, sizeof(buf), ms, mc);
            else if(r==-2) memset(out, 0, sizeof(buf));
            for(q=0;q<sizeof(buf);q++)
                if(out[q]!=fbyte(off+q)){ atomic_fetch_add(&g_bad,1);
                    fprintf(stderr, "  [FAIL] churn rd fid=%llu @%llu\n",
                           (unsigned long long)fid,(unsigned long long)q); break; }
            GUARD_BEGIN("churn.flush", st, (int)fid, 0, 0);
            rb_stream_flush(st, 1);
            GUARD_CHECK();
        }
        fprintf(stderr, "  [churn] %d destroy enter\n", i);
        GUARD_BEGIN("churn.destroy", st, (int)fid, 0, 0);
        rb_stream_destroy(st);
        GUARD_CHECK();
        fprintf(stderr, "  [churn] %d destroy done\n", i);
        { volatile int _s; for(_s=0;_s<15000;_s++); }
    }
    return NULL;
}

static void* statsq(void*p){
    (void)p;
    rb_stats st;
    int n;
    for(n=0; n<4000; n++){          /* [FIX 器具 5] 有界轮询：g_stop 无人置位会永久挂起 */
        if(atomic_load(&g_stop)) break;
        rb_engine_get_stats(g_eng, &st);
        if(st.lost_dirty_bytes != 0){ atomic_fetch_add(&g_bad,1);
            fprintf(stderr, "  [FAIL] lost_dirty=%llu\n",(unsigned long long)st.lost_dirty_bytes);
            return NULL; }
        { volatile int _s; for(_s=0;_s<8000;_s++); }
    }
    return NULL;
}

static void* watchdog(void*p){
    (void)p;
    long long prev[24]; int frozen = 0; int s2; int r;
    for(s2=0;s2<24;s2++) prev[s2] = -1;
    for(r=0;r<120;r++){
        int same = 1; long long cur[24];
        { volatile int _s; for(_s=0;_s<10000000;_s++); }
        fprintf(stderr, "  [watch]");
        for(s2=0;s2<24;s2++){
            cur[s2] = (long long)atomic_load(&g_milestone[s2]);
            fprintf(stderr, " %lld", cur[s2]);
            if(s2<16 && cur[s2]!=prev[s2]) same = 0;
        }
        fprintf(stderr, "\n"); fflush(stderr);
        if(atomic_load(&g_stop)) return NULL;
        if(same && r>10 && (long long)atomic_load(&g_milestone[16]) < 192){
            frozen++;
            if(frozen==12){
                HANDLE f;
                fprintf(stderr, "  [STALL] frozen ~24s, self-dump\n"); fflush(stderr);
                f = CreateFileA("p0v2_stall.dmp", GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if(f != INVALID_HANDLE_VALUE){
                    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                      MiniDumpNormal, NULL, NULL, NULL);
                    CloseHandle(f);
                    fprintf(stderr, "  [STALL] dump written\n"); fflush(stderr);
                } else fprintf(stderr, "  [STALL] dump open failed\n");
                fflush(stderr);
                _exit(71);
            }
        } else frozen = 0;
        for(s2=0;s2<24;s2++) prev[s2] = cur[s2];
    }
    return NULL;
}

int main(void){
    pthread_t w[NFID*2], r[NFID], f[NFID], ch, sq, wd;
    rb_config cfg; int i; rb_stats st;
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 4ull*1024*1024;
    cfg.max_merge_len = 4*1024*1024;
    cfg.readback = rb_fn; cfg.writeback = wb_fn;
    g_disk = malloc(REGION*(NFID+1));
    if(!g_disk){ fprintf(stderr,"  [FAIL] oom\n"); return 2; }
    { rb_u64 q; for(q=0;q<REGION*(NFID+1);q++) g_disk[q]=fbyte(q); }
    pthread_mutex_init(&disk_mx, NULL);
    fprintf(stderr, "  [init] engine create enter\n");
    g_eng = rb_engine_create(&cfg);
    if(!g_eng){ fprintf(stderr,"  [FAIL] engine\n"); return 2; }
    for(i=0;i<NFID;i++){ g_st[i]=rb_stream_create(g_eng,(rb_u64)(i+1));
        if(!g_st[i]){ fprintf(stderr,"  [FAIL] stream %d\n", i); return 2; } }
    fprintf(stderr, "  [init] streams ok, launching threads\n");
    for(i=0;i<NFID;i++){
        pthread_create(&w[2*i],   NULL, writer, (void*)(unsigned long)(2*i));
        pthread_create(&w[2*i+1], NULL, writer, (void*)(unsigned long)(2*i+1));
        pthread_create(&r[i],     NULL, reader, (void*)(unsigned long)(1000+i));
        pthread_create(&f[i],     NULL, flusher,(void*)(unsigned long)(2000+i));
    }
    pthread_create(&ch, NULL, churner, (void*)3);
    pthread_create(&sq, NULL, statsq, NULL);
    pthread_create(&wd, NULL, watchdog, NULL);

    for(i=0;i<NFID*2;i++) pthread_join(w[i],NULL);
    for(i=0;i<NFID;i++) pthread_join(r[i],NULL);
    for(i=0;i<NFID;i++) pthread_join(f[i],NULL);
    pthread_join(ch,NULL); pthread_join(sq,NULL);

    for(i=0;i<NFID;i++) rb_stream_flush(g_st[i],1);
    rb_engine_get_stats(g_eng,&st);
    printf("  evict=%llu dirty=%llu lost=%llu bad=%ld\n",
           (unsigned long long)st.evict_bytes,
           (unsigned long long)st.dirty_bytes,
           (unsigned long long)st.lost_dirty_bytes,
           (long)atomic_load(&g_bad));
    fflush(stdout);
    {
        long bad=0;
        pthread_mutex_lock(&disk_mx);
        for(rb_u64 q=0;q<REGION*(NFID+1);q++)
            if(g_disk[q]!=fbyte(q)){ bad++; if(bad==1)
                fprintf(stderr,"  [FAIL] disk@%llu\n",(unsigned long long)q); }
        pthread_mutex_unlock(&disk_mx);
        if(bad) atomic_fetch_add(&g_bad,bad);
    }
    if(st.lost_dirty_bytes!=0 || st.dirty_bytes!=0) atomic_fetch_add(&g_bad,1);
    if(st.evict_bytes==0){
        fprintf(stderr,"  [FAIL] no cross-stream eviction evidence\n");
        atomic_fetch_add(&g_bad,1);
    }
    for(i=0;i<NFID;i++) rb_stream_destroy(g_st[i]);
    rb_engine_destroy(g_eng);
    free(g_disk);
    if(atomic_load(&g_bad)){ fprintf(stderr,"  [FAIL] bad=%ld\n",(long)atomic_load(&g_bad)); return 1; }
    printf("  [ok] P0-V2 multi-stream collision clean\n");
    return 0;
}
