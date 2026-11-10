# Benchmark results

Until the DDR5 section at the end, all runs are on cutestation: Ryzen 9 5900X,
2x8 GB DDR4 at 3600, deliberately unstable with **tCL 14 / tRCD 14** (label
`3600 c14 rcd14`). Raw records live in `results/runs.jsonl`;
`harness/bench.py summary` aggregates them.

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

## 2026-11-09: DDR5 box, first unstable target

A second box, on our live USB, wired: Ryzen 7 7800X3D (one CCD, 96 MB L3),
ASUS ROG STRIX B650E-I (BIOS 3057), 2x16 GB Corsair CMK32GX5M2B6000C30. The
SPD says SK hynix M-die, 16 Gb x8, single rank. EXPO runs it at 6000
30-36-36-76. The user then set tRRDL 15→12, tWR 90→60 and tRTP 23→16; all
three are below JEDEC at 6000 (15, 90, 23 clocks), though Hynix M-die
usually takes values this tight. Label `7800X3D 6000 c30 rcd36 rp36 ras76
rrdl12 wr60 rtp16`. The EXPO baseline only has one 5 min warm `verify` pass
behind it, so it isn't proven stable either.

New on DDR5: each DIMM's SPD hub has a temperature sensor, which Linux reads
through the `spd5118` driver. `harness/remote/sampler.sh` logs it at 1 Hz
next to Tctl; the samples are in `results/samples/`.

### Cold start, 5 min idle, 10 min `--rate`

First error at **61 s**; the user's own run on this setting failed at 142 s.
402 errors in 600 s. DIMM temperature is the mean of the two DIMMs.

| minute | DIMM °C | Tctl °C | errors |
|-------:|--------:|--------:|-------:|
| 0–1    | 33→39   | 70      | 0      |
| 1–2    | 41      | 70      | 2      |
| 2–3    | 44      | 70      | 1      |
| 3–4    | 46      | 71      | 7      |
| 4–5    | 48      | 71      | 22     |
| 5–6    | 49      | 72      | 59     |
| 6–10   | 50–53   | 72      | 69–86 per min |

- **The error rate follows DIMM temperature, not CPU temperature.** Tctl was
  flat at 70–72 °C from 30 s on, while the DIMMs climbed for the whole run
  and errors went from none to ~80 a minute. On DDR4 we could only infer
  this from Tctl. Cold time-to-first-error here is mostly the time it takes
  the DIMMs to reach ~40 °C.
- **Every error is in one byte lane.** All 402 errors hit only bytes 2 and 6
  of the 64-bit word. A DDR5 subchannel is 32 bits wide, so that is byte
  lane 2 on consecutive beats: one x8 chip position (or its PHY lane), on all
  8 DQs, mostly 2–3 bits per error. DDR4's tCL/tRCD fault was one DQ line
  (`0x8000`) every time. User space can't tell which of the four
  subchannels it is.
- Bandwidth stayed at 82.5 GB/s throughout. That is more than one Zen 4
  CCD's fabric link can read (~64 GB/s at FCLK 2000), so some of `verify`'s
  re-reads are served from the 96 MB L3 (16 threads x 16 MiB = 256 MiB
  working set). Not measured further.

### Stored data or bad reads: `pass` and `recheck`

`verify` errors now say which re-read of the block found them (`pass`, 0 =
the first after the fill). They also say how many of two further re-reads
fail in the same bits (`recheck`), with the block flushed from cache before
each. 2/2 means the data stored in DRAM is wrong; 0/2 means only that read
went wrong.

- **All 9062 errors with a recheck result, from this run and the runs
  below, were `recheck 2/2`.** The DRAM holds the wrong data; none were
  one-off bad reads.
- In the default (`-r 200`) runs, 37–43% were caught on pass 0 or 1, the
  first reads from DRAM after the fill. The rest thin out with time since
  the write. In the first warm run (166 errors): passes 0–19: 94, 20–39:
  23, then 3–11 per 20 passes.

### Re-reads per fill, warm, 2 interleaved rounds of 90 s

| `-r` | errors/min (round 1, 2) | GB/s | P(block fails, per fill) | DIMM °C mean |
|-----:|------------------------:|-----:|-------------------------:|-------------:|
| 200 (default) | 15, 95     | 82.5 | 1.0%, 6.5% | 48.5, 53.7 |
| 20            | 323, 873   | 75   | 2.5%, 6.8% | 51.0, 54.1 |
| 2             | 1594, 3028 | 48   | 2.8%, 5.3% | 53.0, 55.1 |

