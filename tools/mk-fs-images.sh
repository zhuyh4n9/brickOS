#!/usr/bin/env bash
# brickOS prototype — FAT/EROFS 后备镜像生成器(ADR-0022)
#
# ============================ 为什么要有它 ============================
# 门禁要在 QEMU 里验证两件事:
#   ① 第三方 FAT 移植件(third_party/uboot-fat)能读写真卷;
#   ② 第三方 EROFS 移植件(third_party/uboot-erofs)能读真镜像(未压缩 + LZ4)。
# 两者的判据都必须建立在**外部权威工具造出来的字节**上 —— 自研 mkfs 与自研 reader 互为
# 镜像, 谁也证明不了谁。所以这里的镜像是 `mkfs.fat`(dosfstools)与 `mkfs.erofs`
# (erofs-utils)造的, 版本与 sha256 钉在 `prebuilts/toolchain.lock.toml` 里。
#
# ============================ 为什么不是提交二进制 ============================
# 镜像内容会随测试演进而变(改一个用例的期望字节就要重造), 提交进去的字节**无法重放**;
# 且 FAT32 的最小合法卷就是 32 MiB —— 版本库里不该有几十 MiB 的稀疏文件。
# 于是取"锁版本的工具 + 每次重建"这条路: 与 make/ninja 同一条纪律(ADR-0002)。
#
# ============================ 四块卡的分工(必须与驱动/FS 件一致) ============================
#   emmc0  2 MiB   FAT12 卷   → 挂 /fat12
#   emmc1  16 MiB  FAT16 卷   → 挂 /fat16
#   emmc2  32 MiB  FAT32 卷   → 挂 /fat32   (FAT32 的簇数下限决定了它最小就是 32 MiB)
#   emmc3  16 MiB  EROFS 镜像(未压缩 + LZ4) + 卡尾 512 KiB 设备级擦写区 → 挂 /assets
#
# ★ 三条**跨件不变量**(改这里必须同步改那一侧; 由本脚本的断言与自检的判据两侧夹住):
#   1. 每个 FAT 卷的卷标与文件内容 = `third_party/uboot-fat/src/fat_internal.h` 里的常量;
#   2. EROFS 镜像 ≤ 1 MiB(assert), 而擦写区从 **LBA 4096(2 MiB)** 起
#      (`BR_EMMC_SCRATCH_LBA`) ⇒ "镜像与擦写区永不重叠"这条性质是**算出来的**, 不是约定;
#   3. 四块卡的名字/顺序 = `platform/qemu-aarch64/plugin.toml` 的 `[build.target.qemu].extra`
#      (QEMU 按 `-drive id=emmcN` + `-device emmc,drive=emmcN` 一一对应)。
#
# 用法: tools/mk-fs-images.sh [<输出目录>]     缺省 = build
set -euo pipefail

OUT_DIR="${1:-build}"
MKFS_FAT_REL="dosfstools/sbin/mkfs.fat"
FSCK_FAT_REL="dosfstools/sbin/fsck.fat"
MKFS_EROFS_REL="erofs-utils/bin/mkfs.erofs"
DUMP_EROFS_REL="erofs-utils/bin/dump.erofs"
MCOPY_REL="mtools/bin/mcopy"
MMD_REL="mtools/bin/mmd"

# ---------------------------------------------------------------- 工具解析
# 候选序与交叉工具链同款: **prebuilts/toolchain/ 优先**(锁版本、可复现) → PATH 同名工具。
# 都缺 ⇒ 明确报错并给出"取件"命令(而不是静默退化成"没有镜像, 门禁必红")。
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
    echo "mk-fs-images: 找不到 $name" >&2
    echo "  取件: python3 tools/fetch-prebuilt.py dosfstools erofs-utils mtools" >&2
    return 1
}

MKFS_FAT=$(resolve "$MKFS_FAT_REL"   mkfs.fat)
FSCK_FAT=$(resolve "$FSCK_FAT_REL"   fsck.fat)
MKFS_EROFS=$(resolve "$MKFS_EROFS_REL" mkfs.erofs)
DUMP_EROFS=$(resolve "$DUMP_EROFS_REL" dump.erofs)
MCOPY=$(resolve "$MCOPY_REL"         mcopy)
MMD=$(resolve "$MMD_REL"             mmd)

# mtools 会因为"镜像大小不是它算出来的几何"而报错退出(对我们造的超级软盘卷是误报)。
export MTOOLS_SKIP_CHECK=1

mkdir -p "$OUT_DIR"
STAGE="$OUT_DIR/fs-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# ---------------------------------------------------------------- 内容生成
# 内容用 python3 生成(仓库本来就要求 python3: tools/fetch-prebuilt.py)。**逐字节确定**,
# 不用 dd/head/tr 那套"依赖宿主行为"的写法 —— 判据要能逐字节比对。
python3 - "$STAGE" <<'PY'
import os
import sys

stage = sys.argv[1]

def put(rel, data):
    p = os.path.join(stage, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)

# FAT 三卷的置入文件(内容必须与 fat_internal.h 的常量一致)
put("fat/hello12.txt", b"fat12 hello\n")
put("fat/hello16.txt", b"fat16 hello\n")
put("fat/hello32.txt", b"fat32 hello\n")
put("fat/a16.bin",  b"A" * 16)
put("fat/a256.bin", b"B" * 256)
put("fat/a4096.bin", b"C" * 4096)
put("fat/lfn.txt",  b"lfn\n")

