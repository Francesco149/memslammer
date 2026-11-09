# memslammer live USB

A bootable Arch Linux image for running memslammer, and the
[benchmark harness](../README.md#benchmark-harness), on any x86-64 machine.
Plug the stick in and pick it in the boot menu. It never touches the machine's
disks or settings, starts sshd with your key, and finds a network on its own
(Ethernet and Wi-Fi). It's a lab tool: we don't publish the ISO, because it
contains your Wi-Fi passphrase and an ssh host key. This directory is the recipe.

## Build

```
cp live/config.example live/local/config    # ssh keys, Wi-Fi networks
$EDITOR live/local/config
make live                                   # = live/build.sh
cp build/live/memslammer-live-*.iso /run/media/$USER/Ventoy/memslammer-live.iso
```

- Needs sudo plus either mkarchiso (Arch: `pacman -S archiso`) or podman or
  docker. Without mkarchiso it builds in an `archlinux` container, with the
  package cache kept in `build/live/pkg`.
- The first build downloads ~700 MB of packages, a few minutes on a fast
  mirror; rebuilds take about a minute. The ISO is ~900 MB. Copying it under
  a fixed name on the stick makes each build replace the last.
- The first build also creates `live/local/ssh_host_ed25519_key`, and every
  later build reuses it. git ignores `live/local/`.
- Each build packs current Arch packages and the CLI from this checkout.
  Rebuild after changing the CLI, or run the harness's `deploy` (below).

## Boot

- Ventoy: pick the ISO, then `Boot in normal mode`; the image's own menu
  boots its default entry after 3 s. Without Ventoy, `dd` the ISO to a stick.
- UEFI or legacy BIOS. Secure Boot must be off, because the image isn't signed.
- The `serial console` entry also puts a console on ttyS0 at 115200 baud, for
  headless boxes and IPMI serial-over-LAN.
- Keep the stick plugged in. The root image is read from it, not copied to
  RAM (archiso's default would pin ~1 GB the tester then can't use).

tty1 shows a status screen that redraws every 3 s: where to ssh, the board and
BIOS, the DIMMs, the timings the memory controller runs (AMD Zen 2/3 DDR4),
DIMM temperatures when the kernel has a sensor for them, whether memslammer is
running, and any machine check, EDAC error, oops or segfault in the kernel log.
Press any key for a root shell. An ssh login prints the same screen once.

## Connecting

The image answers as `memslammer.local` (mDNS), `memslammer` (LLMNR), and
sends its hostname with DHCP, so routers that register leases may resolve it
too (`memslammer.lan`, or whatever your LAN's domain is). A router that
already knows the machine's MAC tends to keep its old name for it.

Every boot presents the same host key, so tell ssh to check it under one
name, whatever the address:

```
# ~/.ssh/config
Host ms-live
    HostName memslammer.local
    User root
    HostKeyAlias memslammer-live
```

Add the `known_hosts` line that `build.sh` prints. To reach a machine by
address instead: `ssh -o HostKeyAlias=memslammer-live root@10.0.0.42`.

Harness:

```
export MSBENCH_OS=linux MSBENCH_HOST=ms-live
harness/bench.py run memslammer --idle 300     # /root/msbench is already populated
harness/bench.py deploy                        # after changing the CLI
```

If two machines run it at once, the one that boots second takes a numbered
name (`memslammer7.local`, for example). Two that boot at the same moment may
both rename. The status screen shows the name in use.

## Networking

- **Ethernet**: DHCP on every port, including USB adapters and phone
  tethering. With no DHCP server (a cable straight to another PC) it adds a
  169.254/16 address within a few seconds, next to the IPv6 link-local
  address it always has. Leases are keyed by MAC, so a machine keeps its
  address across boots.
- **Wi-Fi**: iwd joins the networks from the config, and networkd runs DHCP
  on them. Wi-Fi comes up alongside Ethernet rather than after a timeout, and
  the default route prefers Ethernet. This way Wi-Fi still works when a cable
  is plugged into a dead port. Radios are rfkill-unblocked at boot. Firmware
  is included for Intel, Realtek, MediaTek, Qualcomm/Atheros, Broadcom
  (brcmfmac) and Marvell.
- By hand, at the console: `iwctl station wlan0 scan`, `iwctl station wlan0 get-networks`,
  `iwctl station wlan0 connect SSID`.

## Leaving the machine alone

- Nothing on the internal disks is mounted, assembled or activated. There's
  no fstab, systemd's gpt-auto generator is masked (it would mount the ESP and
  enable swap partitions), and the image has no mdadm, LVM or udisks.
- No swap or zram, so the tester's memory stays in RAM.
- The hardware clock is never written. chrony sets the system clock from
  NTP (so the journal and the harness's event window are right) without
  `rtcsync`, which keeps the kernel from copying UTC into the RTC every 11
  minutes. That copy would shift the clock of a Windows install, which keeps
  the RTC in local time. systemd-timesyncd, which can't be stopped from doing
  this, is masked. Offline, the clock shows whatever the RTC holds.
- The live system doesn't write EFI variables, the TPM or crash-record
  stores. These units, which can, are masked:
  - `systemd-boot-random-seed`: a seed file on an ESP, plus an EFI variable.
  - `systemd-hibernate-clear` and the hibernate-resume generator: would clear,
    or resume from, the hibernation image of an installed Linux. `noresume`
    on the kernel line backs this up.
  - `systemd-tpm2-setup(-early)` and `systemd-pcrproduct`: a persistent key
    and NV indexes in the TPM.
  - `systemd-pstore`: would archive, then delete, crash records an installed
    OS left. `erst_disable` and `efi_pstore.pstore_disable=1` stop the
    kernel's own crash dumps from going to the ERST store or EFI variables,
    which an oops on unstable RAM would trigger.
  - The factory-reset generator.

  Boot-time PCR measurements still happen; they reset with the machine.
- No persistent state. Writes go to a RAM overlay, and the stick is only read.
- nouveau is blacklisted (the console uses the firmware framebuffer). The
  image carries no NVIDIA firmware, and a GPU driver hang would look like
  memory instability.

## Inside

- Arch Linux with the current `linux` kernel and early-loaded AMD and Intel
  microcode.
- `/usr/local/bin`: `memslammer` (this checkout), `memtimings` (the harness's
  timings reader), `ms-status` (the status screen).
- `/root/msbench`: what `harness/bench.py deploy` copies.
- chrony, and tools: dmidecode, i2c-tools (`decode-dimms`), lm_sensors, htop, tmux, iw,
  ethtool, rsync, nano.
- More packages: `pacman-key --init && pacman-key --populate && pacman -Sy PKG`.
  They install into the RAM overlay, which holds 256 MiB by default. For
  more, add `cow_spacesize=1G` to the boot entry (`e` to edit it in the UEFI
  menu, Tab in the BIOS one).

Files: `profile/` is the archiso profile (`packages.x86_64`, boot entries,
`airootfs/` overlay). `build.sh` copies it and adds the binaries, keys and
Wi-Fi files from `local/config` before running mkarchiso. Config files say
why in comments. Masks are `/dev/null` links, explained above.
