/*
 * Shared logic for the lite frontends.
 * SPDX-License-Identifier: MIT
 */
#include "lite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int lite_durations[] = { 0, 300, 900, 1800, 3600, 7200, 14400 };
const char *const lite_duration_names[] = {
    "Until stopped", "5 minutes", "15 minutes", "30 minutes", "1 hour", "2 hours", "4 hours"
};
const int lite_n_durations = sizeof lite_durations / sizeof lite_durations[0];

static int parse_field(const char *s, unsigned long long *out)
{
    while (*s == ' ') s++;
    if (!*s) { *out = 0; return 0; }
    char *end;
    *out = strtoull(s, &end, 10);
    while (*end == ' ') end++;
    return *end ? -1 : 0;
}

int lite_make_config(ms_config *c, const char *mb, const char *threads, int seconds,
                     int stop_on_error, const char *options, char *err, size_t errlen)
{
    unsigned long long v;
    ms_config_init(c);
    if (parse_field(mb ? mb : "", &v)) { snprintf(err, errlen, "Memory must be a number of MB"); return -1; }
    c->megabytes = v;
    if (parse_field(threads ? threads : "", &v) || v > 4096) { snprintf(err, errlen, "Threads must be a number"); return -1; }
    c->threads = (int)v;
    c->seconds = seconds;
    c->stop_on_error = stop_on_error;

    /* split the extra options on whitespace */
    char buf[512], *argv[64];
    int argc = 0;
    snprintf(buf, sizeof buf, "%s", options ? options : "");
    for (char *p = strtok(buf, " \t\r\n"); p && argc < 64; p = strtok(NULL, " \t\r\n")) argv[argc++] = p;
    char perr[200];
    int r = ms_config_parse(c, argc, argv, perr, sizeof perr);
    if (r == 1) { snprintf(err, errlen, "Options:\n%s", ms_config_help()); return -1; }
    if (r < 0) { snprintf(err, errlen, "Options: %s", perr); return -1; }
    return 0;
}

int lite_start(lite_run *r, const ms_config *c, lite_log_fn log, void *ud, char *err, size_t errlen)
{
    memset(r, 0, sizeof *r);
    r->t = ms_start(c, err, errlen);
    if (!r->t) return -1;
    ms_get_config(r->t, &r->cfg);
    char line[400], args[300];
    ms_config_describe(&r->cfg, line, sizeof line);
    log(ud, line);
    ms_config_to_args(&r->cfg, args, sizeof args);
    snprintf(line, sizeof line, "CLI equivalent: memslammer %s", args);
    log(ud, line);
    return 0;
}

void lite_fmt_hms(double s, char *buf, size_t n)
{
    if (s < 0) s = 0;
    unsigned long t = (unsigned long)s;
    snprintf(buf, n, "%lu:%02lu:%02lu", t / 3600, t / 60 % 60, t % 60);
}

