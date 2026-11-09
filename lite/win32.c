/*
 * memslammer lite for Windows: a small native Win32 window around the core.
 * No dependencies beyond what ships with Windows.
 * SPDX-License-Identifier: MIT
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "lite.h"
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>

enum {
    ID_SYS = 100, ID_MB_L, ID_MB, ID_MB_HINT, ID_TH_L, ID_TH, ID_TH_HINT, ID_DUR_L, ID_DUR,
    ID_OPT_L, ID_OPT, ID_STOPERR, ID_HEAD, ID_START, ID_COPY, ID_PROGRESS, ID_LOG,
    ID_STAT_L0 = 200, ID_STAT0 = 210, TIMER_POLL = 1
};

static const wchar_t *stat_names[5] = { L"Elapsed", L"Coverage", L"Throughput", L"Errors", L"Tested" };

static struct {
    HWND win, sys, mb_l, mb, mb_hint, th_l, th, th_hint, dur_l, dur, opt_l, opt, stoperr;
    HWND head, start, copy, progress, log, stat_l[5], stat[5];
    HFONT font, big, mono;
    UINT dpi;
    int tone, running, marquee;
    lite_run run;
    ms_sysinfo si;
    ITaskbarList3 *taskbar;
} ui;

/* ---------------- helpers ---------------- */

static UINT (WINAPI *pGetDpiForWindow)(HWND);
static BOOL (WINAPI *pSystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);

static int S(int v) { return MulDiv(v, (int)ui.dpi, 96); }

static void set_text(HWND h, const char *utf8)
{
    wchar_t w[2048];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 2048);
    w[2047] = 0;
    SetWindowTextW(h, w);
}

static void get_text(HWND h, char *utf8, int n)
{
    wchar_t w[1024];
    GetWindowTextW(h, w, 1024);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, n, NULL, NULL);
}

static void log_line(void *ud, const char *line)
{
    (void)ud;
    wchar_t w[1024];
    int k = MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 1020);
    if (k <= 0) return;
    wcscpy(w + k - 1, L"\r\n");
    int len = GetWindowTextLengthW(ui.log);
    SendMessageW(ui.log, EM_SETSEL, len, len);
    SendMessageW(ui.log, EM_REPLACESEL, FALSE, (LPARAM)w);
    SendMessageW(ui.log, EM_SCROLLCARET, 0, 0);
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (pSystemParametersInfoForDpi)
        pSystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, ui.dpi);
    else {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, (int)ui.dpi, 96);
    }
    if (ui.font) { DeleteObject(ui.font); DeleteObject(ui.big); DeleteObject(ui.mono); }
    LOGFONTW lf = ncm.lfMessageFont;
    ui.font = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 8 / 5;
    lf.lfWeight = FW_SEMIBOLD;
    ui.big = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy(lf.lfFaceName, L"Consolas");
    ui.mono = CreateFontIndirectW(&lf);
}

static BOOL CALLBACK set_font_cb(HWND h, LPARAM lp)
{
    (void)lp;
    HFONT f = h == ui.head ? ui.big : h == ui.log ? ui.mono : ui.font;
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    return TRUE;
}

