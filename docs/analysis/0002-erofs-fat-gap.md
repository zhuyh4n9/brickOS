# EROFS / FAT → vfs-core 移植 gap 分析(拍板前)

> 目的: 在动手之前把"上游有什么 / 我们缺什么 / 哪些是真实缺口 / 哪些只是看起来像缺口"
> 逐条摆出来, 供拍板。决策落地见 `docs/decisions/0022-erofs-fat.md`。
> 上游: `nxp-imx/uboot-imx` @ `lf_v2026.04`(`bb41975aeb4cbd153b49c9fd62da1d64cd65968d`)
> —— 与 ADR-0021 的 eMMC 移植**同一个 commit**, 于是"第三方来源"这件事在仓库里仍然只有
> 一处可交代的出处。
> 取证命令见附录 A(全部可复现)。

## 0. 结论(TL;DR)

| 问题 | 结论 |
|---|---|
| 上游有吗 | **两件都有**。`fs/fat/{fat.c,fat_write.c}`(34 KB + 49 KB, 读写齐)与 `fs/erofs/**`(9 个文件 ≈ 75 KB, 含压缩映射与 LZ4 解压)。u-boot 的 EROFS 是 2022 年合入的**只读**实现, 面向"简单块读接口", 比 Linux 的 `fs/erofs` 好移一个量级 |
| 最大缺口 | **不是 FS 本身**, 而是三件"文件系统之外"的事: ① 块设备的**数量**(一台 `sdhci-pci` 只挂一张卡, 而四件介质要四块卡); ② 挂载**顺序**(挂载必须晚于块设备的 start 相, 而工具链禁 `native → third_party`); ③ 镜像**从哪来**(判据必须由外部权威工具造, 而宿主三件工具都没有) |
| 许可是不是阻断 | **不阻断**(沿用 ADR-0021 D-3 的三方隔离), 但覆盖面从 1 件扩到 3 件, 登记不变 |
| 设计侧排期 | `7-03` §1: EROFS **v2.0**、FAT **未排期** ⇒ 本刀是**提前落地**, 偏离账写进 ADR-0022 §3 |
| 总工作量 | 约 3000 行新增(两件移植 + 胶水 + 自检), 其中约 2/3 是**上游代码的搬运与改写**, 1/3 是本项目形状的胶水 |

## 1. 两侧架构差异(u-boot vs brickOS)

| 维度 | u-boot | brickOS prototype | 移植影响 |
|---|---|---|---|
| FS 的入口 | **路径字符串**(`fs_read(name, addr, …)`), 由命令层调用 | **四层 ops**(super/inode/file/dentry), 路径走查在 vfs-core | 上游的路径解析不搬; 胶水把 `(dir inode, name)` 翻成路径(ADR-0022 D-3) |
| 介质抽象 | `struct blk_desc` + `disk_partition`(`blk_dread/blk_dwrite`) | `br_bdev_ops_t`(扇区 read/write/flush/geometry) | 换一层 shim(整设备 = 一个分区) |
| 动态内存 | `malloc/calloc/realloc/free` | `br_malloc/br_calloc/br_realloc/br_free`(core 堆) | 逐名替换 |
| 日志 | `printf`/`debug`/`pr_err` | `br_log_debug/info/warn/error`(等级化) | 30 余处 `printf` 换成等级日志 |
| libc | 自带精简实现 | **没有 libc**(只有编译器支持例程 memcpy/memset/memcmp/memmove) | `str*` 自己写小工具(**不**依赖 `runtime/posix`); `sprintf` 去掉 |
| 错误码 | `-EIO/-ENOMEM/…`(Linux 数值) | `BR_ERR(BR_E*)`(与 Linux 逐值对齐, `br_error.h`) | 基本同名同值; `-EFSCORRUPTED` 映射到 `-EIO` |
| 只读面 | 由命令层决定 | `br_fs_ops`/`br_inode_ops` 空槽位 ⇒ `-ENOTSUP` | 只读 FS 要**填槽位**并返回 `-EROFS`(ADR-0022 D-5) |

