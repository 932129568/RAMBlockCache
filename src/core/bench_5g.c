/*
 * RAMBoostFS 5GB 量级基准（Linux/gcc 可编译运行）
 *
 * 目的：在 5GB 工作集上量化"裸盘直写/直读"与"RAMBoostFS 缓存"的差距，
 * 检验延迟写入（写吸收）、回写合并、读命中的实际收益。
 *
 * 方法：纯模拟磁盘（只计延迟、不做真实存储）。数据正确性由测试套件
 * （test_v6 等）保证，基准只测时间，因此结果不受宿主真实磁盘/内存污染，
 * 干净、可复现。
 *
 * 磁盘模型（模拟主流笔记本 NVMe，参数可调，见下）：
 *  - 顺序带宽 3.5 GB/s；
 *  - 随机 IO：4K 单次 200us（≈5000 IOPS 单队列），64K 按 1.5GB/s 与 200us 取大；
 *  - 内存命中：纯 memcpy（宿主内存速度）。
 *
 * 口径：这是"核心引擎"在模拟磁盘上的架构收益；PrimoCache 同机对比需在
 * Windows 上用附带的 diskspd 脚本实测（见 docs/12）。
 *
 * 工作集 5GB：
 *  W1 顺序写 5GB（512K 块）
 *  W2 随机 64K 写 2GB（地址在 5GB 内）
 *  W3 随机 4K 写 512MB（地址在 5GB 内）
 *  R1 顺序重读 5GB（热）
 *  R2 随机 64K 读 2GB（热）
 */

#include "../core/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WORKSET (5ull * 1024 * 1024 * 1024)
static unsigned g_cache_mb = 16384u;   /* 默认 16GB（用户机器）；小内存机器可传参 */

#define SEQ_BW       (3.5e9)   /* 顺序 3.5GB/s，字节/秒 */
#define RND_LAT_NS   (200000)  /* 随机 IO 固定延迟 200us */
#define RND64_BW     (1.5e9)   /* 随机 64K 带宽 */

