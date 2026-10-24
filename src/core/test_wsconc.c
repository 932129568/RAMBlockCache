/*
 * test_wsconc.c —— [D2/D5] 并发写 + 读 + zoned 回写 + 驱逐 完整性测试
 *
 * 场景（与 deliverables/memlevel-perf-design-2026-10-20.md §七 对应）：
 *   8 写者（顺序推进、随机长度、随机数据，各自独占一片稳定前缀）+
 *   8 读者（只读本片已发布稳定前缀，逐字节校验影子）+
 *   4 zoned flusher（rb_stream_flush_zoned 四路并行回写）+
 *   小 max_bytes（2MB）强制驱逐/回收/回写交织。
 *   文件 1GB（跨 4 个 256MB zone → 4 个 flusher 都有活干；每片仅写前 4MB，
 *   盘/影子数组按虚存分配，实驻内存 ~64MB）。
 *
 * 断言（每轮）：
 *   · 磁盘镜像逐字节一致（各片已发布前缀 g_disk == g_shadow）；
 *   · lost_dirty_bytes == 0（驱逐/回写路径零非预期丢弃）；
 *   · dirty_bytes == 0（force flush 净帧）；
 *   · chunk_count 归零（rb_stream_destroy 后）。
 * 10 轮全过 = 分片链表（UAF/漏摘/锁序）+ ents 锁外预扩容 + 淘汰选择分片版
 * 的正确性兜底；ASAN 构建零报告（use-after-free / 堆溢出具象化）。
 *
 * 竞态安全论证（读者为何不会误报、为何不会数据损坏）：
 *   · 写者顺序推进且只向前：发布水位 W_i 之前的字节均只被写过一次 →
 *     [base_i, W_i) 内容稳定；读者只读该区间。
 *   · 发布顺序：rb_write 返回 → direct 区间镜像落盘（disk_mx）+ 失效缓存旧
 *     副本（superseded=1，驱动同款纪律）→ 影子更新 + W_i 发布（shard_mx）。
 *   · miss 填充只回填【请求交集】内的完整子块：miss 区间按子块对齐，末子块
 *     的未发布尾巴在飞——把未发布字节一并 commit 会在直写路径留下陈旧副本
 *     （[WS-RACE] 实证：盘面已新/缓存仍旧/复读仍不符）；裁剪后 full_cover
 *     自然只缓存稳定的内部子块（代价：边缘子块二次读重取，正确性优先）。
 *   · 脏块不被驱逐（evict_victim 仅取 clean；dirty 先由回写/reclaim 落盘
 *     才可变干净）→ 读者 miss 回源盘面时，盘面内容必已等于影子。
 *   · RMW 补读洞必为"未加载"子块（从未脏过 → 回写器不会碰）→ 盘面即写前值。
 *
 * 编译单元 = test_wsconc.c + cache.c（build_all.bat 常规行，Release+ASAN）。
 */

#include "cache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

#define NSHARD    8
#define SHARD_SZ  (128ull * 1024 * 1024)          /* 片间距：跨 4 个 zone */
#define WRITE_SZ  (4ull * 1024 * 1024)            /* 每片实写前缀（顺序推进） */
#define DISKSZ    ((rb_u64)NSHARD * SHARD_SZ)     /* 1GB 逻辑盘（虚存） */
#define CACHEMB   2u                              /* 小缓存强制驱逐 */
#define NFLUSH    4
#define ROUNDS    10
/* 诊断/覆盖开关（argv）：
 * noflush=不起回写线程；bigcache=64MB 缓存（不触发驱逐）；
 * bigwrite=写长上限 64KB→1MB（256 子块）；
 * p21=P2-1 确定性覆盖：max_direct=1——任何写出现 ≥2 个 direct 区间即返回 -2，
 *   强制走"整段镜像+整段失效"主路径（驱动 P2-1 逐字同构；1MB 写长下 direct
 *   区间按 chunk 粒度合并，dcnt 难自然 >160，本模式保证覆盖）；
 * overlap=全员写同一区域（生产同流重叠写；数据=f(off) 确定性——任意交错
 *   收敛同一字节，对竞态损坏价值不可见，QA Round 3 裁定）；
 * slowwb=回写回调 Sleep(1) 模拟慢盘（放大 [stage,writeback] 时长，非 stage
 *   时机窗口；稀缺环节是回写器恰在两笔连续写 absorb 之间 stage 到边界子块）；
 * mirrorall=有 direct 区间时【整段】镜像+失效——应力模式，非驱动行为
 *   （驱动 dc>0 只逐区间镜像、整段仅 -2；本模式放大交错面以暴露竞态，
 *    通过它不证明驱动路径正确性——QA Round 3 裁定口径）。 */
