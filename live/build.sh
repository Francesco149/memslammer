#!/usr/bin/env bash
# Builds the memslammer live ISO (see live/README.md): live/profile plus this
# checkout's CLI, the harness's remote files, and the settings in
# live/local/config (ssh keys, Wi-Fi networks).
#
#   live/build.sh        -> build/live/memslammer-live-VERSION-x86_64.iso
#
# Uses mkarchiso if it's installed (Arch: pacman -S archiso), otherwise runs it
# in an archlinux container with podman or docker. Both need root (sudo).
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname -- "$here")
local_dir=${MSLIVE_LOCAL:-$here/local}
out=$root/build/live
stage=$out/stage
SUDO=${SUDO-sudo}
((EUID)) || SUDO=

die() { echo "live/build.sh: $*" >&2; exit 1; }

# ---- settings: live/local/config calls these ----
keys=() wifi_ssids=() wifi_psks=()
hostname=memslammer
ssh_key() {
    [[ -r $1 ]] || die "ssh_key: can't read $1"
    keys+=("$(<"$1")")
}
wifi() {
    (($# == 2)) || die "usage in config: wifi SSID PASSPHRASE"
    [[ -n $1 ]] || die "wifi: empty SSID"
    ((${#2} >= 8 && ${#2} <= 63)) || die "wifi \"$1\": a WPA passphrase has 8 to 63 characters"
    wifi_ssids+=("$1")
    wifi_psks+=("$2")
}
[[ -f $local_dir/config ]] || die "no $local_dir/config: copy live/config.example there and edit it"
# shellcheck source=/dev/null
. "$local_dir/config"
((${#keys[@]})) || die "$local_dir/config adds no ssh_key, so nobody could log in"

# One host key per checkout, reused by every build: machines booted from the
# stick all present it, so ssh can check it under one alias (README).
hostkey=$local_dir/ssh_host_ed25519_key
[[ -f $hostkey ]] || ssh-keygen -q -t ed25519 -N '' -C "$hostname-live" -f "$hostkey"

# -B: the version string is a compiler flag, so make wouldn't rebuild a binary
# whose sources haven't changed since an older commit.
make -B -C "$root" build/memslammer build/memtimings
version=$(git -C "$root" describe --tags --always --dirty 2>/dev/null | sed 's/^v//' || true)
version=${version:-dev}
iso=$out/memslammer-live-$version-x86_64.iso

# ---- stage a copy of the profile; it holds secrets, so it's private and
# deleted on exit together with mkarchiso's work dir ----
cleanup() { $SUDO rm -rf -- "$stage" "$out/work"; }
trap cleanup EXIT
mkdir -p "$out"
cleanup
mkdir -m 700 "$stage"
cp -a "$here/profile/." "$stage/"
a=$stage/airootfs

sed -i "s/^iso_version=.*/iso_version=\"$version\"/" "$stage/profiledef.sh"
printf '%s\n' "$hostname" >"$a/etc/hostname"
printf '%s, built %s\n' "$version" "$(date -u +%Y-%m-%d)" >"$a/etc/memslammer-live"
install -Dm755 -t "$a/usr/local/bin" "$root/build/memslammer" "$root/build/memtimings"
# What `harness/bench.py deploy` copies, so the harness works before a deploy.
install -Dm755 -t "$a/root/msbench" "$root/build/memslammer" "$root/build/memtimings" \
    "$root"/harness/remote/*.sh
install -d "$a/root/.ssh" "$a/var/lib/iwd"
printf '%s\n' "${keys[@]}" >"$a/root/.ssh/authorized_keys"
install -m600 "$hostkey" "$a/etc/ssh/ssh_host_ed25519_key"
install -m644 "$hostkey.pub" "$a/etc/ssh/ssh_host_ed25519_key.pub"
for i in "${!wifi_ssids[@]}"; do
    # iwd's naming: the SSID itself if it's only alphanumerics, space, - and _,
    # otherwise "=" and the SSID in hex.
    s=${wifi_ssids[i]}
    [[ $s =~ ^[[:alnum:]\ _-]+$ ]] || s="=$(printf %s "$s" | od -An -tx1 | tr -d ' \n')"
    printf '[Security]\nPassphrase=%s\n' "${wifi_psks[i]}" >"$a/var/lib/iwd/$s.psk"
done

# ---- mkarchiso ----
rm -f -- "$iso"
if command -v mkarchiso >/dev/null; then
    $SUDO mkarchiso -v -r -w "$out/work" -o "$out" "$stage"
else
    engine=$(command -v podman || command -v docker) ||
        die "needs mkarchiso (Arch: pacman -S archiso), podman or docker"
    mkdir -p "$out/pkg"   # package cache, reused across builds
    $SUDO "$engine" run --rm --privileged --security-opt label=disable \
        -v "$out:/live" -v "$out/pkg:/var/cache/pacman/pkg" \
        docker.io/library/archlinux:latest \
        bash -euc 'pacman -Syu --noconfirm --needed archiso >/dev/null
                   mkarchiso -v -r -w /live/work -o /live /live/stage'
fi
[[ -f $iso ]] || die "mkarchiso made no $iso"
$SUDO chown "$(id -u):$(id -g)" "$iso"
chmod 600 "$iso"

cat <<EOF

$iso ($(du -h "$iso" | cut -f1))
It contains your Wi-Fi passphrases and the live host key: keep it private.
Copy it to a Ventoy stick, or dd it to a USB drive.
known_hosts line for the ssh config in live/README.md:
memslammer-live $(cut -d' ' -f1,2 "$hostkey.pub")
EOF