- **The fault comes from the fill phase**: the stores plus the reads for
  ownership and write-backs they cause. At the same DIMM temperature, a
  freshly written block fails with about the same probability whether it's
  then read 2, 20 or 200 times. Reading it 100x more adds nothing. So
  errors per minute scale with fills per minute. In round 2, where the
  DIMMs were within 1.5 °C across configs, `-r 20` found 9x more errors per
  minute than the default and `-r 2` 32x, despite lower bandwidth.
- A read-side fault would grow with the number of reads, as the DDR4
  tCL/tRCD fault did; this one doesn't. That looked like tWR. **It isn't**:
  see the tWR 90 run below.
- `verify`'s default spends ~0.5% of its time filling, so it's slow on
  faults that only the fill phase triggers. `-r 20` cost nothing on DDR4 (median 115 s against
  113 s for the default, 2026-11-07) and gave ~10x the error rate here, which
  makes it a candidate default. It still needs a cold-start comparison on
  both boxes.
- The DIMMs heated over the session, so each run was hotter than the one
  before. In round 1 the per-fill probability rises with temperature (1.0%
  → 2.8% over 48.5 → 53 °C). In round 2, within 1.5 °C, the three agree
  (5.3–6.8%).

### Side effects

- The hot runs corrupted the kernel too: 9 `BUG: Bad page state` reports.
  The first two show the same lane: a `mapcount` read `0xffbfffff` instead of
  `0xffffffff` (bit 22, byte 2), and a `memcg` read `0x0006000000000000`
  instead of 0 (bits 49–50, byte 6). The harness counts them as `oops`.
  Reboot the box before any further runs.
- After a run the DIMMs drop fast, then slowly: 55 °C at the end, 49 °C a
  minute later, 45.5 °C after two. The cold run started at 33 °C, where the
  DIMMs had sat flat for the whole 5 min idle. A fixed idle time gives different starting temperatures
  depending on how hot the last run left the DIMMs. On DDR5, waiting for a
  DIMM temperature is the better gate.
- `memtimings` decoded DDR5 wrongly: it showed 3733 MT/s and tRFC 312. It now
  reads the DRAM type from UMC_CFG (0x100 bits 2:0, 1 = DDR5) and shows 6000
  MT/s and tRFC1/2/sb 884/480/390, which match the SPD's 295/160/130 ns. DDR4
  output is unchanged apart from the new `type=` field (checked on the
  5900X). Cmd2T and gear-down aren't decoded on DDR5 yet. Older DDR5 records
  keep the raw registers, so they can be decoded again.

### tWR back to 90

The user reverted only tWR (label `... rrdl12 wr90 rtp16`) and rebooted.