static int opt_noflush = 0, opt_bigcache = 0, opt_mirrorall = 0, opt_bigwrite = 0, opt_p21 = 0;
static int opt_slowwb = 0;   /* 诊断：回写回调 Sleep(1) 模拟慢盘（放大 [stage,writeback] 时长） */
/* [FIX #1] rewrite = 覆盖式重写模式（变异测试牙口）：每写者对同一片跑两趟，
 * 第二趟用另一 seed（同区间不同数据）——Known Issues 披露的"覆盖式重写"构造。
 * 直写/-2 路径对第二趟的逐区间镜像 + 失效，与在途回写的第一趟旧数据形成
 * 持久污染窗口：变异体（stage_revalidate 恒过 / wb_drain 空转）会让旧数据
 * 落在新镜像之后 → 终检 memcmp(disk, shadow) 失败 = 杀死；
 * 修复版（gen 复检弃写 / drain 排空）盘面收敛到第二趟数据 = PASS。
 * 与 overlap 互斥（overlap 数据=f(off) 确定性，两趟相同——显式取随机路径）。 */
static int opt_rewrite = 0;
static int opt_overlap = 0;  /* 诊断：全员写同一区域（生产同流重叠写；数据=f(off) 确定性，损坏价值不可见） */
#define WRITE_CAP (opt_bigwrite ? (1u<<20) : 65536u)   /* 写长上限：默认 64KB；bigwrite=1MB（P2-1 可达） */
#define WRITE_MAXD (opt_p21 ? 1 : 160)                 /* direct 区间上限：p21=1 强制 -2 路径 */

static unsigned char *g_disk;
static unsigned char *g_shadow;
static rb_engine     *g_eng;
static rb_stream     *g_st;
static pthread_mutex_t g_disk_mx;                 /* 盘面回调 + 读者回源 */
static pthread_mutex_t g_shard_mx[NSHARD];        /* 影子区间 + 水位（读者验证） */
static rb_u64         g_wmark[NSHARD];            /* 每片已发布稳定前缀末端 */
static atomic_int     g_stop;                     /* 读者/回写器停止信号 */
static atomic_int     g_rfail;                    /* 读者校验失败计数（轮末汇总） */
static int            g_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); g_fail++; } \
    else { printf("  [ok]   " __VA_ARGS__); printf("\n"); } \
} while (0)

static unsigned nrand(unsigned *s){ *s = *s*1103515245u+12345u; return *s>>1; }
static unsigned char pat(rb_u64 off){ return (unsigned char)(off*131u+7u); }

/* ---- 盘回调：stripe -> disk_mx（唯一方向，无环）---- */
/* [FIX R9-13 回归网] overlap 模式盘面内容确定性 = f(off)（初值即 f、写者只写 f）。
 * wb_fn 是唯一能把缓存内无效数据（典型：被销毁重建块的零数据）带到盘面的路径。
 * 修复前实测复现（2026-10-21 sweep）：chunk 边界起 5 子块全零被回写覆盖正确
 * 盘面——根因 stage 未复核快照脏位（cache.c stage_run [FIX R9-13]）。
 * 本 oracle 在 memcpy 前逐字节校验，违反即打印现场（段/前后文/盘面前值/水位）、
 * 拒绝落盘并 g_fail++（本轮 exit 非 0）——把静默损坏变成带取证的硬失败。 */
