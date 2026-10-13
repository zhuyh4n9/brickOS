#!/usr/bin/env bash
# fetch-qemu — unpack QEMU 10.2.1 (Ubuntu 26.04 "resolute" archive) in place, running directly on
# this host under prebuilts/toolchain/qemu/ (derived, untracked), plus a wrapper at
# prebuilts/toolchain/bin/qemu-system-aarch64. brickie's tool resolution looks there first
# (build.rs::resolve_tool), so `brickie run` needs no config change.
# Why: brickie run and the emmc/fat/erofs gates need `-device emmc`. QEMU 9.1 added TYPE_EMMC but
# set user_creatable = false ("Reason: Soldered on board"), so -device emmc was invalid; only 10.2.0
# removed that line. The repo's reference environment is 10.2.1, fetched verbatim here.
# Not in prebuilts/toolchain.lock.toml: that lock covers the cross toolchain (on PATH, used for
# target builds); this is a host compatibility shim, pinned per distribution, with different
# semantics. Once the host has >= 10.2, prebuilts/toolchain/qemu/ can be deleted whole.
# No root needed: resolute's libc is 2.43 (newer than noble's 2.39), so the binary cannot run
# directly; this invokes resolute's own loader with --library-path, sysroot first and host /lib,
# /usr/lib as fallback. Three packages come from resolute: libc6, libfuse3-4 (soname .so.3 -> .so.4)
# and libslirp0 (symbol version SLIRP_4.9) -- the three non-qemu rows in the PIN table.
# qemu-bundle: QEMU loads compile-time registered modules at startup and would dlopen the host's
# 8.2.2 modules from /usr/lib/x86_64-linux-gnu/qemu/ (mismatch => assert in util/module.c).
# qemu-bundle prefixes the module/data/firmware paths with the directory of the *executable*; since
# the loader is invoked explicitly, the bundle must sit next to the loader, not the qemu binary.
# Provenance: URLs/SHA256s come from the Filename and SHA256 fields of the archive's Packages.gz
# index (vouched for by InRelease), not trust-on-first-use; a replaced package fails verification.
# Requires python3 (downloads, stdlib only) + dpkg-deb (unpacks .deb); no root, no curl/wget/tar.
# Usage: bash tools/fetch-qemu.sh [--check]; --check is self-check only, no download. Exit 0
# success, 1 verification or unpack failure, 2 usage or environment error.
set -euo pipefail

cd "$(dirname "$0")/.." || exit 2

BASE="http://archive.ubuntu.com/ubuntu"
CACHE="prebuilts/toolchain/.cache/qemu"
DEST="prebuilts/toolchain/qemu"
SYS="$DEST/sysroot"
BINDIR="prebuilts/toolchain/bin"
WRAP="$DEST/bin/qemu-system-aarch64"
LINK="$BINDIR/qemu-system-aarch64"
LOADER="$SYS/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"
QFEXE="$SYS/usr/bin/qemu-system-aarch64"
BUNDLE="$SYS/usr/lib/x86_64-linux-gnu/qemu-bundle"

die() { echo "FAIL: $*" >&2; exit "${2:-1}"; }
info() { echo "  $*"; }

