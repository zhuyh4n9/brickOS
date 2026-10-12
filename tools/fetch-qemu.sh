#!/usr/bin/env bash
#
# fetch-qemu — 把 **QEMU 10.2.1**(Ubuntu 26.04 "resolute" 归档)就地解包成"能在这台宿主上直接跑"
#              的一份, 落到 `prebuilts/toolchain/qemu/`(派生目录, 不进版本库), 并交出一个包装器
#              `prebuilts/toolchain/bin/qemu-system-aarch64` —— `brickie` 的工具解析**先看这个目录**
#              (见 `tools/brickie/rust/src/build.rs::resolve_tool`), 所以 `brickie run` 无需改配置。
#
# ── 为什么需要它 ────────────────────────────────────────────────────────────────
# `brickie run` 与 `emmc-test`/`fat-test`/`erofs-test` 等门禁都要 `-device emmc`。QEMU 的 eMMC 卡模型:
#   * **9.1** 引入 `TYPE_EMMC`, 但 `emmc_class_init()` 里 `dc->user_creatable = false`
#     (连注释都写着 "Reason: Soldered on board") ⇒ `-device emmc` 直接报
#     `'emmc' is not a valid device model name`(这就是宿主 QEMU 8.2.2 的症状);
#   * 直到 **10.2.0** 才删掉那一行 ⇒ `-device emmc` 才成为"可插拔设备"。
# 仓库的参考环境正是 10.2.1(见 `docs/analysis/0001-emmc-bdev-gap.md` 的"本地取证"与
# `prebuilts/toolchain.lock.toml` 引用的 Ubuntu 26.04 归档), 本脚本把那一份**原样**取到本地 ——
# 这样"为了跑门禁去换发行版"就不是必须的。
#
# ── 为什么不写进 prebuilts/toolchain.lock.toml ──────────────────────────────────
# 那份锁管的是**交叉工具链**(进 Makefile/setup.sh 的 PATH, 给目标构建用); 这里是**宿主兼容垫片**
# (只在"宿主自带 QEMU < 10.2"时才需要), 按发行版钉版本、且要额外生成 loader 包装器与 bundle ——
# 与那份锁的语义不同, 混在一起会让"交叉工具链"多出一个宿主发行版维度。
# **宿主升到 ≥10.2(例如换 Ubuntu 26.04)之后, `prebuilts/toolchain/qemu/` 可以整体删掉。**
#
# ── 为什么不用 root ─────────────────────────────────────────────────────────────
# resolute 的 libc 是 **2.43**, 高于 noble 的 2.39 ⇒ 那个 qemu 二进制**不能**直接在本机跑
# (会报 `GLIBC_2.4x not found`)。做法是**显式调用** resolute 自带的 loader 并给 `--library-path`:
#   sysroot 优先(resolute 的 libc6/libfuse3-4/libslirp0 —— soname 或符号版本变了, 见下)
#   → 宿主 /lib、/usr/lib 兜底(glib/pixman/zlib… 版本已够, 用宿主的即可)。
# 于是三件必须来自 resolute: `libc6`(2.43)、`libfuse3-4`(soname 从 .so.3 变 .so.4)、
# `libslirp0`(符号版本 `SLIRP_4.9`)。这三件正是下面 PIN 表里非 qemu 的那三条。
#
# ── 为什么还要 qemu-bundle ──────────────────────────────────────────────────────
# QEMU 启动时会逐个加载**编译期登记**的模块。若不隔离, 它会从宿主的
# `/usr/lib/x86_64-linux-gnu/qemu/`(本机是 8.2.2 的模块)里找同名文件 ⇒ dlopen 到"别的构建"的 .so ⇒
# `util/module.c` 的 `assert(QTAILQ_EMPTY(&dso_init_list))` 直接 abort(实测: rc=134)。
# 这里用 QEMU 自带的 **qemu-bundle** 机制: `get_relocated_path()` 在"可执行文件所在目录旁的
# `qemu-bundle/` 存在"时, 把**模块/数据/固件**三类路径统统改成该目录前缀(`dir` 原样接在其后),
# 于是 11 个包解出来的东西全关进 sysroot, 不再碰宿主那套。
# ⚠ 注意"可执行文件所在目录"的判定: 我们是**显式调用 loader**的, 所以 `/proc/self/exe` 指向
# **loader**, bundle 必须放在 **loader 旁边**(`sysroot/usr/lib/x86_64-linux-gnu/qemu-bundle/`),
# 而不是 qemu 二进制旁边。这一条由自检兜底(见文件末尾的 `-device help` 断言)。
#
# ── 出处与可重放 ───────────────────────────────────────────────────────────────
# PIN 表里的 URL/SHA256 **不是**本地首用信任(TOFU): 它们逐条取自归档的
# `dists/resolute/main/binary-amd64/Packages.gz` 索引里的 `Filename` 与 `SHA256` 字段
# (该索引由 `InRelease` 签名背书)。上游换包 ⇒ 校验不过 ⇒ 取件失败, 绝不会静默换版本。
#
# 依赖: python3(下载, 与 `tools/fetch-prebuilt.py` 同款"只用标准库"的口径) + dpkg-deb(解 .deb)。
#       **不需要** root, 也不需要 curl/wget/tar。
#
# 用法:
#   bash tools/fetch-qemu.sh            # 取件 + 解包 + 生成包装器 + 自检
#   bash tools/fetch-qemu.sh --check    # 只自检(不下载; 已就位时用)
#
# 退出码: 0 成功 / 1 校验或解包失败 / 2 用法或环境错
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