static int wb_fn(void*u,rb_u64 fid,rb_u64 off,const void*b,size_t n){
    (void)u;(void)fid;
    if(off+n>DISKSZ) return -1;
    if(opt_slowwb) Sleep(1);   /* 诊断：模拟慢盘，放大 [stage,writeback] 在途窗口（有牙对照） */
    if(opt_overlap){
        const unsigned char*bb=(const unsigned char*)b; size_t i; int bad=-1;
        for(i=0;i<n;i++){
            rb_u64 o=off+i;
            if(o<(rb_u64)WRITE_SZ && bb[i]!=pat(o)){ bad=(int)i; break; }
        }
        if(bad>=0){
            rb_u64 bo=off+(size_t)bad; size_t k; rb_u64 w0=0;
            pthread_mutex_lock(&g_disk_mx);
            printf("  [FAIL] R9-13 回写污染：seg_off=%llu seg_len=%zu bad@+%d(abs=%llu) got=%u exp=%u\n",
                   (unsigned long long)off,n,bad,(unsigned long long)bo,bb[bad],pat(bo));
            printf("    staged:");
            for(k=(bad>32?(size_t)bad-32:0);k<n&&k<(size_t)bad+32;k++) printf(" %02x",bb[k]);
            printf("\n    disk  :");
            for(k=(bad>32?(size_t)bad-32:0);k<n&&k<(size_t)bad+32;k++) printf(" %02x",g_disk[off+k]);
            printf("\n");
            pthread_mutex_unlock(&g_disk_mx);
            pthread_mutex_lock(&g_shard_mx[0]); w0=g_wmark[0]; pthread_mutex_unlock(&g_shard_mx[0]);
            printf("    wmark[0]=%llu（bad_off %s 水位）\n",
                   (unsigned long long)w0, bo<w0?"低于":"高于");
            g_fail++;
            return 0;   /* 拒绝落盘：盘面保持干净，本轮由 g_fail 判负 */
        }
    }
    pthread_mutex_lock(&g_disk_mx);
    memcpy(g_disk+off,b,n);
    pthread_mutex_unlock(&g_disk_mx);
    return 0;
}
static int rb_fn(void*u,rb_u64 fid,rb_u64 off,void*b,size_t n){
    (void)u;(void)fid;
    if(off+n>DISKSZ) return -1;
    pthread_mutex_lock(&g_disk_mx);
    memcpy(b,g_disk+off,n);
    pthread_mutex_unlock(&g_disk_mx);
    return 0;
}

/* 模拟驱动处理 miss：读盘（disk_mx，短持）-> commit -> 交集拷回。
 * [WS-RACE] miss 区间按整子块对齐，两端可越过请求末端进入"写者在飞"区域
 * （请求 ⊆ 已发布前缀，末子块的未发布尾巴在飞）。把未发布字节一并
 * rb_read_commit 会在直写路径留下陈旧副本——直写"镜像→失效"只更盘不更缓存，
 * 读者持旧盘面建块后，其后读取命中陈旧副本（实证：盘面已新/缓存仍旧）。
 * 收敛：只回填【请求交集】内的完整子块（rb_read_commit 的 full_cover 自然
 * 只缓存完整覆盖的内部子块；首尾边缘子块不缓存——代价是二次读重取该子块，
 * 正确性优先）。驱动侧同款收敛见 rbio.c 直写成功后的 invalidate（宽窗口）；
 * 读者盘读+建块之间的残余微窗为存量竞态，见
 * deliverables/wsrace-fix-report-2026-10-21.md §五。 */
