/*
 * memslammer core: configuration, threads, memory, status and error log.
 * Plain portable C; the AVX2 test loops are in engine.c.
 * SPDX-License-Identifier: MIT
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MS_MSVC
#include <immintrin.h>  /* _xgetbv */
#elif !defined(_WIN32)
#include <cpuid.h>
#endif
#if !defined(MS_MSVC) && defined(_WIN32)
#include <cpuid.h>
#endif

#ifndef _WIN32
#include <sys/mman.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

#ifndef MS_VERSION
#define MS_VERSION "dev"
#endif

const char *ms_version(void) { return MS_VERSION; }

/* ---------------- platform ---------------- */

double ms__now(void)
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

void ms__yield(void)
{
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static void *alloc_big(u64 bytes)
{
#ifdef _WIN32
    return VirtualAlloc(NULL, (SIZE_T)bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void free_big(void *p, u64 bytes)
{
    if (!p) return;
#ifdef _WIN32
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, bytes);
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

#ifndef _WIN32
static u64 meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    size_t kl = strlen(key);
    unsigned long long kb = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ':') { kb = strtoull(line + kl + 1, NULL, 10); break; }
    if (f) fclose(f);
    return kb;
}
#endif

static u64 avail_mb(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    return ms.ullAvailPhys >> 20;
#else
    /* MemAvailable counts reclaimable page cache; _SC_AVPHYS_PAGES (MemFree)
     * doesn't, and on Linux the cache often holds GBs. */
    u64 kb = meminfo_kb("MemAvailable");
    if (kb) return kb >> 10;
    return (u64)sysconf(_SC_AVPHYS_PAGES) * (u64)sysconf(_SC_PAGESIZE) >> 20;
#endif
}

static u64 total_mb(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    return ms.ullTotalPhys >> 20;
#else
    return (u64)sysconf(_SC_PHYS_PAGES) * (u64)sysconf(_SC_PAGESIZE) >> 20;
#endif
}

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
#ifdef MS_MSVC
    int x[4];
    __cpuidex(x, (int)leaf, (int)sub);
    for (int i = 0; i < 4; i++) r[i] = (unsigned)x[i];
#else
    r[0] = r[1] = r[2] = r[3] = 0;
    if (__get_cpuid_max(leaf & 0x80000000u, NULL) >= leaf)
        __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}

static u64 xgetbv0(void)
{
#ifdef MS_MSVC
    return _xgetbv(0);
#else
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((u64)hi << 32) | lo;
#endif
}

static struct { int init, avx2, line_size, clflushopt; char brand[49]; } cpu;

static void cpu_detect(void)
{
    if (cpu.init) return;
    unsigned r[4];
    cpuid(0, 0, r);
    unsigned max = r[0];
    cpuid(1, 0, r);
    int osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    cpu.line_size = (r[3] & (1u << 19)) ? (int)((r[1] >> 8) & 0xff) * 8 : 64;
    if (cpu.line_size <= 0) cpu.line_size = 64;
    int os_ymm = osxsave && avx && (xgetbv0() & 6) == 6;
    if (max >= 7) {
        cpuid(7, 0, r);
        cpu.avx2 = os_ymm && ((r[1] >> 5) & 1);
        cpu.clflushopt = (r[1] >> 23) & 1;
    }
    cpuid(0x80000000u, 0, r);
    if (r[0] >= 0x80000004u) {
        unsigned *b = (unsigned *)cpu.brand;
        for (unsigned i = 0; i < 3; i++) cpuid(0x80000002u + i, 0, b + 4 * i);
        cpu.brand[48] = 0;
    }
    cpu.init = 1;
}

