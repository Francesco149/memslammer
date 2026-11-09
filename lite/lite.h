/*
 * Shared logic for the lite frontends (lite/win32.c, lite/gtk.c): building a
 * config from the form, polling a running test, formatting the log.
 * SPDX-License-Identifier: MIT
 */
#ifndef LITE_H
#define LITE_H

#include "../core/memslammer.h"

/* Duration choices offered in the form, in seconds (0 = until stopped). */
extern const int lite_durations[];
extern const char *const lite_duration_names[];
extern const int lite_n_durations;

typedef void (*lite_log_fn)(void *ud, const char *line);

typedef struct lite_run {
    ms_test *t;
    ms_config cfg;          /* resolved */
    uint64_t next_err;
    double log_t;           /* elapsed at the last progress line */
    uint64_t log_bytes;
    double rate_t;          /* throughput sampling */
    uint64_t rate_bytes;
    double gbps;
    int finished;           /* summary logged */
    int stopped_by_user;
} lite_run;

/* Build a config from the form fields. mb and threads may be empty (auto).
 * options holds extra CLI flags, applied last. Returns 0, or -1 with err. */
int lite_make_config(ms_config *c, const char *mb, const char *threads, int seconds,
                     int stop_on_error, const char *options, char *err, size_t errlen);

/* Start a run and log its config. Returns 0, or -1 with err. */
int lite_start(lite_run *r, const ms_config *c, lite_log_fn log, void *ud, char *err, size_t errlen);

/* Poll: logs new errors, a progress line every 30 s, and a summary at the end.
 * Fills s. Call every 100-500 ms while r->t is set. */
void lite_poll(lite_run *r, ms_status *s, lite_log_fn log, void *ud);

void lite_stop(lite_run *r);
void lite_free(lite_run *r);

/* "0:12:34" */
void lite_fmt_hms(double seconds, char *buf, size_t n);

enum { LITE_TONE_NORMAL, LITE_TONE_GOOD, LITE_TONE_BAD };

/* Headline for the status line ("Running", "PASS", "FAIL: 3 errors", ...)
 * and its tone. */
int lite_headline(const lite_run *r, const ms_status *s, char *buf, size_t n);

/* The value column of the stats block: elapsed, coverage, throughput,
 * errors, memory. Each buffer is at least 64 bytes. */
void lite_stats(const lite_run *r, const ms_status *s, char out[5][64]);

#endif