- The boot itself logged a `Bad page state` at 4.8 s, before anything ran,
  with **the same fields as the first one from the earlier boot**: `index`
  `0x200000` instead of 0 and `mapcount` `0xffbfffff` instead of
  `0xffffffff`. Both are lane-2 bits (bits 21 and 22 of their words). The
  kernel writes that 64-byte line with the same content for every page at
  boot. In lane 2 it is, beat by beat, `00 17 60 ff 60 ff 00 00 [00→20] 00
  00 00 [ff→bf] 00 00 00`: a lone `ff` among zeros came back as `bf`. The
  same flips from the same data on two boots means the fault depends on the
  data pattern. The boot also logged 9 `Invalid ELF header magic` (rejected
  kernel modules); those turned out to be on every boot (see "tRRDL 12
  alone").
- Same cold protocol as the tWR 60 run. Errors per minute, minutes 0–7:
  tWR 60: 0, 2, 1, 7, 22, 59, 69; **tWR 90: 0, 1, 2, 15, 17, 29, 52**. First
  error at 69 s (tWR 60: 61 s). All 123 in lane 2, all `recheck 2/2`, 27%
  on pass 0–1.
- The kernel panicked at ~440 s. `harness/panicqr.py` decodes the log
  from a photo of the drm_panic QR code: a NULL pointer dereference in the
  scheduler's load balancer (`cpu_util`, from `sched_balance_softirq`), hit
  by two CPUs at once while they ran memslammer. That fits corrupted
  shared kernel data. The harness lost the connection and used to record
  only the time of the loss; it now keeps the errors it saw (123, first at
  69 s). The DIMM log was in the box's RAM and was lost. Stream the sampler
  to the harness host instead.
- So tWR isn't the cause. At this point we guessed that none of the three
  timings was, because the fault comes only from the fill phase, and
  suspected the electrical margin of lane 2 instead. That reasoning was
  wrong; see the next run.

### Full EXPO again

The user went back to EXPO (tRTP 23, tRRDL 15, tWR 90). Every UMC register
read the same as in the original baseline run. The boot logged no
`Bad page state`. (It also seemed to log no `Invalid ELF header magic`, which
we took as a sign that those 9 messages were corruption. They aren't: see
the correction under "tRRDL 12 alone".) Same cold protocol, with the sampler
streamed to the harness host.

- **0 errors in 600 s**, with the DIMMs going 33.6 → 52 °C, about the same
  path as the unstable runs. The tWR 60 run's rates, applied to the time
  this run spent in each 1 °C bin, predict ~330 errors.
- **So the fault is tRTP 16 or tRRDL 12 (or both).** The fill-phase-only
  result doesn't exclude either. The fill isn't just writes: each store
  first reads its line from DRAM, and dirty lines go back in a scattered
  order. So the fill opens and closes many more DRAM rows than in-order
  re-reads do. Rows opened in quick succession are what tRRDL limits, and a
  read followed soon by a precharge is what tRTP limits. Next: a BIOS
  bisect, one of the two at a time.
- What does hold: on this box the fault corrupts stored data, mostly in the
  fill phase, on one chip position (lane 2, all 8 DQs, the weakest die
  presumably), and gets worse with DIMM temperature. That is what a core
  timing violation on the slowest chip looks like. It also explains why
  the identical boot-time memmap writes flipped identical bits.


### tRTP 16 alone

The user set EXPO with only tRTP lowered, 23→16 (label `7800X3D 6000 c30
rcd36 rp36 ras76 rtp16`; `memtimings` read tRRDL 15, tWR 90, tRTP 16). Same
cold protocol, sampler streamed to the harness host (record
`20261109-212551-a06a`).

- **680 errors in 600 s, first at 139 s** (DIMM 44.8 °C). tRTP 16 on its
  own is enough for the fault, as the user had guessed.
- Errors per minute, minutes 0–9. All three timings: 0, 2, 1, 7, 22, 59,
  69, 86, 73, 83. **tRTP 16 alone: 0, 0, 5, 14, 31, 66, 125, 121, 142,
  176.** This run's DIMMs were about 1 °C warmer throughout (34.8 → 53.8 °C
  against 33.5 → 52.8 °C), which accounts for most of the difference. Per
  DIMM temperature, in errors per minute:

  | DIMM °C | all three | tRTP 16 alone |
  |--------:|----------:|--------------:|
  | 44–45   | 2.3       | 3.8           |
  | 46–47   | 12.8      | 13.8          |
  | 48–49   | 47.2      | 26.0          |
  | 50–51   | 68.8      | 102.1         |
  | 52      | 95.6      | 94.8          |
  | 53      | —         | 194.5         |

  The two agree within the noise of one run each, so tRRDL 12 and tWR 60
  add nothing measurable on top of tRTP 16. Whether tRRDL 12 fails on its
  own is still untested.
- Same signature as before: every error in byte lane 2 (bytes 2 and 6 of
  the word), all 8 DQs, mostly 2–3 bits; all 680 `recheck 2/2`; 25% caught
  on pass 0–1.
- The boot logged no `Bad page state`, and no MCE or kernel BUG during the
  run.
- tRTP is the minimum time from a read to a precharge in the same bank.
  That fits the fill-phase result: the fill's reads for ownership arrive
  out of order, so many are followed soon by a precharge to open another
  row, while `verify`'s in-order re-reads mostly hit an open row. Why an
  early precharge corrupts the stored row rather than only the data being
  read, we haven't established. One possibility is that the column path
  still connected to the sense amplifiers disturbs them as the row closes,
  so the wrong value is written back to the cells.

### `-r 20` against the default, from cold

Same setting (tRTP 16 alone), alternating. Runs 1–3 ran 10 min with
`--rate` after a fixed idle (5–15 min). Runs 4–9 stopped at the first error
and started once both DIMMs read ≤ 35 °C, after 60 s more idle for a sampler
baseline. Seconds to first error:

