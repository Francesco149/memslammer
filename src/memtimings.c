/*
 * memtimings - read the DRAM timings the memory controller is actually using,
 * so every benchmark run can log them.
 * SPDX-License-Identifier: MIT
 *
 * AMD family 17h-1Ah only for now (Zen 1-5). Reads the UMC (unified memory
 * controller) registers over SMN through the PawnIO driver
 * (https://pawnio.eu, installed separately) and its signed AMDFamily17
 * module (from namazso/PawnIO.Modules, not redistributed here).
 *
 * Also prints the CPU Tctl temperature (TEMP line). Note that Tctl follows
 * CPU boost behaviour, not DRAM temperature.
 *
 * usage: memtimings [--raw] [path\to\AMDFamily17.bin]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;

typedef HRESULT (WINAPI *pio_open_t)(PHANDLE);
typedef HRESULT (WINAPI *pio_load_t)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT (WINAPI *pio_exec_t)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef HRESULT (WINAPI *pio_close_t)(HANDLE);

static pio_exec_t pio_exec;
static HANDLE pio;

static int smn_read(u32 addr, u32 *val)
{
    ULONG64 in = addr, out = 0;
    SIZE_T n = 0;
    HRESULT hr = pio_exec(pio, "ioctl_read_smn", &in, 1, &out, 1, &n);
    if (FAILED(hr) || n != 1) return -1;
    *val = (u32)out;
    return 0;
}

static unsigned char *read_file(const char *path, SIZE_T *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n);
    if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *size = n;
    return b;
}

static u32 bits(u32 v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

/* UMC channel register blocks on SMN. Offsets below are relative to these. */
static const u32 umc_base[] = { 0x050000, 0x150000 };
#define REG_FIRST 0x200
#define REG_LAST  0x2fc
#define R(ch, off) regs[ch][((off) - REG_FIRST) / 4]

/* Field layout for DDR4 Zen 2/3 UMCs, checked on a 5900X against the BIOS
 * values (cl/rcd) and the DDR4 7.8 us refresh interval (refi). */
typedef struct { const char *name; u32 off; int hi, lo; } field_t;
static const field_t fields[] = {
    { "cl",    0x204,  5,  0 }, { "rcdrd", 0x204, 21, 16 }, { "rcdwr", 0x204, 29, 24 },
    { "rp",    0x208, 21, 16 }, { "ras",   0x204, 14,  8 }, { "rc",    0x208,  7,  0 },
    { "rrds",  0x20c,  4,  0 }, { "rrdl",  0x20c, 12,  8 }, { "faw",   0x210,  7,  0 },
    { "wtrs",  0x214, 12,  8 }, { "wtrl",  0x214, 22, 16 }, { "wr",    0x218,  7,  0 },
    { "rtp",   0x20c, 28, 24 }, { "cwl",   0x214,  5,  0 }, { "rfc",   0x260, 10,  0 },
    { "refi",  0x230, 15,  0 },
    { 0 }
};

int main(int argc, char **argv)
{
    int raw = 0;
    const char *mod = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--raw")) raw = 1;
        else mod = argv[i];
    }
    char modpath[MAX_PATH];
    if (!mod) {
        GetModuleFileNameA(NULL, modpath, sizeof modpath);
        char *s = strrchr(modpath, '\\');
        strcpy(s ? s + 1 : modpath, "AMDFamily17.bin");
        mod = modpath;
    }

    HMODULE lib = LoadLibraryA("PawnIOLib.dll");
    if (!lib) lib = LoadLibraryA("C:\\Program Files\\PawnIO\\PawnIOLib.dll");
    if (!lib) { printf("TIMINGS error=no_pawnio\n"); return 1; }
    pio_open_t pio_open = (pio_open_t)GetProcAddress(lib, "pawnio_open");
    pio_load_t pio_load = (pio_load_t)GetProcAddress(lib, "pawnio_load");
    pio_close_t pio_close = (pio_close_t)GetProcAddress(lib, "pawnio_close");
    pio_exec = (pio_exec_t)GetProcAddress(lib, "pawnio_execute");
    if (!pio_open || !pio_load || !pio_exec || !pio_close) { printf("TIMINGS error=bad_pawnio\n"); return 1; }

    SIZE_T blob_n;
    unsigned char *blob = read_file(mod, &blob_n);
    if (!blob) { printf("TIMINGS error=no_module path=%s\n", mod); return 1; }
    HRESULT hr = pio_open(&pio);
    if (FAILED(hr)) { printf("TIMINGS error=open hr=0x%08lx\n", (unsigned long)hr); return 1; }
    hr = pio_load(pio, blob, blob_n);
    if (FAILED(hr)) { printf("TIMINGS error=load hr=0x%08lx\n", (unsigned long)hr); return 1; }

    /* Shared with other hardware monitors that drive the same PCI index/data pair. */
    HANDLE mtx = CreateMutexA(NULL, FALSE, "Global\\Access_PCI");
    if (mtx) WaitForSingleObject(mtx, 2000);

    u32 regs[2][(REG_LAST - REG_FIRST) / 4 + 1];
    int ok[2] = { 1, 1 };
    for (int ch = 0; ch < 2; ch++)
        for (u32 off = REG_FIRST; off <= REG_LAST; off += 4)
            if (smn_read(umc_base[ch] + off, &regs[ch][(off - REG_FIRST) / 4])) ok[ch] = 0;

    /* CPU Tctl (reported temperature control), as documented by Linux k10temp:
     * bits 31:21 in 1/8 degC, bit 19 selects the -49 degC range. */
    u32 thm;
    int thm_ok = !smn_read(0x00059800, &thm);

    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    pio_close(pio);

    if (thm_ok)
        printf("TEMP tctl=%.1f\n", (thm >> 21) * 0.125 - ((thm >> 19) & 1 ? 49 : 0));

    for (int ch = 0; ch < 2; ch++) {
        if (!ok[ch]) { printf("TIMINGS ch=%d error=read\n", ch); continue; }
        if (raw) {
            printf("RAW ch=%d", ch);
            for (u32 off = REG_FIRST; off <= REG_LAST; off += 4)
                printf(" %03x=%08x", off, regs[ch][(off - REG_FIRST) / 4]);
            printf("\n");
        }
        printf("TIMINGS ch=%d", ch);
        for (const field_t *f = fields; f->name; f++)
            printf(" %s=%u", f->name, bits(R(ch, f->off), f->hi, f->lo));
        u32 ratio = bits(R(ch, 0x200), 6, 0);
        printf(" mts=%u cmd=%dT gdm=%d\n", (ratio * 200 + 1) / 3,
               bits(R(ch, 0x200), 10, 10) ? 2 : 1, (int)bits(R(ch, 0x200), 11, 11));
    }
    return 0;
}