static void layout(void)
{
    RECT rc;
    GetClientRect(ui.win, &rc);
    int m = S(12), w = rc.right - 2 * m, y = m, rh = S(23), gap = S(6), lw = S(96), fw = S(110);
    MoveWindow(ui.sys, m, y, w, S(18), TRUE);
    y += S(18) + S(10);
    MoveWindow(ui.mb_l, m, y + S(3), lw, rh, TRUE);
    MoveWindow(ui.mb, m + lw, y, fw, rh, TRUE);
    MoveWindow(ui.mb_hint, m + lw + fw + S(8), y + S(3), w - lw - fw - S(8), rh, TRUE);
    y += rh + gap;
    MoveWindow(ui.th_l, m, y + S(3), lw, rh, TRUE);
    MoveWindow(ui.th, m + lw, y, fw, rh, TRUE);
    MoveWindow(ui.th_hint, m + lw + fw + S(8), y + S(3), w - lw - fw - S(8), rh, TRUE);
    y += rh + gap;
    MoveWindow(ui.dur_l, m, y + S(3), lw, rh, TRUE);
    MoveWindow(ui.dur, m + lw, y, S(160), S(200), TRUE);
    y += rh + gap;
    MoveWindow(ui.opt_l, m, y + S(3), lw, rh, TRUE);
    MoveWindow(ui.opt, m + lw, y, w - lw, rh, TRUE);
    y += rh + gap;
    MoveWindow(ui.stoperr, m + lw, y, w - lw, rh, TRUE);
    y += rh + S(12);
    int bw = S(92), bh = S(30);
    MoveWindow(ui.copy, rc.right - m - bw, y, bw, bh, TRUE);
    MoveWindow(ui.start, rc.right - m - 2 * bw - S(8), y, bw, bh, TRUE);
    MoveWindow(ui.head, m, y, w - 2 * bw - S(16), bh, TRUE);
    y += bh + S(10);
    for (int i = 0; i < 5; i++) {
        MoveWindow(ui.stat_l[i], m, y, lw, S(19), TRUE);
        MoveWindow(ui.stat[i], m + lw, y, w - lw, S(19), TRUE);
        y += S(19);
    }
    y += S(8);
    MoveWindow(ui.progress, m, y, w, S(12), TRUE);
    y += S(12) + S(10);
    MoveWindow(ui.log, m, y, w, rc.bottom - y - m, TRUE);
}

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex, int id)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                           ui.win, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void set_form_enabled(BOOL on)
{
    HWND w[] = { ui.mb, ui.th, ui.dur, ui.opt, ui.stoperr };
    for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++) EnableWindow(w[i], on);
}

static void set_marquee(int on)
{
    if (on == ui.marquee) return;
    ui.marquee = on;
    LONG_PTR st = GetWindowLongPtrW(ui.progress, GWL_STYLE);
    SetWindowLongPtrW(ui.progress, GWL_STYLE, on ? st | PBS_MARQUEE : st & ~PBS_MARQUEE);
    SendMessageW(ui.progress, PBM_SETMARQUEE, on, 30);
    if (!on) SendMessageW(ui.progress, PBM_SETPOS, 0, 0);
}

static void taskbar(TBPFLAG flag, double frac)
{
    if (!ui.taskbar) return;
    ITaskbarList3_SetProgressState(ui.taskbar, ui.win, flag);
    if (flag == TBPF_NORMAL || flag == TBPF_ERROR || flag == TBPF_PAUSED)
        ITaskbarList3_SetProgressValue(ui.taskbar, ui.win, (ULONGLONG)(frac * 1000), 1000);
}

/* ---------------- run control ---------------- */

static void finish(void)
{
    KillTimer(ui.win, TIMER_POLL);
    lite_free(&ui.run);
    ui.running = 0;
    set_form_enabled(TRUE);
    SetWindowTextW(ui.start, L"Start");
    EnableWindow(ui.start, TRUE);
}

