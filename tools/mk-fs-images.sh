#!/usr/bin/env bash
# brickOS prototype — FAT/EROFS backing image generator (ADR-0022)
#
# ============================ Why it exists ============================
# The gates verify two things in QEMU:
#   (1) the third-party FAT port (adapter/uboot-fat) can read and write real volumes;
#   (2) the third-party EROFS port (adapter/uboot-erofs) can read real images (uncompressed + LZ4).
# Both must be judged on **bytes produced by an external authoritative tool** — a home-grown mkfs
# and a home-grown reader mirror each other, so neither can prove the other. Hence the images here
# are produced by `mkfs.fat` (dosfstools) and `mkfs.erofs` (erofs-utils), with versions and sha256
# pinned in `prebuilts/toolchain.lock.toml`.
#
# ============================ Why not commit binaries ============================
# Image contents change as tests evolve (changing one expected byte in a case means regenerating),
# and committed bytes **cannot be replayed**; also the smallest legal FAT32 volume is 32 MiB —
# a repo should not hold tens of MiB of sparse files. So we take the "version-locked tools +
# rebuild every time" route: the same discipline as make/ninja (ADR-0002).
#
# ============================ The four cards (must match driver/FS pieces) ============================
#   emmc0  2 MiB   FAT12 volume   -> mounted at /fat12
#   emmc1  16 MiB  FAT16 volume   -> mounted at /fat16
#   emmc2  32 MiB  FAT32 volume   -> mounted at /fat32   (FAT32's cluster-count floor makes 32 MiB its minimum)
#   emmc3  16 MiB  EROFS image (uncompressed + LZ4) + 512 KiB device-level scratch area at card tail -> /assets
#
# * Three **cross-piece invariants** (changing one side here means changing that side too; held on
#   both sides by this script's assertions and the gate's criteria):
#   1. Each FAT volume's label and file contents = the constants in
#      `adapter/uboot-fat/src/fat_internal.h`;
#   2. the EROFS image is <= 1 MiB (assert), and the scratch area starts at **LBA 4096 (2 MiB)**
#      (`BR_EMMC_SCRATCH_LBA`) => "image and scratch area never overlap" is **computed**, not a convention;
#   3. the four cards' names/order = `[build.target.qemu].extra` in `platform/qemu-aarch64/brick.toml`
#      (QEMU pairs `-drive id=emmcN` with `-device emmc,drive=emmcN` one to one).
#
# Usage: tools/mk-fs-images.sh [<output dir>]     default = build
set -euo pipefail

OUT_DIR="${1:-build}"
MKFS_FAT_REL="dosfstools/sbin/mkfs.fat"
FSCK_FAT_REL="dosfstools/sbin/fsck.fat"
MKFS_EROFS_REL="erofs-utils/bin/mkfs.erofs"
DUMP_EROFS_REL="erofs-utils/bin/dump.erofs"
MCOPY_REL="mtools/bin/mcopy"
MMD_REL="mtools/bin/mmd"

# ---------------------------------------------------------------- tool resolution
# Candidate order as for the cross toolchain: **prebuilts/toolchain/ first** (locked version,
# reproducible) -> a same-named tool on PATH. Missing both => fail loudly with the fetch command
# (rather than silently degrading to "no images, gate always red").
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

# mtools exits with an error when "the image size does not match the geometry it computed"
# (a false positive for the super-floppy volumes we create).
export MTOOLS_SKIP_CHECK=1

mkdir -p "$OUT_DIR"
STAGE="$OUT_DIR/fs-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# ---------------------------------------------------------------- content generation
# Content is generated with python3 (the repo already requires python3: tools/fetch-prebuilt.py).
# **Byte-exact**, instead of dd/head/tr style "depends on host behaviour" — criteria must be
# byte-comparable.
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

# Source tree for the EROFS image (**all uncompressed** here; whether to compress is mkfs.erofs's call)
#
# * Sizes are **chosen deliberately** so one image covers all three datalayouts:
#   - `hello.txt` (12 B) / `dir/sub/deep.txt` (5 B) => **FLAT_INLINE** (packed into metadata blocks);
#   - `dir/a.bin` (exactly 4096 B) / `raw.bin` (8192 B, near-random) => **FLAT_PLAIN**;
#   - `comp/lz4.txt` (**70000 B**, highly compressible) => **compressed block (LZ4)**.
#   Why the compressed file must be **larger than one block**: erofs-utils does not compress files
#   of "no more than one block" — one block is the lower bound it occupies anyway, so compressing
#   saves nothing => single-block files always take the uncompressed layout.
#   Measured (recorded in ADR-0022 §4): 2 KiB/4 KiB compressible files are layout 2/0; only 64 KiB
#   is layout 3. 70000 B is also deliberately **not** block-aligned => a tail extent exists, covering
#   the "compressed file tail" branch.
put("erofs/hello.txt", b"erofs hello\n")
put("erofs/comp/lz4.txt", b"ABCDEFGH" * 8750)                    # 70000 B, highly compressible => LZ4
put("erofs/dir/a.bin", bytes(i & 0xFF for i in range(4096)))     # exactly one block => FLAT_PLAIN
put("erofs/dir/sub/deep.txt", b"deep\n")                         # inline => FLAT_INLINE
put("erofs/raw.bin", bytes((i * 1103515245 + 12345) >> 16 & 0xFF for i in range(8192)))  # near-random => incompressible

print("ok   fs-stage content generated (sizes: %s)" % ", ".join(
    "%s=%d" % (os.path.relpath(os.path.join(r, n), stage), os.path.getsize(os.path.join(r, n)))
    for r, _, ns in sorted(os.walk(stage)) for n in sorted(ns)))
PY

# ---------------------------------------------------------------- FAT volumes
# One volume = mkfs.fat (empty volume) + mmd (directory) + mcopy (files). The three differ only in
# bits/capacity/cluster size/contents.
#
# * `-s` (sectors per cluster) **must be given explicitly**, it is not tuning:
#   - FAT12 cluster ceiling is 4084 => a 2 MiB volume with 512 B clusters yields 4096 clusters,
#     straight past the ceiling;
#   - FAT32 cluster floor is 65525 => the volume must use 512 B clusters to reach it (4 KiB clusters
#     fall short even of the floor).
#   Capacity must also clear a **third** boundary (QEMU's `-device emmc`): the backing file size
#   **must be a power of two** (otherwise QEMU refuses it outright: "Invalid eMMC size: 34 MiB /
#   eMMC size has to be a power of 2").
#   The three boundaries together => 64 MiB is the smallest power of two satisfying all of them
#   (34 MiB meets the FAT32 floor but fails the QEMU check; see ADR-0022 §7 finding 2).
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

# Independent verification (not "mkfs said ok so it is ok"): fsck.fat reads back each volume's
# directory and FAT chain.
for v in 0 1 2; do
    "$FSCK_FAT" -n "$OUT_DIR/emmc$v.img" >/dev/null
done
echo "ok   FAT volumes build/emmc0.img (2 MiB FAT12) / emmc1.img (16 MiB FAT16) / emmc2.img (64 MiB FAT32)"

# ---------------------------------------------------------------- EROFS image
ERO_RAW="$STAGE/emmc3.ero"
# `-T 0`: fixed timestamps (reproducible); `-zlz4`: compressed blocks take the LZ4 path; no options
# beyond `--all-root`, so mkfs decides which files are "incompressible/too small" — that is exactly
# where the uncompressed datalayouts (FLAT_INLINE/FLAT_PLAIN) come from naturally, so **one image
# covers both the uncompressed and the LZ4 read paths**.
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
