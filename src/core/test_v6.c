/*
 * RAMBoostFS V6 第二轮验收回归测试（Linux/gcc 可编译运行）
 *
 * 来源：PrimoCache 更新日志映射 + 第二轮隐性 bug 深挖。
 *
 * T1 写放大（PrimoCache v3.0.1 曾修"实际写入 > 请求写入"）：
 *    只写子块 0 与 15，回写总量必须恰好 8KB，不得回写整 64KB。
 * T2 截断一致性（原驱动缺 IRP_MJ_SET_INFORMATION，严重）：
 *    写 256KB 后截断到 64KB / 100KB，回写不得越界、读截断区必须 miss、
 *    非对齐截断尾部零填充。
 * T3 写后释放（移植 PrimoCache"写入后释放相应缓存"）：
 *    回写后缓存块必须释放，再读为 miss。
 * T4 EOF 零语义（RMW 补读在 EOF 之外）：
 *    readback 返回 1（零填充），1KB 写在 100KB 处应整子块缓存，
 *    回写 4KB，用户数据在前、零尾。
 * T5 丢弃全部（删除挂起路径）：discard 后无回写、占用归零。
 */

#include "../core/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISK_SIZE (64u * 1024u * 1024u)
static unsigned char *g_disk;
static int g_fail = 0;

/* 回写探针：总写入字节 + 各调用计数 */
static size_t g_wb_total = 0;
static int    g_wb_calls = 0;

static int wb_fn(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len)
{
    (void)ctx; (void)file_id;
    if (off + len > DISK_SIZE) return -1;
    memcpy(g_disk + off, data, len);
    g_wb_total += len; g_wb_calls++;
    return 0;
}

static int rb_eof_zero(void *ctx, rb_u64 file_id, rb_u64 off, void *buf, size_t len)
{
    (void)ctx; (void)file_id; (void)off;
    memset(buf, 0, len);
    return 0;   /* FIX[R1] 契约对齐：0=成功（零填充也是成功）；非 0 一律按“读失败”走直写兜底 */
}

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); g_fail++; } \
    else { printf("  [ok]   " __VA_ARGS__); printf("\n"); } \
} while (0)

static void fill_pattern(unsigned char *p, size_t n, rb_u64 base)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (unsigned char)((base + i) * 31u + 7u);
}

