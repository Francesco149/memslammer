/*
 * memslammer engine: the test loops. Built with AVX2 enabled (-mavx2 or
 * /arch:AVX2); memslammer.c checks the CPU has AVX2 before any of this runs.
 * SPDX-License-Identifier: MIT
 *
 *   chain   clean-room reimplementation of the technique described in
 *           docs/karhu.md (chained transform + checksum).
 *   verify  fill a block, then re-read and compare it many times, modelled on
 *           the first test of TM5's Absolut profile (docs/tm5.md).
 *
 * Every ingredient is switchable so we can measure which ones make errors
 * show up fast. The defaults are the fastest combination measured so far
 * (docs/results.md).
 */
#include "internal.h"
#include <immintrin.h>
#include <string.h>

/* prefetchnta distance for verify reads (TM5 prefetches 8 strides ahead) */
#define NTA_AHEAD 512

static int line_size = 64;
static int have_clflushopt;

/* ---------------- primitives ---------------- */

MS_TARGET("clflushopt")
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

static void flush_lines(void *p, u64 n)
{
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
static void op_add(u64 *MS_RESTRICT x, u64 n, u64 k) { for (u64 i = 0; i < n; i++) x[i] += k; }
static void op_sub(u64 *MS_RESTRICT x, u64 n, u64 k) { for (u64 i = 0; i < n; i++) x[i] -= k; }
static void op_xor(u64 *MS_RESTRICT x, u64 n, u64 k) { for (u64 i = 0; i < n; i++) x[i] ^= k; }
static void op_not(u64 *MS_RESTRICT x, u64 n, u64 k) { (void)k; for (u64 i = 0; i < n; i++) x[i] = ~x[i]; }

typedef void (*opfn)(u64 *MS_RESTRICT, u64, u64);
static const opfn fwd_ops[4] = { op_add, op_sub, op_xor, op_not };
static const opfn inv_ops[4] = { op_sub, op_add, op_xor, op_not };

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

static u64 worker_seed(int id)
{
    return 0x6d656d736c616d00ull ^ ((u64)id << 32) ^ (u64)(ms__now() * 1e9);
}

/* ---------------- chain mode ---------------- */

void ms__chain_worker(ms_test *t, int id)
{
    const u64 slice = t->slice_bytes, block = t->cfg.block;
    const int passes = t->cfg.passes, flush = t->cfg.flush, yield = t->cfg.yield;
    int op_map[4];
    memcpy(op_map, t->op_map, sizeof op_map);
    char *base = t->arena + (u64)id * slice;
    u64 nblocks = slice / block, words = block / 8;
    u64 seed = worker_seed(id);
    ms_counters *ctr = &t->ctr[id];
    rng_t r;
    for (int i = 0; i < 4; i++) r.s[i] = splitmix(&seed);

    for (u64 cycle = 0; !ld32(&t->stop); cycle++) {
        u64 *q = (u64 *)base;
        for (u64 i = 0; i < slice / 8; i++) q[i] = rng_next(&r);
        if (flush) flush_lines(base, slice);
        u64 c0 = fletcher64(base, slice);

        for (int p = 0; p < passes; p++) {
            if (ld32(&t->stop)) return;
            u64 k = c0;
            for (u64 b = 0; b < nblocks; b++) {
                char *blk = base + b * block;
                fwd_ops[op_map[k & 3]]((u64 *)blk, words, k);
                if (flush) flush_lines(blk, block);
                k = fletcher64(blk, block);
                if (yield) ms__yield();
            }
        }
        for (int p = 0; p < passes; p++) {
            if (ld32(&t->stop)) return;
            u64 k = c0;
            for (u64 b = 0; b < nblocks; b++) {
                char *blk = base + b * block;
                if (flush) flush_lines(blk, block);
                u64 c = fletcher64(blk, block);
                inv_ops[op_map[k & 3]]((u64 *)blk, words, k);
                k = c;
                if (yield) ms__yield();
            }
        }
        if (flush) flush_lines(base, slice);
        if (fletcher64(base, slice) != c0) {
            static const u64 none[4];
            ms__report_error(t, id, cycle, (u64)id * slice, none);
        }
        add64(&ctr->cycles, 1);
        add64(&ctr->blocks, nblocks);
        /* fill + c0 + passes*(rmw = 2) + passes*(read + rmw = 3) + final read */
        add64(&ctr->bytes, slice * (3 + 5 * (u64)passes));
    }
}

/* ---------------- verify mode ---------------- */

typedef __m256i v256;

/* FP heat burst run after every 4 KiB of verify traffic (-H): 8 independent
 * 4-wide double chains of x = (x * m + c) / d. The values converge to a
 * normal fixed point, so there are no denormal or NaN shortcuts. */
static volatile double heat_sink;  /* keeps the burst from being optimised away */

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
    heat_sink = _mm_cvtsd_f64(_mm256_castpd256_pd128(s));
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
static void gen_block(v256 *blk, u64 bytes, u64 first_page, rng_t *seed_rng, int check, v256 *acc,
                      int pattern, int cache, int heat_iters)
{
    u64 nv = bytes / 32;
    v256 a = _mm256_setzero_si256();
    if (pattern == MS_PATTERN_RANDOM) {
        rng_t r = *seed_rng;
        for (u64 i = 0; i < nv; i++) {
            v256 v = _mm256_set_epi64x((long long)rng_next(&r), (long long)rng_next(&r),
                                       (long long)rng_next(&r), (long long)rng_next(&r));
            if (check) {
                if (cache == MS_CACHE_NTA && !(i & 1)) _mm_prefetch((const char *)(blk + i) + NTA_AHEAD, _MM_HINT_NTA);
                a = _mm256_or_si256(a, _mm256_xor_si256(v, _mm256_load_si256(blk + i)));
            } else _mm256_store_si256(blk + i, v);
            if ((i & 127) == 127) heat(heat_iters);
        }
    } else {
        for (u64 pg = 0; pg < nv / 128; pg++) {
            v256 key = page_key(first_page + pg), *p = blk + pg * 128;
            if (pattern == MS_PATTERN_TOGGLE) {
                for (int i = 0; i < 128; i++) {
                    v256 v = _mm256_xor_si256(tmpl[i], key);
                    if (check) {
                        if (cache == MS_CACHE_NTA && !(i & 1)) _mm_prefetch((const char *)(p + i) + NTA_AHEAD, _MM_HINT_NTA);
                        a = _mm256_or_si256(a, _mm256_xor_si256(v, _mm256_load_si256(p + i)));
                    } else _mm256_store_si256(p + i, v);
                }
            } else {
                v256 st = _mm256_xor_si256(tmpl[0], key);
                for (int i = 0; i < 128; i++) {
                    if (check) {
                        if (cache == MS_CACHE_NTA && !(i & 1)) _mm_prefetch((const char *)(p + i) + NTA_AHEAD, _MM_HINT_NTA);
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
static void gen_pages(v256 *blk, u64 npages, u64 bseed, u64 k_mul, u64 k_add, int check, v256 *acc,
                      int heat_iters)
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
        heat(heat_iters);
    }
    if (check) *acc = a;
}

void ms__verify_worker(ms_test *t, int id)
{
    const u64 slice = t->slice_bytes, block = t->cfg.block;
    const int rereads = t->cfg.rereads, pattern = t->cfg.pattern, cache = t->cfg.cache;
    const int order = t->cfg.order, heat_iters = t->cfg.heat, yield = t->cfg.yield;
    char *base = t->arena + (u64)id * slice;
    u64 nblocks = slice / block, npages = block >> 12;
    u64 seed = worker_seed(id);
    u64 base_page = (u64)id * (slice >> 12);
    ms_counters *ctr = &t->ctr[id];

    for (u64 cycle = 0; !ld32(&t->stop); cycle++) {
        for (u64 b = 0; b < nblocks; b++) {
            v256 *blk = (v256 *)(base + b * block);
            u64 page = base_page + b * (block >> 12) + cycle * 0x10001;
            rng_t r;
            for (int i = 0; i < 4; i++) r.s[i] = splitmix(&seed);
            u64 bseed = r.s[0];
            if (order) gen_pages(blk, npages, bseed, 1, 0, 0, NULL, heat_iters);
            else gen_block(blk, block, page, &r, 0, NULL, pattern, cache, heat_iters);
            add64(&ctr->bytes, block);
            for (int k = 0; k < rereads; k++) {
                if (ld32(&t->stop)) return;
                if (cache == MS_CACHE_FLUSH) flush_lines(blk, block);
                v256 acc;
                if (order) gen_pages(blk, npages, bseed, rng_next(&r) | 1, rng_next(&r), 1, &acc, heat_iters);
                else gen_block(blk, block, page, &r, 1, &acc, pattern, cache, heat_iters);
                add64(&ctr->bytes, block);
                if (!_mm256_testz_si256(acc, acc)) {
                    u64 bits[4];
                    _mm256_storeu_si256((v256 *)bits, acc);
                    ms__report_error(t, id, cycle, (u64)((char *)blk - t->arena), bits);
                    break;
                }
            }
            add64(&ctr->blocks, 1);
            if (yield) ms__yield();
        }
        add64(&ctr->cycles, 1);
    }
}

void ms__engine_init(int ls, int clflushopt)
{
    line_size = ls;
    have_clflushopt = clflushopt;
    build_template();
    mul0 = _mm256_set1_epi16(0x7777);
    mul1 = _mm256_set1_epi16(0x3333);
}
