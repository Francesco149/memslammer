# Benchmark results

All runs are on cutestation: Ryzen 9 5900X, 2x8 GB DDR4 at 3600, deliberately
unstable with **tCL 14 / tRCD 14** (label `3600 c14 rcd14`). Raw records live
in `results/runs.jsonl`; `harness/bench.py summary` aggregates them.

## 2026-11-06: first baseline and ablations

| tester / config                              | n  | median TTE | range       |
|----------------------------------------------|----|-----------:|-------------|
| Karhu                                        | 3  | 110 s      | 45–405 s    |
| TM5 Absolut                                  | 13 | 27 s       | 8–321 s     |
| memslammer `chain` (cold box, early batches) | 5  | 66 s       | 15–211 s    |
| memslammer `chain` (hot box, interleaved)    | 3  | 7.8 s      | 7.7–15 s    |
| `verify` mul pattern, cache none/nta         | 3  | no error in 15 min each | |
| `verify` mul pattern, flush                  | 3  | no error in 10 min each | |
| `verify` random, flush                       | 5  | 20 s       | 5–40 s      |
| `verify` random, no flush                    | 3  | 26 s       | 5–45 s      |
| `verify` random, flush, 1 re-read            | 3  | 15 s       | 10–22 s     |

Findings:

- **Data entropy is what matters for this fault.** The same fill and re-verify
  loop never failed with low-entropy deterministic data (a toggle template or
  a 16-bit multiply chain), and failed in seconds with random data.
- Almost every TM5 failure (12 of 13) came from Absolut test 1. Our own
  multiply-chain pattern is not the same as TM5's, so this doesn't say that
  TM5's pattern is weak, only that ours is.
- `prefetchnta` doesn't stop re-reads hitting L3 on Zen 3: verify with
  `-C nta` ran at ~194 GB/s, the same as no cache control. Only `clflushopt`
  forces DRAM traffic (~36 GB/s).
- **Temperature/warm-up dominates time-to-error.** The same config ran ~10x
  faster after the box had been loaded for a while. Only compare interleaved
  runs. With n=3, flush vs no flush vs re-read count are not distinguishable.

## 2026-11-06: rate mode after a 10 min warm-up

Box warmed with 600 s of `chain` (its first error came only at 416 s from
cold), then 3 interleaved rounds of 120 s `--rate` runs, back to back.

| config                          | errors/min (3 runs) | first error (s)  |
|---------------------------------|---------------------|------------------|
| `verify -P random -C none`      | 16.0, 12.0, 19.5    | 12.8, 2.3, 9.3   |
| `verify -P random -C flush`     | 6.0, 8.0, 10.0      | 1.5, 11.5, 5.7   |
| `verify -P random -C flush -r 1`| 4.5, 5.5, 8.0       | 24.5, 16.8, 4.5  |
| `chain`                         | 3.5, 4.5, 7.5       | 21.0, 7.8, 7.8   |
| Karhu                           | 7.5, 8.0, 9.5       | 7.7, 7.5, 7.5    |

- **Warm, everything fails in seconds; cold, it takes minutes.** From cold,
  time-to-first-error mostly measures how fast a test heats the DIMMs, so
  compare warm runs and treat cold time-to-error as a separate metric.
- Karhu's and `chain`'s first error lands at ~7.5 s in nearly every warm run.
  That's the length of one cycle, which is when they first verify anything.
  Their warm first-error time is bounded by cycle length, not sensitivity.
  `verify` checks every 4 MiB pass.
- Within `verify`, which counts one error per failing block fill in every
  variant, **no flush gives about 2x the error rate of flush**, and 200
  re-reads beat 1. Rates aren't comparable across testers, because each
  counts errors in different units.

## 2026-11-06/07: cold-start race

Each run started after 15 min idle (`--idle 900`) and stopped at its first
error. 5 contenders, 3 rounds, order rotated each round. Times in seconds.

| contender                       | round 0 | round 1 | round 2 | median |
|---------------------------------|--------:|--------:|--------:|-------:|
| **`verify`** (random, no flush) | **394** | **218** | **114** | **218** |
| `verify -C flush`               | 609     | 231     | 242     | 242    |
| Karhu                           | 568     | 368     | 320     | 368    |
| `chain`                         | 682     | 411     | 160     | 411    |
| TM5 Absolut                     | 443     | 418     | 319     | 418    |

- `verify` was fastest in every round. If all five were equally fast, the
  odds of one winning all three rounds would be about 1 in 125. Its median is
  ~1.7x faster than Karhu and ~1.9x faster than TM5.
- **15 min idle isn't fully cold.** Idle Tctl at run start climbed from ~46
  to 51–57 °C over the night, and every contender got faster round by round.
  The rotation keeps the comparison fair, but absolute times depend on how
  long the box has been in use.
