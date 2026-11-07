# memslammer

A memory stress test for **DDR overclock validation**, built to do one thing:
make an unstable memory overclock fail **as fast as possible**.

## Approach

1. Study how the testers known to fail OCs fastest actually hit the RAM
   (Karhu RAM Test, TestMem5 with anta777's *Absolut* profile, ...). See
   [docs/](docs/).
2. Reimplement each technique clean-room, with every ingredient switchable.
3. On a real machine, push the memory OC into marginal territory and measure
   **time-to-first-error** for each tester and each ingredient (ablation).
4. Combine what actually matters into the fastest test.

## Status

- [x] Karhu RAM Test algorithm documented: [docs/karhu.md](docs/karhu.md)
- [x] `chain` mode: Karhu-style chained transform/checksum baseline
- [x] TestMem5 MT0 test functions and the Absolut profile documented: [docs/tm5.md](docs/tm5.md)
- [x] Benchmark harness: [harness/](harness/) (Karhu, TM5, memslammer; crash/reboot aware)
- [x] Baseline time-to-first-error at a fixed unstable OC: [docs/results.md](docs/results.md)
- [x] `verify` mode (fill + re-verify, TM5 Absolut T1-like)
- [x] Cold-start race: `verify` fastest from cold (median 218 s vs Karhu 368 s, TM5 418 s)
- [ ] Tune `verify` for cold start

## Build

```
make            # build/memslammer (Linux) and build/memslammer.exe (mingw-w64)
build/memslammer -m 4096 -d 600 -s
```

Run `build/memslammer -h` for options (with no options it tests 90% of free RAM until stopped). The current build
assumes AVX2.

## Benchmark harness

Runs testers on a remote Windows machine over ssh (`MSBENCH_HOST`, default
`cutestation-win`), stops each at its first error, and appends a record to
`results/runs.jsonl`. Each record holds the outcome (`pass`, `error`,
`reboot`, `harness_lost`, `host_down`), the time to first error, and any
WHEA, Kernel-Power 41, or bugcheck events logged during the run.

```
harness/bench.py deploy --tm5 /path/to/TestMem5     # TM5 v0.13.1 folder; not redistributed here
harness/bench.py run karhu --label "3800 trfc 250" --seconds 1800 --repeat 3
harness/bench.py run tm5 --label "3800 trfc 250"            # Absolut @ anta777 by default
harness/bench.py run "tm5:Extreme @ anta777" --label ...
harness/bench.py run memslammer --label ... -- -F           # extra args after --
harness/bench.py run memslammer --warmup 600 --rate --seconds 120 -- -M verify
harness/bench.py timings                                    # what the IMC is running now
harness/bench.py summary
```

- Every run logs the DRAM timings the memory controller is actually running
  (decoded, plus the raw UMC registers). Without `--label`, the label is
  built from them (`3600 c14 rcd14 rp16 ras36 rfc324`), so you can't forget to
  record a setting.
- `--warmup S` runs memslammer `chain` for S seconds first, unrecorded.
  DRAM temperature changes time-to-error by up to ~10x, so compare runs
  started at the same temperature.
- `--idle S` sleeps S seconds before the run so the DIMMs start cold. `--idle 300`
  approximates the real loop (about 5 min in UEFI, then retest). This is
  the metric that matters for real tuning: time to first error from cold.
  CPU Tctl is logged at the start and end of each run, but it follows CPU
  boost, not DRAM temperature.
- `--rate` keeps going past the first error and records every error time and
  errors/min. That gives more signal per run than first-error time once
  everything fails in seconds. Not supported for TM5 (it logs one error per
  test block). Error units differ between testers, so compare rates only
  within one tester/mode.

- Karhu is driven headlessly through `harness/karhu_drv.c`, which loads your
  own licensed `ramtest.dll` (`MSBENCH_KARHU_DLL`).
- Karhu and memslammer test 90% of free RAM unless you pass `--mb`. TM5 sizes
  itself.
- Timings need [PawnIO](https://pawnio.eu) on the target
  (`winget install namazso.PawnIO`). `deploy` fetches its signed
  `AMDFamily17` module (LGPL, not stored in this repo). AMD Zen 2/3 with DDR4
  is decoded so far.
- A run refuses to start if another tester is already running on the target.

## License

MIT. This repository contains no code or binaries from the tools studied.
