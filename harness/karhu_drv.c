/*
 * karhu_drv - headless driver for a user-supplied, licensed Karhu RAM Test
 * ramtest.dll (x64). Calls its exported StartTest/GetErrorCount/StopTest
 * and reports time-to-first-error. No Karhu code is included here.
 * SPDX-License-Identifier: MIT
 *
 * usage: karhu_drv <path\to\x64\ramtest.dll> <megabytes> <threads> <seconds>
 * prints progress lines and a final line:
 *   RESULT tester=karhu errors=N first_error_s=X elapsed_s=Y coverage=Z
 * exit code: 0 no errors, 1 errors, 2 usage/load failure
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef BOOL (*start_fn)(int, int);
typedef BOOL (*stop_fn)(void);
typedef double (*cov_fn)(void);
typedef int (*err_fn)(void);

static stop_fn stop_test;

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    if (stop_test) stop_test();
    ExitProcess(3);
    return TRUE;
}

int main(int argc, char **argv)
{
    if (argc != 5 && !(argc == 6 && !strcmp(argv[5], "nostop"))) {
        fprintf(stderr, "usage: karhu_drv <ramtest.dll> <megabytes> <threads> <seconds> [nostop]\n");
        return 2;
    }
    /* altered search path so the DLL's own vcomp140.dll is found next to it */
    HMODULE h = LoadLibraryExA(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 2; }
    start_fn start = (start_fn)GetProcAddress(h, "StartTest");
    stop_test = (stop_fn)GetProcAddress(h, "StopTest");
    cov_fn cov = (cov_fn)GetProcAddress(h, "GetCoverage");
    err_fn errs = (err_fn)GetProcAddress(h, "GetErrorCount");
    if (!start || !stop_test || !cov || !errs) { fprintf(stderr, "missing exports\n"); return 2; }

    int mb = atoi(argv[2]), threads = atoi(argv[3]);
    double seconds = atof(argv[4]);
    int nostop = argc == 6;
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (!start(mb, threads)) { fprintf(stderr, "StartTest failed: %lu\n", GetLastError()); return 2; }
    printf("START tester=karhu mb=%d threads=%d\n", mb, threads);
    fflush(stdout);

    double first = -1, el = 0, last_print = 0;
    int n = 0;
    for (;;) {
        Sleep(50);
        QueryPerformanceCounter(&t);
        el = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
        int prev = n;
        n = errs();
        if (n > prev) {
            if (first < 0) first = el;
            printf("ERROR t=%.3f count=%d\n", el, n);
            fflush(stdout);
            if (!nostop) break;         /* stop on first error */
        }
        if (el - last_print >= 10) {
            printf("PROGRESS t=%.1f coverage=%.2f errors=%d\n", el, cov(), n);
            fflush(stdout);
            last_print = el;
        }
        if (seconds > 0 && el >= seconds) break;
    }
    double c = cov();
    stop_test();
    printf("RESULT tester=karhu errors=%d first_error_s=%.3f elapsed_s=%.3f coverage=%.2f\n",
           n, first, el, c);
    fflush(stdout);
    return n ? 1 : 0;
}