# ---- PIN table: filename|sha256|archive dir ----
# 11 packages = the target emulator and its companions (qemu-system-arm provides qemu-system-aarch64
# for aarch64-softmmu; every .so module from qemu-system-{common,data,gui,modules-opengl,
# modules-spice} must be present, else QEMU falls back to the host copy and hits the module assert),
# ipxe-qemu (`-M virt` attaches a virtio-net by default and its option ROM lives here), and the
# three resolute libraries. Version: qemu 1:10.2.1+ds-1ubuntu3, resolute main/binary-amd64.
PIN=(
  "qemu-system-arm_10.2.1+ds-1ubuntu3_amd64.deb|37f6309f2e79dc836490e11a1df27848219b0d208a84b7811f3273ccd0daa831|pool/main/q/qemu"
  "qemu-system-common_10.2.1+ds-1ubuntu3_amd64.deb|4af56406c0d00611c9d85a0dcff2eb1c838bfd7fdbdc43420ce513d426655bfb|pool/main/q/qemu"
  "qemu-system-data_10.2.1+ds-1ubuntu3_all.deb|908d03cc03f7d42673c87a428a4689a0078992347db88e32bbbcdf4de918b497|pool/main/q/qemu"
  "qemu-system-gui_10.2.1+ds-1ubuntu3_amd64.deb|83c0973b059f6dafa60d45c0b66761630c762f64d28f8abd7697c593200f5356|pool/main/q/qemu"
  "qemu-system-modules-opengl_10.2.1+ds-1ubuntu3_amd64.deb|c8c3f4b25a8e9530c2c2a9569f3be835673e323a987a34b7cbd86694a0457fd7|pool/main/q/qemu"
  "qemu-system-modules-spice_10.2.1+ds-1ubuntu3_amd64.deb|45317661706b4d0e81bd88a0f789f08f4eb2d6bc19a5af3751d57947892e39da|pool/main/q/qemu"
  "qemu-block-extra_10.2.1+ds-1ubuntu3_amd64.deb|e72b506431977802b2e006e62161a8cef54ea8c5e29828ef6034ee722397e52b|pool/main/q/qemu"
  "ipxe-qemu_1.21.1+git-20250829.969ce2c55+dfsg-3ubuntu2_all.deb|51c33251213ebd3c9613273b7e04890cac7e9fc4ec133d32a86a5f4170719410|pool/main/i/ipxe"
  "libc6_2.43-2ubuntu2_amd64.deb|c13775dc0c984403f3fcad229d14507a9f387763bd07ace1e5f93897ee6b8434|pool/main/g/glibc"
  "libfuse3-4_3.18.2-1_amd64.deb|59ae972173e389eb981aaeae874b3d6aafe7eceff284f94e5c21ee1d049548c6|pool/main/f/fuse3"
  "libslirp0_4.9.1-1ubuntu1_amd64.deb|46fcf69586c8ad63b3d47fd1956fd7469d6dbe3fa1e2533497fa16f7aeb8e2f2|pool/main/libs/libslirp"
)

# ---- self-check (and the whole content of --check) ----
selfcheck() {
    [ -x "$LINK" ] || die "wrapper missing: $LINK — run 'bash tools/fetch-qemu.sh' first"
    local ver
    ver=$("$LINK" --version 2>/dev/null | head -1) || die "wrapper will not run: $LINK"
    case "$ver" in
        *10.2.1*) info "ok   $ver" ;;
        *)        die "wrong version (need 10.2.1): $ver" ;;
    esac
    # Proves three things: it runs, the module dir is isolated, emmc is pluggable.
    if ! "$LINK" -device help 2>/dev/null | grep -q 'name "emmc"'; then
        die "'-device emmc' unavailable — not a 10.2 build, or the module dir is not isolated"
    fi
    info "ok   -device emmc is pluggable (eMMC card model since 10.2)"
}

if [ "${1:-}" = "--check" ]; then
    selfcheck
    exit 0
