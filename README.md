# memslammer

A memory stress test for **DDR overclock validation**, built to do one thing:
make an unstable memory overclock fail **as fast as possible**.

![memslammer after a real failure](site/img/gui-errors.png)

**[Download](https://github.com/Francesco149/memslammer/releases/latest)** ·
**[Website](https://francesco149.github.io/memslammer/)** ·
[raw results](docs/results.md)

- **Fastest from cold** on our test box: median 218 s to the first error,
  against 368 s for Karhu RAM Test and 418 s for TestMem5 anta777 Absolut.
  It won all 3 rounds of the race.
- **Three frontends, one engine**: a CLI, a tiny native window, and a full
  GPU-rendered app. All of them link the same C core.
- **Small and boring to run**: single static binaries, no installer, driver,
  admin rights, network or telemetry. The Windows CLI + GUI zip is ~150 KB.
- **Needs** x86-64 with AVX2 (Haswell, Zen or newer); Windows 10/11 or any
  Linux.

## Downloads

Each release has two archives per OS:

| archive | contents | size |
|---|---|---|
| `memslammer-VER-windows-x64.zip` | `memslammer.exe` (CLI) + `memslammer-lite.exe` (Win32 window) | ~150 KB |
| `memslammer-VER-linux-x64.tar.xz` | `memslammer` (static musl CLI) + `memslammer-lite` (GTK 3 window) | ~50 KB |
| `memslammer-gui-VER-windows-x64.zip` | `memslammer-gui.exe`, the full app | ~6 MB |
| `memslammer-gui-VER-linux-x64.tar.xz` | `memslammer-gui` (needs Vulkan or GL) | ~9 MB |

The full app is a separate download, so the minimal package stays tiny.
`SHA256SUMS.txt` lists checksums. The binaries are built by
[GitHub Actions](.github/workflows/release.yml) from the tagged commit.

## Using it

- Close other programs and start a test. The default is `verify` mode on 90%
  of free RAM, all threads, until stopped. That's the fastest-failing
  configuration we have measured.
- **Any error means the memory overclock is unstable.** The GUIs stop at the
  first error by default.
- Errors come faster once the DIMMs are warm. The same setting failed in
  5–45 s on a hot box and took 2–11 min from cold. The real tuning loop
  (change one timing in UEFI, boot, retest) is a cold start, so that's what
  memslammer is tuned for.
- A clean run is evidence, not proof. We tuned on one machine and one kind
  of fault (tCL/tRCD too low). Use memslammer to reject bad settings
  quickly, and validate your final settings longer.

### Frontends

| | what | Windows | Linux |
|---|---|---:|---:|
| `memslammer` | CLI; scriptable, works over SSH and from live USBs | 99 KB | 84 KB |
| `memslammer-lite` | plain Win32 controls / GTK 3; DPI-aware, keeps the PC awake | 152 KB | 60 KB |
| `memslammer-gui` | [GPUI + gpui-kit](https://gpui-kit.com/): live throughput chart, per-thread coverage, error list, failing-bit map, run history, copyable report | 14 MB | 32 MB |

<p>
<img src="site/img/lite-windows.png" width="49%" alt="memslammer-lite on Windows">
<img src="site/img/gui-bits.png" width="49%" alt="failing bit map">
</p>

### CLI

```
memslammer [options]          # no options: verify 90% of free RAM until stopped
  -m MB  -t THREADS  -d SECONDS  -s (stop on first error)  -M verify|chain
  -h lists every option: block size, re-reads, pattern, cache policy, ...
```

Real output from the test box (warm, unstable setting):

```
> memslammer -s
memslammer verify: 9984 MB, 24 threads, block 16777216, rereads 200, pattern random, cache none, order seq, heat 0, yield 1
[     10.1s] cycles 0, coverage 26.9%, 57.90 GB/s, errors 0
[     20.2s] cycles 0, coverage 53.8%, 57.97 GB/s, errors 0
[     30.2s] cycles 0, coverage 80.8%, 57.86 GB/s, errors 0
[     40.2s] cycles 24, coverage 108.3%, 57.73 GB/s, errors 0
[   41.993s] ERROR thread 5 cycle 1 (total 1) bits 0x0000000000008000
[     42.0s] cycles 24, coverage 113.6%, 57.03 GB/s, errors 1
RESULT tester=memslammer errors=1 first_error_s=41.993 elapsed_s=42.014
```

`bits` marks the bit positions of a 64-bit word that flipped.
Exit status: 0 no errors, 1 errors, 2 bad arguments. Ctrl+C stops cleanly.
The GUIs show the equivalent CLI command for their settings.

## How it works

`verify`: each thread walks its share of memory in 16 MiB blocks. It fills
a block with random data (xoshiro256\*\*, fresh seed per block), then
re-reads and compares it 200 times. The expected values are regenerated on
the fly, so there's no second copy to read. Any mismatch is an error.

What mattered, from [docs/results.md](docs/results.md):

- **Random data.** The same loop with low-entropy patterns never failed.
- **A working set far bigger than L3.** With small blocks or half the
  threads, the re-reads stayed in cache and nothing failed in 15 minutes.
- **Not** cache flushing, non-temporal prefetch, shuffled order or an FP
  heat burst. Those stay available as options for testing on other hardware.
- Starting temperature outweighs every knob we tuned, so every comparison
  interleaves contenders.

`chain` is a clean-room reimplementation of the technique Karhu RAM Test
uses ([docs/karhu.md](docs/karhu.md)), kept as a baseline. `verify` is
modelled on test 1 of TM5's Absolut profile ([docs/tm5.md](docs/tm5.md)).

## Status

- [x] Karhu RAM Test algorithm documented: [docs/karhu.md](docs/karhu.md)
- [x] TestMem5 MT0 test functions and the Absolut profile documented: [docs/tm5.md](docs/tm5.md)
- [x] Benchmark harness (Karhu, TM5, memslammer; crash/reboot aware)
- [x] `verify` mode, fastest from cold (median 218 s vs Karhu 368 s, TM5 418 s)
- [x] Linux: fails as fast as on Windows (median 211 s, 4/4)
- [x] Core library + CLI, lite (Win32/GTK) and full (gpui) frontends; CI releases
- [ ] Tune `verify` for cold start further; test on more machines and fault types (tRFC, DDR5)

## Building

```
make                  # CLI + lite for Linux and Windows: gcc, mingw-w64, GTK 3 headers
                      # (musl-gcc, if installed, makes the Linux CLI static musl)
make gui              # full app for this OS: Rust stable (Linux: Vulkan/X11/Wayland headers)
make test             # self-test: injects bit flips while the test runs, checks they're caught
make dist             # release archives in dist/
```

Layout:

```
core/       the engine: memslammer.h (API), memslammer.c (portable control),
            engine.c (the AVX2 test loops, the only file built with -mavx2)
cli/        command line
lite/       Win32 and GTK 3 windows, sharing lite/common.c
gui/        Rust app: FFI to the core (build.rs compiles it in) + gpui-kit UI
res/        icon, Windows manifest and version resources
tests/      core self-test
harness/    benchmark rig (runs testers on a remote box, records time-to-first-error)
docs/       how Karhu and TM5 stress memory, and every benchmark result
site/       GitHub Pages
```

The core API is poll-based: `ms_start(config)` returns at once,
`ms_get_status`/`ms_get_errors` can be called from any thread, and
`ms_free` stops and cleans up. Frontends never touch the test loops.

## Releasing

```
make release V=0.2.0
```

This tags `v0.2.0` and pushes it. [release.yml](.github/workflows/release.yml)
then builds the CLI and lite binaries (Ubuntu 22.04: musl, mingw-w64,
GTK 3) and the full app on Linux and Windows. It runs the self-test, packages
the four archives with checksums and third-party license notices, and
publishes the GitHub release. A version with a hyphen (`0.2.0-rc1`)
becomes a prerelease. [pages.yml](.github/workflows/pages.yml) publishes
`site/` (one-time setup: Settings > Pages > Source: GitHub Actions).

## Benchmark harness

Runs testers on a remote Windows or Linux machine over ssh (`MSBENCH_HOST`,
default `cutestation-win`), stops each at its first error, and appends a
record to `results/runs.jsonl`. Each record holds the outcome (`pass`, `error`,
`reboot`, `harness_lost`, `host_down`), the time to first error, and any
WHEA, Kernel-Power 41, or bugcheck events logged during the run.

```
harness/bench.py deploy --tm5 /path/to/TestMem5     # TM5 v0.13.1 folder; not redistributed here
harness/bench.py run karhu --label "3800 trfc 250" --seconds 1800 --repeat 3
harness/bench.py run tm5 --label "3800 trfc 250"            # Absolut @ anta777 by default
harness/bench.py run "tm5:Extreme @ anta777" --label ...
harness/bench.py run memslammer --label ... -- -M chain     # extra args after --
harness/bench.py run memslammer --warmup 600 --rate --seconds 120 -- -C flush
harness/bench.py timings                                    # what the IMC is running now
harness/bench.py summary
```

- Every run logs the DRAM timings the memory controller is actually running
  (decoded, plus the raw UMC registers). Without `--label`, the label is
  built from them (`3600 c14 rcd14 rp16 ras36 rfc324`), so you can't forget to
  record a setting. The timings reader (`harness/memtimings.c`) is a harness
  tool and isn't shipped in releases.
- memslammer runs record their mode: without `-M`, the harness passes
  `-M verify` (the default since the frontends split). Older records with no
  `-M` were `chain` runs.
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

### Linux targets

```
export MSBENCH_OS=linux MSBENCH_HOST=cutestation-linux
harness/bench.py deploy                                     # to /root/msbench
harness/bench.py run memslammer --idle 300
```

- Log in as root: reading the timings writes the SMN index register in PCI
  config space (sysfs), and that needs root and no kernel lockdown.
- memslammer only. Karhu and TM5 are Windows programs.
- Free RAM is `MemAvailable`, which includes reclaimable page cache.
  Hardware events come from the kernel log: `mce` counts machine-check, EDAC
  and memory-failure messages (WHEA's role on Windows), and `oops` counts
  kernel oopses and segfaults.
- Records carry `"os": "linux"`, and `summary` lists them separately.

## License

MIT. This repository contains no code or binaries from the tools studied.
The full app bundles open-source Rust crates (GPUI, gpui-kit, Lucide icons
and their dependencies); their notices ship in `THIRD-PARTY-LICENSES.txt`.
