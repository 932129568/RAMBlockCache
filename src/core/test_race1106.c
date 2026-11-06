/*
 * test_race1106.c -- 2026-11-06 WeChat incident-shape repro (user-mode core)
 *
 * Incident shape (from driver log timeline):
 *   1. write-back ON, readers+writers active on one stream
 *   2. soft detach ("stream kept", dirty flushed) + reattach  [00:21]
 *   3. PANIC passthrough window (all device writes bypass cache,
 *      stream+chunks RETAINED)                                [00:23-00:25]
 *   4. WeChat DLL load reads wrong bytes via cache            [00:22, 00:50]
 *   5. TRIM (0x2d1400) fired seconds before both failures      [00:17, 00:50]
 *   6. reboot = fresh cache = healed
 *
 * Model:
 *   g_disk       = simulated device (64MB), trim reads back zeros
 *   F region     [0,4MB)     : immutable "DLL" content, readers verify forever
 *   CHURN region [8MB,24MB)  : 4KB blocks, gen-numbered content; shadow g_gen[]
 *   TRIMV region [32MB,40MB): trimmer deletes ranges (device zeroes them)
 *
 * Threads:
 *   4 readers : verify F (try_cache -> rb_read+commit fallback), verify churn
 *               blocks against shadow gen
 *   2 writers : advance churn gens via rb_write_ex (absorbed, write-back ON)
 *   1 passthrough cycler: every ~2s -> flush force; g_passthrough=1 for 150ms
 *               (writers then write g_disk DIRECTLY + advance gen; device
 *               writes bypass the cache while stream/chunks are RETAINED);
 *               g_passthrough=0. Readers must now see NEW gens through the
 *               kept cache -- stale serve here = THE BUG.
 *   1 trimmer : every ~1.7s -> [FIX 2026-11-06] range-targeted TRIM model
 *               (driver parity): view_mutating window ON -> flush force ->
 *               zero ONE 1MB TRIMV range on g_disk -> invalidate ONLY that
 *               range -> window OFF. The old whole-stream discard_all here
 *               was removed; discard_all now belongs to the relocator (mode 4).
 *
 * Verdicts:
 *   f_err>0        -> immutable-region serve corruption (THE core serve bug).
 *   churn_err>0    -> stale/wrong serve observation. In mode 0 (passthrough
 *                     cycler) this is the expected tooth for the kept-stream
 *                     staleness. Mode 3 has NO passthrough cycler; its
 *                     churn_err is a KNOWN timing-sensitive observation:
 *                     2026-11-06 reproduced 2 of 6 runs (4662 / 2909 errors),
 *                     always with f_err=0 and a clean final F sweep = no
 *                     application-visible stale DLL. See KNOWN_ISSUES C7.
 *                     The hard verdict (exit 2) is kept ON PURPOSE - never
 *                     weaken the judge to make a run look green.
 *   all zero @60s  -> core exonerated at this shape; suspect moves to driver layer
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cache.h"

#define DISK_SZ   (64u<<20)
#define F_OFF     0u
#define F_SZ      (4u<<20)
#define CH_OFF    (8u<<20)
#define CH_SZ     (16u<<20)
#define CH_BLKS   (CH_SZ/4096)
#define TR_OFF    (32u<<20)
#define TR_SZ     (8u<<20)

static unsigned char *g_disk;
static volatile LONG  *g_gen;         /* expected generation per churn block */
static volatile LONG   g_passthrough = 0;
static volatile LONG   g_mutating = 0;   /* metadata-hook window (driver parity) */
static volatile LONG   g_relocating = 0; /* F owned by relocator (app-level exclusivity) */
static volatile LONG   g_stop = 0;
static volatile LONG   g_fgen = 0;    /* F region generation (relocator advances) */
static volatile LONG   g_gated = 1;   /* 1 = commit_gen (post-fix), 0 = legacy ungated (pre-fix parity) */
static volatile LONG   g_f_err = 0, g_churn_err = 0, g_reads = 0, g_f_reads = 0;
static rb_engine *g_eng;
static rb_stream *g_st;

