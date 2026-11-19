/*
 * test_discard_clean.c -- [I48c/I48d 2026-11-10] 丢视图但保脏 回归
 *
 * 真机缺陷（137 号）：attach 热开启 / 切每卷直写路径在 force flush 失败后
 * 无条件 rb_stream_discard_all —— 已 ack 脏块被一并销毁并计入
 * lost_dirty_bytes（e5 实测一次 attach 丢 7,872,512 B）。
 *
 * 修复：core 新增 rb_stream_discard_clean()
 *   ① dirty==0 的 chunk 整块销毁（可从盘重建，不记 lost）；
 *   ② 含脏的 chunk 只清干净子块的 loaded 位（loaded &= dirty），
 *      脏子块的数据与 dirty|loaded 位原样留驻，等 flusher 重试；
 *   ③ view_gen 升代，在途回填失格。
 *
 * 本测试判据：
 *   A 干净块被丢弃：clean chunk 走后 cur_bytes/chunk_count 下降，读为 miss；
 *   B 脏块被保留：dirty_bytes 不归零、lost_dirty_bytes==0、脏子块读命中
 *     且内容正确；
 *   C 同 chunk 混合场景：脏子块命中，干净子块 miss（证明 loaded 位被精确
 *     清除，而不是靠"整块保留"蒙对）；
 *   D 失败/成功两态：注入下层写失败 → flush 后脏仍在且 lost==0；解除注入
 *     再 flush → 脏清零、盘面内容正确、lost 全程 0。
 *
 * 编译：cl /nologo /O2 /W4 /utf-8 /I. test_discard_clean.c cache.c
 */
#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISKSZ   (4u * 1024u * 1024u)
#define BLK      4096u
#define CHUNK    65536u

static unsigned char g_disk[DISKSZ];
static int g_fail_inject;          /* 1 = 下层 writeback 失败 */
static int g_fail;

#define CHECK(c, ...) do { if(!(c)){ printf("  [FAIL] "); printf(__VA_ARGS__); \
                            printf("\n"); g_fail++; } \
                          else { printf("  [ok]   "); printf(__VA_ARGS__); \
                            printf("\n"); } } while(0)

static int wb_fn(void *u, rb_u64 fid, rb_u64 off, const void *b, size_t n)
{
    (void)u; (void)fid;
    if (off + n > DISKSZ) return -1;
    if (g_fail_inject) return -1;
    memcpy(g_disk + off, b, n);
    return 0;
}

static int rb_fn(void *u, rb_u64 fid, rb_u64 off, void *b, size_t n)
{
    (void)u; (void)fid;
    if (off + n > DISKSZ) return -1;
    memcpy(b, g_disk + off, n);
    return 0;
}

static void fill(unsigned char *p, size_t n, unsigned char v)
{
    memset(p, v, n);
}