## 2. 逐文件移植评估

### 2.1 FAT

| 上游 | 大小 | 本插件 | 处理 |
|---|---|---|---|
| `fs/fat/fat.c` | 33900 B | `src/fat_core.c` | 保留引导扇区/BPB 解析、FAT 表项读取、簇链、目录迭代(`fat_itr`)、读文件、`fat_size`/`fat_exists`/`fat_opendir`/`fat_readdir`; 裁掉命令层、`part.h` 分区表遍历 |
| `fs/fat/fat_write.c` | 49038 B | `src/fat_write.c` | 保留 `set_fatent_value`/`set_cluster`/`set_contents`/`flush_dir`/`new_dir_table`/`find_empty_cluster`/`fat_unlink`/`fat_mkdir`/`fat_rename`; **自补 `truncate`**(上游没有) |
| `fs/fat/Kconfig`/`Makefile` | — | — | 不搬(构建由 `plugin.toml` 声明) |

### 2.2 EROFS

| 上游 | 大小 | 本插件 | 处理 |
|---|---|---|---|
| `fs/erofs/super.c` | 3004 B | `src/erofs_super.c` | 超级块读取与 feature 相容性检查; 裁掉多设备表(extra devices) |
| `fs/erofs/namei.c` | 6559 B | `src/erofs_namei.c` | inode 读盘(compact/extended)、目录项查找、路径走查 |
| `fs/erofs/data.c` | 8353 B | `src/erofs_data.c` | `erofs_map_blocks`(flat/chunk)、`erofs_pread`、压缩读的驱动循环 |
| `fs/erofs/zmap.c` | 18765 B | `src/erofs_zmap.c` | **压缩映射迭代**(本刀最重的一块): 逻辑→物理 extent、pcluster 头解析、inline/partial 处理 |
| `fs/erofs/decompress.c` | 4189 B | `src/erofs_decompress.c` | 保留 SHIFTED/INTERLACED/LZ4 三分支; **删掉** deflate/zstd(⇒ `-ENOTSUP`) |
| `fs/erofs/erofs_fs.h` | 14803 B | `src/erofs_fs.h` | **原样保留**(on-disk 结构定义); SPDX 是 `GPL-2.0-only OR Apache-2.0` |
| `fs/erofs/internal.h` | 9684 B | `src/erofs_internal.h` | 裁剪: 去掉 `list_head`/`linux/compat.h`/erofsfuse/mkfs 专用字段 |
| `fs/erofs/fs.c` | 5337 B | — | **不搬**: 它是 u-boot 的 `fs_dir_stream` 粘合层, 由我们的 vfs 胶水取代 |
| `lib/lz4.c` | 14846 B | `src/lz4.c`(或并入 decompress) | 保留 `LZ4_decompress_safe` / `_partial`; SPDX 是 `GPL 2.0+ OR BSD-2-Clause` |

## 3. 依赖 gap 表

| 本插件需要 | 谁提供 | 缺吗 |
|---|---|---|
| `br_fs_ops`/`br_inode_ops`/`br_file_ops` 的契约 | `framework/vfs-core`(native, 已有) | 否 |
| 挂载注册 `br_mount_register` | `framework/vfs-core` | 否 |
| 块设备类 API `br_bdev_get` | `framework/bdev-core`(native, ADR-0021) | 否 |
| 块设备**实例**(emmc0..3) | `third_party/uboot-mmc`(ADR-0021) | **是**: 上一刀只注册 `emmc0`(单例), 本刀要 4 块 ⇒ 主机层必须多实例化 |
| 内存 | `br_malloc/calloc/realloc/free`(core) | 否 |
| `memcpy/memset/memcmp/memmove` | core 的编译器支持例程 | 否 |
| `strlen/strcmp/strncmp/strcpy/strchr/strrchr/tolower` | **core 没有**(`str*` 归 `runtime/posix`) | **是**: 但不该让介质 FS 依赖 POSIX 运行时 ⇒ 各件自带 5 行小工具 |
| `sprintf/printf` | core 的 `br_log_*` 够用 | 否(改写) |
| 外部权威的 mkfs | 宿主 | **是**(见 §4.3) |

