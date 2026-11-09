/*
 * memslammer CLI. The output format is read by harness/bench.py: keep the
 * ERROR, progress and RESULT lines stable.
 * SPDX-License-Identifier: MIT
 */
#include "../core/memslammer.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

static volatile sig_atomic_t interrupted;

#ifdef _WIN32
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (interrupted) return FALSE;  /* second Ctrl+C: default handling, exit */
    interrupted = 1;
    return TRUE;
}
#else
static void on_signal(int sig)
{
    if (interrupted) { signal(sig, SIG_DFL); raise(sig); }
    interrupted = 1;
}
#endif

static void usage(FILE *f)
{
    fprintf(f, "usage: memslammer [options]   (no options: verify 90%% of free RAM until stopped)\n"
               "  -h          show this help\n"
               "  --version   show the version\n%s", ms_config_help());
}

/* bit positions within a 64-bit word that flipped, from the 256-bit mask */
static unsigned long long fold_bits(const ms_error *e)
{
    return (unsigned long long)(e->bits[0] | e->bits[1] | e->bits[2] | e->bits[3]);
}

static uint64_t drain_errors(ms_test *t, uint64_t next)
{
    ms_error e[64];
    size_t n;
    while ((n = ms_get_errors(t, next, e, 64))) {
        for (size_t i = 0; i < n; i++) {
            unsigned long long bits = fold_bits(&e[i]);
            fprintf(stderr, "[%9.3fs] ERROR thread %d cycle %llu (total %llu)",
                    e[i].time, e[i].thread, (unsigned long long)e[i].cycle,
                    (unsigned long long)e[i].index + 1);
            if (bits) fprintf(stderr, " bits 0x%016llx", bits);
            fputc('\n', stderr);
        }
        next = e[n - 1].index + 1;
    }
    return next;
}

int main(int argc, char **argv)
{
    ms_config c;
    char err[256];
    ms_config_init(&c);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) { printf("memslammer %s\n", ms_version()); return 0; }
        if (!strcmp(argv[i], "--help")) { usage(stdout); return 0; }
    }
    int r = ms_config_parse(&c, argc - 1, argv + 1, err, sizeof err);
    if (r == 1) { usage(stdout); return 0; }
    if (r < 0) { fprintf(stderr, "memslammer: %s\n", err); usage(stderr); return 2; }

    ms_test *t = ms_start(&c, err, sizeof err);
    if (!t) { fprintf(stderr, "memslammer: %s\n", err); return 2; }
    ms_get_config(t, &c);
    ms_config_describe(&c, err, sizeof err);
    printf("%s\n", err);
    fflush(stdout);

#ifdef _WIN32
    SetConsoleCtrlHandler(on_ctrl, TRUE);
#else
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif

    ms_status s;
    uint64_t next_err = 0, last_bytes = 0;
    double last = 0;
    int stopped = 0;
    for (;;) {
        sleep_ms(100);
        if (interrupted && !stopped) { ms_stop(t); stopped = 1; }
        ms_get_status(t, &s);
        next_err = drain_errors(t, next_err);
        if (s.state == MS_STATE_FAILED) {
            fprintf(stderr, "memslammer: %s\n", s.message);
            ms_free(t);
            return 1;
        }
        if (s.state == MS_STATE_STARTING) continue;
        int ending = s.state >= MS_STATE_STOPPING;
        if (s.elapsed - last >= 10 || ending) {
            printf("[%9.1fs] cycles %llu, coverage %.1f%%, %.2f GB/s, errors %llu\n",
                   s.elapsed, (unsigned long long)s.cycles, s.coverage,
                   s.elapsed > last ? (s.bytes - last_bytes) / (s.elapsed - last) / 1e9 : 0.0,
                   (unsigned long long)s.errors);
            fflush(stdout);
            last = s.elapsed;
            last_bytes = s.bytes;
        }
        if (ending) break;
    }
    ms_wait(t, 0xffffffffu);
    next_err = drain_errors(t, next_err);
    ms_get_status(t, &s);
    printf("RESULT tester=memslammer errors=%llu first_error_s=%.3f elapsed_s=%.3f\n",
           (unsigned long long)s.errors, s.first_error, s.elapsed);
    ms_free(t);
    return s.errors ? 1 : 0;
}