| run | start DIMM °C | default | `-r 20` | DIMM °C at error |
|----:|--------------:|--------:|--------:|-----------------:|
| 1   | 34.8          | 139     |         | 44.8             |
| 2   | 35.5          |         | 24      | 38.5             |
| 3   | 35.5          | 96      |         | 43.4             |
| 4   | 33.5          |         | 28      | 36.9             |
| 5   | 34.6          | 138     |         | 44.3             |
| 6   | 34.8          |         | 36      | 38.8             |
| 7   | 34.8          | 118     |         | 43.6             |
| 8   | 34.8          |         | 48      | 39.8             |
| 9   | 34.8          | 101     |         | 42.9             |

- **`-r 20`: median 32 s (24–48). Default: median 118 s (96–139).** 3.7x
  faster, and the ranges don't overlap: every `-r 20` run beat every default
  run. If the two were equally fast, that would happen by chance about 1 in
  126 times.
- **The default only fails once the DIMMs reach 43–45 °C; `-r 20` fails at
  37–40 °C.** The fault is already there just above idle temperature. The
  default writes too rarely to catch it. That is ~16 fills per minute
  across all threads against ~120 for `-r 20`, at 82 against 75 GB/s.
- In the two 10 min `--rate` runs, errors per fill agreed at the same DIMM
  temperature (around 49–50 °C: 1.9 for the default, 1.7 for `-r 20`), as
  in the warm ablation. `-r 20` found 3937 errors in 10 min against 680 and
  541 for the default.
- All errors were `recheck 2/2`. 5 of `-r 20`'s 3937 were in byte lane 3
  (bytes 3/7), the first errors outside lane 2, all after 427 s with the
  DIMMs above 51 °C.