## 4. 三处真实缺口(逐条)

### 4.1 缺口一: 块设备的数量 —— SDHCI 主机层的单例

`third_party/uboot-mmc` 的 `sdhci_host.c` 里是 `static sdhci_t s_sdhci;`(全局单例:
寄存器窗口、时钟、总线宽度、通路、信号量、pending 位)。而本刀要四块卡。

* **为什么不能"一卡多卷"**: FS 的挂载要经 `br_bdev_get(name)` 拿一块扇区设备; 一张卡
  上放两个 FS 需要分区/切片(设计 SD-9 的 `br_bdev_partition` 是 v1.x)。本刀不引入它。
* **改造面**: 主机层改成**静态实例表**(`struct br_sdhci`), ISR 的上下文参数 = 实例指针
  (`br_irq_register(virq, isr, h, &attr)` 本来就收 `void *arg`)。协议核心
  (`mmc_core.c`)**一行不用改** —— `br_mmc_host_ops_t` 本来就带 `void *host`。
  这其实是**回到上游形状**(u-boot 里每块卡是一个 `struct sdhci_host *`)。
* **上界**: 见 §4.2。

### 4.2 缺口二: 挂载顺序 × 工具链的 `native → third_party` 禁则

挂载必须发生在**块设备注册之后**。而块设备的注册点在 `uboot-mmc` 的 **start 相**, 理由
是硬的: 驱动用**中断**等命令完成, 而全局开中断发生在 LATE 相之后、start 相之前
(ADR-0021 §2 已论证)。

⇒ FS 件需要一条 `[[dep]] third_party/uboot-mmc kind = "init"` 边来钉住顺序。
⇒ 但 `rules.rs::api_type_edge_reason` 禁止 `native` 插件依赖 `third_party` 插件。
⇒ **推论**: 这两件 FS 只能是 `api_type = third_party`。

这个推论**与代码来源一致**(两件都是 GPL-2.0+ 的 u-boot 代码, 本来就该按 ADR-0021 D-3
登记为三方件)—— 两条独立的理由指到同一个结论, 所以它不是"被工具链逼出来的将就"。

**副产物**: 卡数上界。PCI 的 INTx swizzle 是 `(pin-1+slot) % 4`, 而平台只接了 4 根线
(SPI 3..6), 且 core 的 IRQ 框架规定"每根物理线恰一属主"(INV-A)⇒ slot 3/4/5/6 恰好把
A/B/C/D 用满, 第 5 块卡必然 `-EBUSY`。于是 `BR_EMMC_MAX_DEVS = 4`。

### 4.3 缺口三: 镜像从哪来(判据的外部性)

判据必须是"**我们读出来的字节 == 外部权威工具放进去的字节**"。若 mkfs 也自研, 就成了
"自研件互相印证", 证明不了与真实 FAT/EROFS 的兼容性。宿主三件工具**都没有**:

| 需要 | 提供者 | 宿主有吗 | 处置 |
|---|---|---|---|
| 造 FAT12/16/32 卷 | `mkfs.fat`(dosfstools) | 否 | 锁进 `prebuilts/toolchain.lock.toml` |
| 校验造出的卷 | `fsck.fat`(dosfstools) | 否 | 同上(独立校验, 不是"mkfs 说成功就算成功") |
| 往卷里放文件/目录 | `mcopy`/`mmd`(mtools) | 否 | 同上(不需要宿主机的 mount/loop 特权) |
| 造 EROFS 镜像 | `mkfs.erofs`(erofs-utils) | 否 | 同上 |
| 回读 EROFS 结构 | `dump.erofs`(erofs-utils) | 否 | 同上 |

