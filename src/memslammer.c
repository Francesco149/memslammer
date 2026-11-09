/*
 * memslammer - memory stress tester for overclock validation.
 * SPDX-License-Identifier: MIT
 *
 * Modes:
 *   chain   clean-room reimplementation of the technique described in
 *           docs/karhu.md (chained transform + checksum).
 *   verify  fill a small block, then re-read and compare it many times,
 *           modelled on the first test of TM5's Absolut profile (docs/tm5.md).
 * Every ingredient is switchable so we can measure which ones actually make
 * errors show up fast.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <immintrin.h>
#include <cpuid.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

typedef uint64_t u64;
typedef uint32_t u32;

/* ---------------- config ---------------- */

static struct {
    u64 megabytes;
    int threads;
    u64 block;       /* bytes per work unit (karhu: 4096) */
    int passes;      /* forward passes, same number of reverse (karhu: 4) */
    int flush;       /* clflush(opt) after every block op */
    int yield;       /* Sleep(0)/sched_yield after every block */
    int ops;         /* bitmask of enabled ops: 1 add, 2 sub, 4 xor, 8 not */
    double seconds;  /* 0 = forever */
    int stop_on_error;
    int mode;        /* 0 chain, 1 verify */
    int rereads;     /* verify: compare passes per fill (TM5 T1: 200) */
    int pattern;     /* verify: 0 toggle, 1 mul, 2 random */
    int cache;       /* verify: 0 none, 1 prefetchnta on reads, 2 clflushopt after each pass */
    int order;       /* verify: 0 sequential, 1 shuffled 4 KiB pages on every pass */
    int heat;        /* verify: FP heat iterations after every 4 KiB (random pattern) */
} cfg = { 0, 0, 0, 4, 1, 1, 0xF, 0, 0, 0, 200, 2, 0, 0, 0 };

static const char *pattern_names[] = { "toggle", "mul", "random" };
static const char *cache_names[] = { "none", "nta", "flush" };
static const char *order_names[] = { "seq", "shuffle" };

/* prefetchnta distance for verify reads (TM5 prefetches 8 strides ahead) */
#define NTA_AHEAD 512