/* threads and locks */
#ifdef _WIN32
#define THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
#define THREAD_RETURN return 0
static void mutex_init(ms_mutex *m) { InitializeSRWLock(m); }
static void mutex_destroy(ms_mutex *m) { (void)m; }
static void mutex_lock(ms_mutex *m) { AcquireSRWLockExclusive(m); }
static void mutex_unlock(ms_mutex *m) { ReleaseSRWLockExclusive(m); }
static int thread_start(ms_thread *th, LPTHREAD_START_ROUTINE fn, void *arg)
{
    *th = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *th ? 0 : -1;
}
static void thread_join(ms_thread th) { WaitForSingleObject(th, INFINITE); CloseHandle(th); }
#else
#define THREAD_FN(name) static void *name(void *arg)
#define THREAD_RETURN return NULL
static void mutex_init(ms_mutex *m) { pthread_mutex_init(m, NULL); }
static void mutex_destroy(ms_mutex *m) { pthread_mutex_destroy(m); }
static void mutex_lock(ms_mutex *m) { pthread_mutex_lock(m); }
static void mutex_unlock(ms_mutex *m) { pthread_mutex_unlock(m); }
static int thread_start(ms_thread *th, void *(*fn)(void *), void *arg)
{
    return pthread_create(th, NULL, fn, arg) ? -1 : 0;
}
static void thread_join(ms_thread th) { pthread_join(th, NULL); }
#endif

/* ---------------- config ---------------- */

static const char *mode_names[] = { "chain", "verify" };
static const char *pattern_names[] = { "toggle", "mul", "random", "kpage" };
static const char *cache_names[] = { "none", "nta", "flush" };
static const char *order_names[] = { "seq", "shuffle" };

void ms_config_init(ms_config *c)
{
    memset(c, 0, sizeof *c);
    c->mode = MS_MODE_VERIFY;
    c->yield = 1;
    c->passes = 4;
    c->flush = 1;
    c->ops = 0xF;
    c->rereads = 20;
    c->pattern = MS_PATTERN_RANDOM;
    c->cache = MS_CACHE_NONE;
    c->order = MS_ORDER_SEQ;
    c->late = 1;
}

const char *ms_config_help(void)
{
    return
        "  -m MB       memory to test (default 90% of available)\n"
        "  -t N        threads (default: logical CPUs)\n"
        "  -M MODE     verify (default) or chain\n"
        "  -d SECONDS  stop after this long (default: until stopped)\n"
        "  -s          stop on first error\n"
        "  -b BYTES    block size (default: verify 16 MiB, chain 4096)\n"
        "  -Y          disable yield after each block\n"
        "  -r N        verify: compare passes per fill (default 20)\n"
        "  -P PATTERN  verify: random (default), toggle, mul or kpage (experimental)\n"
        "  -C POLICY   verify reads: none (default), flush (clflushopt before each\n"
        "              pass) or nta (prefetchnta; does not bypass L3 on Zen 3)\n"
        "  -A ORDER    verify re-read order: seq (default) or shuffle (4 KiB pages\n"
        "              in a new pseudo-random order every pass; random pattern only)\n"
        "  -H N        verify: N iterations of an AVX FP heat burst after every\n"
        "              4 KiB read or written (random pattern only; default 0)\n"
        "  -L          verify: skip the late check (each block is otherwise checked\n"
        "              once more just before its next fill, a whole cycle later)\n"
        "  -N          verify: fill with non-temporal stores (experimental)\n"
        "  -p N        chain: forward/reverse passes per cycle (default 4)\n"
        "  -o MASK     chain: enabled ops bitmask: 1 add 2 sub 4 xor 8 not (default 15)\n"
        "  -F          chain: disable cache flushing\n";
}

static int pick(const char *v, const char **names, int n)
{
    for (int i = 0; i < n; i++) if (!strcmp(v, names[i])) return i;
    return -1;
}

static int parse_u64(const char *v, u64 *out)
{
    char *end;
    if (!*v || *v == '-') return -1;
    unsigned long long x = strtoull(v, &end, 0);
    if (*end) return -1;
    *out = x;
    return 0;
}

static int parse_int(const char *v, int *out)
{
    u64 x;
    if (parse_u64(v, &x) || x > 0x7fffffff) return -1;
    *out = (int)x;
    return 0;
}

