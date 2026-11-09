/*
 * Internals shared by memslammer.c (control) and engine.c (test loops).
 * SPDX-License-Identifier: MIT
 *
 * Builds with gcc, clang, mingw-w64 and MSVC: no pthreads or C11 atomics,
 * small shims below instead.
 */
#ifndef MS_INTERNAL_H
#define MS_INTERNAL_H

#include "memslammer.h"

typedef uint64_t u64;
typedef uint32_t u32;

#if defined(_MSC_VER) && !defined(__clang__)
#define MS_MSVC 1
#include <intrin.h>
#define MS_RESTRICT __restrict
#define MS_ALIGN(n) __declspec(align(n))
#define MS_TARGET(x)
#define MS_UNUSED
static __inline u64 ld64(const volatile u64 *p) { return *p; }  /* aligned x64 loads are atomic */
static __inline u64 add64(volatile u64 *p, u64 v)
{ return (u64)_InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v) + v; }
static __inline int ld32(const volatile int *p) { return *p; }
static __inline void st32(volatile int *p, int v) { _InterlockedExchange((volatile long *)p, v); }
#else
#define MS_RESTRICT __restrict__
#define MS_ALIGN(n) __attribute__((aligned(n)))
#define MS_TARGET(x) __attribute__((target(x)))
#define MS_UNUSED __attribute__((unused))
static inline u64 ld64(const volatile u64 *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline u64 add64(volatile u64 *p, u64 v) { return __atomic_add_fetch(p, v, __ATOMIC_RELAXED); }
static inline int ld32(const volatile int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void st32(volatile int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
#endif

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  /* Windows 7: GetActiveProcessorCount, SRW locks */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef SRWLOCK ms_mutex;
typedef HANDLE ms_thread;
#else
#include <pthread.h>
typedef pthread_mutex_t ms_mutex;
typedef pthread_t ms_thread;
#endif

/* Per-thread counters, each written only by its worker. Padded to two cache
 * lines so workers never share a line (adjacent-line prefetch pairs them). */
typedef struct MS_ALIGN(128) ms_counters {
    volatile u64 cycles, blocks, bytes, errors;
    struct ms_test *owner;    /* set before the worker starts, then read-only */
    int id;
} ms_counters;

struct ms_test {
    ms_config cfg;            /* resolved */
    int op_map[4];            /* chain: (key & 3) -> enabled op id */
    u64 slice_bytes;          /* per thread */
    u64 slice_blocks;         /* slice_bytes / block */
    u64 total_bytes;          /* slice_bytes * threads */
    char *arena;

    volatile int stop;
    volatile int state;
    double t_start, t_end;    /* t_start is set before the workers start */

    ms_counters *ctr;         /* [threads] */
    void *ctr_mem;            /* unaligned allocation behind ctr */
    ms_thread *workers;
    ms_thread controller;

    ms_mutex lock;            /* guards everything below */
    ms_error *ring;           /* [MS_ERRORS_KEPT] */
    u64 n_errors;
    double first_error;
    char message[160];
};

/* memslammer.c */
double ms__now(void);
void ms__yield(void);
void ms__report_error(ms_test *t, int thread, u64 cycle, u64 offset, const u64 bits[4]);

/* engine.c (built with AVX2) */
void ms__engine_init(int line_size, int have_clflushopt);
void ms__chain_worker(ms_test *t, int id);
void ms__verify_worker(ms_test *t, int id);

#endif