static void serve_miss(unsigned char*out,rb_u64 reqoff,size_t reqlen,
                       rb_direct_range*ms,int mc){
    int k; rb_u64 reqend=reqoff+reqlen;
    for(k=0;k<mc;k++){
        unsigned char*tmp=(unsigned char*)malloc(ms[k].len);
        rb_u64 mend=ms[k].offset+ms[k].len,lo,hi,clo,chi;
        if(!tmp){ atomic_store(&g_stop,1); return; }
        pthread_mutex_lock(&g_disk_mx);
        memcpy(tmp,g_disk+ms[k].offset,ms[k].len);
        pthread_mutex_unlock(&g_disk_mx);
        clo=(ms[k].offset>reqoff)?ms[k].offset:reqoff;
        chi=(mend<reqend)?mend:reqend;
        if(chi>clo)
            rb_read_commit(g_st,clo,tmp+(size_t)(clo-ms[k].offset),
                           (size_t)(chi-clo));
        lo=ms[k].offset>reqoff?ms[k].offset:reqoff;
        hi=mend<reqend?mend:reqend;
        if(hi>lo) memcpy(out+(size_t)(lo-reqoff),
                         tmp+(size_t)(lo-ms[k].offset),(size_t)(hi-lo));
        free(tmp);
    }
}