int ms_config_parse(ms_config *c, int argc, char **argv, char *err, size_t errlen)
{
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1] || a[2]) { snprintf(err, errlen, "unknown option '%s'", a); return -1; }
        char f = a[1];
        if (f == 'h') return 1;
        if (f == 'F') { c->flush = 0; continue; }
        if (f == 'Y') { c->yield = 0; continue; }
        if (f == 's') { c->stop_on_error = 1; continue; }
        if (f == 'L') { c->late = 0; continue; }
        if (f == 'N') { c->nt_fill = 1; continue; }
        if (!strchr("mtbpdorHMCAP", f)) { snprintf(err, errlen, "unknown option '%s'", a); return -1; }
        if (i + 1 >= argc) { snprintf(err, errlen, "%s needs a value", a); return -1; }
        const char *v = argv[++i];
        int bad = 0, k;
        u64 x = 0;
        switch (f) {
        case 'm': bad = parse_u64(v, &c->megabytes); break;
        case 't': bad = parse_int(v, &c->threads); break;
        case 'b': bad = parse_u64(v, &c->block); break;
        case 'p': bad = parse_int(v, &c->passes); break;
        case 'r': bad = parse_int(v, &c->rereads); break;
        case 'H': bad = parse_int(v, &c->heat); break;
        case 'o': bad = parse_u64(v, &x); c->ops = (int)(x & 0xF); break;
        case 'd': {
            char *end;
            c->seconds = strtod(v, &end);
            bad = !*v || *end || c->seconds < 0;
            break;
        }
        case 'M': bad = (k = pick(v, mode_names, 2)) < 0; if (!bad) c->mode = k; break;
        case 'P': bad = (k = pick(v, pattern_names, 4)) < 0; if (!bad) c->pattern = k; break;
        case 'C': bad = (k = pick(v, cache_names, 3)) < 0; if (!bad) c->cache = k; break;
        case 'A': bad = (k = pick(v, order_names, 2)) < 0; if (!bad) c->order = k; break;
        }
        if (bad) { snprintf(err, errlen, "bad value '%s' for %s", v, a); return -1; }
    }
    return 0;
}

static u64 slice_bytes_of(const ms_config *c)
{
    return (c->megabytes << 20) / (u64)c->threads / c->block * c->block;
}

int ms_config_resolve(ms_config *c, char *err, size_t errlen)
{
    if (c->mode != MS_MODE_CHAIN && c->mode != MS_MODE_VERIFY) { snprintf(err, errlen, "bad mode"); return -1; }
    if (c->pattern < 0 || c->pattern > 3 || c->cache < 0 || c->cache > 2 || c->order < 0 || c->order > 1) {
        snprintf(err, errlen, "bad pattern, cache or order setting");
        return -1;
    }
    if (!c->threads) c->threads = cpu_count();
    if (c->threads < 1 || c->threads > 4096) { snprintf(err, errlen, "bad thread count %d", c->threads); return -1; }
    if (!c->megabytes) c->megabytes = avail_mb() * 9 / 10;
    u64 tot = total_mb();
    if (tot && c->megabytes > tot) {
        snprintf(err, errlen, "%llu MB is more than this machine's %llu MB of RAM",
                 (unsigned long long)c->megabytes, (unsigned long long)tot);
        return -1;
    }
    c->ops &= 0xF;
    if (!c->ops) c->ops = 0xF;
    if (!c->block) c->block = c->mode == MS_MODE_VERIFY ? 16u << 20 : 4096;
    if (c->block < 64 || c->block % 64) { snprintf(err, errlen, "block must be a multiple of 64"); return -1; }
    if (c->mode == MS_MODE_VERIFY && c->block % 4096) { snprintf(err, errlen, "verify block must be a multiple of 4096"); return -1; }
    if (c->passes < 1) c->passes = 1;
    if (c->rereads < 1) c->rereads = 1;
    if (c->heat < 0) c->heat = 0;
    if (c->heat && c->pattern != MS_PATTERN_RANDOM) { snprintf(err, errlen, "-H needs -P random"); return -1; }
    if (c->order && (c->pattern != MS_PATTERN_RANDOM || c->cache == MS_CACHE_NTA || (c->block & (c->block - 1)))) {
        snprintf(err, errlen, "-A shuffle needs -P random, no -C nta, and a power-of-two block");
        return -1;
    }
    if (!slice_bytes_of(c)) {
        snprintf(err, errlen, "%llu MB is too little for %d threads with %llu-byte blocks",
                 (unsigned long long)c->megabytes, c->threads, (unsigned long long)c->block);
        return -1;
    }
    return 0;
}

