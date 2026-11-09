/*
 * memtimings - read the DRAM timings the memory controller is actually using,
 * so every benchmark run can log them.
 * SPDX-License-Identifier: MIT
 *
 * AMD family 17h-1Ah only for now (Zen 1-5). Reads the UMC (unified memory
 * controller) registers over SMN (system management network):
 *
 *   Windows: through the PawnIO driver (https://pawnio.eu, installed
 *            separately) and its signed AMDFamily17 module (from
 *            namazso/PawnIO.Modules, not redistributed here).
 *   Linux:   through the SMN index/data pair in the root complex's PCI config
 *            space (0x60/0x64 on 00:00.0), via sysfs. Needs root and no kernel
 *            lockdown. The kernel drives the same pair (k10temp, amd_nb) under
 *            its own lock, which we can't take, so a concurrent access could
 *            in theory see the wrong register. We only read, a few hundred
 *            times per invocation.
 *
 * Also prints the CPU Tctl temperature (TEMP line). Note that Tctl follows
 * CPU boost behaviour, not DRAM temperature. On DDR5, the DIMMs' own sensors
 * are readable on Linux through the spd5118 driver (hwmon).
 *
 * usage: memtimings [--raw] [path\to\AMDFamily17.bin]   (module path: Windows only)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;

#ifdef _WIN32
#include <windows.h>

typedef HRESULT (WINAPI *pio_open_t)(PHANDLE);
typedef HRESULT (WINAPI *pio_load_t)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT (WINAPI *pio_exec_t)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef HRESULT (WINAPI *pio_close_t)(HANDLE);

static pio_exec_t pio_exec;
static pio_close_t pio_close;
static HANDLE pio, mtx;

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

/* Prints a TIMINGS error line and returns -1 on failure. */
static int smn_open(const char *mod)
{
    char modpath[MAX_PATH];
    if (!mod) {
        GetModuleFileNameA(NULL, modpath, sizeof modpath);
        char *s = strrchr(modpath, '\\');
        strcpy(s ? s + 1 : modpath, "AMDFamily17.bin");
        mod = modpath;
    }

    HMODULE lib = LoadLibraryA("PawnIOLib.dll");
    if (!lib) lib = LoadLibraryA("C:\\Program Files\\PawnIO\\PawnIOLib.dll");
    if (!lib) { printf("TIMINGS error=no_pawnio\n"); return -1; }
    pio_open_t pio_open = (pio_open_t)GetProcAddress(lib, "pawnio_open");
    pio_load_t pio_load = (pio_load_t)GetProcAddress(lib, "pawnio_load");
    pio_close = (pio_close_t)GetProcAddress(lib, "pawnio_close");
    pio_exec = (pio_exec_t)GetProcAddress(lib, "pawnio_execute");
    if (!pio_open || !pio_load || !pio_exec || !pio_close) { printf("TIMINGS error=bad_pawnio\n"); return -1; }

    SIZE_T blob_n;
    unsigned char *blob = read_file(mod, &blob_n);
    if (!blob) { printf("TIMINGS error=no_module path=%s\n", mod); return -1; }
    HRESULT hr = pio_open(&pio);
    if (FAILED(hr)) { printf("TIMINGS error=open hr=0x%08lx\n", (unsigned long)hr); return -1; }
    hr = pio_load(pio, blob, blob_n);
    if (FAILED(hr)) { printf("TIMINGS error=load hr=0x%08lx\n", (unsigned long)hr); return -1; }

    /* Shared with other hardware monitors that drive the same PCI index/data pair. */
    mtx = CreateMutexA(NULL, FALSE, "Global\\Access_PCI");
    if (mtx) WaitForSingleObject(mtx, 2000);
    return 0;
}

static void smn_close(void)
{
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    pio_close(pio);
}

#else /* Linux */
#include <fcntl.h>
#include <unistd.h>

#define ROOT_DEV "/sys/bus/pci/devices/0000:00:00.0/"
static int pcifd = -1;

static int smn_read(u32 addr, u32 *val)
{
    if (pwrite(pcifd, &addr, 4, 0x60) != 4) return -1;
    return pread(pcifd, val, 4, 0x64) == 4 ? 0 : -1;
}

static int smn_open(const char *mod)
{
    (void)mod;
    char vendor[16] = "";
    FILE *f = fopen(ROOT_DEV "vendor", "r");
    if (f) { if (!fgets(vendor, sizeof vendor, f)) vendor[0] = 0; fclose(f); }
    if (strncmp(vendor, "0x1022", 6)) { printf("TIMINGS error=not_amd\n"); return -1; }
    pcifd = open(ROOT_DEV "config", O_RDWR);
    if (pcifd < 0) { printf("TIMINGS error=open_pci_config need=root\n"); return -1; }
    return 0;
}