/* ================= 写者：顺序推进 + 随机长/数据 ================= */
typedef struct { int wid; unsigned seed; } WArg;
static void* writer(void*p){
    WArg*a=(WArg*)p; int wid=a->wid; unsigned seed=a->seed;
    /* [D2-race] overlap 模式：全员写同一区域（生产驱动同流 8MB 块/并发 IRP
     * 天然重叠——-2 竞态需要"写者范围内有他人脏子块"，分片写者永不重叠、
     * 竞态不可见）。数据 = f(off) 确定性函数（与影子/读者校验同源，
     * 任意交错收敛同一字节），影子验证仍成立。
     * [FIX #1] rewrite 优先于 overlap：两趟必须不同数据，强制随机路径。 */
    rb_u64 base=(rb_u64)((opt_overlap&&!opt_rewrite)?0:wid)*SHARD_SZ;
    rb_u64 end=base+WRITE_SZ;
    unsigned char*buf=(unsigned char*)malloc((size_t)WRITE_SZ);
    size_t i; int sweep, nsweep=opt_rewrite?2:1;
    if(!buf){ atomic_store(&g_stop,1); return NULL; }
    for(sweep=0;sweep<nsweep && !atomic_load(&g_stop);sweep++){
    rb_u64 pos=base;
    unsigned sw_seed = sweep==0 ? seed : (seed ^ 0xBEEF1234u);
    if(sweep==1){
        /* 覆盖式重写入口：复位本片水位（读者若在会自然等待新区间；
         * rewrite 模式不建读者，见 run_round）。 */
        pthread_mutex_lock(&g_shard_mx[wid]);
        g_wmark[wid]=base;
        pthread_mutex_unlock(&g_shard_mx[wid]);
    }
    for(i=0;i<(size_t)WRITE_SZ;i++)
        buf[i]=(opt_overlap&&!opt_rewrite)?(unsigned char)((base+i)*131u+7u):(unsigned char)nrand(&sw_seed);
    while(pos<end && !atomic_load(&g_stop)){
        size_t room=(size_t)(end-pos);
        size_t cap = room<WRITE_CAP?room:WRITE_CAP;
        size_t len=1+(size_t)(nrand(&seed)%cap);
        rb_direct_range dr[160]; int dc=0,k;
        int wrc = rb_write(g_st,pos,buf+(size_t)(pos-base),len,dr,&dc,WRITE_MAXD);
        /* 直写落地策略：
         *  -2（wrc<0，dcnt=max_direct+1）→ 整段镜像 + 整段失效
         *                    （驱动 P2-1 整块直写路径，与 rbio.c:535-548 逐字同构）；
         *  mirrorall 且 dc>0 → 整段镜像 + 整段失效
         *                    【应力模式，非驱动行为】：驱动 dc>0 只逐区间镜像
         *                    （rbio.c:551-586），整段仅 -2 溢出；本条款把逐区间
         *                    升级为整段以放大在途回写交错面（"加压"），用于暴露
         *                    竞态，通过它**不证明**驱动路径正确性；
         *  dc>0（默认）    → 逐区间镜像 + 逐区间失效（驱动逐字同构）；
         *  dc==0（全吸收） → 不碰盘不失效（驱动亦如此，数据走正常回写——
         *                    core 的 wb_drain 只保护 direct 路径）。
         * 【同构口径精确化】"与 P2-1 逐字同构"仅对 wrc!=0 / dc>160 两个
         * 子句成立；mirrorall 且 dc>0 子句为加压模式、非驱动行为（QA
         * Round 3 裁定，finding-3 闭环）。 */
        if(wrc!=0 || dc>160 || (opt_mirrorall && dc>0)){
            /* -2（direct 溢出）：整段按 direct 落盘（缓存退化，数据正确）；
             * mirrorall 且 dc>0：同上落盘形态，但属加压模式（非驱动行为）。 */
            pthread_mutex_lock(&g_disk_mx);
            memcpy(g_disk+pos,buf+(size_t)(pos-base),len);
            pthread_mutex_unlock(&g_disk_mx);
            /* [FIX WS-RACE] 驱动同款纪律：直写成功后失效同区间缓存旧副本
             * （superseded=1）。core 的 direct 区间=未吸收，但洞期间并发读
             * 回填可在缓存留下写前副本；不失效则其后读取命中陈旧副本
             * （test_wsconc 实证：盘面已新/缓存仍旧/复读仍不符）。 */
            rb_stream_invalidate_range(g_st,pos,len);
        }else if(dc>0){
            pthread_mutex_lock(&g_disk_mx);
            for(k=0;k<dc;k++){
                rb_u64 o=dr[k].offset; size_t l=dr[k].len;
                if(o<pos){ if(l<(size_t)(pos-o)) l=0; else l-=(size_t)(pos-o); o=pos; }
                if(o+l>end) l=(size_t)(end-o);
                if(l) memcpy(g_disk+o,buf+(size_t)(o-base),l);
            }
            pthread_mutex_unlock(&g_disk_mx);
            /* [FIX WS-RACE] 驱动同款：逐区间直写成功后失效（先镜像后失效） */
            for(k=0;k<dc;k++){
                rb_u64 o=dr[k].offset; size_t l=dr[k].len;
                if(o<pos){ if(l<(size_t)(pos-o)) l=0; else l-=(size_t)(pos-o); o=pos; }
                if(o+l>end) l=(size_t)(end-o);
                if(l) rb_stream_invalidate_range(g_st,o,l);
            }
        }
        pos+=len;
        /* 发布：影子先于水位（读者可见时缓存/盘皆具稳定值）。
         * 仅拷本次段（顺序推进不回头，读者只读 < 水位的稳定区）。
         * overlap 模式全员同区：影子用共享锁（g_shard_mx[0]），数据 f(off)
         * 幂等；水位取 max（读者读全区间均可——初值即 f(off)）。 */
        pthread_mutex_lock(&g_shard_mx[(opt_overlap&&!opt_rewrite)?0:wid]);
        memcpy(g_shadow+(pos-len),buf+(size_t)(pos-len-base),len);
        if(pos>g_wmark[(opt_overlap&&!opt_rewrite)?0:wid]) g_wmark[(opt_overlap&&!opt_rewrite)?0:wid]=pos;
        pthread_mutex_unlock(&g_shard_mx[(opt_overlap&&!opt_rewrite)?0:wid]);
    }
    }
    free(buf);
    return NULL;
}

