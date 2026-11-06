/* V7.1 几何覆盖：同流写→读命中比对，多 block_size。
 * [2026-11-06 扩展，59-64k-audit §5.2] Stage-1 卫生批（F1/F4/F5/F10）配套：
 *   ① run_one(65536/131072)：F1 摘档后创建期回落 4096（legal[] 行为断言）；
 *   ② test_f4_slab：free-stack 字节上限（256MB 预算）各档 cap 断言 + 32K 档
 *      大容量填充→discard_all 收缩后留池 ≤512（旧纯块数上限会到 560，回归即红）；
 *   ③ test_f5_batch：单批块数自适应（4K→16 不变、32K→7）经 grows/nfree 断言；
 *   ④ test_tail(16K/32K)：卷尾非对齐读/写 + truncate 非对齐清尾路径（审计
 *      5.2 #1 / F13 的 core 面；驱动侧 LINK-3 DiskSize 钳位属内核面，由驱动
 *      冒烟覆盖，此处断言 core 等价语义：RMW 合并、清尾零、越界 chunk 销毁）。
 * 统计面：slab 断言经 /DRB_SLAB_TEST_HOOK=1 编译钩子（rb_slab_test_*，生产/
 * 内核构建零符号）；F10 几何 getter 用公开 rb_engine_block_size()。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cache.h"

#if !defined(RB_SLAB_TEST_HOOK) || !RB_SLAB_TEST_HOOK
#error "test_geom requires /DRB_SLAB_TEST_HOOK=1 (slab 断言面；见 _build_all.ps1/_build_asan.ps1/build_all.bat)"
#endif

static unsigned char disk[1u<<23];
static int wb(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t len){
    (void)u;(void)fid; memcpy(disk+off,d,len); return 0; }
static int rb(void *u, rb_u64 fid, rb_u64 off, void *buf, size_t len){
    (void)u;(void)fid; memcpy(buf,disk+off,len); return 0; }
/* F4/F5 专用：无盘模型（数据内容不校验，wb/rb 均空操作）——churn 测试需要
 * ~280MB 偏移空间，用模式函数替代物理盘缓冲。 */
static int wb_nop(void *u, rb_u64 fid, rb_u64 off, const void *d, size_t len){
    (void)u;(void)fid;(void)off;(void)d;(void)len; return 0; }
static int rb_nop(void *u, rb_u64 fid, rb_u64 off, void *buf, size_t len){
    (void)u;(void)fid;(void)off; memset(buf,0,len); return 0; }

static int run_one(rb_u32 bs){
    rb_u32 eff;
    printf("--- bs=%u ---\n",bs); fflush(stdout);
    rb_config c; memset(&c,0,sizeof(c));
    c.max_bytes=64ull*1024*1024; c.writeback=wb; c.readback=rb; c.block_size=bs;
    rb_engine *e=rb_engine_create(&c);
    if(!e){ printf("[FAIL] create bs=%u\n",bs); return 1; }
    /* [F1 2026-11-06] 白名单外档位（65536/131072）创建期回落 4096（cache.c
     * legal[] 与驱动 rbctl.c rb_legal_bs 同步摘除）；合法档位原样生效。 */
    eff=rb_engine_block_size(e);
    if(bs==4096u||bs==8192u||bs==16384u||bs==32768u){
        if(eff!=bs){ printf("[FAIL] bs=%u 合法档回读 block_size=%u\n",bs,eff); return 1; }
    }else{
        if(eff!=4096u){ printf("[FAIL] bs=%u 摘档后应回落 4096，实际=%u\n",bs,eff); return 1; }
        printf("[ok] bs=%u 已摘档：创建回落 4096 档（与 4K 档等价）\n",bs);
    }
    size_t total=256u*1024;
    unsigned char *wr=malloc(total);
    if(!wr){ printf("[FAIL] malloc bs=%u\n",bs); return 1; }
    for(size_t i=0;i<total;i++) wr[i]=(unsigned char)((i*7+bs)&0xff);
    rb_stream *s=rb_stream_create(e,1);
    rb_direct_range dr[64]; int dc=0;
    int rc=rb_write(s,0,wr,total,dr,&dc,64);
    if(rc!=0){ printf("[FAIL] write bs=%u rc=%d dc=%d\n",bs,rc,dc); return 1; }
    /* 同流读回（命中） */
    int bad=0; size_t off=0;
    while(off<total){
        unsigned char rd[8192]; memset(rd,0,sizeof(rd));
        size_t chunk=sizeof(rd); if(off+chunk>total)chunk=total-off;
        rb_direct_range miss[32]; int mc=0;
        rb_read(s,off,rd,chunk,miss,&mc,32);
        if(mc!=0){ printf("[FAIL] bs=%u off=%llu unexpected miss=%d\n",bs,(unsigned long long)off,mc); bad=1; break; }
        if(memcmp(rd,wr+off,chunk)!=0){ bad=1; break; }
        off+=chunk;
    }
    rb_stream_destroy(s);
    rb_stats st; rb_engine_get_stats(e,&st);
    if(bad||st.dirty_bytes!=0){ printf("[FAIL] bs=%u bad=%d dirty=%llu\n",bs,bad,(unsigned long long)st.dirty_bytes); return 1; }
    printf("[ok] bs=%u eff=%u chunk=%u 写后读命中一致 脏=0\n", bs, eff, (unsigned)(eff*16));
    free(wr); rb_engine_destroy(e);
    return 0;
}

