# Karhu RAM Test 1.0.0.1: how it works

Notes from studying the licensed copy on the test machine (x64 `ramtest.dll`,
driven by a .NET GUI). This describes the algorithm in our own words; no Karhu
code or binaries are included in this repository.

## Architecture

- `ramtest.exe` is a thin WinForms GUI. It P/Invokes four exports of
  `ramtest.dll`: `StartTest(int megabytes, int threads)`, `StopTest()`,
  `GetCoverage()` and `GetErrorCount()`.
- GUI defaults: megabytes = 90% of *available* physical memory, threads =
  `Environment.ProcessorCount` (logical CPUs, so 24 on a 5900X).
- `StartTest` clamps threads to 1..64, does a single `VirtualAlloc`
  (`MEM_COMMIT|MEM_RESERVE`, `PAGE_READWRITE`, normal 4 KiB pages, no large
  pages, no locking), and starts one control thread that opens an OpenMP
  parallel region (`vcomp140`) with N threads.
- Each thread owns one contiguous slice of `total / N` bytes. Slices never
  overlap and threads never touch each other's memory.
- The work unit is the **OS page size** (4096 bytes, from `GetSystemInfo`).

## Primitives (CPU-dispatched at DLL load: SSE2 / AVX / AVX2)

All of them operate on one 4 KiB block, in place, as unaligned 64-bit lanes:

| id | op       | effect on each u64                        |
|----|----------|-------------------------------------------|
| 0  | add      | `x += k`                                  |
| 1  | sub      | `x -= k`                                  |
| 2  | xor      | `x ^= k`                                  |
| 3  | not      | `x = ~x`                                  |
| -  | checksum | Fletcher-64 over u32 words (mod 2^32-1)   |
| -  | flush    | `mfence; clflushopt` (or `clflush`) on every cache line of the block; `mfence` |

The AVX2 path uses plain 256-bit `vmovdqu` loads and stores, not streaming
(non-temporal) stores. Cache bypass comes entirely from the explicit flush.

## Main loop (per thread)

```
loop forever:
    fill every 4 KiB block of my slice with RtlGenRandom (crypto RNG) output
    flush(slice); C0 = fletcher64(slice)

    repeat 4 times:                         # forward passes
        k = C0
        for block b in slice, ascending:
            apply op[k & 3](b, k)           # add/sub/xor/not using key k
            flush(b)
            k = fletcher64(b)               # read back from DRAM, chained key
            Sleep(0)

    repeat 4 times:                         # reverse passes
        k = C0
        for block b in slice, ascending:
            flush(b)
            c = fletcher64(b)               # key the forward pass derived here
            apply inverse op[k & 3](b, k)   # add<->sub, xor/not self-inverse
            k = c
            Sleep(0)

    flush(slice)
    if fletcher64(slice) != C0: errors += 1
```

Each reverse pass exactly undoes the most recent outstanding forward pass,
because the key for block *b+1* is always the checksum of block *b* after its
transform. After 4 + 4 passes memory should be bit-identical to the random
fill, and one whole-slice checksum checks it.

## What this means for stress

1. **Every block makes a write-flush-read round trip to DRAM.** Each block is
   read-modify-written (about 4 KiB of loads and stores), every line is
   explicitly flushed, then immediately re-read for the checksum. Data never
   lives in cache long enough to hide a DRAM error. It also forces constant
   read↔write bus turnarounds (tWTR/tRTW/tWR and friends) on the same open
   rows: 64 lines of one 4 KiB block, which mostly fall in the same DRAM row.
2. **Random data, re-randomised every cycle.** Crypto-random fill, then
   add/sub with carry propagation, so the bit patterns on the bus are close to
   maximum entropy and change every pass. That's good for SI/crosstalk/SSO
   style failures, and it never settles into a pattern that an unstable
   setting happens to tolerate.