**为什么不是"把镜像提交进仓库"**: 镜像内容会随用例演进(改一个期望字节就要重造),
提交进去的字节无法重放; 且 FAT32 的最小合法卷是 34 MiB, 而 QEMU 要求后备文件是 2 的幂 ⇒ 64 MiB(见 §5.3)——
版本库里不该有几十 MiB 的稀疏文件。

**候选序**(与交叉工具链同款, 见 `br-wa-toolchain-001` 的形状):
`prebuilts/toolchain/<into>/...` → PATH 上的同名工具。于是"没有网络"与"用了锁版本"
两条路都成立, 且都是**同一份语义**。

## 5. 工具与格式的实测取证

### 5.1 erofs-utils 的压缩门槛(直接决定夹具尺寸)

`mkfs.erofs -zlz4` 对**不超过一个块**的文件**不压缩** —— 一个块是它无论如何都要占的
下界, 压了也不省。实测(`dump.erofs --ls --path=…` 读 `Layout` 字段):

| 源文件 | 大小 | 实测 layout | 含义 |
|---|---|---|---|
| `med.txt` | 1024 B("ABCDEFGH"×128) | 2 | FLAT_INLINE(未压缩) |
| `mid.txt` | 4096 B("ABCDEFGH"×512) | 0 | FLAT_PLAIN(未压缩) |
| `big.txt` | 65536 B("ABCDEFGH"×8192) | 3 | **压缩**(比值 6.25%) |

⇒ 压缩件的夹具取 **70000 B**(且刻意不是 4096 的整数倍, 覆盖尾部 tail extent)。
一份镜像因此能同时覆盖三条 datalayout(未压缩内联 / 未压缩平铺 / 压缩)。

### 5.2 erofs-utils 的版本与宿主库

resolute 的 `erofs-utils 1.9.4-1` 链接 **`libcrypto.so.4`**(OpenSSL 4), 而常见宿主机只有
`libcrypto.so.3` ⇒ 直接 `error while loading shared libraries`。一个"造镜像的工具"不该再
牵出一串宿主库依赖, 故锁 **1.8.10-1**(只依赖宿主本就有的 liblz4/libuuid/libzstd, 且
"缺省不压缩"与 `-zlz4` 都在)。同类事实: `.deb` 的 `./usr/...` 会被 `strip = 1` 剥掉一层,
所以锁表里的落点是 `<into>/sbin/...`、`<into>/bin/...`(**没有** `usr/`)。

### 5.3 FAT 的容量边界(由 mkfs.fat 的告警反推)

* **FAT12**: 簇数上限 4084 ⇒ 2 MiB 卷必须用 **1024 B 簇**(512 B 簇会给 4096 个簇)。
* **FAT32**: 簇数下限 65525 ⇒ 512 B 簇下 **32 MiB 只给 64496 个簇**(实测告警),
  **34 MiB** 才过线。**再加一条 QEMU 约束**: `-device emmc` 要求后备文件大小是 2 的幂
  (34 MiB 被拒载)⇒ 最终取 **64 MiB**。
* **FAT16**: 16 MiB + 512 B 簇 = 32768 个簇, 落在 [4085, 65524] 内, 无需特殊处理。

⇒ 三个卷的容量与簇大小都是被这些边界夹出来的, 不是随手取的。

### 5.4 四块卡在 QEMU 上真的能起

```
qemu-system-aarch64 -machine virt,gic-version=3,highmem=off -cpu cortex-a53 -m 128M -nographic \
  -drive if=none,id=emmc0,format=raw,file=e0.img -device sdhci-pci,addr=3 -device emmc,drive=emmc0 \
  -drive if=none,id=emmc1,format=raw,file=e1.img -device sdhci-pci,addr=4 -device emmc,drive=emmc1 \
  …
```
实测: 正常启动(无 "bus already has a card" 之类的拒载)。`pci-core` 的枚举面
(`BR_PCI_MAX_DEVS = 8`)与 dev-core 的注册表都装得下。

