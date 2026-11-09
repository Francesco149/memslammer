/*
 * Core self-test: clean runs must report no errors, and bit flips injected
 * into the test memory while it runs must be caught and reported correctly.
 * Uses internal.h to reach the test memory.
 * SPDX-License-Identifier: MIT
 */
#include "../core/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
static void nap(int ms) { Sleep(ms); }
#else
#include <time.h>
static void nap(int ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ms_test *start(ms_config *c)
{
    char err[200];
    ms_test *t = ms_start(c, err, sizeof err);
    if (!t) { printf("FAIL: ms_start: %s\n", err); exit(1); }
    return t;
}

static int wait_state(ms_test *t, int state, int ms)
{
    ms_status s;
    for (int i = 0; i < ms / 10; i++) {
        ms_get_status(t, &s);
        if (s.state >= state) return 1;
        nap(10);
    }
    return 0;
}

static void test_config(void)
{
    ms_config c;
    char err[200];
    ms_config_init(&c);
    CHECK(c.mode == MS_MODE_VERIFY && c.rereads == 200 && c.pattern == MS_PATTERN_RANDOM, "defaults");
    char *a1[] = { "-m", "512", "-t", "3", "-M", "chain", "-F", "-s", "-d", "2.5" };
    CHECK(ms_config_parse(&c, 10, a1, err, sizeof err) == 0, "parse: %s", err);
    CHECK(c.megabytes == 512 && c.threads == 3 && c.mode == MS_MODE_CHAIN && !c.flush && c.stop_on_error && c.seconds == 2.5, "parsed values");
    char *a2[] = { "-x" };
    CHECK(ms_config_parse(&c, 1, a2, err, sizeof err) == -1, "unknown flag rejected");
    char *a3[] = { "-m", "lots" };
    CHECK(ms_config_parse(&c, 2, a3, err, sizeof err) == -1, "bad number rejected");
    char *a4[] = { "-h" };
    CHECK(ms_config_parse(&c, 1, a4, err, sizeof err) == 1, "-h");

    ms_config_init(&c);
    c.megabytes = 1;
    c.threads = 4;
    CHECK(ms_config_resolve(&c, err, sizeof err) == -1, "too little memory rejected");
    ms_config_init(&c);
    c.heat = 8;
    c.pattern = MS_PATTERN_MUL;
    CHECK(ms_config_resolve(&c, err, sizeof err) == -1, "-H without random rejected");

    ms_config_init(&c);
    c.megabytes = 1024; c.threads = 2; c.mode = MS_MODE_CHAIN; c.flush = 0; c.seconds = 60;
    char args[200];
    ms_config_to_args(&c, args, sizeof args);
    CHECK(!strcmp(args, "-M chain -m 1024 -t 2 -d 60 -F"), "to_args: '%s'", args);
}

static void test_clean(int mode)
{
    ms_config c;
    ms_config_init(&c);
    c.mode = mode; c.megabytes = 256; c.threads = 2; c.seconds = 2;
    ms_test *t = start(&c);
    CHECK(ms_wait(t, 10000), "clean %d: finished", mode);
    ms_status s;
    ms_get_status(t, &s);
    CHECK(s.state == MS_STATE_DONE, "clean %d: state %d", mode, s.state);
    CHECK(s.errors == 0 && s.first_error < 0, "clean %d: %llu errors", mode, (unsigned long long)s.errors);
    CHECK(s.elapsed >= 2 && s.elapsed < 4, "clean %d: elapsed %.2f", mode, s.elapsed);
    CHECK(s.bytes > 0, "clean %d: no traffic", mode);
    CHECK(s.megabytes == 256, "clean %d: %llu MB", mode, (unsigned long long)s.megabytes);
    printf("clean %s: %.1f GB/s, %llu cycles\n", mode ? "verify" : "chain", s.bytes / s.elapsed / 1e9,
           (unsigned long long)s.cycles);
    ms_free(t);
}

/* Flip bit 37 of the first word of every block: whichever block each thread
 * is re-reading is hit, so every thread must report it. */
static void test_verify_flip(int stop_on_error)
{
    ms_config c;
    ms_config_init(&c);
    c.megabytes = 256; c.threads = 2; c.rereads = 1000000; c.stop_on_error = stop_on_error;
    ms_test *t = start(&c);
    CHECK(wait_state(t, MS_STATE_RUNNING, 10000), "verify: running");
    nap(300);
    for (u64 off = 0; off < t->total_bytes; off += t->cfg.block)
        *(volatile u64 *)(t->arena + off) ^= 1ull << 37;

    ms_status s;
    for (int i = 0; i < 500; i++) {
        ms_get_status(t, &s);
        if (s.errors >= (stop_on_error ? 1u : 2u)) break;
        nap(10);
    }
    if (stop_on_error) CHECK(ms_wait(t, 5000), "verify: stopped itself on error");
    else ms_stop(t), ms_wait(t, 5000);
    ms_get_status(t, &s);
    CHECK(s.errors >= 1, "verify: flip not caught");
    CHECK(s.first_error > 0, "verify: first_error %.3f", s.first_error);
    if (!stop_on_error) CHECK(s.errors == 2, "verify: expected one error per thread, got %llu", (unsigned long long)s.errors);

    ms_error e[8];
    size_t n = ms_get_errors(t, 0, e, 8);
    CHECK(n == s.errors, "verify: ring has %zu of %llu", n, (unsigned long long)s.errors);
    int seen[2] = { 0, 0 };
    for (size_t i = 0; i < n; i++) {
        CHECK(e[i].index == i, "verify: index %llu", (unsigned long long)e[i].index);
        CHECK(e[i].bits[0] == 1ull << 37 && !e[i].bits[1] && !e[i].bits[2] && !e[i].bits[3],
              "verify: bits %016llx %016llx %016llx %016llx", (unsigned long long)e[i].bits[0],
              (unsigned long long)e[i].bits[1], (unsigned long long)e[i].bits[2], (unsigned long long)e[i].bits[3]);
        CHECK(e[i].offset % t->cfg.block == 0 && e[i].offset < t->total_bytes, "verify: offset %llu", (unsigned long long)e[i].offset);
        CHECK(e[i].offset / t->slice_bytes == (u64)e[i].thread, "verify: offset outside thread %d's slice", e[i].thread);
        if (e[i].thread >= 0 && e[i].thread < 2) seen[e[i].thread] = 1;
    }
    if (!stop_on_error) CHECK(seen[0] && seen[1], "verify: both threads report");
    CHECK(ms_get_errors(t, n, e, 8) == 0, "verify: nothing past the end");
    printf("verify flip (stop_on_error=%d): %llu errors, first at %.3f s\n", stop_on_error,
           (unsigned long long)s.errors, s.first_error);
    ms_free(t);
}

static void test_chain_flip(void)
{
    ms_config c;
    ms_config_init(&c);
    c.mode = MS_MODE_CHAIN; c.megabytes = 64; c.threads = 2;
    ms_test *t = start(&c);
    CHECK(wait_state(t, MS_STATE_RUNNING, 10000), "chain: running");
    ms_status s;
    u64 x = 12345;
    for (int i = 0; i < 400; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        u64 off = (x >> 16) % t->total_bytes & ~7ull;
        *(volatile u64 *)(t->arena + off) ^= 1ull << (x >> 58);
        nap(5);
        ms_get_status(t, &s);
        if (s.errors) break;
    }
    ms_stop(t);
    ms_wait(t, 5000);
    ms_get_status(t, &s);
    CHECK(s.errors >= 1, "chain: flips not caught");
    ms_error e;
    if (ms_get_errors(t, 0, &e, 1))
        CHECK(e.offset == (u64)e.thread * t->slice_bytes, "chain: offset is the thread's slice");
    printf("chain flip: %llu errors\n", (unsigned long long)s.errors);
    ms_free(t);
}

int main(void)
{
    ms_sysinfo si;
    ms_get_sysinfo(&si);
    printf("memslammer %s self-test on %s, %d CPUs, %llu MB free\n", ms_version(), si.cpu,
           si.logical_cpus, (unsigned long long)si.avail_mb);
    if (!si.avx2) { printf("no AVX2, skipping\n"); return 0; }
    test_config();
    test_clean(MS_MODE_VERIFY);
    test_clean(MS_MODE_CHAIN);
    test_verify_flip(0);
    test_verify_flip(1);
    test_chain_flip();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