/* ---------------- platform ---------------- */

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static void yield_cpu(void)
{
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void *alloc_big(u64 bytes)
{
#ifdef _WIN32
    return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static int cpu_count(void)
{
#ifdef _WIN32
    return (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

static u64 avail_mb(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX ms = { .dwLength = sizeof ms };
    GlobalMemoryStatusEx(&ms);
    return ms.ullAvailPhys >> 20;
#else
    /* MemAvailable counts reclaimable page cache; _SC_AVPHYS_PAGES (MemFree)
     * doesn't, and on Linux the cache often holds GBs. */
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    unsigned long long kb = 0;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    if (f) fclose(f);
    if (kb) return kb >> 10;
    return (u64)sysconf(_SC_AVPHYS_PAGES) * (u64)sysconf(_SC_PAGESIZE) >> 20;
#endif
}

/* ---------------- primitives ---------------- */

static int line_size = 64;
static int have_clflushopt;

__attribute__((target("clflushopt")))
static void flush_opt(void *p, u64 n)
{
    char *c = p;
    _mm_mfence();
    for (u64 i = 0; i < n; i += line_size) _mm_clflushopt(c + i);
    _mm_mfence();
}

static void flush_legacy(void *p, u64 n)
{
    char *c = p;
    _mm_mfence();
    for (u64 i = 0; i < n; i += line_size) _mm_clflush(c + i);
    _mm_mfence();
}

static void flush(void *p, u64 n)
{
    if (!cfg.flush) return;
    if (have_clflushopt) flush_opt(p, n); else flush_legacy(p, n);
}

/* Fletcher-64 over u32 words, mod 2^32-1 (same definition as karhu). */
static u64 fletcher64(const void *p, u64 n)
{
    const u32 *w = p;
    u64 words = n / 4, a = 0, b = 0;
    while (words) {
        u64 chunk = words > 92679 ? 92679 : words;
        words -= chunk;
        for (u64 i = 0; i < chunk; i++) { a += *w++; b += a; }
        a = (a & 0xffffffff) + (a >> 32);
        b = (b & 0xffffffff) + (b >> 32);
    }
    a = (a & 0xffffffff) + (a >> 32);
    b = (b & 0xffffffff) + (b >> 32);
    if (a == 0xffffffff) a = 0;
    if (b == 0xffffffff) b = 0;
    return (b << 32) | a;
}

/* Ops are written as plain loops; -O3 -mavx2 vectorises them to vpaddq etc. */
static void op_add(u64 *restrict x, u64 n, u64 k) { for (u64 i = 0; i < n; i++) x[i] += k; }
static void op_sub(u64 *restrict x, u64 n, u64 k) { for (u64 i = 0; i < n; i++) x[i] -= k; }
static void op_xor(u64 *restrict x, u64 n, u64 k) { for (u64 i = 0; i < n; i++) x[i] ^= k; }
static void op_not(u64 *restrict x, u64 n, u64 k) { (void)k; for (u64 i = 0; i < n; i++) x[i] = ~x[i]; }

typedef void (*opfn)(u64 *restrict, u64, u64);
static opfn fwd_ops[4] = { op_add, op_sub, op_xor, op_not };
static opfn inv_ops[4] = { op_sub, op_add, op_xor, op_not };
static int op_map[4];  /* (key & 3) -> enabled op id */

/* xoshiro256** for the fill; fast, and not tied to an OS crypto RNG. */
typedef struct { u64 s[4]; } rng_t;
static inline u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }
static inline u64 rng_next(rng_t *r)
{
    u64 *s = r->s, res = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = rotl(s[3], 45);
    return res;
}
static u64 splitmix(u64 *x)
{
    u64 z = (*x += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* ---------------- test ---------------- */

static char *arena;
static u64 slice_bytes;
static atomic_int stop_flag;
static atomic_uint_fast64_t total_errors, total_cycles, total_bytes;
static double t_start;
static _Atomic double t_first_error = -1;

typedef struct { int id; } targ_t;

static void report_error(int id, u64 cycle)
{
    double t = now_s() - t_start, expect = -1;
    atomic_compare_exchange_strong(&t_first_error, &expect, t);
    u64 n = atomic_fetch_add(&total_errors, 1) + 1;
    fprintf(stderr, "[%9.3fs] ERROR thread %d cycle %llu (total %llu)\n",
            t, id, (unsigned long long)cycle, (unsigned long long)n);
    if (cfg.stop_on_error) atomic_store(&stop_flag, 1);
}

static void *worker(void *vp)
{
    targ_t *ta = vp;
    char *base = arena + (u64)ta->id * slice_bytes;
    u64 nblocks = slice_bytes / cfg.block, words = cfg.block / 8;
    u64 seed = 0x6d656d736c616d00ull ^ ((u64)ta->id << 32) ^ (u64)(now_s() * 1e9);
    rng_t r;
    for (int i = 0; i < 4; i++) r.s[i] = splitmix(&seed);

    for (u64 cycle = 0; !atomic_load_explicit(&stop_flag, memory_order_relaxed); cycle++) {
        u64 *q = (u64 *)base;
        for (u64 i = 0; i < slice_bytes / 8; i++) q[i] = rng_next(&r);
        flush(base, slice_bytes);
        u64 c0 = fletcher64(base, slice_bytes);

        for (int p = 0; p < cfg.passes; p++) {
            if (atomic_load_explicit(&stop_flag, memory_order_relaxed)) return NULL;
            u64 k = c0;
            for (u64 b = 0; b < nblocks; b++) {
                char *blk = base + b * cfg.block;
                fwd_ops[op_map[k & 3]]((u64 *)blk, words, k);
                flush(blk, cfg.block);
                k = fletcher64(blk, cfg.block);
                if (cfg.yield) yield_cpu();
            }
        }
        for (int p = 0; p < cfg.passes; p++) {
            if (atomic_load_explicit(&stop_flag, memory_order_relaxed)) return NULL;
            u64 k = c0;
            for (u64 b = 0; b < nblocks; b++) {
                char *blk = base + b * cfg.block;
                flush(blk, cfg.block);
                u64 c = fletcher64(blk, cfg.block);
                inv_ops[op_map[k & 3]]((u64 *)blk, words, k);
                k = c;
                if (cfg.yield) yield_cpu();
            }
        }
        flush(base, slice_bytes);
        if (fletcher64(base, slice_bytes) != c0) report_error(ta->id, cycle);
        atomic_fetch_add(&total_cycles, 1);
        /* fill + c0 + passes*(rmw = 2) + passes*(read + rmw = 3) + final read */
        atomic_fetch_add(&total_bytes, slice_bytes * (3 + 5 * (u64)cfg.passes));
    }
    return NULL;
}

/* ---------------- verify mode ---------------- */

typedef __m256i v256;

/* FP heat burst run after every 4 KiB of verify traffic (-H): 8 independent
 * 4-wide double chains of x = (x * m + c) / d. The values converge to a
 * normal fixed point, so there are no denormal or NaN shortcuts. */
static _Atomic double heat_sink;  /* keeps the burst from being optimised away */

static void heat(int iters)
{
    if (iters <= 0) return;
    __m256d x[8], m = _mm256_set1_pd(0.9999), c = _mm256_set1_pd(1.25), d = _mm256_set1_pd(1.0001);
    for (int j = 0; j < 8; j++) x[j] = _mm256_set1_pd(1.0 + j * 0.125);
    for (int i = 0; i < iters; i++)
        for (int j = 0; j < 8; j++)
            x[j] = _mm256_div_pd(_mm256_add_pd(_mm256_mul_pd(x[j], m), c), d);
    __m256d s = x[0];
    for (int j = 1; j < 8; j++) s = _mm256_add_pd(s, x[j]);
    atomic_store_explicit(&heat_sink, _mm256_cvtsd_f64(s), memory_order_relaxed);
}

/* 4 KiB template: within each 64 B line the four 8 B beats alternate x, ~x,
 * so every DQ line toggles on every beat; x's u16 lanes are a base value
 * rotated by lane index plus line index, so the pattern also walks across
 * the byte lanes from line to line. A per-page key is XORed in, which keeps
 * the beat-to-beat complement intact. */
static v256 tmpl[128];

static uint16_t rotl16(uint16_t x, int k) { k &= 15; return (uint16_t)((x << k) | (x >> ((16 - k) & 15))); }

static void build_template(void)
{
    uint16_t *t = (uint16_t *)tmpl;
    for (int line = 0; line < 64; line++)
        for (int beat = 0; beat < 4; beat++)
            for (int lane = 0; lane < 4; lane++) {
                uint16_t x = rotl16(0x5a3c, lane + line);
                t[line * 16 + beat * 4 + lane] = (beat & 1) ? (uint16_t)~x : x;
            }
}

static inline v256 page_key(u64 page)
{
    u64 k = page * 0x9e3779b97f4a7c15ull;
    return _mm256_set1_epi16((short)(k >> 48));
}

/* multiply chain constants, the same values Absolut passes as Param0/Param1 */
static v256 mul0, mul1;

/* Generate (fill) or check (xor-accumulate) one block. pages = block / 4096. */
static void gen_block(v256 *blk, u64 bytes, u64 first_page, rng_t *seed_rng, int check, v256 *acc)
{
    u64 nv = bytes / 32;
    v256 a = _mm256_setzero_si256();
    if (cfg.pattern == 2) {
        rng_t r = *seed_rng;
        for (u64 i = 0; i < nv; i++) {
            v256 v = _mm256_set_epi64x((long long)rng_next(&r), (long long)rng_next(&r),
                                       (long long)rng_next(&r), (long long)rng_next(&r));
            if (check) {
                if (cfg.cache == 1 && !(i & 1)) _mm_prefetch((const char *)(blk + i) + NTA_AHEAD, _MM_HINT_NTA);
                a = _mm256_or_si256(a, _mm256_xor_si256(v, _mm256_load_si256(blk + i)));
            } else _mm256_store_si256(blk + i, v);
            if ((i & 127) == 127) heat(cfg.heat);
        }
    } else {
        for (u64 pg = 0; pg < nv / 128; pg++) {
            v256 key = page_key(first_page + pg), *p = blk + pg * 128;
            if (cfg.pattern == 0) {
                for (int i = 0; i < 128; i++) {
                    v256 v = _mm256_xor_si256(tmpl[i], key);
                    if (check) {
                        if (cfg.cache == 1 && !(i & 1)) _mm_prefetch((const char *)(p + i) + NTA_AHEAD, _MM_HINT_NTA);
                        a = _mm256_or_si256(a, _mm256_xor_si256(v, _mm256_load_si256(p + i)));
                    } else _mm256_store_si256(p + i, v);
                }
            } else {
                v256 st = _mm256_xor_si256(tmpl[0], key);
                for (int i = 0; i < 128; i++) {
                    if (check) {
                        if (cfg.cache == 1 && !(i & 1)) _mm_prefetch((const char *)(p + i) + NTA_AHEAD, _MM_HINT_NTA);
                        a = _mm256_or_si256(a, _mm256_xor_si256(st, _mm256_load_si256(p + i)));
                    } else _mm256_store_si256(p + i, st);
                    st = _mm256_add_epi16(_mm256_mullo_epi16(st, mul0), mul1);
                }
            }
        }
    }
    if (check) *acc = a;
}

/* Random data seeded per 4 KiB page, so pages can be filled and checked in any
 * order. Pass k visits page (k_mul * i + k_add) mod npages for i = 0..npages-1;
 * k_mul is odd and npages a power of two, so that is a permutation. */
static void gen_pages(v256 *blk, u64 npages, u64 bseed, u64 k_mul, u64 k_add, int check, v256 *acc)
{
    v256 a = _mm256_setzero_si256();
    for (u64 i = 0; i < npages; i++) {
        u64 pg = (k_mul * i + k_add) & (npages - 1), s = bseed ^ (pg * 0xd1b54a32d192ed03ull);
        rng_t r;
        for (int j = 0; j < 4; j++) r.s[j] = splitmix(&s);
        v256 *p = blk + pg * 128;
        for (int j = 0; j < 128; j++) {
            v256 v = _mm256_set_epi64x((long long)rng_next(&r), (long long)rng_next(&r),
                                       (long long)rng_next(&r), (long long)rng_next(&r));
            if (check) a = _mm256_or_si256(a, _mm256_xor_si256(v, _mm256_load_si256(p + j)));
            else _mm256_store_si256(p + j, v);
        }
        heat(cfg.heat);
    }
    if (check) *acc = a;
}

static void *worker_verify(void *vp)
{
    targ_t *ta = vp;
    char *base = arena + (u64)ta->id * slice_bytes;
    u64 nblocks = slice_bytes / cfg.block, npages = cfg.block >> 12;
    u64 seed = 0x6d656d736c616d00ull ^ ((u64)ta->id << 32) ^ (u64)(now_s() * 1e9);
    u64 base_page = (u64)ta->id * (slice_bytes >> 12);

    for (u64 cycle = 0; !atomic_load_explicit(&stop_flag, memory_order_relaxed); cycle++) {
        for (u64 b = 0; b < nblocks; b++) {
            v256 *blk = (v256 *)(base + b * cfg.block);
            u64 page = base_page + b * (cfg.block >> 12) + cycle * 0x10001;
            rng_t r;
            for (int i = 0; i < 4; i++) r.s[i] = splitmix(&seed);
            u64 bseed = r.s[0];
            if (cfg.order) gen_pages(blk, npages, bseed, 1, 0, 0, NULL);
            else gen_block(blk, cfg.block, page, &r, 0, NULL);
            for (int k = 0; k < cfg.rereads; k++) {
                if (atomic_load_explicit(&stop_flag, memory_order_relaxed)) return NULL;
                if (cfg.cache == 2) { if (have_clflushopt) flush_opt(blk, cfg.block); else flush_legacy(blk, cfg.block); }
                v256 acc;
                if (cfg.order) gen_pages(blk, npages, bseed, rng_next(&r) | 1, rng_next(&r), 1, &acc);
                else gen_block(blk, cfg.block, page, &r, 1, &acc);
                if (!_mm256_testz_si256(acc, acc)) { report_error(ta->id, cycle); break; }
            }
            atomic_fetch_add(&total_bytes, cfg.block * (1 + (u64)cfg.rereads));
            if (cfg.yield) yield_cpu();
        }
        atomic_fetch_add(&total_cycles, 1);
    }
    return NULL;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: memslammer [options]   (no options: test 90%% of free RAM forever)\n"
        "  -h          show this help\n"
        "  -m MB       memory to test (default 90%% of available)\n"
        "  -t N        threads (default: logical CPUs)\n"
        "  -M MODE     chain (default) or verify\n"
        "  -b BYTES    block size (default: chain 4096, verify 16 MiB)\n"
        "  -p N        chain: forward/reverse passes per cycle (default 4)\n"
        "  -d SECONDS  stop after this long (default: forever)\n"
        "  -o MASK     enabled ops bitmask: 1 add 2 sub 4 xor 8 not (default 15)\n"
        "  -F          chain: disable cache flushing\n"
        "  -r N        verify: compare passes per fill (default 200)\n"
        "  -P PATTERN  verify: random (default), toggle or mul\n"
        "  -C POLICY   verify reads: none (default), flush (clflushopt before each\n"
        "              pass) or nta (prefetchnta; does not bypass L3 on Zen 3)\n"
        "  -A ORDER    verify re-read order: seq (default) or shuffle (4 KiB pages\n"
        "              in a new pseudo-random order every pass; random pattern only)\n"
        "  -H N        verify: N iterations of an AVX FP heat burst after every\n"
        "              4 KiB read or written (random pattern only; default 0)\n"
        "  -Y          disable yield after each block\n"
        "  -s          stop on first error\n");
    exit(2);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || !a[1] || a[2]) usage();
        char c = a[1];
        if (c == 'h') usage();
        if (c == 'F') { cfg.flush = 0; continue; }
        if (c == 'Y') { cfg.yield = 0; continue; }
        if (c == 's') { cfg.stop_on_error = 1; continue; }
        if (i + 1 >= argc) usage();
        char *v = argv[++i];
        switch (c) {
        case 'm': cfg.megabytes = strtoull(v, 0, 0); break;
        case 't': cfg.threads = atoi(v); break;
        case 'b': cfg.block = strtoull(v, 0, 0); break;
        case 'p': cfg.passes = atoi(v); break;
        case 'd': cfg.seconds = atof(v); break;
        case 'o': cfg.ops = (int)strtol(v, 0, 0) & 0xF; break;
        case 'r': cfg.rereads = atoi(v); break;
        case 'H': cfg.heat = atoi(v); break;
        case 'M':
            if (!strcmp(v, "chain")) cfg.mode = 0;
            else if (!strcmp(v, "verify")) cfg.mode = 1;
            else usage();
            break;
        case 'C':
            cfg.cache = -1;
            for (int j = 0; j < 3; j++) if (!strcmp(v, cache_names[j])) cfg.cache = j;
            if (cfg.cache < 0) usage();
            break;
        case 'A':
            cfg.order = -1;
            for (int j = 0; j < 2; j++) if (!strcmp(v, order_names[j])) cfg.order = j;
            if (cfg.order < 0) usage();
            break;
        case 'P':
            cfg.pattern = -1;
            for (int j = 0; j < 3; j++) if (!strcmp(v, pattern_names[j])) cfg.pattern = j;
            if (cfg.pattern < 0) usage();
            break;
        default: usage();
        }
    }
    if (!cfg.threads) cfg.threads = cpu_count();
    if (!cfg.megabytes) cfg.megabytes = avail_mb() * 9 / 10;
    if (!cfg.ops) cfg.ops = 0xF;
    if (!cfg.block) cfg.block = cfg.mode ? 16u << 20 : 4096;
    if (cfg.block < 64 || cfg.block % 64) { fprintf(stderr, "block must be a multiple of 64\n"); return 2; }
    if (cfg.mode && cfg.block % 4096) { fprintf(stderr, "verify block must be a multiple of 4096\n"); return 2; }
    if (cfg.rereads < 1) cfg.rereads = 1;
    if (cfg.heat && cfg.pattern != 2) { fprintf(stderr, "-H needs -P random\n"); return 2; }
    if (cfg.order && (cfg.pattern != 2 || cfg.cache == 1 || (cfg.block & (cfg.block - 1)))) {
        fprintf(stderr, "-A shuffle needs -P random, no -C nta, and a power-of-two block\n");
        return 2;
    }

    /* map key&3 onto enabled ops, cycling through them */
    int en[4], ne = 0;
    for (int i = 0; i < 4; i++) if (cfg.ops & (1 << i)) en[ne++] = i;
    for (int i = 0; i < 4; i++) op_map[i] = en[i % ne];

    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d) && (d & (1u << 19))) line_size = ((b >> 8) & 0xff) * 8;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) have_clflushopt = (b >> 23) & 1;

    slice_bytes = (cfg.megabytes << 20) / cfg.threads / cfg.block * cfg.block;
    u64 total = slice_bytes * cfg.threads;
    arena = alloc_big(total);
    build_template();
    mul0 = _mm256_set1_epi16(0x7777);
    mul1 = _mm256_set1_epi16(0x3333);
    if (!arena) { fprintf(stderr, "allocation of %llu MB failed\n", (unsigned long long)(total >> 20)); return 1; }

    if (cfg.mode)
        printf("memslammer verify: %llu MB, %d threads, block %llu, rereads %d, pattern %s, cache %s, order %s, heat %d, yield %d\n",
               (unsigned long long)(total >> 20), cfg.threads, (unsigned long long)cfg.block,
               cfg.rereads, pattern_names[cfg.pattern], cache_names[cfg.cache], order_names[cfg.order],
               cfg.heat, cfg.yield);
    else
        printf("memslammer chain: %llu MB, %d threads, block %llu, passes %d, flush %s, yield %d, ops 0x%x\n",
               (unsigned long long)(total >> 20), cfg.threads, (unsigned long long)cfg.block,
               cfg.passes, cfg.flush ? (have_clflushopt ? "clflushopt" : "clflush") : "off",
               cfg.yield, cfg.ops);
    fflush(stdout);

    pthread_t *th = calloc(cfg.threads, sizeof *th);
    targ_t *ta = calloc(cfg.threads, sizeof *ta);
    t_start = now_s();
    for (int i = 0; i < cfg.threads; i++) {
        ta[i].id = i;
        pthread_create(&th[i], NULL, cfg.mode ? worker_verify : worker, &ta[i]);
    }

    double last = t_start;
    u64 last_bytes = 0;
    while (!atomic_load(&stop_flag)) {
        sleep_ms(100);
        double t = now_s();
        if (cfg.seconds > 0 && t - t_start >= cfg.seconds) atomic_store(&stop_flag, 1);
        if (t - last >= 10 || atomic_load(&stop_flag)) {
            u64 bytes = atomic_load(&total_bytes);
            printf("[%9.1fs] cycles %llu, coverage %.1f%%, %.2f GB/s, errors %llu\n",
                   t - t_start, (unsigned long long)atomic_load(&total_cycles),
                   100.0 * atomic_load(&total_cycles) / cfg.threads,
                   (bytes - last_bytes) / (t - last) / 1e9,
                   (unsigned long long)atomic_load(&total_errors));
            fflush(stdout);
            last = t; last_bytes = bytes;
        }
    }
    for (int i = 0; i < cfg.threads; i++) pthread_join(th[i], NULL);
    double tfe = atomic_load(&t_first_error);
    printf("RESULT tester=memslammer errors=%llu first_error_s=%.3f elapsed_s=%.3f\n",
           (unsigned long long)atomic_load(&total_errors), tfe, now_s() - t_start);
    return atomic_load(&total_errors) ? 1 : 0;
}