void ms_config_describe(const ms_config *c, char *buf, size_t n)
{
    cpu_detect();
    unsigned long long mb = (slice_bytes_of(c) * (u64)c->threads) >> 20;
    if (c->mode == MS_MODE_VERIFY)
        snprintf(buf, n, "memslammer verify: %llu MB, %d threads, block %llu, rereads %d, pattern %s, cache %s, order %s, heat %d, yield %d, late %d, fill %s",
                 mb, c->threads, (unsigned long long)c->block, c->rereads, pattern_names[c->pattern],
                 cache_names[c->cache], order_names[c->order], c->heat, c->yield, !!c->late,
                 c->nt_fill ? "nt" : "store");
    else
        snprintf(buf, n, "memslammer chain: %llu MB, %d threads, block %llu, passes %d, flush %s, yield %d, ops 0x%x",
                 mb, c->threads, (unsigned long long)c->block, c->passes,
                 c->flush ? (cpu.clflushopt ? "clflushopt" : "clflush") : "off", c->yield, c->ops);
}

void ms_config_to_args(const ms_config *c, char *buf, size_t n)
{
    ms_config d;
    ms_config_init(&d);
    size_t len = 0;
    buf[0] = 0;
#define ADD(...) do { if (len < n) len += snprintf(buf + len, n - len, __VA_ARGS__); } while (0)
    if (c->mode != d.mode) ADD(" -M %s", mode_names[c->mode]);
    if (c->megabytes) ADD(" -m %llu", (unsigned long long)c->megabytes);
    if (c->threads) ADD(" -t %d", c->threads);
    if (c->seconds > 0) ADD(" -d %g", c->seconds);
    if (c->stop_on_error) ADD(" -s");
    if (c->block && c->block != (c->mode == MS_MODE_VERIFY ? 16u << 20 : 4096u)) ADD(" -b %llu", (unsigned long long)c->block);
    if (!c->yield) ADD(" -Y");
    if (c->mode == MS_MODE_VERIFY) {
        if (c->rereads != d.rereads) ADD(" -r %d", c->rereads);
        if (c->pattern != d.pattern) ADD(" -P %s", pattern_names[c->pattern]);
        if (c->cache != d.cache) ADD(" -C %s", cache_names[c->cache]);
        if (c->order != d.order) ADD(" -A %s", order_names[c->order]);
        if (c->heat) ADD(" -H %d", c->heat);
        if (!c->late) ADD(" -L");
        if (c->nt_fill) ADD(" -N");
    } else {
        if (c->passes != d.passes) ADD(" -p %d", c->passes);
        if ((c->ops & 0xF) != 0xF && c->ops) ADD(" -o %d", c->ops);
        if (!c->flush) ADD(" -F");
    }
#undef ADD
    if (buf[0] == ' ') memmove(buf, buf + 1, strlen(buf));
}

/* ---------------- test lifecycle ---------------- */

void ms__report_error(ms_test *t, int thread, u64 cycle, u64 offset, const u64 bits[4], int pass, int recheck)
{
    double now = ms__now() - t->t_start;
    mutex_lock(&t->lock);
    if (t->first_error < 0) t->first_error = now;
    ms_error *e = &t->ring[t->n_errors % MS_ERRORS_KEPT];
    e->index = t->n_errors++;
    e->time = now;
    e->thread = thread;
    e->cycle = cycle;
    e->offset = offset;
    memcpy(e->bits, bits, sizeof e->bits);
    e->pass = pass;
    e->recheck = recheck;
    mutex_unlock(&t->lock);
    add64(&t->ctr[thread].errors, 1);
    if (t->cfg.stop_on_error) st32(&t->stop, 1);
}

