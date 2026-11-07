# TestMem5 v0.13.1 + anta777 "Absolut": how it works

Notes from studying TestMem5 (CoolCmd's maintained v0.13.1 build: `TM5.exe`,
`TM5.dll`, test module `bin/MT0.dll`) and the bundled
`Absolut @ anta777.cfg`. These are descriptions in our own words, and no TM5
code is included here. Unless a point is marked *(inferred)*, it was read
directly from the code.

## Architecture

- `TM5.exe` is the host and sequence runner. `TM5.dll` is obfuscated and
  self-patching (thread counts and similar values are written into
  immediates at runtime). It hooks MT0's imports to trap errors and size
  working sets.
- `MT0.dll` (about 7.5 KB of hand-written 32-bit asm) holds the tests. Its
  exports are `SimpleTest`, `RefreshStable`, `MirrorMove`, `MirrorMove128`,
  `BlockMove` (an empty stub) and `MemManager`. Each is driven by small
  numbered commands (reset, fill, run, ...) through a per-test 128-byte slot
  holding the cfg values.
- Memory is allocated through **AWE**: `AllocateUserPhysicalPages` gives
  locked physical pages, and they are mapped `Testing Window Size` MB at a
  time into one fixed virtual window. TM5 grants itself `SeLockMemoryPrivilege`
  if needed. Without AWE it falls back to `VirtualAlloc` + `VirtualLock`.
- Each core gets its own pool and its own copy of the test slots, so cores
  split memory and never share blocks *(inferred: per-core processes with a
  shared mapping)*. Phase changes are barrier-synchronised across all
  workers, so every core starts each test at the same moment.

## Data: no RNG at all

Every pattern is derived from the **virtual address** of the 4 KiB page:

```
d = ((addr >> 12) + (addr >> 28)) & 0xFFFF;   a = ~d
48-byte seed: beat i (8 bytes = 4 x u16) is d or a alternating per beat,
              each u16 rotated left 1 bit more than the previous one
```

Consecutive 64-bit bus beats are bitwise complements, so **every DQ line
toggles on every beat**, and the rotation shifts the pattern across the byte
lanes. It's a deterministic worst case for signal integrity.

Pattern Mode changes the seed row by row:

- **0**: none. Every 64 B row of a block holds the same line.
- **1**: XOR with fixed constants, then add a fixed increment per store. (The
  constants are read from the PE import directory, which is apparently a
  build bug, but they behave as fixed constants.)
- **2**: nonlinear 16-bit multiply chain (`pmullw`) using Param0/Param1 as
  16-bit multipliers (Absolut uses 0x7777 / 0x3333). The sequence is
  deterministic, so verify replays it exactly.

Because the virtual window is fixed under AWE, **every window gets identical
data**. Address-aliasing faults between windows can't show up in the data.

## Access instructions

Only temporal 128-bit `movaps` loads and stores, plus `prefetchnta` 8 strides
ahead. There are no non-temporal stores, no `clflush`, no fences, no locked
ops, and no `rep movs` anywhere. Cache is defeated by working-set size
(blocks ≥ 4 MB per core, ~1 GB windows), not by explicit flushes.

## Tests

### SimpleTest (write then verify R times)

- Row = 64 B. `Parameter k` sets a stride of `k × channels × 64 B` (k = 0/1:
  linear). With stride > 64 B the block is covered in `stride/64`
  interleaved phases, taken in descending order.
- Write the whole block, then **re-read and verify it R times**, where
  `R = max(1, test_time% × global_time% / 2000)`. Verification
  XOR-accumulates into a 128-bit mask and stops at the first bad pass.
- It's nominally 4 passes, but all 4 seed identically (the pass index only
  perturbs bits < 12, which the generator ignores).
- **Parameter 256** (T15): touches 64 B every 32 KiB in 512 phases, which
  likely means a new DRAM row on almost every access *(inferred)*: maximum
  activate/precharge pressure (tRCD/tRP/tRAS/tRC/tRRD/tFAW).

### RefreshStable (retention background check)

Fills or verifies every 4 KiB page with the address pattern. There is no
sleep. The host runs it **implicitly around every other test**: before each
test runs on a block, that block's RefreshStable data, written one whole test
earlier, is verified, and afterwards the block is refilled. Data therefore sits
idle for seconds to minutes while the rest of memory is hammered. That
catches tREFI/tRFC that are too loose or too tight, and disturbance or leakage.