- TM5 heats the CPU the most (Tctl 85–89 °C at the end versus ~80 °C for the
  others) and still isn't fastest, so CPU heat alone doesn't explain
  time-to-error.

## 2026-11-07: tuning `verify`, 5 min idle

The standard protocol from here on is `--idle 300`: about the time an
overclocker spends in UEFI before retesting. One change per variant from the
default (4 MiB blocks, 200 re-reads, 24 threads). 3 rotated rounds, seconds.

| variant          | runs                   | median | range   |
|------------------|------------------------|-------:|---------|
| `-b 16 MiB`      | 97, 88, 130            | **97** | 88–130  |
| `-r 2000`        | 107, 243, 95           | 107    | 95–243  |
| default (4 MiB)  | 236, 55, 113           | 113    | 55–236  |
| `-r 20`          | 115, 122, 56           | 115    | 56–122  |
| `-b 1 MiB`       | no error in 15 min, x3 | –      |         |
| `-t 12`          | no error in 15 min, x3 | –      |         |

- **The working set must far exceed L3.** Both variants that never failed
  shrink it below the 32 MB L3 of each CCD: 12 threads x 1 MiB, or 6 threads
  x 4 MiB per CCD. The re-reads then stay in cache and DRAM sees almost no
  reads. The smaller thread count probably hurts for that reason, not
  because SMT matters in itself.
- 16 MiB blocks had the best median and the narrowest spread. Re-read count
  makes little difference between 20 and 2000.

## 2026-11-07: block size and the L3 check, 5 min idle

| variant              | runs           | median | range   |
|----------------------|----------------|-------:|---------|
| `-b 16 MiB` (6 runs) | 108, 106, 109 (+97, 88, 130) | 107 | 88–130 |
| `-b 256 MiB`         | 114, 102, 90   | 102    | 90–114  |
| `-b 64 MiB`          | 165, 244, 151  | 165    | 151–244 |
| `-t 12 -b 16 MiB`    | 74, 244, 205   | 205    | 74–244  |

- **The L3 explanation holds.** 12 threads never failed with 4 MiB blocks
  (24 MB per CCD), but do fail with 16 MiB blocks (96 MB per CCD). They're
  still slower than 24 threads.
- **Block size isn't monotonic**: 64 MiB is clearly worse than 16 and 256
  (its best run is slower than every 16 MiB run). The cause isn't known yet.
  256 MiB covers only half the memory here, because the 490 MB per-thread
  slice holds just one whole block.
- **16 MiB is now the `verify` default**: best combination of median and
  consistency across 6 runs.

## 2026-11-08: re-read order, 5 min idle

`verify -A shuffle` re-reads the 4 KiB pages of each 16 MiB block in a new
pseudo-random order on every pass, to open more DRAM rows per second than
in-order streaming. It runs at ~38 GB/s against ~57 GB/s in order. 4 rounds,
order alternated each round, seconds.

| variant          | runs                | median | range   |
|------------------|---------------------|-------:|---------|
| default (seq)    | 397, 341, 355, 374  | **365**| 341–397 |
| `-A shuffle`     | 311, 494, 459, 412  | 435    | 311–494 |

- **Shuffled order doesn't help.** It has the worse median and the wider
  spread, and it heats the CPU less (end Tctl ~72–76 °C against ~75 °C).
  Seq stays the default; `-A shuffle` stays available as an option.
- **The baseline moved ~3x between days.** The same default config failed in
  88–130 s on 2026-11-07 and in 341–397 s today. Start Tctl was ~47 °C in
  every run today. Compare only runs within one batch, and re-run a control
  in every batch.

## 2026-11-08: FP heat burst (Karhu 1.1 "Stress FPU" idea), 5 min idle

`verify -H 64` runs 64 iterations of an 8-vector AVX mul/add/div chain after
every 4 KiB read or written. At 64 the box still reads at the full ~57 GB/s;
at 128 it drops to 40 GB/s and at 256 to 24 GB/s. 4 rounds, order alternated
each round, seconds.

| variant          | runs                    | median | start / end Tctl (°C) |
|------------------|-------------------------|-------:|-----------------------|
| default          | 308, 253, 667, 143      | **280**| 47–55 / 75–80         |
| `-H 64`          | 475, 245, 332, 598      | 404    | 47–50 / 76–79         |

- **No gain.** The burst didn't raise end Tctl measurably either: a
  memory-bound run already sits at roughly the same CPU temperature.
  Extra FP work at full bandwidth doesn't heat the memory controller or
  DIMMs faster. `-H` stays, default 0.
- The fastest run in the batch (143 s) is the only one that started warm
  (55 °C after a long session). Starting temperature still outweighs every
  tuning knob tried today.
- Default spread today is 143–667 s. With n=4, differences under ~1.5x in
  median are noise.