/* ================= 读者：只读本片稳定前缀，逐字节校验 ================= */
static void* reader(void*p){
    int wid=(int)(intptr_t)p;
    /* overlap 模式：全员同区，读者固定读 0 号区（初值即 f(off)，任意可读） */
    rb_u64 base=(rb_u64)(opt_overlap?0:wid)*SHARD_SZ;
    unsigned seed=0x9000u+(unsigned)wid*77u;
    unsigned char*out=(unsigned char*)malloc(65536);
    if(!out){ atomic_store(&g_stop,1); return NULL; }
    while(!atomic_load(&g_stop)){
        rb_u64 w,span,off; size_t len; int r;
        rb_direct_range ms[160]; int mc=0;
        int mwid = opt_overlap?0:wid;
        pthread_mutex_lock(&g_shard_mx[mwid]);
        w=g_wmark[mwid];
        pthread_mutex_unlock(&g_shard_mx[mwid]);
        if(w<=base+1){ usleep(100); continue; }
        span=(rb_u64)(nrand(&seed)%(unsigned)(w-base-1));
        len=1+(size_t)(nrand(&seed)%65536);
        if(span+len>w-base-1) len=(size_t)(w-base-1-span);
        if(len==0) continue;
        off=base+span;
        r=rb_read(g_st,off,out,len,ms,&mc,160);
        if(r==-2){
            pthread_mutex_lock(&g_disk_mx);
            memcpy(out,g_disk+off,len);
            pthread_mutex_unlock(&g_disk_mx);
        }else{
            serve_miss(out,off,len,ms,mc);
        }
        /* [FIX QA5-8] overlap 模式写者统一持 g_shard_mx[0] 改共享影子区 0，
         * 读者验证必须持同一把锁（mwid）——原持 g_shard_mx[wid] 与数据不匹配，
         * 构成测试器具数据竞争（只误报、不漏报）。 */
        pthread_mutex_lock(&g_shard_mx[mwid]);
        if(memcmp(out,g_shadow+off,len)!=0){
            size_t d; for(d=0;d<len&&out[d]==g_shadow[off+d];d++);
            /* 复读探针：同区间再读一次，区分"瞬态竞态"与"持久错值" */
            static unsigned char probe[65536];
            rb_direct_range pm[160]; int pmc=0; int pr; size_t pd;
            unsigned char diskbyte;
            pr=rb_read(g_st,off,probe,len,pm,&pmc,160);
            if(pr!=-2) serve_miss(probe,off,len,pm,pmc);
            else { pthread_mutex_lock(&g_disk_mx);
                   memcpy(probe,g_disk+off,len);
                   pthread_mutex_unlock(&g_disk_mx); }
            for(pd=0;pd<len&&probe[pd]==g_shadow[off+pd];pd++);
            pthread_mutex_lock(&g_disk_mx);
            diskbyte=g_disk[off+d];
            pthread_mutex_unlock(&g_disk_mx);
            printf("  [FAIL] 读者%d off=%llu(len=%zu) diff@%zu got=%u exp=%u "
                   "W@read=%llu（距水位 %lld）；复读@%zu（%s）；盘面该字节=%u(%s)\n",
                wid,(unsigned long long)off,len,d,
                d<len?out[d]:0,d<len?g_shadow[off+d]:0,
                (unsigned long long)w,(long long)(w-(off+d)),
                pd, pd>=len?"一致=瞬态":"仍不符=持久",
                diskbyte, diskbyte==(d<len?g_shadow[off+d]:0)?"盘面对":"盘面也旧");
            /* [FIX QA5-8] 与入口同一把锁（mwid）：break 前也要正确释放 */
            pthread_mutex_unlock(&g_shard_mx[mwid]);
            atomic_fetch_add(&g_rfail,1);
            atomic_store(&g_stop,1);
            break;
        }
        pthread_mutex_unlock(&g_shard_mx[mwid]);
    }
    free(out);
    return NULL;
}