static unsigned char pat_f(rb_u64 off, LONG gen)
{ return (unsigned char)(((off * 2654435761u) >> 13) + (unsigned)gen * 37u); }
static unsigned char pat_churn(rb_u32 blk, LONG gen, rb_u64 off_in_blk)
{ return (unsigned char)((blk * 2246822519u + (unsigned)gen * 3266489917u + (unsigned)off_in_blk) >> 11); }

static int rb_fn(void *ctx, rb_u64 fid, rb_u64 off, void *buf, size_t len)
{ (void)ctx; (void)fid; if (off + len > DISK_SZ) return 1; memcpy(buf, g_disk + off, len); return 0; }
static int wb_fn(void *ctx, rb_u64 fid, rb_u64 off, const void *buf, size_t len)
{ (void)ctx; (void)fid; if (off + len > DISK_SZ) return 1; memcpy(g_disk + off, buf, len); return 0; }

/* driver-shaped read: try_cache fast path, fallback rb_read(miss plan)+disk+commit.
   gated=1 captures view gen pre-plan and commits via commit_gen (post-fix parity);
   gated=0 uses the legacy ungated commit (pre-fix parity, for A/B demonstration).
   Mutating-window parity: while the metadata-hook window is open the driver
   passes ALL reads through -- model: direct g_disk read, no cache involvement. */
static int read_like_driver(rb_u64 off, unsigned char *out, size_t len, int gated)
{
    if (g_mutating) { memcpy(out, g_disk + off, len); return 0; }
    if (rb_read_try_cache(g_st, off, out, len) == 1) return 0;
    {   rb_direct_range miss[64]; int nmiss = 0, i;
        unsigned char *tmp;
        rb_u64 vgen = gated ? rb_stream_view_gen(g_st) : 0;
        int rc = rb_read(g_st, off, out, len, miss, &nmiss, 64);
        if (rc == -2 && nmiss > 64) return 1;           /* overflow: give up this round */
        for (i = 0; i < nmiss; i++) {
            tmp = (unsigned char *)malloc(miss[i].len);
            if (!tmp) return 1;
            memcpy(tmp, g_disk + miss[i].offset, miss[i].len);   /* device read */
            if (gated) rb_read_commit_gen(g_st, miss[i].offset, tmp, miss[i].len, vgen);
            else       rb_read_commit(g_st, miss[i].offset, tmp, miss[i].len);
            /* copy the just-read miss range into the user buffer */
            {   size_t o = 0;
                if (miss[i].offset < off) o = (size_t)(off - miss[i].offset);
                if (o < miss[i].len) {
                    size_t cp = miss[i].len - o;
                    if ((size_t)(miss[i].offset + o + cp) > (size_t)(off + len)) cp = (size_t)(off + len) - (size_t)(miss[i].offset + o);
                    memcpy(out + (size_t)(miss[i].offset + o - off), tmp + o, cp);
                }
            }
            free(tmp);
        }
        rb_read_copy_dirty(g_st, off, out, len);        /* overlay dirty (driver parity) */
    }
    return 0;
}

/* F verify with PERSISTENCE discriminator: a mismatch is only counted if a
   re-read (after 1ms) mismatches again -- transient in-flight reads straddling
   a relocation are legal; PERSISTENT stale serve (cache poisoning) is the bug. */
static int verify_f(rb_u64 off, unsigned char *buf)
{
    size_t k; int bad = 0; LONG gen = g_fgen;
    if (read_like_driver(off, buf, 65536, g_gated) == 0) {
        for (k = 0; k < 65536; k++)
            if (buf[k] != pat_f(off + k, gen)) { bad = 1; break; }
        if (bad) {
            Sleep(1);
            bad = 0;
            if (read_like_driver(off, buf, 65536, g_gated) == 0) {
                for (k = 0; k < 65536; k++)
                    if (buf[k] != pat_f(off + k, g_fgen)) { bad = 1; break; }
            }
            if (bad) {
                InterlockedIncrement(&g_f_err);
                if (g_f_err <= 3)
                    printf("  [F-ERR-PERSISTENT] off=%llu+%zu got=%02X want(gen=%ld)=%02X\n",
                           (unsigned long long)off, k, buf[k], g_fgen, pat_f(off + k, g_fgen));
            }
        }
        InterlockedIncrement(&g_f_reads);
    }
    return 0;
}