THREAD_FN(worker_main)
{
    ms_counters *c = arg;
    if (c->owner->cfg.mode == MS_MODE_VERIFY) ms__verify_worker(c->owner, c->id);
    else ms__chain_worker(c->owner, c->id);
    THREAD_RETURN;
}

static void fail(ms_test *t, const char *msg)
{
    mutex_lock(&t->lock);
    snprintf(t->message, sizeof t->message, "%s", msg);
    mutex_unlock(&t->lock);
    st32(&t->state, MS_STATE_FAILED);
}

THREAD_FN(controller)
{
    ms_test *t = arg;
    int n = t->cfg.threads, started = 0;
#ifdef _WIN32
    /* keep the machine awake for the duration of the test */
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
#endif
    t->arena = alloc_big(t->total_bytes);
    if (!t->arena) {
        char msg[96];
        snprintf(msg, sizeof msg, "allocation of %llu MB failed", (unsigned long long)(t->total_bytes >> 20));
        fail(t, msg);
        goto out;
    }
    t->t_start = ms__now();
    for (; started < n && !ld32(&t->stop); started++)
        if (thread_start(&t->workers[started], worker_main, &t->ctr[started])) break;
    if (started < n && !ld32(&t->stop)) {
        st32(&t->stop, 1);
        for (int i = 0; i < started; i++) thread_join(t->workers[i]);
        free_big(t->arena, t->total_bytes);
        t->arena = NULL;
        fail(t, "could not start worker threads");
        goto out;
    }
    st32(&t->state, MS_STATE_RUNNING);
    while (!ld32(&t->stop)) {
        sleep_ms(50);
        if (t->cfg.seconds > 0 && ms__now() - t->t_start >= t->cfg.seconds) st32(&t->stop, 1);
    }
    st32(&t->state, MS_STATE_STOPPING);
    for (int i = 0; i < started; i++) thread_join(t->workers[i]);
    t->t_end = ms__now();
    free_big(t->arena, t->total_bytes);
    t->arena = NULL;
    st32(&t->state, MS_STATE_DONE);
out:
#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS);
#endif
    THREAD_RETURN;
}

static void destroy(ms_test *t)
{
    free(t->ring);
    free(t->workers);
    free(t->ctr_mem);
    mutex_destroy(&t->lock);
    free(t);
}

ms_test *ms_start(const ms_config *in, char *err, size_t errlen)
{
    ms_config c = *in;
    if (ms_config_resolve(&c, err, errlen)) return NULL;
    cpu_detect();
    if (!cpu.avx2) {
        snprintf(err, errlen, "this CPU (or OS) does not support AVX2, which memslammer needs");
        return NULL;
    }
    ms_test *t = calloc(1, sizeof *t);
    if (!t) { snprintf(err, errlen, "out of memory"); return NULL; }
    t->cfg = c;
    int en[4], ne = 0;
    for (int i = 0; i < 4; i++) if (c.ops & (1 << i)) en[ne++] = i;
    for (int i = 0; i < 4; i++) t->op_map[i] = en[i % ne];
    t->slice_bytes = slice_bytes_of(&c);
    t->slice_blocks = t->slice_bytes / c.block;
    t->total_bytes = t->slice_bytes * (u64)c.threads;
    t->first_error = -1;
    mutex_init(&t->lock);
    t->ctr_mem = calloc((size_t)c.threads + 1, sizeof(ms_counters));
    t->workers = calloc((size_t)c.threads, sizeof(ms_thread));
    t->ring = calloc(MS_ERRORS_KEPT, sizeof(ms_error));
    if (!t->ctr_mem || !t->workers || !t->ring) { destroy(t); snprintf(err, errlen, "out of memory"); return NULL; }
    t->ctr = (ms_counters *)(((uintptr_t)t->ctr_mem + 127) & ~(uintptr_t)127);
    for (int i = 0; i < c.threads; i++) { t->ctr[i].owner = t; t->ctr[i].id = i; }

    static volatile int engine_ready;
    if (!ld32(&engine_ready)) { ms__engine_init(cpu.line_size, cpu.clflushopt); st32(&engine_ready, 1); }

    t->state = MS_STATE_STARTING;
    if (thread_start(&t->controller, controller, t)) {
        destroy(t);
        snprintf(err, errlen, "could not start the test thread");
        return NULL;
    }
    return t;
}