/* ================= zoned flusher ================= */
typedef struct { int zone; } FArg;
static void* flusher(void*p){
    int zone=((FArg*)p)->zone;
    rb_flushws*ws=rb_flushws_create();
    if(!ws){ atomic_store(&g_stop,1); return NULL; }
    while(!atomic_load(&g_stop)){
        size_t w=rb_stream_flush_zoned(g_st,zone,NFLUSH,ws);
        if(w==0) usleep(200);   /* 无活可干：让出核，避免空转钉核 */
    }
    rb_flushws_free(ws);
    return NULL;
}

/* ================= 单轮 ================= */
static int run_round(int round)
{
    rb_config cfg;
    pthread_t wt[NSHARD], rt[NSHARD], ft[NFLUSH];
    WArg wa[NSHARD]; FArg fa[NFLUSH];
    rb_stats st;
    int i, bad = 0;

    printf("== 轮次 %d/%d（8 写者 + 8 读者 + %d zoned flusher + 2MB 缓存）==\n",
           round, ROUNDS, NFLUSH);

    /* 盘/影子：仅实写前缀需具初值（其余虚存零页，缓存永不触碰） */
    for(i=0;i<NSHARD;i++){
        rb_u64 base=(rb_u64)i*SHARD_SZ; size_t k;
        for(k=0;k<(size_t)WRITE_SZ;k++){
            g_disk[base+k]=pat(base+k);
            g_shadow[base+k]=pat(base+k);
        }
        pthread_mutex_init(&g_shard_mx[i],NULL);
        g_wmark[i]=base;
    }
    atomic_store(&g_stop,0);
    atomic_store(&g_rfail,0);

    memset(&cfg,0,sizeof(cfg));
    cfg.max_bytes=(rb_u64)(opt_bigcache?64u:CACHEMB)*1024*1024;
    cfg.writeback=wb_fn; cfg.readback=rb_fn; cfg.block_size=4096;
    g_eng=rb_engine_create(&cfg);
    g_st=rb_stream_create(g_eng,(rb_u64)(1000+round));
    if(!g_eng||!g_st){ printf("  [FAIL] 引擎/流创建失败\n"); return 1; }

    if(!opt_noflush)
        for(i=0;i<NFLUSH;i++){ fa[i].zone=i; pthread_create(&ft[i],NULL,flusher,&fa[i]); }
    for(i=0;i<NSHARD;i++){ wa[i].wid=i; wa[i].seed=0x1234u+(unsigned)round*999u+(unsigned)i*13u;
                           pthread_create(&wt[i],NULL,writer,&wa[i]); }
    /* [FIX #1] rewrite 模式不建读者：两趟之间的水位复位对读者是瞬态
     * （非数据损坏），终端 memcmp 才是该模式的杀手断言。 */
    if(!opt_rewrite)
        for(i=0;i<NSHARD;i++) pthread_create(&rt[i],NULL,reader,(void*)(intptr_t)i);

    for(i=0;i<NSHARD;i++) pthread_join(wt[i],NULL);
    atomic_store(&g_stop,1);            /* 写净：停读者与回写器循环 */
    /* [FIX #1] rewrite 模式未建读者（水位复位瞬态对读者是噪声）——join 同步守卫 */
    if(!opt_rewrite)
        for(i=0;i<NSHARD;i++) pthread_join(rt[i],NULL);
    rb_stream_flush(g_st,1);            /* serial force（与 zoned 互斥，排空在途） */
    if(!opt_noflush)
        for(i=0;i<NFLUSH;i++) pthread_join(ft[i],NULL);
    rb_stream_flush(g_st,1);            /* 幂等净帧（读者已停，无新脏） */

    /* ---- 终校验 ---- */
    rb_engine_get_stats(g_eng,&st);
    CHECK(st.dirty_bytes==0, "轮%d：force flush 后脏=0（got %llu）",
          round,(unsigned long long)st.dirty_bytes);
    CHECK(st.lost_dirty_bytes==0, "轮%d：lost_dirty=0（got %llu）",
          round,(unsigned long long)st.lost_dirty_bytes);
    { int diskbad=0;
      for(i=0;i<NSHARD;i++){
        rb_u64 base=(rb_u64)i*SHARD_SZ, w;
        pthread_mutex_lock(&g_shard_mx[i]);
        w=g_wmark[i];
        pthread_mutex_unlock(&g_shard_mx[i]);
        if(w<=base) continue;   /* 未写片（overlap 模式仅 0 号区）跳过 */
        if(memcmp(g_disk+base,g_shadow+base,(size_t)(w-base))!=0){
          size_t d; for(d=0;d<(size_t)(w-base)&&g_disk[base+d]==g_shadow[base+d];d++);
          printf("  [FAIL] 轮%d：磁盘!=影子 片%d @+%zu got=%u exp=%u\n",
              round,i,d,d<(size_t)(w-base)?g_disk[base+d]:0,
              d<(size_t)(w-base)?g_shadow[base+d]:0);
          diskbad=1;
        }
      }
      CHECK(diskbad==0, "轮%d：磁盘镜像逐字节一致（各片已发布前缀）",round);
      if(diskbad) bad=1; }
    if(st.dirty_bytes||st.lost_dirty_bytes) bad=1;
    if(atomic_load(&g_rfail)){
        printf("  [FAIL] 轮%d：读者校验失败 %d 次\n",round,(int)atomic_load(&g_rfail));
        bad=1;
    }

    /* chunk_count 归零（销毁后） */
    rb_stream_destroy(g_st); g_st=NULL;
    rb_engine_get_stats(g_eng,&st);
    CHECK(st.chunk_count==0, "轮%d：销毁后 chunk_count=0（got %llu）",
          round,(unsigned long long)st.chunk_count);
    if(st.chunk_count) bad=1;
    rb_engine_destroy(g_eng); g_eng=NULL;

    for(i=0;i<NSHARD;i++) pthread_mutex_destroy(&g_shard_mx[i]);
    return bad;
}

