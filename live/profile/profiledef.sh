#!/usr/bin/env bash
# shellcheck disable=SC2034
# archiso profile for the memslammer live USB. Don't build this directory
# directly: live/build.sh copies it, adds the binaries, ssh keys and Wi-Fi
# settings, sets iso_version, and runs mkarchiso on the copy.

iso_name="memslammer-live"
iso_label="MSLIVE_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="memslammer <https://github.com/Francesco149/memslammer>"
iso_application="memslammer live USB"
iso_version="$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y.%m.%d)"
install_dir="arch"
bootmodes=('bios.syslinux'
           'uefi.systemd-boot')
pacman_conf="pacman.conf"
airootfs_image_type="squashfs"
airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '19' '-b' '1M')
# copytoram=n: archiso copies the root image to RAM when booted from USB with
# enough memory. That pins ~1 GB the tester can't use, so read it from the
# stick instead (keep the stick plugged in).
# consoleblank=0: keep the status screen on during long runs.
# noresume: never resume a hibernation image an installed OS left behind.
# erst_disable, efi_pstore.pstore_disable=1: an oops or panic (likely, on
# unstable memory) must not write a crash dump into the platform's ERST store
# or EFI variables. (Arch already defaults EFI pstore to off.)
kernel_params_x86_64="copytoram=n consoleblank=0 noresume erst_disable efi_pstore.pstore_disable=1"
file_permissions=(
  ["/etc/shadow"]="0:0:400"
  ["/etc/ssh/ssh_host_ed25519_key"]="0:0:600"
  ["/root"]="0:0:750"
  ["/root/.ssh/"]="0:0:700"
  ["/root/msbench/"]="0:0:755"
  ["/usr/local/bin/"]="0:0:755"
  ["/var/lib/iwd/"]="0:0:700"
)