static void tick(void)
{
    ms_status s;
    char head[96], stats[5][64];
    lite_poll(&ui.run, &s, log_line, NULL);
    int tone = lite_headline(&ui.run, &s, head, sizeof head);
    if (tone != ui.tone) { ui.tone = tone; InvalidateRect(ui.head, NULL, TRUE); }
    set_text(ui.head, head);
    lite_stats(&ui.run, &s, stats);
    for (int i = 0; i < 5; i++) set_text(ui.stat[i], stats[i]);

    double frac = ui.run.cfg.seconds > 0 ? s.elapsed / ui.run.cfg.seconds : 0;
    if (frac > 1) frac = 1;
    int done = s.state == MS_STATE_DONE || s.state == MS_STATE_FAILED;
    if (ui.run.cfg.seconds > 0) {
        set_marquee(0);
        SendMessageW(ui.progress, PBM_SETPOS, (WPARAM)(frac * 1000), 0);
    } else set_marquee(s.state == MS_STATE_RUNNING);
    SendMessageW(ui.progress, PBM_SETSTATE, s.errors ? PBST_ERROR : PBST_NORMAL, 0);

    if (s.errors) taskbar(TBPF_ERROR, ui.run.cfg.seconds > 0 ? frac : 1);
    else if (done) taskbar(TBPF_NOPROGRESS, 0);
    else if (ui.run.cfg.seconds > 0) taskbar(TBPF_NORMAL, frac);
    else taskbar(TBPF_INDETERMINATE, 0);

    if (done) {
        finish();
        if (s.errors && GetForegroundWindow() != ui.win) {
            FLASHWINFO fw = { sizeof fw, ui.win, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
            FlashWindowEx(&fw);
        }
    }
}

static void on_start(void)
{
    if (ui.running) {
        lite_stop(&ui.run);
        EnableWindow(ui.start, FALSE);
        return;
    }
    char mb[32], th[32], opt[512], err[1500];
    get_text(ui.mb, mb, sizeof mb);
    get_text(ui.th, th, sizeof th);
    get_text(ui.opt, opt, sizeof opt);
    int d = (int)SendMessageW(ui.dur, CB_GETCURSEL, 0, 0);
    ms_config c;
    if (lite_make_config(&c, mb, th, lite_durations[d < 0 ? 0 : d],
                         SendMessageW(ui.stoperr, BM_GETCHECK, 0, 0) == BST_CHECKED, opt, err, sizeof err) ||
        lite_start(&ui.run, &c, log_line, NULL, err, sizeof err)) {
        wchar_t w[1500];
        MultiByteToWideChar(CP_UTF8, 0, err, -1, w, 1500);
        w[1499] = 0;
        MessageBoxW(ui.win, w, L"memslammer", MB_ICONWARNING | MB_OK);
        return;
    }
    ui.running = 1;
    set_form_enabled(FALSE);
    SetWindowTextW(ui.start, L"Stop");
    SendMessageW(ui.progress, PBM_SETPOS, 0, 0);
    SetTimer(ui.win, TIMER_POLL, 200, NULL);
    tick();
}

static void on_copy(void)
{
    int n = GetWindowTextLengthW(ui.log);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t));
    if (!g) return;
    GetWindowTextW(ui.log, GlobalLock(g), n + 1);
    GlobalUnlock(g);
    if (OpenClipboard(ui.win)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        CloseClipboard();
    } else GlobalFree(g);
}

/* ---------------- window ---------------- */

