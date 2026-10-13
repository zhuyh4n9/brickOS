#!/usr/bin/env bash
# brickOS prototype — FAT/EROFS backing image generator.
# The gates check the third-party FAT port (read/write real volumes) and the EROFS port
# (uncompressed + LZ4 images) against bytes from an external authoritative tool: mkfs.fat
# (dosfstools) and mkfs.erofs (erofs-utils), version- and sha256-pinned in
# prebuilts/toolchain.lock.toml. Images are not committed (contents change with the tests and
# cannot be replayed; FAT32's minimum is 32 MiB), so the locked tools rebuild them every time.
# Cards (names/order match [build.target.qemu].extra; paired one-to-one with -device emmc):
#   emmc0   2 MiB  FAT12 -> /fat12      emmc1  16 MiB  FAT16 -> /fat16
#   emmc2  32 MiB  FAT32 -> /fat32      emmc3  16 MiB  EROFS + 512 KiB scratch at tail -> /assets
# Cross-piece invariants: each FAT volume's label/contents are the constants in
# fs/uboot-fat/glue/fat_glue.h; the EROFS image is <= 1 MiB (asserted) and the scratch area
# starts at LBA 4096 / 2 MiB (BR_EMMC_SCRATCH_LBA), so overlap is computed, not conventional; the
# four card names/order come from the platform plugin's [build.target.qemu].extra.
# Usage: tools/mk-fs-images.sh [<output dir>]     default = build
set -euo pipefail

OUT_DIR="${1:-build}"
MKFS_FAT_REL="dosfstools/sbin/mkfs.fat"
FSCK_FAT_REL="dosfstools/sbin/fsck.fat"
MKFS_EROFS_REL="erofs-utils/bin/mkfs.erofs"
DUMP_EROFS_REL="erofs-utils/bin/dump.erofs"
MCOPY_REL="mtools/bin/mcopy"
MMD_REL="mtools/bin/mmd"

# ---- tool resolution ----
# Candidate order as for the cross toolchain: prebuilts/toolchain/ first (locked version), else a
# same-named tool on PATH; missing both fails loudly with the fetch command.
resolve() {
    local rel="$1" name="$2"
    if [ -x "prebuilts/toolchain/$rel" ]; then
        printf '%s\n' "prebuilts/toolchain/$rel"
        return 0
    fi
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
        return 0
    fi
    echo "mk-fs-images: $name not found" >&2
    echo "  fetch: python3 tools/fetch-prebuilt.py dosfstools erofs-utils mtools" >&2
    return 1
}

MKFS_FAT=$(resolve "$MKFS_FAT_REL"   mkfs.fat)
FSCK_FAT=$(resolve "$FSCK_FAT_REL"   fsck.fat)
MKFS_EROFS=$(resolve "$MKFS_EROFS_REL" mkfs.erofs)
DUMP_EROFS=$(resolve "$DUMP_EROFS_REL" dump.erofs)
MCOPY=$(resolve "$MCOPY_REL"         mcopy)
MMD=$(resolve "$MMD_REL"             mmd)

# mtools rejects a size/geometry mismatch (a false positive for our super-floppy volumes).
export MTOOLS_SKIP_CHECK=1

mkdir -p "$OUT_DIR"
STAGE="$OUT_DIR/fs-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# ---- content generation ----
# Content is generated with python3, byte-exactly (dd/head/tr behaviour would be host-dependent).
python3 - "$STAGE" <<'PY'
import os
import sys

stage = sys.argv[1]

def put(rel, data):
    p = os.path.join(stage, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)

# Files placed in the three FAT volumes (contents must match the constants in fat_internal.h)
put("fat/hello12.txt", b"fat12 hello\n")
put("fat/hello16.txt", b"fat16 hello\n")
put("fat/hello32.txt", b"fat32 hello\n")
put("fat/a16.bin",  b"A" * 16)
put("fat/a256.bin", b"B" * 256)
put("fat/a4096.bin", b"C" * 4096)
put("fat/lfn.txt",  b"lfn\n")