/* T1: 无写放大 */
static void test_no_write_amplification(void)
{
    printf("== T1: 写放大（只写子块 0/15，回写必须恰好 8KB） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 1);

    char w[4096]; fill_pattern((unsigned char *)w, sizeof(w), 0x31);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 0, w, 4096, direct, &dc, 16);
    dc = 0;
    rb_write(s, 60 * 1024, w, 4096, direct, &dc, 16);
    g_wb_total = 0; g_wb_calls = 0;
    rb_stream_flush(s, 1);
    CHECK(g_wb_total == 8192, "回写总量 = 8KB（实际 %zu）", g_wb_total);
    CHECK(g_wb_calls == 2, "回写 2 次（实际 %d）", g_wb_calls);
    CHECK(memcmp(g_disk, w, 4096) == 0, "子块0 磁盘一致");
    CHECK(memcmp(g_disk + 60 * 1024, w, 4096) == 0, "子块15 磁盘一致");
    CHECK(g_disk[4096] == 0 && g_disk[59 * 1024] == 0, "中间未写子块保持零");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

/* T2: 截断一致性 */
static void test_truncate(void)
{
    printf("== T2: 截断一致性（256KB -> 64KB / 100KB） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 16ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 2);

    static char data[256 * 1024];
    fill_pattern((unsigned char *)data, sizeof(data), 0x77);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 0, data, sizeof(data), direct, &dc, 16);
    CHECK(dc == 0, "256KB 全部入缓存");

    g_wb_total = 0;
    rb_stream_truncate(s, 64 * 1024);
    CHECK(g_wb_total == 64 * 1024, "截断到 64KB：只回写 64KB（实际 %zu）", g_wb_total);

    /* 读截断区：必须 miss */
    char rbuf[4096]; rb_direct_range miss[16]; int mc = 0;
    int r = rb_read(s, 128 * 1024, rbuf, 4096, miss, &mc, 16);
    CHECK(r == 0 && mc == 1, "读 128KB（截断区）为 miss");
    /* 读保留区：命中 */
    mc = 0;
    r = rb_read(s, 0, rbuf, 4096, miss, &mc, 16);
    CHECK(r == 1 && mc == 0, "读 0（保留区）命中");
    CHECK(memcmp(g_disk, data, 64 * 1024) == 0, "磁盘 0..64KB 一致");
    CHECK(g_disk[128 * 1024] == 0, "磁盘 128KB 未被回写（无复活）");

    /* 非对齐截断：256KB -> 100KB */
    dc = 0;
    rb_write(s, 0, data, sizeof(data), direct, &dc, 16);
    g_wb_total = 0;
    rb_stream_truncate(s, 100 * 1024);
    /* 100KB = 25 个 4KB 子块：chunk0 16 个 + chunk1 前 9 个（64..100KB） */
    CHECK(g_wb_total == 100 * 1024, "截断到 100KB：回写 100KB（实际 %zu）", g_wb_total);
    CHECK(memcmp(g_disk, data, 100 * 1024) == 0, "磁盘 0..100KB 数据一致");
    {
        size_t k; int tailZero = 1;
        for (k = 100 * 1024; k < 128 * 1024; k++)
            if (g_disk[k] != 0) { tailZero = 0; break; }
        CHECK(tailZero, "磁盘 100..128KB 尾部零填充");
    }

    /* 真正非对齐：256KB -> 100KB+100B */
    dc = 0;
    rb_write(s, 0, data, sizeof(data), direct, &dc, 16);
    g_wb_total = 0;
    rb_stream_truncate(s, 100 * 1024 + 100);
    /* chunk0 64KB + chunk1 subs0..8 36KB + 边界子块9 4KB = 104KB */
    CHECK(g_wb_total == 104 * 1024, "截断到 100KB+100B：回写 104KB（实际 %zu）", g_wb_total);
    CHECK(memcmp(g_disk, data, 100 * 1024 + 100) == 0, "磁盘 0..100KB+100 数据一致");
    {
        size_t k; int tailZero = 1;
        for (k = 100 * 1024 + 100; k < 128 * 1024; k++)
            if (g_disk[k] != 0) { tailZero = 0; break; }
        CHECK(tailZero, "磁盘 100KB+100..128KB 零填充（含边界子块尾部）");
    }

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

/* T3: 写后释放 */
static void test_write_release(void)
{
    printf("== T3: 写后释放（回写后块释放） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    cfg.write_release = 1;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 3);

    char data[128 * 1024];
    fill_pattern((unsigned char *)data, sizeof(data), 0x99);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 0, data, sizeof(data), direct, &dc, 16);
    rb_stats st;
    rb_engine_get_stats(eng, &st);
    CHECK(st.cur_bytes == 128 * 1024, "回写前占用 128KB");
    rb_stream_flush(s, 1);
    rb_engine_get_stats(eng, &st);
    CHECK(st.cur_bytes == 0, "回写后占用归零（实际 %llu）",
          (unsigned long long)st.cur_bytes);
    char rbuf[4096]; rb_direct_range miss[16]; int mc = 0;
    CHECK(rb_read(s, 0, rbuf, 4096, miss, &mc, 16) == 0 && mc == 1,
          "释放后再读为 miss");

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

/* T4: EOF 零语义 */
static void test_eof_zero(void)
{
    printf("== T4: EOF 零语义（1KB 写在 100KB，整子块缓存） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    cfg.readback = rb_eof_zero;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 4);

    char w[1024]; fill_pattern((unsigned char *)w, sizeof(w), 0x55);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 100 * 1024, w, sizeof(w), direct, &dc, 16);
    CHECK(dc == 0, "1KB 写经 EOF 零语义整子块入缓存（无直写）");
    g_wb_total = 0;
    rb_stream_flush(s, 1);
    CHECK(g_wb_total == 4096, "回写 4KB（实际 %zu）", g_wb_total);
    CHECK(memcmp(g_disk + 100 * 1024, w, 1024) == 0, "前 1KB 为用户数据");
    {
        int tailZero = 1, k;
        for (k = 1024; k < 4096; k++)
            if (g_disk[100 * 1024 + k] != 0) { tailZero = 0; break; }
        CHECK(tailZero, "后 3KB 零填充");
    }

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

/* T5: 丢弃全部 */
static void test_discard_all(void)
{
    printf("== T5: 丢弃全部（删除挂起路径） ==\n");
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.max_bytes = 8ull * 1024 * 1024;
    cfg.writeback = wb_fn;
    rb_engine *eng = rb_engine_create(&cfg);
    rb_stream *s = rb_stream_create(eng, 5);

    char data[128 * 1024];
    fill_pattern((unsigned char *)data, sizeof(data), 0xCC);
    rb_direct_range direct[16]; int dc = 0;
    rb_write(s, 0, data, sizeof(data), direct, &dc, 16);
    g_wb_total = 0;
    rb_stream_discard_all(s);
    rb_stats st; rb_engine_get_stats(eng, &st);
    CHECK(st.cur_bytes == 0, "丢弃后占用归零（实际 %llu）",
          (unsigned long long)st.cur_bytes);
    CHECK(g_wb_total == 0, "丢弃不触发回写（实际 %zu）", g_wb_total);

    rb_stream_destroy(s); rb_engine_destroy(eng);
}

int main(void)
{
    g_disk = (unsigned char *)malloc(DISK_SIZE);
    if (!g_disk) { printf("无法分配模拟磁盘\n"); return 1; }
    memset(g_disk, 0, DISK_SIZE);

    test_no_write_amplification();
    test_truncate();
    test_write_release();
    test_eof_zero();
    test_discard_all();

    printf("\n%s  (失败 %d)\n", g_fail ? "存在失败用例" : "全部用例通过", g_fail);
    free(g_disk);
    return g_fail ? 1 : 0;
}