# ── PIN 表: 文件名|sha256 ──────────────────────────────────────────────────────
# 11 件 = ① 目标机模拟器与它的同伴(qemu-system-arm 提供 aarch64-softmmu 的 qemu-system-aarch64;
#   qemu-system-{common,data,gui,modules-opengl,modules-spice} 提供的 .so **模块必须齐** ——
#   少一个名字, QEMU 就会回退到宿主那份 ⇒ 又踩模块断言; 名字齐了, 打不开也只是无害告警);
# ② ipxe-qemu: `-M virt` 会默认挂一块 virtio-net, 它的 option ROM 在这里;
# ③ 三件 resolute 库(libc6 / libfuse3-4 / libslirp0), 理由见文件头。
# 版本: qemu 1:10.2.1+ds-1ubuntu3, 其余见文件名。全部来自 resolute main/binary-amd64。
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

# ── 自检(也是 --check 的全部内容) ─────────────────────────────────────────────
selfcheck() {
    [ -x "$LINK" ] || die "包装器不在场: $LINK —— 先跑一次 'bash tools/fetch-qemu.sh'"
    local ver
    ver=$("$LINK" --version 2>/dev/null | head -1) || die "包装器跑不起来: $LINK"
    case "$ver" in
        *10.2.1*) info "ok   $ver" ;;
        *)        die "版本不对(要 10.2.1): $ver" ;;
    esac
    # 这一条同时证明三件事: 能跑、模块目录被隔离、emmc 真的可插拔。
    if ! "$LINK" -device help 2>/dev/null | grep -q 'name "emmc"'; then
        die "『-device emmc』不可用 —— 不是 10.2 的构建, 或模块目录没被隔离"
    fi
    info "ok   -device emmc 可插拔(eMMC 卡模型 10.2 起)"
}

if [ "${1:-}" = "--check" ]; then
    selfcheck
    exit 0