int main(void)
{
    rb_config cfg;
    rb_engine *eng;
    rb_stream *s;
    rb_stats st;
    rb_direct_range miss[16];
    int mc;
    unsigned char buf[BLK];
    unsigned char out[CHUNK];
    int i;

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes  = 1024u * 1024u;
    cfg.block_size = BLK;
    cfg.writeback  = wb_fn;
    cfg.readback   = rb_fn;
    eng = rb_engine_create(&cfg);
    CHECK(eng != NULL, "engine created");
    if (!eng) return 1;
    s = rb_stream_create(eng, 0x1234);
    CHECK(s != NULL, "stream created");
    if (!s) { rb_engine_destroy(eng); return 1; }

    /* ---- 预置盘面：chunk0 / chunk1 全 0x11 ---- */
    fill(g_disk, 2u * CHUNK, 0x11);

    /* 读入两个 chunk（干净、全 loaded） */
    for (i = 0; i < 2; i++) {
        int hit = rb_read(s, (rb_u64)i * CHUNK, out, CHUNK, miss, &mc, 16);
        if (hit == 0) {                     /* 全 miss：按驱动行为回填 */
            memcpy(out + 0, g_disk + (rb_u64)i * CHUNK, CHUNK);
            rb_read_commit(s, (rb_u64)i * CHUNK, out, CHUNK);
        }
    }
    rb_engine_get_stats(eng, &st);
    CHECK(st.cur_bytes == 2u * CHUNK && st.dirty_bytes == 0,
          "pre: two clean chunks cached (cur=%llu dirty=%llu)",
          (unsigned long long)st.cur_bytes, (unsigned long long)st.dirty_bytes);

    /* chunk0 盘面被外部改写（模拟离场期直写）；chunk1 的 sub1 也被改写 */
    fill(g_disk + 0u * CHUNK, CHUNK, 0x22);
    fill(g_disk + 1u * CHUNK + BLK, BLK, 0x33);

    /* chunk1 的 sub0 写脏（只脏 4K，其余 15 个子块仍是干净已加载） */
    fill(buf, BLK, 0x44);
    {
        rb_direct_range d[16]; int dc = 0;
        int r = rb_write(s, 1u * CHUNK, buf, BLK, d, &dc, 16);
        CHECK(r == 0, "write dirty sub0 of chunk1 (rc=%d)", r);
    }
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == BLK, "pre: dirty=%llu (expect 4096)",
          (unsigned long long)st.dirty_bytes);

    /* ---- 核心动作 ---- */
    rb_stream_discard_clean(s);
    rb_engine_get_stats(eng, &st);
    CHECK(st.lost_dirty_bytes == 0, "A: lost_dirty_bytes == 0 (got %llu)",
          (unsigned long long)st.lost_dirty_bytes);
    CHECK(st.dirty_bytes == BLK, "B: dirty retained 4096 (got %llu)",
          (unsigned long long)st.dirty_bytes);
    CHECK(st.cur_bytes == CHUNK && st.chunk_count == 1,
          "A: clean chunk destroyed, one chunk kept (cur=%llu chunks=%llu)",
          (unsigned long long)st.cur_bytes,
          (unsigned long long)st.chunk_count);

    /* 脏子块：命中且内容 = 0x44 */
    fill(out, BLK, 0xAA);
    CHECK(rb_read(s, 1u * CHUNK, out, BLK, miss, &mc, 16) == 1,
          "B: dirty sub0 read hits cache");
    CHECK(out[0] == 0x44 && out[BLK - 1] == 0x44,
          "B: dirty sub0 content = 0x44 (got 0x%02x)", out[0]);

    /* 同 chunk 干净子块：必须 miss（loaded 已清）——若仍命中说明清理不彻底 */
    fill(out, BLK, 0xAA);
    CHECK(rb_read(s, 1u * CHUNK + BLK, out, BLK, miss, &mc, 16) == 0,
          "C: clean sub1 in dirty chunk is a MISS (stale view dropped)");
    CHECK(out[0] == 0xAA, "C: miss leaves caller buffer untouched (got 0x%02x)",
          out[0]);
    CHECK(g_disk[1u * CHUNK + BLK] == 0x33,
          "C: disk still holds external 0x33 (got 0x%02x)",
          g_disk[1u * CHUNK + BLK]);

    /* 被销毁的 chunk0：读 miss，盘面内容是外部改写的 0x22 */
    fill(out, BLK, 0xAA);
    CHECK(rb_read(s, 0, out, BLK, miss, &mc, 16) == 0,
          "A: dropped clean chunk0 reads MISS");
    CHECK(g_disk[0] == 0x22, "A: chunk0 disk = external 0x22 (got 0x%02x)",
          g_disk[0]);

    /* ---- D: 下层写失败 -> 脏保留；恢复后 flush 收敛 ---- */
    g_fail_inject = 1;
    (void)rb_stream_flush(s, 1);
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == BLK && st.lost_dirty_bytes == 0,
          "D: failed flush keeps dirty, lost stays 0 (dirty=%llu lost=%llu)",
          (unsigned long long)st.dirty_bytes,
          (unsigned long long)st.lost_dirty_bytes);
    rb_stream_discard_clean(s);           /* 再次调用也不得丢脏 */
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == BLK && st.lost_dirty_bytes == 0,
          "D: discard_clean after failed flush still data-safe (dirty=%llu lost=%llu)",
          (unsigned long long)st.dirty_bytes,
          (unsigned long long)st.lost_dirty_bytes);

    g_fail_inject = 0;
    (void)rb_stream_flush(s, 1);
    rb_engine_get_stats(eng, &st);
    CHECK(st.dirty_bytes == 0, "D: flush after recovery clears dirty (got %llu)",
          (unsigned long long)st.dirty_bytes);
    CHECK(st.lost_dirty_bytes == 0, "D: lost_dirty_bytes still 0 (got %llu)",
          (unsigned long long)st.lost_dirty_bytes);
    CHECK(g_disk[1u * CHUNK] == 0x44,
          "D: dirty sub0 landed on disk = 0x44 (got 0x%02x)",
          g_disk[1u * CHUNK]);

    rb_stream_destroy(s);
    rb_engine_destroy(eng);
    printf("RESULT: %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