void lite_poll(lite_run *r, ms_status *s, lite_log_fn log, void *ud)
{
    char line[256];
    ms_get_status(r->t, s);

    ms_error e[32];
    size_t n;
    while ((n = ms_get_errors(r->t, r->next_err, e, 32))) {
        for (size_t i = 0; i < n; i++) {
            unsigned long long bits = e[i].bits[0] | e[i].bits[1] | e[i].bits[2] | e[i].bits[3];
            int k = snprintf(line, sizeof line, "[%9.3fs] ERROR thread %d cycle %llu (total %llu)",
                             e[i].time, e[i].thread, (unsigned long long)e[i].cycle,
                             (unsigned long long)e[i].index + 1);
            if (bits) snprintf(line + k, sizeof line - k, " bits 0x%016llx", bits);
            log(ud, line);
        }
        r->next_err = e[n - 1].index + 1;
    }

    if (s->state >= MS_STATE_RUNNING && s->state <= MS_STATE_DONE) {
        if (s->elapsed - r->rate_t >= 2.0) {
            r->gbps = (s->bytes - r->rate_bytes) / (s->elapsed - r->rate_t) / 1e9;
            r->rate_t = s->elapsed;
            r->rate_bytes = s->bytes;
        }
        if (s->state == MS_STATE_RUNNING && s->elapsed - r->log_t >= 30) {
            snprintf(line, sizeof line, "[%9.1fs] cycles %llu, coverage %.1f%%, %.2f GB/s, errors %llu",
                     s->elapsed, (unsigned long long)s->cycles, s->coverage,
                     (s->bytes - r->log_bytes) / (s->elapsed - r->log_t) / 1e9,
                     (unsigned long long)s->errors);
            log(ud, line);
            r->log_t = s->elapsed;
            r->log_bytes = s->bytes;
        }
    }

    if (r->finished) return;
    if (s->state == MS_STATE_FAILED) {
        snprintf(line, sizeof line, "Could not start: %s", s->message);
        log(ud, line);
        r->finished = 1;
    } else if (s->state == MS_STATE_DONE) {
        char hms[32];
        lite_fmt_hms(s->elapsed, hms, sizeof hms);
        if (s->errors)
            snprintf(line, sizeof line, "FAIL: %llu error%s, first after %.1f s (ran %s, coverage %.1f%%)",
                     (unsigned long long)s->errors, s->errors == 1 ? "" : "s", s->first_error, hms, s->coverage);
        else
            snprintf(line, sizeof line, "%s: no errors in %s (coverage %.1f%%)",
                     r->stopped_by_user ? "Stopped" : "PASS", hms, s->coverage);
        log(ud, line);
        r->finished = 1;
    }
}

void lite_stop(lite_run *r)
{
    if (!r->t) return;
    r->stopped_by_user = 1;
    ms_stop(r->t);
}

void lite_free(lite_run *r)
{
    if (r->t) ms_free(r->t);
    r->t = NULL;
}

int lite_headline(const lite_run *r, const ms_status *s, char *buf, size_t n)
{
    switch (s->state) {
    case MS_STATE_STARTING:
        snprintf(buf, n, "Allocating %llu MB...", (unsigned long long)s->megabytes);
        return LITE_TONE_NORMAL;
    case MS_STATE_RUNNING:
        if (s->errors) { snprintf(buf, n, "FAILING: %llu error%s", (unsigned long long)s->errors, s->errors == 1 ? "" : "s"); return LITE_TONE_BAD; }
        snprintf(buf, n, "Running");
        return LITE_TONE_NORMAL;
    case MS_STATE_STOPPING:
        snprintf(buf, n, "Stopping...");
        return s->errors ? LITE_TONE_BAD : LITE_TONE_NORMAL;
    case MS_STATE_DONE:
        if (s->errors) { snprintf(buf, n, "FAIL: %llu error%s", (unsigned long long)s->errors, s->errors == 1 ? "" : "s"); return LITE_TONE_BAD; }
        snprintf(buf, n, r->stopped_by_user ? "Stopped, no errors" : "PASS");
        return LITE_TONE_GOOD;
    default:
        snprintf(buf, n, "Could not start");
        return LITE_TONE_BAD;
    }
}

void lite_stats(const lite_run *r, const ms_status *s, char out[5][64])
{
    char a[24], b[24];
    lite_fmt_hms(s->elapsed, a, sizeof a);
    if (r->cfg.seconds > 0) {
        lite_fmt_hms(r->cfg.seconds, b, sizeof b);
        snprintf(out[0], 64, "%s / %s", a, b);
    } else snprintf(out[0], 64, "%s", a);
    snprintf(out[1], 64, "%.1f%%", s->coverage);
    snprintf(out[2], 64, "%.1f GB/s%s", s->state == MS_STATE_DONE && s->elapsed > 0 ? s->bytes / s->elapsed / 1e9 : r->gbps,
             s->state == MS_STATE_DONE ? " (average)" : "");
    if (s->errors) snprintf(out[3], 64, "%llu (first at %.1f s)", (unsigned long long)s->errors, s->first_error);
    else snprintf(out[3], 64, "0");
    snprintf(out[4], 64, "%llu MB, %d threads", (unsigned long long)s->megabytes, s->threads);
}