int main(int argc, char **argv)
{
    int round, bad = 0, a;
    for(a=1;a<argc;a++){
        if(!strcmp(argv[a],"noflush")) opt_noflush=1;
        else if(!strcmp(argv[a],"bigcache")) opt_bigcache=1;
        else if(!strcmp(argv[a],"bigwrite")) opt_bigwrite=1;
        else if(!strcmp(argv[a],"p21")) opt_p21=1;
        else if(!strcmp(argv[a],"slowwb")) opt_slowwb=1;
        else if(!strcmp(argv[a],"overlap")) opt_overlap=1;
        else if(!strcmp(argv[a],"mirrorall")) opt_mirrorall=1;
        else if(!strcmp(argv[a],"rewrite")) opt_rewrite=1;
    }
    g_disk=(unsigned char*)malloc((size_t)DISKSZ);
    g_shadow=(unsigned char*)malloc((size_t)DISKSZ);
    if(!g_disk||!g_shadow){ printf("oom\n"); return 1; }
    pthread_mutex_init(&g_disk_mx,NULL);
    atomic_init(&g_stop,0);
    atomic_init(&g_rfail,0);

    for(round=1;round<=ROUNDS;round++) bad += run_round(round);

    pthread_mutex_destroy(&g_disk_mx);
    free(g_disk); free(g_shadow);
    if(bad||g_fail){ printf("\ntest_wsconc: FAIL（轮失败=%d 断言失败=%d）\n",bad,g_fail); return 1; }
    printf("\ntest_wsconc: PASS（%d 轮：并发写+读+zoned 回写+驱逐，镜像/记账/链表全净）\n",ROUNDS);
    return 0;
}