- **Both 10 min `--rate` runs corrupted the box in their hot second half**,
  the default one as well. After `-r 20`, sshd reset new connections during
  key exchange. After the default, `dmesg` filled with `list_del
  corruption` warnings (the page cache's LRU list) and `journalctl` failed
  with a symbol lookup error in `libsystemd-shared`, a flipped bit in a
  shared library held in RAM. Each needed a reboot. The stop-at-first-error
  runs logged no kernel events, and four fit in one boot.
- **On DDR4 the same change made no measurable difference**: `-r 20` 115,
  122, 56 s against the default's 236, 55, 113 s (2026-11-07, 5 min idle).
  That fault was read-side, so fewer re-reads per fill could have hurt
  there; at n=3 it didn't visibly.
- 3 of the 4 boots with tRTP 16 whose logs we checked (one also with
  tRRDL 12) logged one or two `Bad page state` reports before anything ran, all lane-2 flips in `struct page` fields (`mapping`
  0x20000 / 0x830000, `index` 0x800000, `mapcount` 0xfffeffff,
  `memcg_data` 0x280000).

### tRRDL 12 alone

The last leg of the bisect: EXPO with only tRRDL lowered, 15→12 (label
`7800X3D 6000 c30 rcd36 rp36 ras76 rrdl12`; `memtimings` read tRRDL 12,
tWR 90, tRTP 23). Same cold run as the others, 10 min of default `verify`,
this time in stop-at-first-error mode so that a failure wouldn't corrupt the
box (record `20261110-014517-c92c`).

- **0 errors in 600 s**, DIMMs 35.3 → 54.1 °C. That is slightly hotter than
  the tRTP 16 run, which logged 680 errors on the same path, and the same
  result as full EXPO.
- **0 errors in a further 300 s of `-r 20`**, started right after with the
  DIMMs at 54–55 °C (record `20261110-015521-2cbf`). On tRTP 16, `-r 20`
  failed in 24–48 s from cold; warm, at 51–54 °C with all three timings
  lowered, its first error came within 0.3 s.
- **So tRTP 16 is the whole fault.** tRRDL 12 passes everything tRTP 16
  fails, and tWR 60 made no difference on top of tRTP 16 (above). The
  bisect is complete: of the three timings the user changed, only tRTP
  needs to go back.
- No `Bad page state` at boot or during the runs, no MCE, no oops. Another
  30 min of hot runs with the new build followed (below), also without an
  error: 45 min of testing on this setting in all.

| setting (EXPO otherwise)        | 10 min cold, default `verify` | boot `Bad page state` |
|---------------------------------|------------------------------:|-----------------------|
| EXPO                            | 0 errors                      | none                  |
| tRRDL 12                        | 0 errors                      | none                  |
| tRTP 16                         | 680 errors, first at 139 s    | in 2 of 3 boots       |
| tRRDL 12 + tRTP 16 + tWR 90     | 123 errors in 440 s, then a kernel panic | yes        |
| tRRDL 12 + tRTP 16 + tWR 60     | 402 errors, first at 61 s     | not checked           |

**Correction: `Invalid ELF header magic` is not a sign of corruption.** This
boot logged the same 9 messages, at 3.97 s, in the same instant the Ventoy
stick's partitions appear and device-mapper loads. They come from the boot
path (most likely Ventoy's hook loading modules), on every boot. The full
EXPO boot looked clean only because that check went through `journalctl`,
which prints these lines as `[blob data]`: they contain a raw 0x7f byte.
`dmesg` shows them. `Bad page state` remains a real sign.

### What the pass numbers say about the tRTP fault

The tRTP 16 logs record which re-read found each error (`pass`). Counting
them per pass (2026-11-09 runs; "warm 2" is the second ablation round):

| run                | errors | pass 0 | pass 1 | pass 2 | pass 3 | passes 8+ |
|--------------------|-------:|-------:|-------:|-------:|-------:|----------:|
| `-r 2`, warm 2     | 4544   | 3678   | 866    |        |        |           |
| `-r 20`, warm 2    | 1310   | 570    | 94     | 72     | 69     | 330       |
| `-r 20`, cold 10 min | 3937 | 1599   | 224    | 204    | 188    | 1089      |
| `-r 200`, warm 2   | 143    | 58     | 3      | 3      | 2      | 68        |

- **Two things corrupt a block.** Its own fill: pass 0, the first read
  after it, finds 41–44% of the errors with 20 or 200 re-reads in these
  runs (21% in the 10 min cold default run) and 81% with 2. And something
  later: a block that read back correctly keeps failing on the passes
  after that, with the stored data wrong each time (`recheck 2/2`).
- **The later rate follows how much of the time the threads spend
  filling.** Averaged over all later passes, one pass finds 0.7% of what
  pass 0 finds with `-r 200`, 7% with `-r 20` and 22–24% with `-r 2`. The
  threads spend about 1%, 9% and 50% of their time filling. The ratio of
  the two is 0.74–0.79 in all four `-r 200` and `-r 20` warm runs (0.45 for
  `-r 2`, which also moves a third less data). So a block being re-read
  while others fill is at about the same risk, per unit of time, as a block
  being filled.
- **Memory that is only sitting there is far safer.** In the warm `-r 20`
  run the 16 blocks under re-read took 740 such errors in 90 s, about one
  per block every 2 s. If the 1700 idle blocks and the kernel's own memory
  failed at that rate, the box would not last a second. It lasts minutes.
  So what gets corrupted is memory in use (being read, or just written)
  while fills are going on.
- That fits tRTP, the time from a read to the precharge that closes its
  row. Sequential re-reads rarely force a row closed right after reading
  it. Fills do: each adds a read-for-ownership stream and a write-back
  stream a few MB behind it, so the controller keeps closing rows to open
  others in the same bank. It leaves one thing open: a fill's own reads
  are of lines it is about to overwrite, so they shouldn't leave a
  trace, yet pass 0 finds the most. `-N` (below) fills without those
  reads, and the late check watches the idle blocks; both are there to
  settle this on the next tRTP 16 boot.

### New in the tester: late check, non-temporal fill

- **Late check (on by default, `-L` turns it off).** Until now `verify`
  never looked at a block again after its re-reads: a block was filled,
  read 200 times within ~0.7 s, and then sat for a whole cycle (70 s here)
  until the next fill overwrote it unseen. At any moment 99% of the memory
  under test held data nobody would check. A row that is read every few ms
  is also being refreshed by those reads, so a refresh or retention
  problem (tREFI too long, for one) could not show at all. Now each block
  is compared once more just before its next fill, as TM5's RefreshStable
  does around its tests ([tm5.md](tm5.md)). Such errors print `pass late`.
  A block that already failed a re-read isn't reported a second time.
- **`-N` fills with non-temporal stores** (experimental): no read for
  ownership, and the block isn't left in cache. It's there to find out
  what in the fill triggers the tRTP fault.
- The self-test flips a bit in every block while the threads are busy
  elsewhere: with the late check 14–15 of 16 blocks are caught (the others
  were being filled at that moment), without it none.

Neither could be measured against a fault today: the box was on tRRDL 12,
which is stable. What could be checked there, right after the runs above,
DIMMs at 54–57 °C, 60–120 s each:

| config                       | GB/s | fills of the whole memory per min | errors |
|------------------------------|-----:|----------------------------------:|-------:|
| default                      | 82.4 | 0.86                              | 0      |
| default, `-L`                | 82.4 | 0.86                              | 0      |
| `-r 20`, before the change   | 75.0 | 7.5                               | 0      |
| `-r 20`                      | 75.4 | 7.2                               | 0      |
| `-r 20 -N`                   | 80.2 | 7.7                               | 0      |
| `-r 2`                       | 52.3 | 27.7                              | 0      |
| `-r 2 -N`                    | 63.9 | 33.9                              | 0      |
| `chain`                      | 66.0 | 4.0 cycles, each with 9 writes of the memory | 0 |

- **The late check costs nothing at the default** (one extra read per 201)
  and 4% of the fills at `-r 20`. No false errors on real hardware: 10 min
  across these variants, then a 20 min `-r 20` soak with the DIMMs at
  46–57 °C (record `20261110-021413-79d9`: 144 fills of the whole memory,
  every block late-checked each time).
- Both were then measured on tRTP 16: see
  [the race below](#the-race-on-trtp-16-re-reads-order-and-fill).
- **The late check still needs a retention fault to prove itself there**,
  which neither box has shown yet: tREFI raised until it fails with the
  DIMMs hot would be one.

### Harness fixes from this round

- `events.sh` read the kernel log through `journalctl`. When corruption
  broke `journalctl`, it silently reported 0 events. It now falls back to
  `dmesg` and says so (`src=dmesg`, or `src=dmesg_wrapped` when the ring
  buffer had already lost older messages, so the count is a lower bound),
  and prints `error=` when neither works. It also counts `list_add`/
  `list_del corruption` reports as oopses.
- It also filtered by wall-clock time. The live USB boots with the RTC's
  time, which this Windows box keeps in local time (UTC+1), and chrony
  steps the clock back later. So early-boot messages are stamped an hour
  ahead, and `--since` counted the boot's own `Bad page state` as a new
  event during the first hour. It now filters on time since boot.
- `bench.py run` gained `--gate-dimm C` (wait until every DIMM reads ≤ C)
  and `--sample` (stream the 1 Hz temperature log to
  `results/samples/<run id>.tsv`), and records the DIMM temperatures at the
  start and end of each run. Until now that was done by hand around each
  run.

### The race on tRTP 16: re-reads, order and fill

2026-11-10, EXPO + tRTP 16 again (`rtp=16 rrdl=15 wr=90` read back), the
binary with the late check, one boot of 12 h. The boot logged one lane-2
`Bad page state` before anything ran.

**First error, from cold.** 3 rotated rounds, stop at the first error, 60 s
idle after the gate. The idle DIMMs sat at 35.25–36.5 °C today and never
reached the 35 °C gate, so round 1 waited its 20 min cap every time; rounds
2–3 used 36 °C. `-r 2 -N` joined in round 2. The last column is the first
error of the 120 s `--rate` runs below, same start. Seconds:

| config              | round 1 | round 2 | round 3 | rate runs  | median |
|---------------------|--------:|--------:|--------:|-----------:|-------:|
| `-r 2 -A shuffle`   |         |         |         | 5.0        | 5.0    |
| `-r 5 -A shuffle`   |         |         |         | 6.7        | 6.7    |
| `-r 2`              | 4.1     | 20.6    | 7.4     | 7.5, 15.1  | **7.5**|
| `-r 20 -A shuffle`  | 26.9    | 20.7    | 4.3     | 2.4        | 12.5   |
| `-r 2 -N`           |         | 17.4    | 10.4    | 26.4       | 17.4   |
| `-r 1`              |         |         |         | 23.7       | 23.7   |
| `chain`             | 41.3    | 27.8    | 14.1    |            | 27.8   |
| `-r 20 -N`          | 30.9    | 69.8    | 38.6    | 38.7       | 38.7   |
| `-r 20`             | 42.0    | 42.3    | 14.6    | 37.5       | 39.8   |

Start DIMM temperature (mean of the two) was 35.0–35.75 °C in round 1 and
35.75–36.5 °C after. First errors came at 36.3–42.4 °C. No kernel events in
any of the 17 stop-at-first-error runs.

**Error rate, from cold.** 120 s `--rate` each, gate 36.5 °C, every run
from 36.1–36.25 °C to 44.9–46.1 °C. All 863 errors were `recheck 2/2` and
in byte lane 2. "Fills" is how many times the whole memory was written.

| config              | GB/s | fills | errors | first read after the fill | later re-reads | late check |
|---------------------|-----:|------:|-------:|-----:|------:|-----:|
| `-r 2 -A shuffle`   | 46.7 | 49    | 205    | 134  | 37    | 34   |
| `-r 2`              | 52.3 | 55    | 137    | 72   | 19    | 46   |
| `-r 2`, 1.5 h later | 52.3 | 55    | 91     | 55   | 9     | 27   |
| `-r 1`              | 48.0 | 68    | 133    | 85   | –     | 48   |
| `-r 5 -A shuffle`   | 50.1 | 30    | 128    | 65   | 48    | 15   |
| `-r 20 -A shuffle`  | 53.8 | 10    | 84     | 16   | 62    | 6    |
| `-r 20`             | 75.3 | 14    | 33     | 15   | 11    | 7    |
| `-r 2 -N`           | 63.7 | 67    | 28     | 9    | 6     | 13   |
| `-r 20 -N`          | 80.0 | 15    | 24     | 0    | 16    | 8    |

- **Fewer re-reads per fill find this fault sooner, all the way down.**
  `-r 2`: median 7.5 s over 5 runs (4–21). `-r 20`: 39.8 s over 4 (15–42).
  The default took 96–139 s yesterday (median 118, older binary, DIMMs
  about 1 °C cooler at the start). `-r 2` found 2.8–4.2x the errors of
  `-r 20` in 120 s. Errors found by the first read after a fill stay at
  1.0–1.3 per fill of the memory for `-r 1`, `-r 2` and `-r 20`: the fills
  do it, and `-r 2` makes four times as many.
- **`-r 1` is no better than `-r 2`**: 133 errors against 137 and 91.
- **Shuffled re-reads help here**, unlike on DDR4. At `-r 20` they found
  5.6x the later-pass errors (62 against 11) and 2.5x the total, at 71% of
  the bandwidth. With `-r 2` the total was 205 against 137 and 91. Its
  first errors were also the earliest, but that is one run per shuffled
  `-r 2`/`-r 5` config.
- **The non-temporal fill is worse for this fault.** It moves more data
  (22% more fills at `-r 2`) and the first read after a fill found 9 errors
  in 82 fills, against 87 in 69 for the normal fill at the same settings:
  0.11 per fill against 1.26. At `-r 20` it found none at all. `-N` stays
  an option; the normal fill stays the default.
- **That answers yesterday's open point.** The normal fill reads each line
  before overwriting it (read for ownership) and writes it back later; the
  non-temporal fill only writes. The reads are what the fill needs to
  corrupt data, which is what a short tRTP should do: the row is closed
  too soon after a read. The memory is in 2 MiB pages here, so the lines
  around the one being read, in the same DRAM row, belong to the same
  block and were written a moment earlier. Our reading, not something
  these runs show directly: a read closed too early costs other cells in
  its row their charge. Shuffled re-reads, which jump to another row after
  every 4 KiB, fit that too.
- **The late check found 7–46% of the errors of every run** (21% at
  `-r 20`, 30–34% at `-r 2`, 36% at `-r 1`). Before it, those blocks were
  overwritten unseen. Part of that is damage done by the last re-read
  itself; the rest happened while the block sat untouched. `-r 2` puts a
  bound on it: its second read found 19 and 9 errors, and the late check
  46 and 27, so more than half the late errors arrived while the block was
  idle.
- **Run-to-run spread is about ±25%**: the two `-r 2` runs gave 137 and 91
  errors from the same start. Differences below 1.5x in the table are not
  established.
- **`chain` is no faster than `-r 20`** (median 27.8 s against 39.8 s, 3
  runs, ranges overlap). It only checks at the end of a cycle, 14 s apart.
- Round 3 was the fastest round for three of the five configs. The room
  warmed through the morning (starts went from 35.0 to 36.5 °C), so
  compare within a round. `-r 2` beat `-r 20` in all three.
- The fifth `--rate` run (`-r 20 -A shuffle`) logged one more lane-2 `Bad
  page state` (`memcg_data` 0x6000000000000). The four runs after it, on
  the same boot, logged nothing and the box stayed reachable.
- On the DDR4 box `-r 20` and `-A shuffle` had made no measurable
  difference to a read-side fault, and `-r 2` had not been run there. That
  run is [the next section](#2026-11-10-re-read-count-on-the-ddr4-box-5-min-idle).

## 2026-11-10: re-read count on the DDR4 box, 5 min idle

The 5900X box on its usual target (`3600 c14 rcd14 rp16 ras36 rfc324` read
back), booted from our live USB, on Wi-Fi, the same binary as the tRTP race
above. Standard protocol: 5 min idle before every run, stop at the first
error, 90% of free RAM (13.8 GB). 4 rounds, order rotated so every config
ran once in each slot. All 16 runs failed. Seconds:

| config             | round 1 | round 2 | round 3 | round 4 | median  | range   | GB/s | end Tctl (°C) |
|--------------------|--------:|--------:|--------:|--------:|--------:|---------|-----:|--------------:|
| `-r 20`            | 97      | 113     | 111     | 181     | **112** | 97–181  | 50.7 | 78–81         |
| `-r 2 -A shuffle`  | 106     | 75      | 240     | 166     | 136     | 75–240  | 31.6 | 73–77         |
| `-r 2`             | 211     | 68      | 176     | 230     | 194     | 68–230  | 36.0 | 76–79         |
| default (`-r 200`) | 209     | 222     | 160     | 236     | 216     | 160–236 | 56.0 | 78–82         |

- **`-r 20` beat the default in all four rounds**, median 112 s against
  216 s. One of the 16 cross pairs goes the other way (181 s against
  160 s); with equal configs a split that lopsided comes up about 1 time in
  35. On 2026-11-07 the same pair gave 115, 122, 56 s against 236, 55,
  113 s (4 MiB blocks then). So `-r 20` has now been the faster or equal
  one in both batches, where we had expected a read-side fault to prefer
  more re-reads.
- **`-r 2` is not better than the default here** (median 194 s against
  216 s, ranges on top of each other) and lost to `-r 20` in three rounds
  of four. It moves 36 GB/s against 51 and 56, a quarter of it writes, and
  heats the CPU less. On the tRTP fault it was 5x faster than `-r 20`; on
  this one it gives that back.
- **Shuffled order at `-r 2` shows nothing either way**: 75–240 s.
- **Every first error was the same bit, `0x8000`, and `recheck 0/2`**: the
  stored data was right and one read of it was wrong. That is the opposite
  of the DDR5 fault (`2/2` in all of today's 863 rate-run errors), so
  writing more often can't help by itself. Why `-r 20` still beats the
  default here is not explained by these runs.
- Two of `-r 2`'s first errors were caught by the late check, also `0/2`:
  a bad read during the late check itself, not data that decayed.
- Start Tctl was 45–46 °C through run 9 and 39–41 °C from run 10 on (the
  room cooled in the evening). Five of the seven later runs were slower
  than all of their config's earlier ones; the rotation spreads that over
  the configs.
- No machine-check or oops messages in any run.

Taken together with the DDR5 box:

| fault                                   | default | `-r 20` | `-r 2`  |
|-----------------------------------------|--------:|--------:|--------:|
| DDR5, tRTP 16: stored data corrupted    | 118 s   | 40 s    | 7.5 s   |
| DDR4, tCL/tRCD 14: bad reads            | 216 s   | 112 s   | 194 s   |

(medians; the DDR5 default is from the day before.) `-r 20` is faster than
the default on both faults. `-r 2` is much the fastest on one and no
better than the default on the other.

`verify` now defaults to 20 re-reads per fill (0.2.0). Three warm runs with
the new default right after, on the DDR4 box, back to back:

| run | first error | bits | pass | recheck |
|---|---:|---|---:|---|
| by hand, not recorded      | 138.4 s | `0x00ff000000000000` | 6 | 0/2 |
| `20261110-190758-5173`     | 192.3 s | `0x8000`             | 4 | 0/2 |
| `20261110-191112-5b2a`     | 31.4 s  | `0x8000`             | 7 | 0/2 |

- The first of them is the only error on this box so far that was not bit
  `0x8000`: one read returned all eight bits of byte 6 flipped, and the two
  re-reads of the same word were right. One event; it has not recurred.
- The box had idled for an hour before the first run, hence the spread.