## 6. 许可 gap(不阻断, 但覆盖面扩大)

| 件 | 许可 | 处置 |
|---|---|---|
| `fs/fat/**` | GPL-2.0+ | 进 `third_party/uboot-fat`(`api_type = third_party`, 自带 LICENSE/NOTICE) |
| `fs/erofs/**` | GPL-2.0+(`erofs_fs.h` 是 `GPL-2.0-only OR Apache-2.0`) | 进 `third_party/uboot-erofs`, NOTICE 逐文件标真实 SPDX |
| `lib/lz4.c` | `GPL 2.0+ OR BSD-2-Clause` | 同上, NOTICE 标注 |

残余风险与 ADR-0021 §5 同源(`br-wa-tp-001`): 静态链进同一镜像后整镜像在分发时构成
GPL 组合作品 —— **目录隔离不等于许可传染**。本刀把它从 1 件扩到 3 件, 没有消除它。

## 7. 建议的落地架构

```
build/emmc0.img(2 MiB FAT12)    build/emmc1.img(16 MiB FAT16)
build/emmc2.img(64 MiB FAT32)   build/emmc3.img(16 MiB; EROFS ≤1 MiB + 卡尾擦写区)
        ▲                                ▲
        │ 由 tools/mk-fs-images.sh 用外部权威工具造(mkfs.fat/mcopy/mkfs.erofs)
        │
QEMU virt ─4×─ sdhci-pci ─4×─ emmc
        │
   third_party/uboot-mmc        静态实例表; 每卡一个 bdev(emmc0..3)
        │
   third_party/uboot-fat        FAT12/16/32 读写 ⇒ 挂 /fat12 /fat16 /fat32
   third_party/uboot-erofs      只读 + LZ4      ⇒ 挂 /assets
        │  (两件都是 api_type=third_party: 既为来源, 也为那条 init 边)
        ▼
   framework/vfs-core(挂载表 + 走查)── framework/bdev-core(类 API)
        ▼
   runtime/posix ── iface/posix ── app/hello(APP-FS-001..007 端到端样例)
```

## 8. 分期计划与验收判据

| 阶段 | 内容 | 判据 |
|---|---|---|
| P1 | `uboot-mmc` 多实例(4 卡) | 四块 `emmcN` 都就绪; 切一块卡的通路不影响别的(TC-EMMC-006); `emmc-test` 绿 |
| P2 | 宿主工具链(锁表/取件/造镜像) | `ok   fs images`; `fsck.fat` 回读通过; EROFS 镜像 ≤ 1 MiB 断言 |
| P3 | `third_party/uboot-fat` | `[FATCONF] 8/0`; `fat-test` 绿 |
| P4 | `third_party/uboot-erofs` | `[EROFCONF] 8/0`; `erofs-test` 绿 |
| P5 | APP 侧 POSIX 端到端 | `[APPCONF]` 14/0(含 `APP-FS-001..007`) |
| P6 | 声明面/门禁/文档 | `brickie check` 0 错 0 警; `brickie test` 全绿 |

## 9. 拍板点(当时的三个岔口)

1. **块设备与隔离**: 多卡真 eMMC + 两件 third_party 移植件 / 单卡 FAT + 内嵌 RAM 盘 EROFS /
   两件都 clean-room native + 新增 native 块设备。→ **选第一种**。
2. **EROFS 压缩面**: 只做未压缩 / 未压缩 + LZ4。→ **选第二种**(LZ4)。
3. **FAT 功能面**: 只读 / 读写齐。→ **选读写齐**。

## 附录 A: 本文事实的取证命令(可复现)