static DWORD WINAPI reader(LPVOID p)
{ (void)p;
    unsigned char *buf = (unsigned char *)malloc(256 * 1024);   /* PER-THREAD: static buf = threads stomp each other */
    if (!buf) return 1;
    while (!g_stop) {
        /* F verify: 64KB slices (skipped while relocator owns F: app-level exclusivity) */
        if (!g_relocating) {
            rb_u64 off = F_OFF + (rb_u64)(InterlockedIncrement(&g_reads) % (F_SZ / 65536)) * 65536;
            verify_f(off, buf);
        }
        /* churn verify: 8 random blocks; gen-stable check (writer may advance
           between our shadow snapshot and the read -- retry once on gen change,
           only count as error if content matches NEITHER generation) */
        {   int b; for (b = 0; b < 8; b++) {
                rb_u32 blk = (rb_u32)(((rand() << 8) ^ GetCurrentThreadId()) % CH_BLKS);
                rb_u64 off = CH_OFF + (rb_u64)blk * 4096;
                LONG gen = g_gen[blk]; size_t k; int bad = 0;
                if (read_like_driver(off, buf, 4096, g_gated) == 0) {
                    for (k = 0; k < 4096; k++)
                        if (buf[k] != pat_churn(blk, gen, k)) { bad = 1; break; }
                    if (bad) {
                        LONG gen2 = g_gen[blk]; int bad2 = 0;
                        if (gen2 != gen) {          /* gen advanced: re-check against new gen */
                            for (k = 0; k < 4096; k++)
                                if (buf[k] != pat_churn(blk, gen2, k)) { bad2 = 1; break; }
                            if (!bad2) bad = 0;      /* content == new gen: benign ordering */
                        }
                        if (bad) {                   /* matches neither: real wrong-serve */
                            InterlockedIncrement(&g_churn_err);
                            if (g_churn_err <= 8)
                                printf("  [CHURN-ERR] blk=%u gen=%ld got=%02X want=%02X (off=%llu)\n",
                                       blk, gen, buf[k], pat_churn(blk, gen, k),
                                       (unsigned long long)(off + k));
                        }
                    }
                }
            }
        }
        Sleep(1);
    }
    free(buf);
    return 0;
}

static DWORD WINAPI writer(LPVOID p)
{ (void)p;
    static unsigned char blk[4096];
    while (!g_stop) {
        rb_u32 b = (rb_u32)(((rand() << 8) ^ GetCurrentThreadId()) % CH_BLKS);
        LONG g = g_gen[b] + 1; size_t k;
        for (k = 0; k < 4096; k++) blk[k] = pat_churn(b, g, k);
        if (g_passthrough || g_mutating) {
            /* passthrough (panic) / mutating-window: device write bypassing the
               cache -- exactly the driver's passthrough semantics for both */
            memcpy(g_disk + CH_OFF + (rb_u64)b * 4096, blk, 4096);
            g_gen[b] = g;
        } else {
            rb_direct_range dr[8]; int dn = 0;
            if (rb_write_ex(g_st, CH_OFF + (rb_u64)b * 4096, blk, 4096, dr, &dn, 8, NULL) >= 0)
                g_gen[b] = g;   /* absorbed (or direct-listed) */
            else
                g_gen[b] = g;   /* write failed at core level; keep shadow consistent with intent */
        }
        Sleep(2);
    }
    return 0;
}

static DWORD WINAPI passthrough_cycler(LPVOID p)
{ (void)p;
    int cyc = 0;
    while (!g_stop) {
        Sleep(2000);
        if (g_stop) break;
        rb_stream_flush(g_st, 1);           /* soft-detach parity: dirty flushed */
        /* NOTE: chunks + stream RETAINED (the "stream kept" semantic) */
        InterlockedExchange(&g_passthrough, 1);
        Sleep(150);                          /* device writes bypass cache here */
        InterlockedExchange(&g_passthrough, 0);
        printf("  [pt-cycle %d] passthrough window closed (cycle %d, errors so far f=%ld churn=%ld)\n",
               ++cyc, cyc, g_f_err, g_churn_err);
    }
    return 0;
}