# EROFS 镜像的源树(**全部未压缩**在这里写; 压不压由 mkfs.erofs 决定)
#
# ★ 尺寸是**刻意选的**, 为的是让一份镜像同时覆盖三条 datalayout:
#   - `hello.txt`(12 B)/ `dir/sub/deep.txt`(5 B) ⇒ **FLAT_INLINE**(塞在元数据块里);
#   - `dir/a.bin`(恰好 4096 B)/ `raw.bin`(8192 B 近似随机)⇒ **FLAT_PLAIN**;
#   - `comp/lz4.txt`(**70000 B**, 高度可压)⇒ **压缩块(LZ4)**。
#   为什么压缩件必须**大于一个块**: erofs-utils 对"不超过一个块"的文件不做压缩 ——
#   一个块是它无论如何都要占的下界, 压了也不省 ⇒ 单块文件一律走未压缩布局。
#   实测(记在 ADR-0022 §4): 2 KiB/4 KiB 的可压文件都是 layout 2/0; 64 KiB 的才是 layout 3。
#   70000 B 还刻意**不是**块对齐 ⇒ 尾部有 tail extent, 覆盖"压缩件尾巴"这条分支。
put("erofs/hello.txt", b"erofs hello\n")
put("erofs/comp/lz4.txt", b"ABCDEFGH" * 8750)                    # 70000 B, 高度可压 ⇒ LZ4
put("erofs/dir/a.bin", bytes(i & 0xFF for i in range(4096)))     # 恰好一块 ⇒ FLAT_PLAIN
put("erofs/dir/sub/deep.txt", b"deep\n")                         # 内联 ⇒ FLAT_INLINE
put("erofs/raw.bin", bytes((i * 1103515245 + 12345) >> 16 & 0xFF for i in range(8192)))  # 近随机 ⇒ 不可压

print("ok   fs-stage 内容生成(sizes: %s)" % ", ".join(
    "%s=%d" % (os.path.relpath(os.path.join(r, n), stage), os.path.getsize(os.path.join(r, n)))
    for r, _, ns in sorted(os.walk(stage)) for n in sorted(ns)))
PY

# ---------------------------------------------------------------- FAT 卷
# 一个卷 = mkfs.fat(空卷) + mmd(目录) + mcopy(文件)。三卷只差"位数/容量/簇大小/内容"。
#
# ★ `-s`(每簇扇区数)是**必须显式给**的, 不是调优:
#   - FAT12 的簇数上限 4084 ⇒ 2 MiB 卷配 512 B 簇会得到 4096 个簇, 直接越过上限;
#   - FAT32 的簇数下限 65525 ⇒ 卷必须配 512 B 簇才够(配 4 KiB 簇连下限都够不到)。
#   容量还要过**第三**条边界(QEMU 的 `-device emmc`): 后备文件的大小**必须是 2 的幂**
#   (否则 QEMU 直接拒载: "Invalid eMMC size: 34 MiB / eMMC size has to be a power of 2")。
#   三条边界合起来 ⇒ 64 MiB 是满足全部条件的最小 2 的幂(34 MiB 能满足 FAT32 下限, 但
#   过不了 QEMU 那一关; 见 ADR-0022 §7 实测发现 2)。
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

# 独立校验(不是"mkfs 说成功就算成功"): fsck.fat 逐卷回读目录与 FAT 链。
for v in 0 1 2; do
    "$FSCK_FAT" -n "$OUT_DIR/emmc$v.img" >/dev/null
done
echo "ok   FAT 卷 build/emmc0.img(2 MiB FAT12)/ emmc1.img(16 MiB FAT16)/ emmc2.img(64 MiB FAT32)"

# ---------------------------------------------------------------- EROFS 镜像
ERO_RAW="$STAGE/emmc3.ero"
# `-T 0`: 固定时间戳(可复现); `-zlz4`: 压缩块走 LZ4 路径; **不加 `--all-root`** 之外的选项,
# 让 mkfs 自己决定哪些文件"压不动/太小" —— 那正是未压缩 datalayout(FLAT_INLINE/FLAT_PLAIN)
# 的天然来源, 于是**一份镜像同时覆盖未压缩与 LZ4 两条读路径**。
"$MKFS_EROFS" -T 0 -zlz4 "$ERO_RAW" "$STAGE/erofs" >/dev/null
"$DUMP_EROFS" "$ERO_RAW" >/dev/null       # 结构回读(镜像自洽性的独立检查)

ERO_SIZE=$(wc -c < "$ERO_RAW" | tr -d ' ')
ERO_LIMIT=$(( 1024 * 1024 ))
if [ "$ERO_SIZE" -gt "$ERO_LIMIT" ]; then
    echo "mk-fs-images: EROFS 镜像 ${ERO_SIZE} B 超过 1 MiB 上限 —— 会与 emmc3 的擦写区" >&2
    echo "  (BR_EMMC_SCRATCH_LBA = 4096 ⇒ 2 MiB 起)发生重叠的风险, 拒绝继续" >&2
    exit 1
fi

# 卡 = 16 MiB, 镜像落在扇区 0 起, 擦写区(2 MiB..2.5 MiB)保持全零。
ERO_CARD="$OUT_DIR/emmc3.img"
rm -f "$ERO_CARD"
truncate -s 16M "$ERO_CARD"
dd if="$ERO_RAW" of="$ERO_CARD" bs=4096 conv=notrunc status=none
echo "ok   EROFS 卡 build/emmc3.img(16 MiB; 镜像 ${ERO_SIZE} B ≤ 1 MiB, 擦写区 2 MiB 起)"

echo "ok   fs images(4 块卡: FAT12/16/32 卷 + EROFS 镜像)"