```bash
# ① 上游有 fs/fat 与 fs/erofs(以及各文件大小)
curl -s "https://api.github.com/repos/nxp-imx/uboot-imx/contents/fs?ref=lf_v2026.04"     | grep '"name"'
curl -s "https://api.github.com/repos/nxp-imx/uboot-imx/contents/fs/erofs?ref=lf_v2026.04" | grep -E '"name"|"size"'

# ② QEMU 接不接受四块 sdhci-pci + 四块 emmc
qemu-system-aarch64 -machine virt,gic-version=3,highmem=off -cpu cortex-a53 -m 128M -nographic \
  -drive if=none,id=emmc0,format=raw,file=/tmp/e0.img -device sdhci-pci,addr=3 -device emmc,drive=emmc0 \
  -drive if=none,id=emmc1,format=raw,file=/tmp/e1.img -device sdhci-pci,addr=4 -device emmc,drive=emmc1 \
  -drive if=none,id=emmc2,format=raw,file=/tmp/e2.img -device sdhci-pci,addr=5 -device emmc,drive=emmc2 \
  -drive if=none,id=emmc3,format=raw,file=/tmp/e3.img -device sdhci-pci,addr=6 -device emmc,drive=emmc3
#   → 正常启动(超时被 kill 即为"没有拒载")

# ③ 造镜像工具能不能跑(用锁表里钉的那两个版本)
python3 tools/fetch-prebuilt.py dosfstools erofs-utils mtools
prebuilts/toolchain/dosfstools/sbin/mkfs.fat --help
prebuilts/toolchain/erofs-utils/bin/mkfs.erofs build/emmc3.img build/fs-stage/erofs

# ④ erofs 的每条 datalayout(§5.1 的表)
prebuilts/toolchain/erofs-utils/bin/dump.erofs --ls --path=/comp/lz4.txt build/emmc3.img

# ⑤ FAT 的容量边界(§5.3 的表)
truncate -s 32M t.img && prebuilts/toolchain/dosfstools/sbin/mkfs.fat -F 32 -s 1 -n T t.img
truncate -s 34M t.img && prebuilts/toolchain/dosfstools/sbin/mkfs.fat -F 32 -s 1 -n T t.img
# ⑥ QEMU 对 emmc 后备文件的大小约束(非 2 的幂直接拒载)
qemu-system-aarch64 -machine virt,gic-version=3,highmem=off -cpu cortex-a53 -m 128M -nographic \
  -drive if=none,id=e,format=raw,file=$PWD/t.img -device sdhci-pci,addr=3 -device emmc,drive=e
```

## 附录 B: 上游关键代码位置(供实现期对照)

```
fs/fat/fat.c
  determine_legacy_fat_bits()   501   FAT 类型判定(n==0x0ffffff8 ? 32 : …)
  get_fs_info()                 624   BPB → fsdata(FAT 表起点/簇大小/根目录)
  fat_itr_root/child/next()     806+  目录迭代器(本移植保留)
  fat_exists/fat_size          1232/1274
  fat_read_file                1311  (offset + len 的读, 对应 br_file_ops.read)
fs/fat/fat_write.c
  set_fatent_value()            441   FAT 表项写(12/16/32 三种宽度)
  set_contents()                929   写文件内容(含簇分配)
  flush_dir()                   655   目录项落盘
  new_dir_table()               825   建目录(含 "." "..")
  fat_unlink/mkdir/rename    1723/1788/1986
fs/erofs/
  super.c   erofs_read_superblock()   —— 超级块在 **偏移 1024** 处找 magic 0xE0F5E1E2
  namei.c   erofs_read_inode_from_disk() / erofs_namei()
  data.c    erofs_map_blocks_flatmode() / erofs_read_raw_data() / erofs_pread()
  zmap.c    z_erofs_map_blocks_iter() —— 压缩映射(本刀最重的一块)
  decompress.c  z_erofs_decompress() —— SHIFTED / INTERLACED / LZ4
lib/lz4.c   LZ4_decompress_safe() / LZ4_decompress_safe_partial()
```