void ms_stop(ms_test *t) { if (t) st32(&t->stop, 1); }

int ms_wait(ms_test *t, unsigned timeout_ms)
{
    double end = ms__now() + timeout_ms / 1000.0;
    for (;;) {
        if (ld32(&t->state) >= MS_STATE_DONE) return 1;
        if (ms__now() >= end) return 0;
        sleep_ms(5);
    }
}

void ms_free(ms_test *t)
{
    if (!t) return;
    ms_stop(t);
    thread_join(t->controller);
    free_big(t->arena, t->total_bytes);
    destroy(t);
}

void ms_get_status(const ms_test *t, ms_status *s)
{
    memset(s, 0, sizeof *s);
    s->state = ld32(&t->state);
    s->threads = t->cfg.threads;
    s->megabytes = t->total_bytes >> 20;
    if (s->state == MS_STATE_RUNNING || s->state == MS_STATE_STOPPING) s->elapsed = ms__now() - t->t_start;
    else if (s->state == MS_STATE_DONE) s->elapsed = t->t_end - t->t_start;
    u64 blocks = 0;
    for (int i = 0; i < t->cfg.threads; i++) {
        s->cycles += ld64(&t->ctr[i].cycles);
        s->bytes += ld64(&t->ctr[i].bytes);
        blocks += ld64(&t->ctr[i].blocks);
    }
    s->coverage = 100.0 * (double)blocks / ((double)t->slice_blocks * t->cfg.threads);
    ms_mutex *lock = (ms_mutex *)&t->lock;
    mutex_lock(lock);
    s->errors = t->n_errors;
    s->first_error = t->first_error;
    memcpy(s->message, t->message, sizeof s->message);
    mutex_unlock(lock);
}

int ms_get_thread(const ms_test *t, int thread, ms_thread_status *out)
{
    if (thread < 0 || thread >= t->cfg.threads) return -1;
    out->cycles = ld64(&t->ctr[thread].cycles);
    out->errors = ld64(&t->ctr[thread].errors);
    out->coverage = 100.0 * (double)ld64(&t->ctr[thread].blocks) / (double)t->slice_blocks;
    return 0;
}

size_t ms_get_errors(const ms_test *t, uint64_t first, ms_error *out, size_t max)
{
    ms_mutex *lock = (ms_mutex *)&t->lock;
    mutex_lock(lock);
    u64 n = t->n_errors, lo = n > MS_ERRORS_KEPT ? n - MS_ERRORS_KEPT : 0;
    if (first < lo) first = lo;
    size_t k = 0;
    for (u64 i = first; i < n && k < max; i++, k++) out[k] = t->ring[i % MS_ERRORS_KEPT];
    mutex_unlock(lock);
    return k;
}

void ms_get_config(const ms_test *t, ms_config *c) { *c = t->cfg; }

void ms_get_sysinfo(ms_sysinfo *s)
{
    memset(s, 0, sizeof *s);
    cpu_detect();
    const char *b = cpu.brand;
    while (*b == ' ') b++;
    snprintf(s->cpu, sizeof s->cpu, "%s", *b ? b : "unknown CPU");
    for (size_t l = strlen(s->cpu); l && s->cpu[l - 1] == ' '; l--) s->cpu[l - 1] = 0;
    s->logical_cpus = cpu_count();
    s->total_mb = total_mb();
    s->avail_mb = avail_mb();
    s->avx2 = cpu.avx2;
}