/* ---------------- 时钟 ---------------- */
static rb_u64 now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (rb_u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static void sleep_ns(rb_u64 ns)
{
    struct timespec ts;
    if (ns < 1000) return;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    nanosleep(&ts, NULL);
}

/* ---------------- 模拟磁盘（纯计时） ---------------- */
static rb_u64 g_disk_next_seq;   /* 顺序预期位置 */

static void disk_cost(rb_u64 off, size_t len)
{
    rb_u64 sequential = (off == g_disk_next_seq);
    rb_u64 cost;
    if (sequential) {
        cost = (rb_u64)((double)len / SEQ_BW * 1e9);
    } else {
        rb_u64 lat = RND_LAT_NS;
        rb_u64 bw = (len >= 64 * 1024)
                    ? (rb_u64)((double)len / RND64_BW * 1e9) : 0;
        cost = lat > bw ? lat : bw;
    }
    g_disk_next_seq = off + len;
    sleep_ns(cost);
}

/* 核心回调：写盘 / 读盘（核心 flush 与 RMW 走这里）。纯计时，不做真实存储。 */
static int wb_fn(void *ctx, rb_u64 file_id, rb_u64 off, const void *data, size_t len)
{
    (void)ctx; (void)file_id; (void)data;
    disk_cost(off, len);
    return 0;
}
static int rb_fn(void *ctx, rb_u64 file_id, rb_u64 off, void *buf, size_t len)
{
    (void)ctx; (void)file_id;
    disk_cost(off, len);
    memset(buf, 0, len);
    return 0;
}

/* 直写路径：模拟应用直接写盘 / 读盘 */
static void direct_write(rb_u64 off, const void *buf, size_t len)
{
    (void)buf;
    disk_cost(off, len);
}
static void direct_read(rb_u64 off, void *buf, size_t len)
{
    disk_cost(off, len);
    memset(buf, 0xAB, len);
}

/* ---------------- 随机偏移序列（可复现） ---------------- */
static rb_u64 rng_state = 0x123456789abcdef0ull;
static rb_u64 rnd_u64(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static rb_u64 rnd_off(rb_u64 block)
{
    return (rnd_u64() % (WORKSET / block)) * block;
}

/* ---------------- 辅助：写核心（直写区间在真实驱动里直接落盘，这里计为磁盘成本） ---------------- */
static void core_write(rb_stream *s, rb_u64 off, const void *buf, size_t len)
{
    rb_direct_range direct[64]; int dc = 0;
    int k;
    rb_write(s, off, buf, len, direct, &dc, 64);
    for (k = 0; k < dc; k++)
        disk_cost(direct[k].offset, direct[k].len);
}

static void core_read(rb_stream *s, rb_u64 off, void *buf, size_t len)
{
    rb_direct_range miss[64]; int mc = 0;
    int k;
    rb_read(s, off, buf, len, miss, &mc, 64);
    for (k = 0; k < mc; k++) {
        char *tmp = (char *)malloc(miss[k].len);
        disk_cost(miss[k].offset, miss[k].len);
        memset(tmp, 0, miss[k].len);
        rb_read_commit(s, miss[k].offset, tmp, miss[k].len);
        {
            rb_u64 lo = miss[k].offset > off ? miss[k].offset : off;
            rb_u64 hi = (miss[k].offset + miss[k].len < off + len)
                        ? miss[k].offset + miss[k].len : off + len;
            if (hi > lo)
                memcpy((char *)buf + (lo - off),
                       tmp + (lo - miss[k].offset), hi - lo);
        }
        free(tmp);
    }
}

/* ---------------- 场景 ---------------- */
static void *g_blk;      /* 复用块缓冲 */
static size_t g_blkcap = 512 * 1024;

static rb_u64 run_direct(int mode)
{
    rb_u64 t0 = now_ns();
    rb_u64 i;
    if (mode == 0) {                       /* W1 顺序写 5GB */
        for (i = 0; i < WORKSET; i += 512 * 1024)
            direct_write(i, g_blk, 512 * 1024);
    } else if (mode == 1) {                /* W2 随机 64K 写 2GB */
        for (i = 0; i < 2ull * 1024 * 1024 * 1024; i += 64 * 1024)
            direct_write(rnd_off(64 * 1024), g_blk, 64 * 1024);
    } else if (mode == 2) {                /* W3 随机 4K 写 512MB */
        for (i = 0; i < 512ull * 1024 * 1024; i += 4096)
            direct_write(rnd_off(4096), g_blk, 4096);
    } else if (mode == 3) {                /* R1 顺序读 5GB */
        for (i = 0; i < WORKSET; i += 512 * 1024)
            direct_read(i, g_blk, 512 * 1024);
    } else {                               /* R2 随机 64K 读 2GB */
        for (i = 0; i < 2ull * 1024 * 1024 * 1024; i += 64 * 1024)
            direct_read(rnd_off(64 * 1024), g_blk, 64 * 1024);
    }
    return now_ns() - t0;
}

/* 缓存模式：front=前台完成时间（写吸收），返回值=含后台回写总时间 */
static rb_u64 run_cached(int mode, rb_u64 *front)
{
    rb_config cfg; memset(&cfg, 0, sizeof(cfg));
    rb_engine *eng; rb_stream *s;
    rb_u64 t0 = now_ns(), t1;
    rb_u64 i;

    cfg.max_bytes = (rb_u64)g_cache_mb * 1024 * 1024;
    cfg.writeback = wb_fn;
    cfg.readback = rb_fn;
    eng = rb_engine_create(&cfg);
    s = rb_stream_create(eng, 1);

    if (mode == 0) {
        for (i = 0; i < WORKSET; i += 512 * 1024)
            core_write(s, i, g_blk, 512 * 1024);
    } else if (mode == 1) {
        for (i = 0; i < 2ull * 1024 * 1024 * 1024; i += 64 * 1024)
            core_write(s, rnd_off(64 * 1024), g_blk, 64 * 1024);
    } else if (mode == 2) {
        for (i = 0; i < 512ull * 1024 * 1024; i += 4096)
            core_write(s, rnd_off(4096), g_blk, 4096);
    } else if (mode == 3) {
        for (i = 0; i < WORKSET; i += 512 * 1024)
            core_read(s, i, g_blk, 512 * 1024);
    } else {
        for (i = 0; i < 2ull * 1024 * 1024 * 1024; i += 64 * 1024)
            core_read(s, rnd_off(64 * 1024), g_blk, 64 * 1024);
    }
    t1 = now_ns();
    *front = t1 - t0;

    /* 后台回写（写场景）：把脏数据落盘 */
    if (mode < 3)
        rb_engine_flush(eng, 1);

    rb_stream_destroy(s);
    rb_engine_destroy(eng);
    return now_ns() - t0;
}

static const char *NAMES[5] = {
    "W1 顺序写 5GB", "W2 随机64K写 2GB", "W3 随机4K写 512MB",
    "R1 顺序读 5GB(热)", "R2 随机64K读 2GB(热)"
};

int main(int argc, char **argv)
{
    int m;
    if (argc >= 2) {
        unsigned v = (unsigned)strtoul(argv[1], NULL, 10);
        if (v >= 256) g_cache_mb = v;
    }

    g_blk = malloc(g_blkcap);
    if (!g_blk) { printf("分配失败\n"); return 1; }
    memset(g_blk, 0xAB, g_blkcap);

    printf("RAMBoostFS 5GB 基准（缓存 %uMB；模拟 NVMe：顺序3.5GB/s / 随机4K 200us / 随机64K 1.5GB/s）\n",
           g_cache_mb);
    printf("%-22s %12s %12s %12s %10s\n",
           "场景", "裸盘(ms)", "缓存前台(ms)", "缓存含回写", "前台加速");

    for (m = 0; m < 5; m++) {
        rb_u64 td, tc, tf;
        g_disk_next_seq = 0; rng_state = 0x123456789abcdef0ull;
        td = run_direct(m);
        g_disk_next_seq = 0; rng_state = 0x123456789abcdef0ull;
        tc = run_cached(m, &tf);
        printf("%-22s %12.0f %12.0f %12.0f %9.1fx\n",
               NAMES[m], td / 1e6, tf / 1e6, tc / 1e6,
               (double)td / (double)tf);
    }
    printf("\n说明：\n");
    printf(" - 缓存前台=应用实际等待时间（写吸收/读命中）；缓存含回写=含后台落盘总时间。\n");
    printf(" - 缓存小于数据集时，写满后前台按磁盘速度回写（属正常）；缓存≥数据集时写全吸收。\n");
    printf(" - 写场景含回写时间与裸盘接近（数据最终都要落盘），但应用在前台已完成，\n");
    printf("   编译/解压等突发写不再被磁盘阻塞；读场景热数据由内存服务。\n");
    printf(" - 这是核心引擎在模拟磁盘上的架构收益；PrimoCache 同机对比见 docs/12 的 diskspd 脚本。\n");

    return 0;
}