fi
[ $# -eq 0 ] || die "用法: bash tools/fetch-qemu.sh [--check]" 2

command -v python3  >/dev/null || die "缺 python3(下载用; 只用标准库)" 2
command -v dpkg-deb >/dev/null || die "缺 dpkg-deb(解 .deb 用; dpkg 自带)" 2

mkdir -p "$CACHE" "$SYS" "$DEST/bin" "$BINDIR"

# ── ① 取件 + 校验 ─────────────────────────────────────────────────────────────
for row in "${PIN[@]}"; do
    IFS='|' read -r fname sha dir <<<"$row"
    out="$CACHE/$fname"
    if [ -f "$out" ] && [ "$(sha256sum "$out" | cut -d' ' -f1)" = "$sha" ]; then
        info "cached $fname"
    else
        rm -f "$out"
        info "fetch  $fname"
        python3 - "$BASE/$dir/$fname" "$out" <<'PY' || die "下载失败: $fname"
import sys, urllib.request
urllib.request.urlretrieve(sys.argv[1], sys.argv[2])
PY
    fi
    got=$(sha256sum "$out" | cut -d' ' -f1)
    [ "$got" = "$sha" ] || die "$fname 的 sha256 不符
  期望 $sha
  实得 $got
  (上游换包了? 不许静默跟随 —— 核对归档后显式改本脚本的 PIN 表)"
done

# ── ② 解包(11 件合并进同一个 sysroot; dpkg-deb -x 幂等) ────────────────────────
for row in "${PIN[@]}"; do
    IFS='|' read -r fname _rest <<<"$row"
    dpkg-deb -x "$CACHE/$fname" "$SYS" || die "解包失败: $fname"
done
[ -f "$QFEXE" ]  || die "解包后找不到 $QFEXE"
[ -f "$LOADER" ] || die "解包后找不到 resolute 的 loader: $LOADER"
info "ok   解包 11 件 ⇒ $SYS"

# ── ③ qemu-bundle: 把模块/数据/固件三类路径关进 sysroot(见文件头) ───────────────
# 目录名必须与归档里的绝对路径一致 —— get_relocated_path() 是拿编译进二进制的
# CONFIG_QEMU_MODDIR 等**绝对路径**直接接在 bundle 后面的。
# ★ 软链的目标必须是**绝对路径**: `$SYS` 是相对仓库根的, 而 `ln -s` 把目标串**原样**
#   存下、由解析方按**软链自己所在目录**解释 ⇒ 相对目标一律解析失败
#   (症状: `qemu-system-aarch64: failed to find romfile "efi-virtio.rom"`,
#    因为 bundle 里那三条软链全是断的)。这里先取绝对路径再链。
SYS_ABS="$(cd "$SYS" && pwd)"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/usr/lib/x86_64-linux-gnu" "$BUNDLE/usr/lib/ipxe" "$BUNDLE/usr/share"
ln -sfn "$SYS_ABS/usr/lib/x86_64-linux-gnu/qemu" "$BUNDLE/usr/lib/x86_64-linux-gnu/qemu"
ln -sfn "$SYS_ABS/usr/share/qemu"                "$BUNDLE/usr/share/qemu"
# ipxe 的 ROM 位置**随发行版布局变**: noble 是 `/usr/lib/ipxe/qemu/`, 而 resolute 的
# ipxe-qemu 把 `efi-*.rom` 直接装进 `/usr/share/qemu/`(上面那条软链已经覆盖)。所以这一条
# **按需**建 —— 建一条悬空链只会让下一个读这段的人以为它必需(固件搜索是按名字找, 悬空目录无害)。
if [ -d "$SYS/usr/lib/ipxe/qemu" ]; then
    ln -sfn "$SYS_ABS/usr/lib/ipxe/qemu" "$BUNDLE/usr/lib/ipxe/qemu"
fi
# 自检: 两条**必需**软链都得能解析 —— 否则 QEMU 起不来, 而且要等到跑门禁才发现。
for p in usr/lib/x86_64-linux-gnu/qemu usr/share/qemu; do
    [ -e "$BUNDLE/$p" ] || die "qemu-bundle 软链断了: $BUNDLE/$p"
done
info "ok   qemu-bundle(模块 $(ls "$SYS/usr/lib/x86_64-linux-gnu/qemu" | wc -l) 个 .so / 数据 / ipxe ROM)"

# ── ④ 包装器: 显式调用 resolute 的 loader, sysroot 优先、宿主库兜底 ─────────────
cat >"$WRAP" <<'SH'
#!/bin/sh
# 由 tools/fetch-qemu.sh 生成 —— 勿手改(改它请改那个脚本)。
#
# 为什么要有这一层: 这份 qemu 是 Ubuntu 26.04(resolute)的构建, 动态段里写着要 glibc 2.43,
# 而宿主(如 noble)只有 2.39 ⇒ 不能直接执行。解法 = 显式调用 resolute 自带的 loader,
# 并让 sysroot 里的库优先、宿主 /lib 与 /usr/lib 兜底(glib/pixman/zlib 这些版本够用)。
set -e
self=$0
while [ -L "$self" ]; do                       # 解软链: bin/qemu-system-aarch64 → ../qemu/bin/…
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

# ── ⑤ brickie 的工具解析先看 prebuilts/toolchain/bin/(resolve_tool) ─────────────
ln -sfn "../qemu/bin/qemu-system-aarch64" "$LINK"
info "ok   $LINK → ../qemu/bin/qemu-system-aarch64"

# ── ⑥ 自检 ────────────────────────────────────────────────────────────────────
selfcheck
echo "ok   QEMU 10.2.1 就位: prebuilts/toolchain/qemu(宿主自带的那份不受影响)"
