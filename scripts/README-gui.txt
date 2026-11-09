memslammer-gui @VERSION@ - memory stress test for DDR overclock validation

The full desktop app: live throughput chart, per-thread progress, error
list with failing bit positions, run history and a copyable report. It
runs the same test engine as the CLI and lite window.

No installer; run memslammer-gui. Linux needs Vulkan or OpenGL drivers.
Needs an AVX2 CPU (Haswell / Zen or newer). Run history is kept in
%APPDATA%\memslammer (Windows) or ~/.local/share/memslammer (Linux).

The command line and small native window are a separate, smaller download.

https://github.com/Francesco149/memslammer  ·  MIT license (LICENSE.txt);
bundled open-source components: THIRD-PARTY-LICENSES.txt