fi
[ $# -eq 0 ] || die "usage: bash tools/fetch-qemu.sh [--check]" 2

command -v python3  >/dev/null || die "python3 missing (used for downloads; standard library only)" 2
command -v dpkg-deb >/dev/null || die "dpkg-deb missing (unpacks .deb; ships with dpkg)" 2

mkdir -p "$CACHE" "$SYS" "$DEST/bin" "$BINDIR"

# ---- (1) fetch + verify ----
for row in "${PIN[@]}"; do
    IFS='|' read -r fname sha dir <<<"$row"
    out="$CACHE/$fname"
    if [ -f "$out" ] && [ "$(sha256sum "$out" | cut -d' ' -f1)" = "$sha" ]; then
        info "cached $fname"
    else
        rm -f "$out"
        info "fetch  $fname"
        python3 - "$BASE/$dir/$fname" "$out" <<'PY' || die "download failed: $fname"
import sys, urllib.request
urllib.request.urlretrieve(sys.argv[1], sys.argv[2])
PY
    fi
    got=$(sha256sum "$out" | cut -d' ' -f1)
    [ "$got" = "$sha" ] || die "sha256 mismatch for $fname
  expected $sha
  got      $got
  (did upstream replace the package? never follow silently — check the archive, then update the PIN table in this script)"
done

# ---- (2) unpack (all 11 into one sysroot; dpkg-deb -x is idempotent) ----
for row in "${PIN[@]}"; do
    IFS='|' read -r fname _rest <<<"$row"
    dpkg-deb -x "$CACHE/$fname" "$SYS" || die "unpack failed: $fname"
done
[ -f "$QFEXE" ]  || die "after unpacking, $QFEXE is missing"
[ -f "$LOADER" ] || die "after unpacking, resolute's loader is missing: $LOADER"
info "ok   unpacked 11 packages => $SYS"

# ---- (3) qemu-bundle: keep the module/data/firmware paths inside the sysroot ----
# The directory names must match the absolute paths compiled into the binary (CONFIG_QEMU_MODDIR);
# symlink targets must be absolute, because ln -s stores the target verbatim and it is resolved
# relative to the symlink's own directory (relative targets dangle: "failed to find romfile").
SYS_ABS="$(cd "$SYS" && pwd)"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/usr/lib/x86_64-linux-gnu" "$BUNDLE/usr/lib/ipxe" "$BUNDLE/usr/share"
ln -sfn "$SYS_ABS/usr/lib/x86_64-linux-gnu/qemu" "$BUNDLE/usr/lib/x86_64-linux-gnu/qemu"
ln -sfn "$SYS_ABS/usr/share/qemu"                "$BUNDLE/usr/share/qemu"
# The ipxe ROM location varies by distribution: resolute's ipxe-qemu installs efi-*.rom straight
# into /usr/share/qemu/ (covered above), so create noble's /usr/lib/ipxe/qemu/ only if present.
if [ -d "$SYS/usr/lib/ipxe/qemu" ]; then
    ln -sfn "$SYS_ABS/usr/lib/ipxe/qemu" "$BUNDLE/usr/lib/ipxe/qemu"
fi
# Both required symlinks must resolve, otherwise QEMU will not start.
for p in usr/lib/x86_64-linux-gnu/qemu usr/share/qemu; do
    [ -e "$BUNDLE/$p" ] || die "qemu-bundle symlink is dangling: $BUNDLE/$p"
done
info "ok   qemu-bundle ($(ls "$SYS/usr/lib/x86_64-linux-gnu/qemu" | wc -l) .so modules / data / ipxe ROMs)"

# ---- (4) wrapper: invoke resolute's loader explicitly, sysroot first, host libs as fallback ----
cat >"$WRAP" <<'SH'
#!/bin/sh
# Generated by tools/fetch-qemu.sh — do not edit by hand (edit that script instead).
# This layer exists because the qemu is a resolute build requiring glibc 2.43 while the host has
# 2.39: resolute's own loader is invoked explicitly, sysroot libraries first, host /lib as fallback.
set -e
self=$0
while [ -L "$self" ]; do                       # resolve symlinks: bin/qemu-system-aarch64 -> ../qemu/bin/...
    target=$(readlink "$self")
    case $target in
        /*) self=$target ;;
        *)  self=$(dirname "$self")/$target ;;
    esac
done
here=$(CDPATH= cd -- "$(dirname -- "$self")" && pwd)
sys=$here/../sysroot
exec "$sys/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" \
     --library-path "$sys/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu" \
     "$sys/usr/bin/qemu-system-aarch64" "$@"
SH
chmod +x "$WRAP"

# ---- (5) brickie tool resolution looks in prebuilts/toolchain/bin/ first (resolve_tool) ----
ln -sfn "../qemu/bin/qemu-system-aarch64" "$LINK"
info "ok   $LINK -> ../qemu/bin/qemu-system-aarch64"

# ---- (6) self-check ----
selfcheck
echo "ok   QEMU 10.2.1 in place: prebuilts/toolchain/qemu (the host copy is untouched)"
