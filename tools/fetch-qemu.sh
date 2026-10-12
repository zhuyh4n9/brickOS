#!/usr/bin/env bash
#
# fetch-qemu — unpack **QEMU 10.2.1** (Ubuntu 26.04 "resolute" archive) in place into a copy
#              that runs directly on this host, under `prebuilts/toolchain/qemu/` (derived dir,
#              not tracked), plus a wrapper
#              `prebuilts/toolchain/bin/qemu-system-aarch64` — `brickie` tool resolution **looks in
#              this directory first** (see `tools/brickie/rust/src/build.rs::resolve_tool`), so
#              `brickie run` needs no config change.
#
# ── Why this is needed ────────────────────────────────────────────────────────
# `brickie run` and the gates `emmc-test`/`fat-test`/`erofs-test` all need `-device emmc`. QEMU's eMMC card model:
#   * **9.1** introduced `TYPE_EMMC`, but `emmc_class_init()` sets `dc->user_creatable = false`
#     (the comment even says "Reason: Soldered on board") => `-device emmc` fails with
#     `'emmc' is not a valid device model name` (this is the symptom on host QEMU 8.2.2);
#   * only **10.2.0** removed that line => `-device emmc` became a pluggable device.
# The repo's reference environment is exactly 10.2.1 (see the local evidence in
# `docs/analysis/0001-emmc-bdev-gap.md` and the Ubuntu 26.04 archive referenced by
# `prebuilts/toolchain.lock.toml`); this script fetches that build **verbatim** — so switching
# distributions just to run the gates is no longer required.
#
# ── Why not put it in prebuilts/toolchain.lock.toml ───────────────────────────
# That lock covers the **cross toolchain** (goes on PATH from Makefile/setup.sh, used for target
# builds); this is a **host compatibility shim** (needed only when the host QEMU is < 10.2), pinned
# per distribution and also generating a loader wrapper and bundle — different semantics from that
# lock, and mixing them would add a host-distribution dimension to the cross toolchain.
# **Once the host has >= 10.2 (e.g. Ubuntu 26.04), `prebuilts/toolchain/qemu/` can be deleted whole.**
#
# ── Why no root is needed ─────────────────────────────────────────────────────
# resolute's libc is **2.43**, newer than noble's 2.39 => that qemu binary **cannot** run directly
# here (it reports `GLIBC_2.4x not found`). The fix is to **explicitly invoke** resolute's own
# loader with `--library-path`:
#   sysroot first (resolute's libc6/libfuse3-4/libslirp0 — soname or symbol versions changed, see below)
#   -> host /lib, /usr/lib as fallback (glib/pixman/zlib etc. are new enough; host copies are fine).
# So three packages must come from resolute: `libc6` (2.43), `libfuse3-4` (soname changed from .so.3
# to .so.4), `libslirp0` (symbol version `SLIRP_4.9`). Those are the three non-qemu rows in the PIN table below.
#
# ── Why qemu-bundle as well ───────────────────────────────────────────────────
# At startup QEMU loads each **compile-time registered** module. Without isolation it looks for
# same-named files in the host `/usr/lib/x86_64-linux-gnu/qemu/` (8.2.2 modules on this machine)
# => dlopen of a .so from a different build => `assert(QTAILQ_EMPTY(&dso_init_list))` in
# `util/module.c` aborts (measured: rc=134).
# This uses QEMU's own **qemu-bundle** mechanism: when a `qemu-bundle/` sits next to the executable's
# directory, `get_relocated_path()` prefixes the **modules/data/firmware** paths with that directory
# (the original `dir` is appended as-is), so everything from the 11 packages stays inside the
# sysroot and the host copies are never touched.
# Note how "the executable's directory" is determined: we **invoke the loader explicitly**, so
# `/proc/self/exe` points at the **loader**, and the bundle must sit **next to the loader**
# (`sysroot/usr/lib/x86_64-linux-gnu/qemu-bundle/`), not next to the qemu binary. The self-check at
# the end of this file (`-device help` assertion) guards this.
#
# ── Provenance and replayability ──────────────────────────────────────────────
# The URLs/SHA256s in the PIN table are **not** local trust-on-first-use (TOFU): each one is taken
# from the `Filename` and `SHA256` fields of the archive's
# `dists/resolute/main/binary-amd64/Packages.gz` index (that index is vouched for by the
# `InRelease` signature). If upstream replaces a package, verification fails and fetching fails;
# versions are never silently swapped.
#
# Requires: python3 (downloads, standard library only, same stance as `tools/fetch-prebuilt.py`) + dpkg-deb (unpacks .deb).
#       **No** root, and no curl/wget/tar.
#
# Usage:
#   bash tools/fetch-qemu.sh            # fetch + unpack + generate wrapper + self-check
#   bash tools/fetch-qemu.sh --check    # self-check only (no download; for an already-populated tree)
#
# Exit codes: 0 success / 1 verification or unpack failure / 2 usage or environment error
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