3. **All logical CPUs at once, each on its own slice.** With 24 threads walking
   different regions, the memory controller sees many concurrent streams
   scattered across banks/ranks, which means lots of bank conflicts and
   page-misses mixed with row hits inside each block.
4. **Data-dependent control flow.** The op choice and the key come from the
   checksum of data just read back from DRAM, so a single flipped bit
   corrupts the rest of the forward chain and is amplified, never masked.
5. **`Sleep(0)` after every block.** It yields 1M+ times per second across
   threads, so there's constant scheduler churn and thread migration. It also
   produces small power and current transients on the CPU/IMC, which may
   matter for marginal VSOC/VDDP/IMC settings.

## Weaknesses / things it does not do

- Error detection happens only **once per full cycle** (fill + 9 sweeps of the
  slice). The latency to a detected error is a full cycle, and you get no
  address, bit, or count information (at most +1 per thread per cycle).
- No address-pattern variety: blocks are always walked in ascending order,
  and lines within a block are always sequential. No strided, random,
  row-hammer-like, or bank-targeted access.
- No classic coupling-fault patterns (moving inversions, walking bits,
  checkerboards). Randomness stands in for all of them.
- No large pages, so the TLB and page walks add some overhead. Physical
  placement is whatever the OS gives.
- Fletcher-64 can in theory miss specific error combinations; negligible in
  practice.

## Version 1.1.0.0 (studied 2026-11-08)

`ramtest.dll` is never in the download. The activation server returns it with
`license.key`, and `license.key` is bound to the app version, so each new GUI
version needs a re-activation before it runs.

**With default settings 1.1 runs exactly the 1.0.0.1 test.** The worker loop,
op table, checksum, pass counts and coverage accounting are unchanged. The
release adds three optional knobs, which the GUI sets through new exports
(`SetCpuCache(int)`, `SetRng(int)`, `SetStressFpu(bool)`) right before
`StartTest`:

| setting   | values                    | what it actually does                           |
|-----------|---------------------------|-------------------------------------------------|
| CPU cache | 0 Disabled                | allocation is `PAGE_NOCACHE`; no flushes        |
|           | 1 Write-combine           | allocation is `PAGE_WRITECOMBINE`; no flushes   |
|           | **2 Default**             | normal pages + clflush(opt) per block (= 1.0.0.1) |
|           | 3 Enabled                 | normal pages, no flushes at all (like our `chain -F`) |
| RNG       | **0 Default**             | RtlGenRandom per 4 KiB page (= 1.0.0.1)          |
|           | 1 XORWOW                  | Marsaglia xorwow fills the slice as u32 words; state seeded once per thread from RtlGenRandom |
| Stress FPU| **off** / on              | extra FP work inside each block step (below)     |

- **The flush is now gated on mode 2.** In "Enabled" mode the forward
  transform and checksum run in cache, so with a 4 KiB block most of the
  traffic never reaches DRAM until eviction. Expect more coverage per second
  but less DRAM read-after-write.
- **Stress FPU doesn't add threads.** Each worker runs a short FP burst
  right after transforming a block (forward passes) and right before
  checksumming one (reverse passes): 64 iterations of a chain over 8 FP vectors
  (scalar, xmm or ymm doubles depending on the CPU) of mul, add, div, then sub, each depending on
  its neighbour. It's latency-bound and div-heavy rather than a throughput
  power virus, and it also inserts a delay between a block's write and its
  read-back. The GUI warns it can damage the CPU without enough cooling, and
  the README pitches it at memory-controller instability that depends on
  temperature.
- Also new: the log records "ERROR DETECTED!" and the settings used, and
  telemetry includes them.

## Throughput model

Per cycle each slice is touched about 1 (fill) + 1 (checksum) + 8 × (RMW +
read) + 1 (final checksum) times, so roughly 25 slice-sized transfers per
cycle. All flushes write back dirty lines, so write traffic is about as large
as read traffic.