static DWORD WINAPI trimmer(LPVOID p)
{ (void)p;
    int n = 0;
    while (!g_stop) {
        Sleep(1700);
        if (g_stop) break;
        /* [FIX 2026-11-06] faithful TRIM model (range-targeted, driver parity):
           window open -> flush dirty -> zero the trimmed range (device effect)
           -> invalidate THAT range only -> window close */
        rb_stream_set_view_mutating(g_st, 1);
        rb_stream_flush(g_st, 1);
        {   rb_u64 off = TR_OFF + (rb_u64)(rand() % (TR_SZ / (1 << 20))) * (1 << 20);
            memset(g_disk + off, 0, 1 << 20);          /* forwarded trim: device zeroes */
            rb_stream_invalidate_range(g_st, off, 1 << 20); }
        rb_stream_set_view_mutating(g_st, 0);
        n++;
    }
    (void)n;
    return 0;
}

/* [FIX 2026-11-06] relocator: models the app-updater sequence that produces the
   incident -- device-level content replacement under F (rename materializes new
   file content; mtime may even be preserved) + the hook's flush+discard_all
   BEFORE the replacement lands. In-flight reads (disk read issued pre-replace)
   that COMMIT after the discard re-poison the view with pre-replace bytes =
   persistent stale serve (the 11-04/11-06 "DLL entry point" family). */
static DWORD WINAPI relocator(LPVOID p)
{ (void)p;
    static unsigned char tmp[65536];
    int cyc = 0;
    while (!g_stop) {
        Sleep(3000);
        if (g_stop) break;
        /* hook parity with app-level exclusivity: F is owned by the updater
           for the whole transition (readers skip verification), window open
           (all cache traffic passthrough), then window closes. */
        g_relocating = 1;
        rb_stream_set_view_mutating(g_st, 1);
        rb_stream_flush(g_st, 1);            /* hook parity: dirty flushed */
        rb_stream_discard_all(g_st);         /* hook parity: view dropped (bumps gen) */
        {   LONG gen = g_fgen + 1;
            rb_u64 off; int s;
            for (s = 0; s < (int)(F_SZ / 65536); s++) {
                off = F_OFF + (rb_u64)s * 65536;
                { size_t k; for (k = 0; k < 65536; k++) tmp[k] = pat_f(off + k, gen); }
                memcpy(g_disk + off, tmp, 65536);   /* rename materializes new content */
            }
            InterlockedExchange(&g_fgen, gen);
        }
        rb_stream_set_view_mutating(g_st, 0);
        g_relocating = 0;
        cyc++;
        printf("  [reloc %d] F generation -> %ld (errors f=%ld churn=%ld)\n",
               cyc, g_fgen, g_f_err, g_churn_err);
    }
    return 0;
}

/* ===== mode 5: dual-stream isolation (the C:+D: co-attach suspicion) =====
 * Two streams over TWO simulated devices (per-file_id dispatch in callbacks),
 * each with its own immutable region, generation, and relocator phase.
 * Any cross-stream serve (R6-01 key-collision class) is caught: reader A must
 * NEVER see pattern-B bytes and vice versa, at ANY generation. */
static unsigned char *g_diskA, *g_diskB;
static volatile LONG  g_fgenA = 0, g_fgenB = 0;
static volatile LONG  g_x_err = 0, g_x_reads = 0;
static volatile LONG  g_reloc_busy = 0;   /* relocators rewriting device: readers skip (torn device state is legal transient) */
static rb_stream *g_stB;

static unsigned char pat_b(rb_u64 off, LONG gen)
{ return (unsigned char)(((off * 40503u) >> 9) + (unsigned)gen * 91u); }

static int rb_fn5(void *ctx, rb_u64 fid, rb_u64 off, void *buf, size_t len)
{ (void)ctx; unsigned char *d = (fid == 424243) ? g_diskB : g_diskA;
  if (off + len > 8u << 20) return 1; memcpy(buf, d + off, len); return 0; }
