/*
 * memslammer core: the memory test engine, independent of any frontend.
 * SPDX-License-Identifier: MIT
 *
 * Usage:
 *
 *   ms_config c;
 *   ms_config_init(&c);                 defaults: verify mode, 90% of free RAM
 *   ... edit fields, or ms_config_parse() CLI-style flags ...
 *   ms_test *t = ms_start(&c, err, sizeof err);
 *   loop: ms_get_status(t, &s); ms_get_errors(t, next, ...);
 *   ms_stop(t);                         optional: also stops on its own when
 *                                       the duration ends or on first error
 *   ms_free(t);                         waits for the test to finish, frees it
 *
 * ms_start returns at once. Allocation and the test itself run on background
 * threads, so a GUI can call everything here from its UI thread. All
 * functions taking an ms_test are safe to call from any thread.
 */
#ifndef MEMSLAMMER_H
#define MEMSLAMMER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *ms_version(void);

typedef struct ms_test ms_test;

enum { MS_MODE_CHAIN = 0, MS_MODE_VERIFY = 1 };
enum { MS_PATTERN_TOGGLE = 0, MS_PATTERN_MUL = 1, MS_PATTERN_RANDOM = 2, MS_PATTERN_KPAGE = 3 };
enum { MS_CACHE_NONE = 0, MS_CACHE_NTA = 1, MS_CACHE_FLUSH = 2 };
enum { MS_ORDER_SEQ = 0, MS_ORDER_SHUFFLE = 1 };

typedef struct ms_config {
    uint64_t megabytes;  /* memory to test; 0 = 90% of available */
    int threads;         /* 0 = logical CPUs */
    int mode;            /* MS_MODE_* */
    uint64_t block;      /* bytes per work unit; 0 = chain 4096, verify 16 MiB */
    double seconds;      /* stop after this long; 0 = until ms_stop */
    int stop_on_error;
    int yield;           /* yield the CPU after each block */
    /* chain mode */
    int passes;          /* forward passes per cycle, same number of reverse */
    int flush;           /* clflush after every block op */
    int ops;             /* enabled ops: 1 add, 2 sub, 4 xor, 8 not */
    /* verify mode */
    int rereads;         /* compare passes per fill */
    int pattern;         /* MS_PATTERN_* */
    int cache;           /* MS_CACHE_* */
    int order;           /* MS_ORDER_* */
    int heat;            /* FP heat iterations per 4 KiB (random pattern only) */
} ms_config;

/* Defaults: the fastest-failing configuration we have measured. */
void ms_config_init(ms_config *c);

/* Apply CLI-style flags ("-m", "4096", "-M", "verify", ...). argv excludes the
 * program name. Returns 0 on success, 1 if -h was given, -1 on a bad flag
 * (with a message in err). */
int ms_config_parse(ms_config *c, int argc, char **argv, char *err, size_t errlen);

/* Help text for the flags ms_config_parse accepts, one per line. */
const char *ms_config_help(void);

/* Fill in automatic values (0 fields) and check the combination. Returns 0,
 * or -1 with a message in err. ms_start does this itself. */
int ms_config_resolve(ms_config *c, char *err, size_t errlen);

/* One-line summary of a resolved config, e.g.
 * "memslammer verify: 4096 MB, 24 threads, block 16777216, ...". */
void ms_config_describe(const ms_config *c, char *buf, size_t n);

/* Same config written back as CLI flags (only the ones that differ from the
 * defaults), so a GUI run can be reproduced from the command line. */
void ms_config_to_args(const ms_config *c, char *buf, size_t n);

enum {
    MS_STATE_STARTING = 0,  /* allocating and touching the test memory */
    MS_STATE_RUNNING,
    MS_STATE_STOPPING,      /* stop requested, waiting for worker threads */
    MS_STATE_DONE,
    MS_STATE_FAILED         /* could not start; see ms_status.message */
};

typedef struct ms_status {
    int state;              /* MS_STATE_* */
    double elapsed;         /* seconds since the workers started */
    double first_error;     /* seconds, -1 if none */
    uint64_t errors;
    uint64_t cycles;        /* completed cycles over all threads */
    uint64_t bytes;         /* bytes read + written over all threads */
    double coverage;        /* percent; 100 = each thread has tested every block
                             * of its share once (one full cycle) */
    uint64_t megabytes;     /* memory under test */
    int threads;
    char message[160];      /* why the test failed to start */
} ms_status;

void ms_get_status(const ms_test *t, ms_status *s);

/* Per-thread counters, for frontends that show each thread. */
typedef struct ms_thread_status {
    uint64_t cycles;
    uint64_t errors;
    double coverage;        /* percent of this thread's share, as in ms_status */
} ms_thread_status;

/* Returns -1 if thread is out of range. */
int ms_get_thread(const ms_test *t, int thread, ms_thread_status *out);

typedef struct ms_error {
    uint64_t index;         /* 0 for the first error of the run, then 1, 2, ... */
    double time;            /* seconds since start */
    int thread;
    uint64_t cycle;
    uint64_t offset;        /* byte offset of the failing block in the test memory */
    uint64_t bits[4];       /* verify: OR of (expected ^ read) over the failing
                             * pass, for each bit of a 32-byte vector, i.e. four
                             * consecutive 64-bit words. 0 in chain mode. */
    int pass;               /* verify: which re-read of the block found it, 0 = the
                             * first after the fill. -1 in chain mode. */
    int recheck;            /* verify: right after the error the block is flushed
                             * from cache and read twice more; this counts the
                             * re-reads that failed again in the same bits.
                             * 2 = the data stored in DRAM is wrong; 0 = it is
                             * right and the failing read itself went wrong.
                             * -1 in chain mode. */
} ms_error;

/* The last MS_ERRORS_KEPT errors are kept. Copies up to max of them with
 * index >= first into out, oldest first, and returns how many it copied. */
#define MS_ERRORS_KEPT 4096
size_t ms_get_errors(const ms_test *t, uint64_t first, ms_error *out, size_t max);

/* The config the test is running with (automatic values filled in). */
void ms_get_config(const ms_test *t, ms_config *c);

ms_test *ms_start(const ms_config *c, char *err, size_t errlen);
void ms_stop(ms_test *t);
/* Wait up to timeout_ms for the test to end (DONE or FAILED). Returns 1 if it
 * has ended. Does not stop it. */
int ms_wait(ms_test *t, unsigned timeout_ms);
/* Stop the test if needed, wait for it, and free it. */
void ms_free(ms_test *t);

typedef struct ms_sysinfo {
    char cpu[64];           /* CPU brand string */
    int logical_cpus;
    uint64_t total_mb;
    uint64_t avail_mb;      /* what the OS could hand us now */
    int avx2;               /* CPU and OS support AVX2; needed to run */
} ms_sysinfo;

void ms_get_sysinfo(ms_sysinfo *s);

#ifdef __cplusplus
}
#endif
#endif