/* ---- F4：free-stack 字节上限 ---- */
static int test_f4_slab(void){
    int fail=0;
    printf("--- F4 slab 字节上限 ---\n"); fflush(stdout);
    /* ① 各档 cap = min(2048, 256MB/chunk) 直接断言（chunk=bs*16） */
    {
        static const rb_u32 bss[4]={4096u,8192u,16384u,32768u};
        static const rb_u32 caps[4]={2048u,2048u,1024u,512u};
        size_t z;
        for(z=0;z<4;z++){
            rb_config c; memset(&c,0,sizeof(c));
            c.max_bytes=64ull*1024*1024; c.writeback=wb_nop; c.readback=rb_nop;
            c.block_size=bss[z];
            rb_engine *e=rb_engine_create(&c);
            if(!e){ printf("[FAIL] F4 cap bs=%u create\n",bss[z]); fail=1; continue; }
            if(rb_slab_test_cap(e)!=caps[z]){
                printf("[FAIL] F4 cap bs=%u = %u (期望 %u)\n",
                       bss[z],rb_slab_test_cap(e),caps[z]); fail=1;
            }else printf("[ok] F4 cap bs=%u -> %u 块（预算 256MB / chunk %uKB）\n",
                         bss[z],caps[z],(unsigned)(bss[z]*16u>>10));
            rb_engine_destroy(e);
        }
    }
    /* ② 32K 档（chunk=512KB，cap=512）：填 560 块（280MB，低于 evict_high
     * 不触发淘汰）→ 强刷 → discard_all 全量收缩 → 留池必须 ≤512。
     * 旧纯块数上限（2048）下留池=560 → 本断言即回归红线。
     * 收缩面选 rb_stream_discard_all（生产路径：Enabled/WriteBack 关闭自愈，
     * rbctl.c RbDiscardAllCachedData 同链路）。 */
    {
        const rb_u32 bs=32768u, chunk=bs*16u;   /* 512KB */
        const int nfill=560;                    /* 280MB > 256MB 预算 */
        rb_config c; memset(&c,0,sizeof(c));
        c.max_bytes=384ull*1024*1024; c.writeback=wb_nop; c.readback=rb_nop; c.block_size=bs;
        rb_engine *e=rb_engine_create(&c);
        rb_stream *s; unsigned char *blk; int i; rb_stats st;
        if(!e){ printf("[FAIL] F4 churn create\n"); return 1; }
        s=rb_stream_create(e,9);
        blk=malloc(chunk);
        if(!blk){ printf("[FAIL] F4 malloc\n"); rb_stream_destroy(s); rb_engine_destroy(e); return 1; }
        for(i=0;i<(int)chunk;i++) blk[i]=(unsigned char)(i*31+9);
        for(i=0;i<nfill;i++){
            rb_direct_range dr[8]; int dc=0;
            if(rb_write(s,(rb_u64)i*chunk,blk,chunk,dr,&dc,8)!=0||dc>0){
                printf("[FAIL] F4 fill i=%d rc/dfail\n",i); fail=1; break;
            }
        }
        if(!fail){
            rb_engine_get_stats(e,&st);
            if(st.chunk_count!=(rb_u64)nfill){
                printf("[FAIL] F4 live=%llu (期望 %d)\n",
                       (unsigned long long)st.chunk_count,nfill); fail=1;
            }
            /* 填充期零淘汰 → 池空、批数=560/7=80（同时是 F5 的 32K 档批断言） */
            if(rb_slab_test_nfree(e)!=0u||rb_slab_test_grows(e)!=80u){
                printf("[FAIL] F4 填充后 nfree=%u grows=%u (期望 0/80)\n",
                       rb_slab_test_nfree(e),rb_slab_test_grows(e)); fail=1;
            }
        }
        if(!fail){
            rb_engine_flush(e,1);
            rb_stream_discard_all(s);
            if(rb_slab_test_nfree(e)>512u){
                printf("[FAIL] F4 收缩后留池=%u > 512（字节上限失效）\n",
                       rb_slab_test_nfree(e)); fail=1;
            }else if(rb_slab_test_nfree(e)!=512u){
                printf("[FAIL] F4 收缩后留池=%u (期望恰 512=256MB/512KB)\n",
                       rb_slab_test_nfree(e)); fail=1;
            }else printf("[ok] F4 32K 档 560 块全量收缩后留池=512（≤256MB 预算；旧上限为 560）\n");
            /* 收缩后新建块应从池里复用（grows 不增） */
            { rb_direct_range dr[8]; int dc=0;
              if(rb_write(s,(rb_u64)nfill*chunk,blk,4096,dr,&dc,8)==0&&dc==0){
                  if(rb_slab_test_grows(e)!=80u||rb_slab_test_nfree(e)!=511u){
                      printf("[FAIL] F4 复用后 nfree=%u grows=%u (期望 511/80)\n",
                             rb_slab_test_nfree(e),rb_slab_test_grows(e)); fail=1;
                  }else printf("[ok] F4 收缩后新块自留池复用（nfree 511，grows 不增）\n");
              } }
        }
        free(blk); rb_stream_destroy(s); rb_engine_destroy(e);
    }
    return fail;
}

