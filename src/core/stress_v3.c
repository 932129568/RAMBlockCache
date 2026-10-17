/* RAMBoostFS V3 多流并发压力测试（锁安全模型，见文件尾说明） */
#include "cache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NS        4
#define PART      (8ull*1024*1024)
#define DISKSZ    (NS*PART)
#define OPS       6000
#define SUB       4096
#define SUBS      (PART/SUB)

static unsigned char *g_disk;
static pthread_mutex_t lk[NS];
static rb_engine *eng;
static rb_stream *g_st[NS];

static int wb_fn(void *u, rb_u64 fid, rb_u64 off, const void *b, size_t n){
    (void)u;(void)fid;
    if (off+n > DISKSZ) return -1;
    memcpy(g_disk+off, b, n); return 0;
}
static int rb_fn(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n){
    return wb_fn(u,fid,off,b,n);
}

typedef struct { int si; unsigned seed; } Warg;

static void *worker(void *p){
    Warg *a=(Warg*)p; int si=a->si;
    unsigned r=a->seed;
    rb_stream *s=g_st[si];          /* 流已在 main 串行预创建 */
    pthread_mutex_lock(&lk[si]);
    for(int op=0; op<OPS; op++){
        r = r*1103515245u+12345u;
        int kind = (r>>16)%100;
        rb_u64 subi = (r>>3)%SUBS;
        rb_u64 base = (rb_u64)si*PART + subi*SUB;
        char tmp[SUB];
        if(kind<55){
            for(int k=0;k<SUB;k++) tmp[k]=(char)(r>>(k%32))+(char)k;
            rb_direct_range dr[32]; int dc=0;
            rb_write(s, base, tmp, SUB, dr, &dc, 32);
            for(int d=0;d<dc;d++)
                if(dr[d].offset+dr[d].len <= (rb_u64)(si+1)*PART)
                    memcpy(g_disk+dr[d].offset, tmp+(dr[d].offset-base), dr[d].len);
        } else {
            char out[SUB];
            rb_direct_range ms[32]; int mc=0;
            int rr=rb_read(s, base, out, SUB, ms, &mc, 32);
            if(rr==1 && mc==0){
                char out2[SUB]; rb_direct_range m2[32]; int c2=0;
                rb_read(s, base, out2, SUB, m2, &c2, 32);
                if(c2==0 && memcmp(out,out2,SUB)!=0){
                    printf("[流%d] 两次缓存读不一致 op=%d\n",si,op); exit(2);
                }
            }
            for(int m=0;m<mc;m++){
                memcpy(tmp, g_disk+ms[m].offset, ms[m].len);
                rb_read_commit(s, ms[m].offset, tmp, ms[m].len);
            }
        }
    }
    pthread_mutex_unlock(&lk[si]);
    return NULL;
}

static void *bg(void *p){
    (void)p;
    for(int it=0; it<400; it++){
        usleep(2000);
        rb_engine_tick(eng, (rb_u64)it*500);
        for(int si=0; si<NS; si++)
            if(pthread_mutex_trylock(&lk[si])==0){
                rb_stream_flush(g_st[si], 0);
                if(it%7==0) rb_stream_idle_reclaim(g_st[si]);
                pthread_mutex_unlock(&lk[si]);
            }
    }
    return NULL;
}

int main(void){
    g_disk = malloc(DISKSZ);
    if(!g_disk){printf("oom\n");return 1;}
    for(rb_u64 i=0;i<DISKSZ;i++) g_disk[i]=(unsigned char)(i*131+7);
    rb_config cfg; memset(&cfg,0,sizeof(cfg));
    cfg.max_bytes=16ull*1024*1024;
    cfg.readback=rb_fn; cfg.writeback=wb_fn;
    cfg.idle_reclaim_ms=5000;
    eng=rb_engine_create(&cfg);
    for(int i=0;i<NS;i++) pthread_mutex_init(&lk[i],NULL);
    /* 先单线程串行创建全部流（避免并发改引擎流链表），并完成发布 */
    for(int i=0;i<NS;i++) g_st[i]=rb_stream_create(eng,(rb_u64)(i+1));
    pthread_t wt[NS], bgt; Warg wa[NS];
    for(int i=0;i<NS;i++){ wa[i].si=i; wa[i].seed=0x1234+i*999; }
    for(int i=0;i<NS;i++) pthread_create(&wt[i],NULL,worker,&wa[i]);
    pthread_create(&bgt,NULL,bg,NULL);
    for(int i=0;i<NS;i++) pthread_join(wt[i],NULL);
    pthread_join(bgt,NULL);
    for(int si=0;si<NS;si++){ pthread_mutex_lock(&lk[si]); rb_stream_flush(g_st[si],1); pthread_mutex_unlock(&lk[si]); }
    rb_stats st; rb_engine_get_stats(eng,&st);
    printf("脏=%llu 回写=%llu 淘汰=%llu trim=%llu 空闲释放=%llu 丢弃=%llu\n",
        (unsigned long long)st.dirty_bytes,(unsigned long long)st.wb_bytes,
        (unsigned long long)st.evict_bytes,(unsigned long long)st.trim_count,
        (unsigned long long)st.idle_reclaim_bytes,(unsigned long long)st.lost_dirty_bytes);
    if(st.dirty_bytes!=0){printf("FAIL 脏数据\n");return 1;}
    if(st.lost_dirty_bytes!=0){printf("FAIL 丢弃\n");return 1;}
    rb_engine_destroy(eng);
    printf("V3 多流并发压力测试通过\n");
    free(g_disk);
    return 0;
}