static int wb_fn5(void *ctx, rb_u64 fid, rb_u64 off, const void *buf, size_t len)
{ (void)ctx; unsigned char *d = (fid == 424243) ? g_diskB : g_diskA;
  if (off + len > 8u << 20) return 1; memcpy(d + off, buf, len); return 0; }

static LONG gen_of_stream(rb_stream *st) { return (st == g_stB) ? g_fgenB : g_fgenA; }

/* per-stream read with device dispatch: like read_like_driver but explicit stream+disk */
static int read_like_driver_st(rb_stream *st, unsigned char *disk,
                               rb_u64 off, unsigned char *out, size_t len)
{
    if (rb_read_try_cache(st, off, out, len) == 1) return 0;
    {   rb_direct_range miss[16]; int nmiss = 0, i;
        unsigned char *tmp;
        /* [FIX] 世代必须在 miss 计划【前】捕获（驱动同序）——提交时才捕获恒通过=门控失效 */
        rb_u64 vgen = rb_stream_view_gen(st);
        int rc = rb_read(st, off, out, len, miss, &nmiss, 16);
        if (rc == -2) return 1;
        for (i = 0; i < nmiss; i++) {
            tmp = (unsigned char *)malloc(miss[i].len);
            if (!tmp) return 1;
            memcpy(tmp, disk + miss[i].offset, miss[i].len);
            rb_read_commit_gen(st, miss[i].offset, tmp, miss[i].len, vgen);
            {   size_t o = 0;
                if (miss[i].offset < off) o = (size_t)(off - miss[i].offset);
                if (o < miss[i].len) {
                    size_t cp = miss[i].len - o;
                    if ((size_t)(miss[i].offset + o + cp) > (size_t)(off + len)) cp = (size_t)(off + len) - (size_t)(miss[i].offset + o);
                    memcpy(out + (size_t)(miss[i].offset + o - off), tmp + o, cp);
                }
            }
            free(tmp);
        }
    }
    return 0;
}

static int verify5(rb_stream *st, unsigned char *disk, unsigned char *out,
                   rb_u64 off, LONG gen,
                   unsigned char (*pat)(rb_u64, LONG), const char *tag)
{
    size_t k; int bad = 0;
    if (read_like_driver_st(st, disk, off, out, 65536) == 0) {
        for (k = 0; k < 65536; k++)
            if (out[k] != pat(off + k, gen)) { bad = 1; break; }
        if (bad) {
            Sleep(1); bad = 0;
            if (read_like_driver_st(st, disk, off, out, 65536) == 0)
                for (k = 0; k < 65536; k++)
                    if (out[k] != pat(off + k, gen_of_stream(st))) { bad = 1; break; }
            if (bad) {
                InterlockedIncrement(&g_x_err);
                if (g_x_err <= 8)
                    printf("  [X-ERR %s] off=%llu+%zu got=%02X want=%02X ownDisk=%02X otherPat=%02X\n",
                           tag, (unsigned long long)(off + k), k, out[k],
                           pat(off + k, gen), disk[off + k],
                           (st == g_stB) ? pat_f(off + k, g_fgenA) : pat_b(off + k, g_fgenB));
            }
        }
        InterlockedIncrement(&g_x_reads);
    }
    return 0;
}

typedef struct { rb_stream *st; unsigned char *disk; volatile LONG *genp;
                 unsigned char (*pat)(rb_u64, LONG); const char *tag; } R5ARG;

static DWORD WINAPI reader5(LPVOID p)
{
    R5ARG *a = (R5ARG *)p;
    unsigned char *buf = (unsigned char *)malloc(256 * 1024);
    if (!buf) return 1;
    while (!g_stop) {
        if (g_reloc_busy) { Sleep(1); continue; }   /* device mid-relocation: torn state legal, skip */
        {   rb_u64 off = (rb_u64)(InterlockedIncrement(&g_x_reads) % 64) * 65536;
            verify5(a->st, a->disk, buf, off, *a->genp, a->pat, a->tag); }
        Sleep(1);
    }
    free(buf);
    return 0;
}