# ── PIN table: filename|sha256|archive dir ────────────────────────────────────
# 11 packages = (1) the target emulator and its companions (qemu-system-arm provides
#   qemu-system-aarch64 for aarch64-softmmu; the .so modules from
#   qemu-system-{common,data,gui,modules-opengl,modules-spice} must **all** be present —
#   miss one name and QEMU falls back to the host copy => hits the module assert again; with
#   every name present, a failed open is only a harmless warning);
#   (2) ipxe-qemu: `-M virt` attaches a virtio-net by default and its option ROM lives here;
#   (3) three resolute libraries (libc6 / libfuse3-4 / libslirp0), see the file header.
# Version: qemu 1:10.2.1+ds-1ubuntu3, others as in the filenames. All from resolute main/binary-amd64.
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

# ── Self-check (and the whole content of --check) ─────────────────────────────
selfcheck() {
    [ -x "$LINK" ] || die "wrapper missing: $LINK — run 'bash tools/fetch-qemu.sh' first"
    local ver
    ver=$("$LINK" --version 2>/dev/null | head -1) || die "wrapper will not run: $LINK"
    case "$ver" in
        *10.2.1*) info "ok   $ver" ;;
        *)        die "wrong version (need 10.2.1): $ver" ;;
    esac
    # This one check proves three things at once: it runs, the module dir is isolated, emmc is pluggable.
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

# ── (1) fetch + verify ────────────────────────────────────────────────────────
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

# ── (2) unpack (all 11 into one sysroot; dpkg-deb -x is idempotent) ────────────
for row in "${PIN[@]}"; do
    IFS='|' read -r fname _rest <<<"$row"
    dpkg-deb -x "$CACHE/$fname" "$SYS" || die "unpack failed: $fname"
done
[ -f "$QFEXE" ]  || die "after unpacking, $QFEXE is missing"
[ -f "$LOADER" ] || die "after unpacking, resolute's loader is missing: $LOADER"
info "ok   unpacked 11 packages => $SYS"

# ── (3) qemu-bundle: keep the module/data/firmware paths inside the sysroot (see header) ──
# The directory names must match the absolute paths in the archive — get_relocated_path()
# appends the **absolute paths** compiled into the binary (CONFIG_QEMU_MODDIR etc.) to the bundle.
# * Symlink targets must be **absolute**: `$SYS` is relative to the repo root, and `ln -s` stores
#   the target string **verbatim**, resolved by the reader relative to the **symlink's own
#   directory** => relative targets always fail to resolve
#   (symptom: `qemu-system-aarch64: failed to find romfile "efi-virtio.rom"`, because all three
#   symlinks in the bundle were dangling). So take absolute paths first, then link.
SYS_ABS="$(cd "$SYS" && pwd)"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/usr/lib/x86_64-linux-gnu" "$BUNDLE/usr/lib/ipxe" "$BUNDLE/usr/share"
ln -sfn "$SYS_ABS/usr/lib/x86_64-linux-gnu/qemu" "$BUNDLE/usr/lib/x86_64-linux-gnu/qemu"
ln -sfn "$SYS_ABS/usr/share/qemu"                "$BUNDLE/usr/share/qemu"
# The ipxe ROM location **varies with the distribution layout**: noble uses `/usr/lib/ipxe/qemu/`,
# while resolute's ipxe-qemu installs `efi-*.rom` straight into `/usr/share/qemu/` (already covered
# by the symlink above). So create this one **on demand** — a dangling link would only make the next
# reader think it is required (firmware is found by name, so a dangling directory is harmless).
if [ -d "$SYS/usr/lib/ipxe/qemu" ]; then
    ln -sfn "$SYS_ABS/usr/lib/ipxe/qemu" "$BUNDLE/usr/lib/ipxe/qemu"
fi
# Self-check: both **required** symlinks must resolve — otherwise QEMU will not start, and that
# would only surface when a gate runs.
for p in usr/lib/x86_64-linux-gnu/qemu usr/share/qemu; do
    [ -e "$BUNDLE/$p" ] || die "qemu-bundle symlink is dangling: $BUNDLE/$p"
done
info "ok   qemu-bundle ($(ls "$SYS/usr/lib/x86_64-linux-gnu/qemu" | wc -l) .so modules / data / ipxe ROMs)"

# ── (4) wrapper: invoke resolute's loader explicitly, sysroot first, host libs as fallback ──
cat >"$WRAP" <<'SH'
#!/bin/sh
# Generated by tools/fetch-qemu.sh — do not edit by hand (edit that script instead).
#
# Why this layer exists: this qemu is an Ubuntu 26.04 (resolute) build and its dynamic section
# requires glibc 2.43, while the host (e.g. noble) only has 2.39 => it cannot be executed directly.
# The fix is to invoke resolute's own loader explicitly, letting sysroot libraries win and host
# /lib and /usr/lib act as fallback (glib/pixman/zlib and friends are new enough there).
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

# ── (5) brickie tool resolution looks in prebuilts/toolchain/bin/ first (resolve_tool) ──
ln -sfn "../qemu/bin/qemu-system-aarch64" "$LINK"
info "ok   $LINK -> ../qemu/bin/qemu-system-aarch64"

# ── (6) self-check ────────────────────────────────────────────────────────────
selfcheck
echo "ok   QEMU 10.2.1 in place: prebuilts/toolchain/qemu (the host copy is untouched)"