# Source tree for the EROFS image (all uncompressed here; whether to compress is mkfs.erofs's call).
# Sizes are chosen so one image covers all three datalayouts: hello.txt (12 B) and dir/sub/deep.txt
# (5 B) => FLAT_INLINE; dir/a.bin (exactly 4096 B) and raw.bin (8192 B, near-random) => FLAT_PLAIN;
# comp/lz4.txt (70000 B, highly compressible) => compressed block (LZ4). erofs-utils does not
# compress files of one block or less, so a compressible single-block file always takes the
# uncompressed layout; 70000 B is also not block-aligned, covering the compressed-tail branch.
put("erofs/hello.txt", b"erofs hello\n")
put("erofs/comp/lz4.txt", b"ABCDEFGH" * 8750)                    # 70000 B, highly compressible => LZ4
put("erofs/dir/a.bin", bytes(i & 0xFF for i in range(4096)))     # exactly one block => FLAT_PLAIN
put("erofs/dir/sub/deep.txt", b"deep\n")                         # inline => FLAT_INLINE
put("erofs/raw.bin", bytes((i * 1103515245 + 12345) >> 16 & 0xFF for i in range(8192)))  # near-random => incompressible

print("ok   fs-stage content generated (sizes: %s)" % ", ".join(
    "%s=%d" % (os.path.relpath(os.path.join(r, n), stage), os.path.getsize(os.path.join(r, n)))
    for r, _, ns in sorted(os.walk(stage)) for n in sorted(ns)))
PY

# ---- FAT volumes ----
# One volume = mkfs.fat + mmd (directory) + mcopy (files); the three differ only in bits/capacity.
# -s (sectors per cluster) is not tuning: FAT12's ceiling is 4084 clusters (2 MiB at 512 B clusters
# yields 4096) and FAT32's floor is 65525 (needs 512 B clusters). QEMU's -device emmc adds a third
# boundary: the backing file must be a power of two; 64 MiB is the smallest such size.
make_fat() {
    local img="$1" bytes="$2" bits="$3" spc="$4" label="$5" hello="$6" abin="$7"
    rm -f "$img"
    truncate -s "$bytes" "$img"
    "$MKFS_FAT" -F "$bits" -s "$spc" -n "$label" "$img" >/dev/null
    "$MMD"  -i "$img" ::/dir
    "$MCOPY" -i "$img" "$STAGE/fat/$hello" "::/hello.txt"
    "$MCOPY" -i "$img" "$STAGE/fat/$abin"  "::/dir/a.bin"
}

make_fat "$OUT_DIR/emmc0.img" "2M"  12 2 "BRKFAT12" hello12.txt a16.bin
make_fat "$OUT_DIR/emmc1.img" "16M" 16 1 "BRKFAT16" hello16.txt a256.bin
make_fat "$OUT_DIR/emmc2.img" "64M" 32 1 "BRKFAT32" hello32.txt a4096.bin
"$MCOPY" -i "$OUT_DIR/emmc2.img" "$STAGE/fat/lfn.txt" "::/long name file.txt"

# Independent verification: fsck.fat reads back each volume's directory and FAT chain.
for v in 0 1 2; do
    "$FSCK_FAT" -n "$OUT_DIR/emmc$v.img" >/dev/null
done
echo "ok   FAT volumes build/emmc0.img (2 MiB FAT12) / emmc1.img (16 MiB FAT16) / emmc2.img (64 MiB FAT32)"

# ---- EROFS image ----
ERO_RAW="$STAGE/emmc3.ero"
# -T 0 fixes timestamps (reproducible); -zlz4 sends compressed blocks down the LZ4 path. mkfs decides
# which files are too small/incompressible, so one image covers both the uncompressed and LZ4 paths.
"$MKFS_EROFS" -T 0 -zlz4 "$ERO_RAW" "$STAGE/erofs" >/dev/null
"$DUMP_EROFS" "$ERO_RAW" >/dev/null       # structural read-back (independent check of image self-consistency)

ERO_SIZE=$(wc -c < "$ERO_RAW" | tr -d ' ')
ERO_LIMIT=$(( 1024 * 1024 ))
if [ "$ERO_SIZE" -gt "$ERO_LIMIT" ]; then
    echo "mk-fs-images: EROFS image is ${ERO_SIZE} B, over the 1 MiB limit — risk of overlapping" >&2
    echo "  the emmc3 scratch area (BR_EMMC_SCRATCH_LBA = 4096 => starts at 2 MiB); refusing to continue" >&2
    exit 1
fi

# Card = 16 MiB, image lands at sector 0, the scratch area (2 MiB..2.5 MiB) stays all zero.
ERO_CARD="$OUT_DIR/emmc3.img"
rm -f "$ERO_CARD"
truncate -s 16M "$ERO_CARD"
dd if="$ERO_RAW" of="$ERO_CARD" bs=4096 conv=notrunc status=none
echo "ok   EROFS card build/emmc3.img (16 MiB; image ${ERO_SIZE} B <= 1 MiB, scratch area from 2 MiB)"

echo "ok   fs images (4 cards: FAT12/16/32 volumes + EROFS image)"