static void smn_close(void) { close(pcifd); }
#endif

static u32 bits(u32 v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

/* UMC channel register blocks on SMN. Offsets below are relative to these. */
static const u32 umc_base[] = { 0x050000, 0x150000 };
#define REG_FIRST 0x200
#define REG_LAST  0x2fc
#define R(ch, off) regs[ch][((off) - REG_FIRST) / 4]

/* UMC config register. Bits 2:0 are the DRAM type on Zen 4 and later
 * (1 = DDR5), the same check Linux amd64_edac makes; DDR4 UMCs read 0. */
#define UMC_CFG 0x100

/* Field layout for DDR4 Zen 2/3 UMCs, checked on a 5900X against the BIOS
 * values (cl/rcd) and the DDR4 7.8 us refresh interval (refi).
 *
 * Zen 4 DDR5 (checked on a 7800X3D against SPD and BIOS values) keeps this
 * layout for every field except rfc, and the clock word changes:
 *   0x200 bits 15:0  MEMCLK in MHz (0x0bb8 = 3000 for DDR5-6000)
 *   0x260..0x26c     four tRFC registers, bits 15:0 tRFC1, 31:16 tRFC2. Only
 *                    one holds the programmed value; the others keep a reset
 *                    value with a much smaller tRFC1, so the largest wins.
 *   0x2c0..0x2cc     the same for tRFCsb, bits 10:0.
 * Cmd2T and gear-down moved out of 0x200 bits 10/11; where they went isn't
 * confirmed yet, so they are not printed for DDR5. */
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
    if (smn_open(mod)) return 1;

    u32 regs[2][(REG_LAST - REG_FIRST) / 4 + 1], cfg[2];
    int ok[2] = { 1, 1 };
    for (int ch = 0; ch < 2; ch++) {
        if (smn_read(umc_base[ch] + UMC_CFG, &cfg[ch])) ok[ch] = 0;
        for (u32 off = REG_FIRST; off <= REG_LAST; off += 4)
            if (smn_read(umc_base[ch] + off, &regs[ch][(off - REG_FIRST) / 4])) ok[ch] = 0;
    }

    /* CPU Tctl (reported temperature control), as documented by Linux k10temp:
     * bits 31:21 in 1/8 degC, bit 19 selects the -49 degC range. */
    u32 thm;
    int thm_ok = !smn_read(0x00059800, &thm);

    smn_close();

    if (thm_ok)
        printf("TEMP tctl=%.1f\n", (thm >> 21) * 0.125 - ((thm >> 19) & 1 ? 49 : 0));

    for (int ch = 0; ch < 2; ch++) {
        if (!ok[ch]) { printf("TIMINGS ch=%d error=read\n", ch); continue; }
        int ddr5 = bits(cfg[ch], 2, 0) == 1;
        if (raw) {
            printf("RAW ch=%d %03x=%08x", ch, UMC_CFG, cfg[ch]);
            for (u32 off = REG_FIRST; off <= REG_LAST; off += 4)
                printf(" %03x=%08x", off, regs[ch][(off - REG_FIRST) / 4]);
            printf("\n");
        }
        printf("TIMINGS ch=%d type=%s", ch, ddr5 ? "ddr5" : "ddr4");
        u32 rfc = 0, rfcsb = 0;
        if (ddr5)
            for (u32 off = 0; off < 16; off += 4) {
                if (bits(R(ch, 0x260 + off), 15, 0) > bits(rfc, 15, 0)) rfc = R(ch, 0x260 + off);
                if (bits(R(ch, 0x2c0 + off), 10, 0) > rfcsb) rfcsb = bits(R(ch, 0x2c0 + off), 10, 0);
            }
        for (const field_t *f = fields; f->name; f++) {
            if (ddr5 && !strcmp(f->name, "rfc"))
                printf(" rfc=%u rfc2=%u rfcsb=%u", bits(rfc, 15, 0), bits(rfc, 31, 16), rfcsb);
            else
                printf(" %s=%u", f->name, bits(R(ch, f->off), f->hi, f->lo));
        }
        if (ddr5) {
            printf(" mts=%u\n", bits(R(ch, 0x200), 15, 0) * 2);
        } else {
            u32 ratio = bits(R(ch, 0x200), 6, 0);
            printf(" mts=%u cmd=%dT gdm=%d\n", (ratio * 200 + 1) / 3,
                   bits(R(ch, 0x200), 10, 10) ? 2 : 1, (int)bits(R(ch, 0x200), 11, 11));
        }
    }
    return 0;
}