static DWORD WINAPI relocator5(LPVOID p)
{
    R5ARG *a = (R5ARG *)p;
    unsigned char *tmp = (unsigned char *)malloc(65536);   /* PER-THREAD: static tmp = two relocators interleave (3rd harness bug class) */
    int cyc = 0;
    if (!tmp) return 1;
    while (!g_stop) {
        Sleep(2500);
        if (g_stop) break;
        InterlockedIncrement(&g_reloc_busy);
        rb_stream_set_view_mutating(a->st, 1);
        rb_stream_flush(a->st, 1);
        rb_stream_discard_all(a->st);
        {   LONG gen = *a->genp + 1; int s;
            for (s = 0; s < 128; s++) {
                rb_u64 off = (rb_u64)s * 65536;
                size_t k;
                for (k = 0; k < 65536; k++) tmp[k] = a->pat(off + k, gen);
                memcpy(a->disk + off, tmp, 65536);
            }
            InterlockedExchange(a->genp, gen);
        }
        rb_stream_set_view_mutating(a->st, 0);
        InterlockedDecrement(&g_reloc_busy);
        cyc++;
        printf("  [reloc5 %s %d] gen->%ld (x_err=%ld)\n", a->tag, cyc, *a->genp, g_x_err);
    }
    free(tmp);
    return 0;
}

static int run_mode5(void)
{
    HANDLE th[12]; int i, nth = 0;
    static R5ARG aa, ab, ba, bb;
    ULONG64 fa = 0;

    g_diskA = (unsigned char *)VirtualAlloc(NULL, 8u << 20, MEM_COMMIT, PAGE_READWRITE);
    g_diskB = (unsigned char *)VirtualAlloc(NULL, 8u << 20, MEM_COMMIT, PAGE_READWRITE);
    if (!g_diskA || !g_diskB) return 1;
    for (fa = 0; fa < (4u << 20); fa++) { g_diskA[fa] = pat_f(fa, 0); g_diskB[fa] = pat_b(fa, 0); }

    {   rb_config cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.max_bytes = (rb_u64)32 * 1024 * 1024;
        cfg.writeback = wb_fn5; cfg.readback = rb_fn5; cfg.block_size = 4096;
        g_eng = rb_engine_create(&cfg);
        g_st  = rb_stream_create(g_eng, 424242);
        g_stB = rb_stream_create(g_eng, 424243); }
    if (!g_eng || !g_st || !g_stB) return 1;

    aa.st = g_st;  aa.disk = g_diskA; aa.genp = &g_fgenA; aa.pat = pat_f; aa.tag = "A";
    ab.st = g_st;  ab.disk = g_diskA; ab.genp = &g_fgenA; ab.pat = pat_f; ab.tag = "A2";
    ba.st = g_stB; ba.disk = g_diskB; ba.genp = &g_fgenB; ba.pat = pat_b; ba.tag = "B";
    bb.st = g_stB; bb.disk = g_diskB; bb.genp = &g_fgenB; bb.pat = pat_b; bb.tag = "B2";

    printf("== mode5: dual-stream isolation (A on diskA, B on diskB), 30s ==\n");
    th[nth++] = CreateThread(NULL, 0, relocator5, &aa, 0, NULL);   /* A relocator */
    th[nth++] = CreateThread(NULL, 0, relocator5, &ba, 0, NULL);   /* B relocator (independent phase) */
    th[nth++] = CreateThread(NULL, 0, reader5, &aa, 0, NULL);
    th[nth++] = CreateThread(NULL, 0, reader5, &ab, 0, NULL);
    th[nth++] = CreateThread(NULL, 0, reader5, &ba, 0, NULL);
    th[nth++] = CreateThread(NULL, 0, reader5, &bb, 0, NULL);

    Sleep(30000);
    InterlockedExchange(&g_stop, 1);
    Sleep(500);
    for (i = 0; i < nth; i++) WaitForSingleObject(th[i], 3000);
    rb_stream_flush(g_st, 1); rb_stream_flush(g_stB, 1);

    printf("== RESULT mode5: x_err=%ld (reads=%ld) ==\n", g_x_err, g_x_reads);
    printf("VERDICT: %s\n", g_x_err ? "CROSS-STREAM CONTAMINATION REPRODUCED" : "streams fully isolated");
    return g_x_err ? 2 : 0;
}

