memslammer @VERSION@ - memory stress test for DDR overclock validation

  memslammer       command line; run with -h for options
  memslammer-lite  small native window (Win32 on Windows, GTK 3 on Linux)

Same test engine in both, no installer, no dependencies (Linux: the lite
window needs GTK 3). Default: verify mode on 90% of free RAM until stopped,
the fastest-failing configuration we have measured. Any error means the
memory overclock is unstable. Needs an AVX2 CPU (Haswell / Zen or newer).

The full GPU-rendered app (charts, error bit map, history) is a separate
download: memslammer-gui.

https://github.com/Francesco149/memslammer  ·  MIT license (LICENSE.txt)