### MirrorMove / MirrorMove128 (traffic generators, no compare)

Swap chunks from the front of a region with their mirror chunk at the back,
two pointers converging and then crossing, so each repetition restores memory
exactly. Nothing is compared inside the test. Any corruption shows up in the
following RefreshStable verify.

- `MirrorMove` Parameter 4: quarters, **8 concurrent streams** (4 up, 4
  down), 16 B units. Parameter 2/3 give 4/6 streams. The default is 2 streams
  of 64 B with the lanes reversed.
- `MirrorMove128` Parameter p: 128 B units, `p+1` interleaved phases (stride
  `128·(p+1)`).

These produce heavy read/write mixing, opposing streams, and bank conflicts:
stress on bus turnaround and controller scheduling.

## Errors

The test functions return 0 and leave an OR-mask of failing bit positions.
The host logs "error in test #t". No address or count is kept, so the
granularity is one error per (test, block).

## The Absolut profile

Global Time% 1670, 3 cycles, 1 GB window.

| T  | function        | detail                              | block       | R   |
|----|-----------------|-------------------------------------|-------------|-----|
| 1  | SimpleTest m2   | 0x7777/0x3333 multiply chain        | 4 MB        | 200 |
| 2  | SimpleTest m1   | linear                              | window      | 6   |
| 3  | MirrorMove128   | 3 phases                            | window      | 16  |
| 4  | MirrorMove      | 8 streams                           | window      | 16  |
| 5  | MirrorMove128   | 2 phases                            | window      | 16  |
| 6–12 | SimpleTest m0 | linear                              | 4…256 MB    | 200…6 |
| 14 | SimpleTest m0   | linear                              | window      | 6   |
| 15 | SimpleTest m0   | 32 KiB stride, 512 phases           | window      | 6   |

Sequence: `1,4,6,15`, then six groups of `[mirror, 2, SimpleTest(next block
size), 15]` with the mirror rotating 3→5→4 and the block growing 8→256 MB,
then `5,14,15`.

Note the R × block ≈ constant (~800 MB re-read per block): small blocks are
re-read hundreds of times while still partly cache/row-buffer hot, and large
blocks are re-read a few times through DRAM.

## Why it finds OC errors fast *(inferred)*

1. **Every-beat full DQ toggle data** gives worst-case SSO/crosstalk/ISI,
   which stresses drive strength, ODT, VDDQ and the IMC's receivers.
2. **Write then immediately re-read many times**, on all cores, stresses
   write-to-read turnaround, and on Ryzen also the fabric (FCLK/UCLK).
3. **T15 between every group** is a row-miss storm: every access is an
   activate, which tests tRCD/tRP/tRAS/tRC/tRRD/tFAW and transient power.
4. **Mirror tests** produce opposing multi-stream read/write mixes, which
   cause bank conflicts and turnaround stress.
5. **The implicit retention check around everything** catches refresh-timing
   problems (tRFC/tREFI) under neighbouring load.
6. **Barrier-synchronised phase changes** produce sharp, simultaneous load
   steps across all cores.

## Weaknesses

- The data space is tiny: mode 0 repeats one 64 B line per block, the passes
  are identical, and every window has the same data. Data-dependent faults
  outside these patterns are never exercised.
- No explicit cache control. Small-block tests are partly cache-served.
- Detection is coarse: one flag per (test, block), no address.
- 32-bit and SSE-only: 128-bit accesses only, no AVX2/AVX-512 width.

## Contrast with Karhu (see karhu.md)

| | Karhu | TM5 Absolut |
|---|---|---|
| data | crypto-random, re-randomised every cycle, add/sub carry mixing | deterministic address pattern, full DQ toggle per beat |
| cache | explicit `clflushopt` on every 4 KiB block | none; big working sets |
| access | sequential within a 4 KiB block, blocks ascending | linear, strided (row-miss), mirrored multi-stream |
| verify | chained checksum, once per cycle | XOR compare, R rereads, plus retention check |
| scheduling | `Sleep(0)` after every block, free-running threads | barrier-synced phases |

So the two tools take nearly opposite approaches. Karhu relies on entropy plus
a forced DRAM round trip, while TM5 relies on worst-case deterministic
patterns, access-order variety and retention. The benchmark phase should show
which properties matter for which timings.