/* ---- F5：单批块数自适应（4K→16 不变、32K→7）---- */
static int test_f5_batch(void){
    int fail=0;
    printf("--- F5 slab 单批自适应 ---\n"); fflush(stdout);
    {   /* 4K 档：chunk=64KB，batch=min(16,(4MB-64KB)/64KB)=16 不变 */
        const rb_u32 bs=4096u, chunk=bs*16u;
        rb_config c; memset(&c,0,sizeof(c));
        c.max_bytes=64ull*1024*1024; c.writeback=wb_nop; c.readback=rb_nop; c.block_size=bs;
        rb_engine *e=rb_engine_create(&c);
        rb_stream *s; unsigned char *blk; int i;
        if(!e){ printf("[FAIL] F5 4K create\n"); return 1; }
        s=rb_stream_create(e,10); blk=malloc(chunk);
        if(!blk){ printf("[FAIL] F5 malloc\n"); rb_stream_destroy(s); rb_engine_destroy(e); return 1; }
        for(i=0;i<(int)chunk;i++) blk[i]=(unsigned char)(i*17+3);
        for(i=0;i<16;i++){ rb_direct_range dr[8]; int dc=0;
            if(rb_write(s,(rb_u64)i*chunk,blk,chunk,dr,&dc,8)!=0||dc>0){ fail=1; break; } }
        if(!fail&&(rb_slab_test_grows(e)!=1u||rb_slab_test_nfree(e)!=0u)){
            printf("[FAIL] F5 4K 16 块后 grows=%u nfree=%u (期望 1/0)\n",
                   rb_slab_test_grows(e),rb_slab_test_nfree(e)); fail=1;
        }else if(!fail) printf("[ok] F5 4K 档 16 块=1 批（batch=16 不变）\n");
        if(!fail){ rb_direct_range dr[8]; int dc=0;
            if(rb_write(s,16ull*chunk,blk,chunk,dr,&dc,8)==0&&dc==0){
                if(rb_slab_test_grows(e)!=2u||rb_slab_test_nfree(e)!=15u){
                    printf("[FAIL] F5 4K 17 块后 grows=%u nfree=%u (期望 2/15)\n",
                           rb_slab_test_grows(e),rb_slab_test_nfree(e)); fail=1;
                }else printf("[ok] F5 4K 档第 17 块触发第 2 批\n");
            } }
        free(blk); rb_stream_destroy(s); rb_engine_destroy(e);
    }
    {   /* 32K 档：chunk=512KB，batch=min(16,(4MB-64KB)/512KB)=7 */
        const rb_u32 bs=32768u, chunk=bs*16u;
        rb_config c; memset(&c,0,sizeof(c));
        c.max_bytes=64ull*1024*1024; c.writeback=wb_nop; c.readback=rb_nop; c.block_size=bs;
        rb_engine *e=rb_engine_create(&c);
        rb_stream *s; unsigned char *blk; int i;
        if(!e){ printf("[FAIL] F5 32K create\n"); return 1; }
        s=rb_stream_create(e,11); blk=malloc(chunk);
        if(!blk){ printf("[FAIL] F5 malloc\n"); rb_stream_destroy(s); rb_engine_destroy(e); return 1; }
        for(i=0;i<(int)chunk;i++) blk[i]=(unsigned char)(i*19+5);
        for(i=0;i<7;i++){ rb_direct_range dr[8]; int dc=0;
            if(rb_write(s,(rb_u64)i*chunk,blk,chunk,dr,&dc,8)!=0||dc>0){ fail=1; break; } }
        if(!fail&&(rb_slab_test_grows(e)!=1u||rb_slab_test_nfree(e)!=0u)){
            printf("[FAIL] F5 32K 7 块后 grows=%u nfree=%u (期望 1/0)\n",
                   rb_slab_test_grows(e),rb_slab_test_nfree(e)); fail=1;
        }else if(!fail) printf("[ok] F5 32K 档 7 块=1 批（batch=7=(4MB-64KB)/512KB）\n");
        if(!fail){ rb_direct_range dr[8]; int dc=0;
            if(rb_write(s,7ull*chunk,blk,chunk,dr,&dc,8)==0&&dc==0){
                if(rb_slab_test_grows(e)!=2u||rb_slab_test_nfree(e)!=6u){
                    printf("[FAIL] F5 32K 8 块后 grows=%u nfree=%u (期望 2/6)\n",
                           rb_slab_test_grows(e),rb_slab_test_nfree(e)); fail=1;
                }else printf("[ok] F5 32K 档第 8 块触发第 2 批\n");
            } }
        free(blk); rb_stream_destroy(s); rb_engine_destroy(e);
    }
    return fail;
}

