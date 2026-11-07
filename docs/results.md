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