## 2026-11-09: Linux, 5 min idle

The same box, booted from a live Ubuntu 26.04 USB (kernel 7.0, amd-pstate-epp,
`powersave` governor with EPP `balance_performance`). The memory controller
reported the same OC: `3600 c14 rcd14 rp16 ras36 rfc324`. Default `verify`
(16 MiB blocks) with the test size pinned to 10900 MB to match the Windows
runs, 4 runs. The box can't switch OS without a reboot, so there's no Windows
control in this batch. Compare against earlier Windows batches with the same
protocol instead.

| OS / batch                               | runs                                | median  | range   | start / end Tctl (°C) |
|------------------------------------------|-------------------------------------|--------:|---------|-----------------------|
| **Linux**, 2026-11-09                    | 278, 196, 226, 170                  | **211** | 170–278 | 50–53 / 84            |
| Windows, 2026-11-08 (default, 8 runs)    | 397, 341, 355, 374, 308, 253, 667, 143 | 348  | 143–667 | 47–55 / 75–80         |
| Windows, 2026-11-07 (`-b 16 MiB`, 6 runs)| 97, 88, 130, 108, 106, 109          | 107     | 88–130  | 55–57 / 81–82         |

- **The Linux build catches the fault as fast as the Windows build.** All 4
  runs failed. The median falls between the two Windows days, and so does
  the spread. Throughput is the same (57–60 GB/s). With day-to-day swings of
  ~3x, "about the same" is the strongest claim the data supports.
- No machine-check, EDAC or oops messages in the kernel log. Windows logged no
  WHEA events either: the fault is silent data corruption that only a tester
  sees.
- Start Tctl puts the three batches in order: ~55 °C → 107 s, ~52 °C → 211 s,
  ~47 °C → 348 s. That fits the earlier finding that starting temperature
  dominates, though it's only three points.
- End Tctl is ~84 °C on Linux against 75–82 °C on Windows at the same
  bandwidth. amd-pstate may boost harder than Windows' Balanced plan. This
  can't be separated from the day effect.

## 2026-11-09: which bits flip

The frontends' error log records, for every error, which bit positions of
a 64-bit word differed in the failing pass. Same target
(`3600 c14 rcd14`), Windows, default `verify` on 90% of free RAM:

| run | first error | errors | bits that flipped |
|---|---:|---:|---|
| full app, stop on error off, 5:07 | 160.4 s | 5 | `0x8000` in all 5 |
| CLI `-s`, right after (warm) | 42.0 s | 1 | `0x8000` |
| lite window, right after (hot) | 4.3 s | 1 | `0x8000` |

- **Every error flipped the same bit**, in all three runs: bit 15 of the 64-bit word, which is
  byte lane 1, bit 7 on the DDR4 bus. A pure timing fault (tCL/tRCD too
  low) hits one DQ line, presumably the one with the least margin, rather
  than random bits. With two DIMMs and channel interleaving, user space
  can't tell which DIMM or channel it is.
- In the full-app run, thread 11 failed twice in the same 16 MiB block
  (offset 4624 MiB, cycles 4 and 6), so some physical rows also seem weaker
  than others.
- Time to first error tracked how hot the box was: 160 s, then 42 s right
  after, then 4 s right after that.
- Untested idea: a pattern aimed at that bit, or at those rows, might fail
  faster. It would also be machine-specific, so it can't be a default.
- The full app's core is built with MSVC (`cl /arch:AVX2`) and ran at
  56–57 GB/s, the same as the mingw-w64 CLI (57–58 GB/s).

## 2026-11-09: Linux live USB, 5 min idle

The same box booted from our own live USB ([live/](../live/README.md): Arch
Linux, kernel 7.2.14), on Wi-Fi, with the harness over ssh. The memory
controller reported the same OC: `3600 c14 rcd14 rp16 ras36 rfc324`. Default
`verify` on 90% of free RAM (13823 MB requested; the Ubuntu batch above pinned
10900 MB), 2 runs, each right after a fresh boot plus `--idle 300`.

| OS / batch                               | runs     | median  | range   | start / end Tctl (°C) |
|------------------------------------------|----------|--------:|---------|-----------------------|
| **Linux live USB**, 2026-11-09           | 255, 207 | **231** | 207–255 | 45 / 81               |
| Linux, Ubuntu live USB, 2026-11-09       | 278, 196, 226, 170 | 211 | 170–278 | 50–53 / 84      |

- Both runs failed, inside the Ubuntu batch's range, so the live USB works
  as a test platform: same fault, same speed (56 GB/s). Two runs with a
  different test size support "consistent", nothing finer.
- Both first errors flipped bit `0x8000`, like every error whose bits we have
  recorded.
- No machine-check, EDAC or oops messages in the kernel log.