/* ---- 卷尾非对齐读/写 + truncate 非对齐清尾（审计 5.2 #1 / F13 core 面） ---- */
static int test_tail(rb_u32 bs){
    const rb_u64 vol=(5ull<<20)+(rb_u64)bs*2u+3333u;   /* 卷长非 bs 整数倍 */
    const rb_u64 nsz=vol-(rb_u64)(bs/2u)-777u;         /* truncate 目标（非对齐） */
    const rb_u64 chunk=(rb_u64)bs*16u;
    rb_u64 beyond=((nsz|((rb_u64)chunk-1))+1);          /* nsz 之后首个 chunk 起点 */
    int bad=0;
    printf("--- tail bs=%u vol=%llu nsz=%llu ---\n",
           bs,(unsigned long long)vol,(unsigned long long)nsz); fflush(stdout);
    memset(disk,0,sizeof(disk));
    rb_config c; memset(&c,0,sizeof(c));
    c.max_bytes=64ull*1024*1024; c.writeback=wb; c.readback=rb; c.block_size=bs;
    rb_engine *e=rb_engine_create(&c);
    if(!e){ printf("[FAIL] tail bs=%u create\n",bs); return 1; }
    unsigned char *wr=malloc((size_t)vol);
    rb_stream *s;
    if(!wr){ printf("[FAIL] tail malloc\n"); rb_engine_destroy(e); return 1; }
    for(rb_u64 i=0;i<vol;i++) wr[(size_t)i]=(unsigned char)((i*13+(rb_u64)bs)&0xff);
    s=rb_stream_create(e,12);
    /* 写 [0,vol)：混合非对齐长度——尾子块部分覆盖走 RMW 补读合并 */
    { rb_u64 pos=0;
      while(pos<vol){
        size_t wl=65536+(size_t)((pos/4096)%3)*4093;
        rb_direct_range dr[64]; int dc=0;
        if((rb_u64)wl>vol-pos) wl=(size_t)(vol-pos);
        if(rb_write(s,pos,wr+(size_t)pos,wl,dr,&dc,64)!=0||dc>0){
            printf("[FAIL] tail bs=%u write pos=%llu dc=%d\n",
                   bs,(unsigned long long)pos,dc); bad=1; break;
        }
        pos+=wl;
      } }
    /* 再写一个 nsz 之后的完整 chunk（truncate 的"整块销毁"分支用） */
    if(!bad){ rb_direct_range dr[64]; int dc=0;
      if(rb_write(s,beyond,wr,(size_t)chunk,dr,&dc,64)!=0||dc>0){
          printf("[FAIL] tail bs=%u write beyond\n",bs); bad=1;
      } }
    /* 读回：非对齐窗口扫描 + 全量顺序，全部命中且内容一致 */
    if(!bad){
        for(rb_u64 off=1;off+1000u<=vol;off+=(rb_u64)bs/2u+1u){
            unsigned char rd[1000]; rb_direct_range miss[32]; int mc=0;
            rb_read(s,off,rd,1000,miss,&mc,32);
            if(mc!=0||memcmp(rd,wr+(size_t)off,1000)!=0){
                printf("[FAIL] tail bs=%u 读 off=%llu mc=%d\n",
                       bs,(unsigned long long)off,mc); bad=1; break;
            }
        }
    }
    if(!bad){
        rb_u64 off=0;
        while(off<vol){
            unsigned char rd[8192]; rb_direct_range miss[32]; int mc=0;
            size_t rc2=sizeof(rd); if(off+(rb_u64)rc2>vol) rc2=(size_t)(vol-off);
            rb_read(s,off,rd,rc2,miss,&mc,32);
            if(mc!=0||memcmp(rd,wr+(size_t)off,rc2)!=0){
                printf("[FAIL] tail bs=%u 顺序读 off=%llu mc=%d\n",
                       bs,(unsigned long long)off,mc); bad=1; break;
            }
            off+=rc2;
        }
    }
    if(!bad) printf("[ok] tail bs=%u 卷尾非对齐写(RMW 合并)+读命中一致\n",bs);
    /* truncate 非对齐：清尾路径（cache.c new_size&(block_size-1) 分支）+
     * 整块销毁（start>=new_size）+ 收尾强刷（脏=0） */
    if(!bad){
        rb_stream_truncate(s,nsz);
        if(rb_stream_dirty_bytes(s)!=0){
            printf("[FAIL] tail bs=%u truncate 后脏=%llu\n",
                   bs,(unsigned long long)rb_stream_dirty_bytes(s)); bad=1;
        }
        /* [0,nsz) 数据不受影响 */
        for(rb_u64 off=0;off<nsz;off+=(rb_u64)bs/2u+1u){
            unsigned char rd[1000]; rb_direct_range miss[32]; int mc=0;
            size_t rc2=1000; if(off+(rb_u64)rc2>nsz) rc2=(size_t)(nsz-off);
            if(rc2==0) break;
            rb_read(s,off,rd,rc2,miss,&mc,32);
            if(mc!=0||memcmp(rd,wr+(size_t)off,rc2)!=0){
                printf("[FAIL] tail bs=%u truncate 后读 off=%llu mc=%d\n",
                       bs,(unsigned long long)off,mc); bad=1; break;
            }
        }
    }
    if(!bad){
        /* 清尾断言：含 nsz 的部分子块保持 loaded、[nsz,子块尾) 被清零 */
        rb_u64 sub_end=(nsz|(rb_u64)(bs-1))+1;
        size_t tl=(size_t)(sub_end-nsz);
        unsigned char *rd=malloc(tl); rb_direct_range miss[32]; int mc=0;
        if(!rd){ printf("[FAIL] tail malloc2\n"); bad=1; }
        else{
            memset(rd,0xAA,tl);
            rb_read(s,nsz,rd,tl,miss,&mc,32);
            if(mc!=0){ printf("[FAIL] tail bs=%u 清尾子块应命中 mc=%d\n",bs,mc); bad=1; }
            else for(size_t q=0;q<tl;q++) if(rd[q]!=0){
                printf("[FAIL] tail bs=%u 清尾字节[%llu]=%u 非零\n",
                       bs,(unsigned long long)nsz+(unsigned long long)q,rd[q]); bad=1; break;
            }
            if(!bad) printf("[ok] tail bs=%u truncate 清尾：[%llu,%llu) 命中且全零\n",
                            bs,(unsigned long long)nsz,(unsigned long long)sub_end);
            free(rd);
        }
    }
    if(!bad){
        /* 整块销毁断言：nsz 之后的 chunk 已销毁 → 读必 miss */
        rb_direct_range miss[32]; int mc=0; unsigned char rd[64];
        rb_read(s,beyond,rd,64,miss,&mc,32);
        if(mc<=0){ printf("[FAIL] tail bs=%u 越界 chunk 应 miss mc=%d\n",bs,mc); bad=1; }
        else printf("[ok] tail bs=%u 越界 chunk 已销毁（读 miss=%d）\n",bs,mc);
    }
    free(wr); rb_stream_destroy(s); rb_engine_destroy(e);
    if(!bad) printf("[ok] tail bs=%u 全部通过\n",bs);
    return bad;
}

int main(void){
    int fail=0;
    fail|=run_one(4096);
    fail|=run_one(8192);
    fail|=run_one(16384);
    fail|=run_one(32768);
    fail|=run_one(65536);    /* [F1] 摘档：回落 4096 档等价 + block_size 回读断言 */
    fail|=run_one(131072);   /* [F1] 同上 */
    fail|=test_f4_slab();
    fail|=test_f5_batch();
    fail|=test_tail(16384);
    fail|=test_tail(32768);
    printf(fail? "几何测试 FAILED\n" : "几何测试 PASS\n");
    return fail;
}