int main(int argc, char **argv)
{
    HANDLE th[16]; int i, nth = 0;
    ULONG64 f_err0, c_err0;
    int mode = (argc > 1) ? atoi(argv[1]) : 0;   /* 0=full 1=readers-only 2=+writers 3=+trim 4=+relocator 5=dual-stream */
    if (argc > 2) g_gated = atoi(argv[2]);       /* 1=commit_gen (post-fix) 0=legacy ungated (pre-fix parity) */
    if (mode == 5) return run_mode5();

    g_disk = (unsigned char *)VirtualAlloc(NULL, DISK_SZ, MEM_COMMIT, PAGE_READWRITE);
    g_gen  = (volatile LONG *)VirtualAlloc(NULL, CH_BLKS * sizeof(LONG), MEM_COMMIT, PAGE_READWRITE);
    if (!g_disk || !g_gen) { printf("alloc fail\n"); return 1; }

    /* device initial state: F pattern; churn gen=0 content; TRIMV garbage */
    for (i = 0; i < (int)F_SZ; i++)  g_disk[F_OFF + i] = pat_f(i, 0);
    {   rb_u32 b; size_t k;
        for (b = 0; b < CH_BLKS; b++)
            for (k = 0; k < 4096; k++)
                g_disk[CH_OFF + (rb_u64)b * 4096 + k] = pat_churn(b, 0, k); }

    {   rb_config cfg; memset(&cfg, 0, sizeof(cfg));
        cfg.max_bytes = (rb_u64)32 * 1024 * 1024;
        cfg.writeback = wb_fn; cfg.readback = rb_fn; cfg.block_size = 4096;
        g_eng = rb_engine_create(&cfg);
        g_st  = rb_stream_create(g_eng, 424242); }
    if (!g_eng || !g_st) { printf("engine/stream fail\n"); return 1; }

    printf("== test_race1106 mode=%d (4 readers%s%s%s%s) gated=%ld, 30s ==\n",
           mode,
           mode >= 2 ? " + 2 writers" : "",
           (mode == 0 || mode == 3 || mode == 4) ? " + trimmer" : "",
           mode == 0 ? " + passthrough" : "",
           mode >= 4 ? " + relocator" : "",
           g_gated);
    if (mode == 0 || mode == 3 || mode == 4) th[nth++] = CreateThread(NULL, 0, trimmer, NULL, 0, NULL);
    if (mode == 0)                           th[nth++] = CreateThread(NULL, 0, passthrough_cycler, NULL, 0, NULL);
    if (mode >= 4)                           th[nth++] = CreateThread(NULL, 0, relocator, NULL, 0, NULL);
    for (i = 0; i < 4; i++) th[nth++] = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    if (mode >= 2) for (i = 0; i < 2; i++) th[nth++] = CreateThread(NULL, 0, writer, NULL, 0, NULL);

    Sleep(30000);
    f_err0 = g_f_err; c_err0 = g_churn_err;
    InterlockedExchange(&g_stop, 1);
    Sleep(500);
    for (i = 0; i < nth; i++) WaitForSingleObject(th[i], 3000);

    rb_stream_flush(g_st, 1);

    /* final full verification of F through the cache */
    {   static unsigned char buf[256 * 1024]; int bad = 0;
        rb_u64 off; size_t k;
        for (off = F_OFF; off < F_OFF + F_SZ && !bad; off += 65536) {
            if (read_like_driver(off, buf, 65536, g_gated) == 0)
                for (k = 0; k < 65536; k++)
                    if (buf[k] != pat_f(off + k, g_fgen)) { bad = 1; printf("  [FINAL-F] off=%llu\n", (unsigned long long)(off + k)); break; }
        }
        printf("final F sweep: %s\n", bad ? "MISMATCH" : "clean");
    }

    printf("== RESULT: f_err=%lu (reads=%lu) churn_err=%lu ==\n",
           g_f_err, g_reads, g_churn_err);
    (void)f_err0; (void)c_err0;
    if (f_err0 || c_err0) {
        printf("VERDICT: PERSISTENT WRONG-SERVE REPRODUCED\n");
        return 2;
    }
    printf("VERDICT: clean (no persistent stale serve)\n");
    return 0;
}