static void create_controls(void)
{
    char buf[200];
    wchar_t w[200];
    ui.sys = add(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0, ID_SYS);
    snprintf(buf, sizeof buf, "%s, %d threads, %llu MB RAM", ui.si.cpu, ui.si.logical_cpus,
             (unsigned long long)ui.si.total_mb);
    set_text(ui.sys, buf);

    ui.mb_l = add(L"STATIC", L"Memory (MB)", SS_LEFT, 0, ID_MB_L);
    swprintf(w, 200, L"%llu", (unsigned long long)(ui.si.avail_mb * 9 / 10));
    ui.mb = add(L"EDIT", w, WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_MB);
    swprintf(w, 200, L"of %llu MB free (default 90%%)", (unsigned long long)ui.si.avail_mb);
    ui.mb_hint = add(L"STATIC", w, SS_LEFT, 0, ID_MB_HINT);

    ui.th_l = add(L"STATIC", L"Threads", SS_LEFT, 0, ID_TH_L);
    swprintf(w, 200, L"%d", ui.si.logical_cpus);
    ui.th = add(L"EDIT", w, WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_TH);
    swprintf(w, 200, L"logical CPUs: %d", ui.si.logical_cpus);
    ui.th_hint = add(L"STATIC", w, SS_LEFT, 0, ID_TH_HINT);

    ui.dur_l = add(L"STATIC", L"Duration", SS_LEFT, 0, ID_DUR_L);
    ui.dur = add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, ID_DUR);
    for (int i = 0; i < lite_n_durations; i++) {
        MultiByteToWideChar(CP_UTF8, 0, lite_duration_names[i], -1, w, 200);
        SendMessageW(ui.dur, CB_ADDSTRING, 0, (LPARAM)w);
    }
    SendMessageW(ui.dur, CB_SETCURSEL, 0, 0);

    ui.opt_l = add(L"STATIC", L"Options", SS_LEFT, 0, ID_OPT_L);
    ui.opt = add(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_OPT);
    SendMessageW(ui.opt, EM_SETCUEBANNER, FALSE, (LPARAM)L"extra CLI flags, e.g. -C flush or -M chain (-h lists them)");

    ui.stoperr = add(L"BUTTON", L"Stop on first error", WS_TABSTOP | BS_AUTOCHECKBOX, 0, ID_STOPERR);
    SendMessageW(ui.stoperr, BM_SETCHECK, BST_CHECKED, 0);

    ui.head = add(L"STATIC", ui.si.avx2 ? L"Ready" : L"This CPU has no AVX2", SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 0, ID_HEAD);
    ui.tone = ui.si.avx2 ? LITE_TONE_NORMAL : LITE_TONE_BAD;
    ui.start = add(L"BUTTON", L"Start", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, ID_START);
    ui.copy = add(L"BUTTON", L"Copy log", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_COPY);
    if (!ui.si.avx2) EnableWindow(ui.start, FALSE);

    for (int i = 0; i < 5; i++) {
        ui.stat_l[i] = add(L"STATIC", stat_names[i], SS_LEFT, 0, ID_STAT_L0 + i);
        ui.stat[i] = add(L"STATIC", L"-", SS_LEFT | SS_NOPREFIX, 0, ID_STAT0 + i);
    }
    ui.progress = add(PROGRESS_CLASSW, L"", 0, 0, ID_PROGRESS);
    SendMessageW(ui.progress, PBM_SETRANGE32, 0, 1000);
    ui.log = add(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                 WS_EX_CLIENTEDGE, ID_LOG);
    SendMessageW(ui.log, EM_SETLIMITTEXT, 0, 0);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        ui.win = h;
        ui.dpi = pGetDpiForWindow ? pGetDpiForWindow(h) : 96;
        make_fonts();
        create_controls();
        EnumChildWindows(h, set_font_cb, 0);
        layout();
        SetFocus(ui.start);
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(460);
        mm->ptMinTrackSize.y = S(520);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        ui.dpi = HIWORD(wp);
        make_fonts();
        EnumChildWindows(h, set_font_cb, 0);
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        if ((HWND)lp == ui.head && ui.tone != LITE_TONE_NORMAL)
            SetTextColor(dc, ui.tone == LITE_TONE_GOOD ? RGB(0x1e, 0x8a, 0x3c) : RGB(0xc4, 0x2b, 0x1c));
        else if ((HWND)lp == ui.sys || (HWND)lp == ui.mb_hint || (HWND)lp == ui.th_hint ||
                 GetDlgCtrlID((HWND)lp) / 10 == ID_STAT_L0 / 10)
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        else SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_START || LOWORD(wp) == IDOK) { if (IsWindowEnabled(ui.start)) on_start(); return 0; }
        if (LOWORD(wp) == ID_COPY) { on_copy(); return 0; }
        break;
    case DM_GETDEFID:
        return MAKELRESULT(ID_START, DC_HASDEFID);
    case WM_TIMER:
        if (wp == TIMER_POLL && ui.running) tick();
        return 0;
    case WM_CLOSE:
        if (ui.running) {
            SetWindowTextW(ui.head, L"Stopping...");
            UpdateWindow(ui.head);
            finish();  /* stops the test and waits for the workers */
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    pGetDpiForWindow = (void *)GetProcAddress(user32, "GetDpiForWindow");
    pSystemParametersInfoForDpi = (void *)GetProcAddress(user32, "SystemParametersInfoForDpi");

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
        CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&ui.taskbar);
    ms_get_sysinfo(&ui.si);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"memslammer_lite";
    RegisterClassExW(&wc);

    wchar_t title[64];
    swprintf(title, 64, L"memslammer lite %hs", ms_version());
    /* initial size in DIPs, scaled for the primary monitor */
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(580, dpi, 96), MulDiv(640, dpi, 96), NULL, NULL, inst, NULL);
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (ui.taskbar) ITaskbarList3_Release(ui.taskbar);
    CoUninitialize();
    return 0;
}
