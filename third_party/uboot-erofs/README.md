# third_party/uboot-erofs — u-boot EROFS 移植(ability / fs / third_party)

> ⚠ **本插件是 GPL-2.0-or-later**, 来源与逐文件对照见 [NOTICE](NOTICE), 全文见
> [LICENSE.GPL-2.0](LICENSE.GPL-2.0)。它和 [uboot-mmc](../uboot-mmc/README.md) 一起
> 构成本仓库内的 GPL 代码(ADR-0021 D-3)。

## 1. 它是什么

把 u-boot 的 **EROFS 只读压缩文件系统**移植进 brickOS, 出口是 `framework/vfs-core`
的 `br_fs_ops` / `br_inode_ops` / `br_file_ops` 契约:

```
emmc3(16 MiB 的 eMMC 卡, 镜像从扇区 0 开始; 由 third_party/uboot-mmc 注册)
        │  br_bdev_get("emmc3") + geometry() → {512 B 扇区, n_sectors, max_rw=64}
        ▼
  src/erofs_super.c       偏移 1024 处找 magic 0xE0F5E1E2 → blkszbits/meta_blkaddr/root_nid
        ▼
  adapter/erofs_vfs.c §①  块层 shim: 字节偏移 → 扇区; 单次 ≤ max_rw(逐扇区, 整扇区中转)
        ▼
  src/erofs_namei.c       inode 解析(32 B COMPACT / 64 B EXTENDED)+ 目录块内名字查找
  src/erofs_data.c        平面/stub 映射 + chunk 映射 + 读路径(erofs_pread)
  src/erofs_zmap.c        压缩映射迭代(4B/2B 紧凑索引、big pcluster、lookbehind)
  src/erofs_decompress.c  INTERLACED / SHIFTED / **LZ4**(自带解码器)
        ▼
  adapter/erofs_vfs.c §②  br_fs_ops.mount → 根 inode; lookup → 瞬态路径包装; read → erofs_pread
        ▼
  framework/vfs-core  /assets/hello.txt 等路径对 APP 可见
```

**分层依据**是上游的文件边界: `super/namei/data/zmap/decompress` 各一件(逐字对应
上游同名文件), `fs.c` 对应 `adapter/erofs_vfs.c`(它在本项目里有两位读者 —— 块设备与 VFS,
于是分两节: 块层 shim 与四层 ops)。`adapter/erofs_compat.h` / `src/erofs_internal.h` /
`src/erofs_fs.h` 是上游 `internal.h` + `erofs_fs.h` 的对应物(兼容层 / 内部契约 / on-disk 真值)。

## 2. 卷与接口(私有契约, 不导出)

| 项 | 值 |
|---|---|
| 块设备 | `emmc3`(16 MiB 镜像从**扇区 0** 开始; 镜像由宿主 `tools/mk-fs-images.sh` 造) |
| 挂载点 | `/assets`(只读; plugin.toml 的 `[[mount]]` 与 `erofs_internal.h` 两处真值) |
| 块大小 | 4096 B(必须 `blkszbits == 12`; 更小的块在挂载期 -ENOTSUP) |
| 支持的数据布局 | 0 `FLAT_PLAIN` / 2 `FLAT_INLINE`(未压缩) · 3 `COMPRESSED_COMPACT` / 1 `COMPRESSED_FULL`(LZ4) |
| 支持的算法 | LZ4(含 0padding、SHIFTED、INTERLACED 的未压缩面); LZMA/DEFLATE ⇒ `-ENOTSUP` |
| inode 模型 | **瞬态路径包装**(设计 SD-3): 每次 lookup `br_malloc` 一个 `{相对路径, struct erofs_inode}` |
| 只读 | 名字空间变更 8 个槽位全填, 一律 `-EROFS`; 写权限打开也 `-EROFS` |

## 3. 与上游的偏离(摘要; 逐条见 [NOTICE](NOTICE))

| # | 偏离 | 为什么 |
|---|---|---|
| 1 | 兼容层自持(`adapter/erofs_compat.h`), 无 libc / 无 u-boot | 裸机 unikernel |
| 2 | `IS_ENABLED(CONFIG_*)` 拆掉, 改运行期算法分派 | 任务书硬约束; 也让"镜像用了哪种算法"变成运行期事实 |
| 3 | 4 KiB 缓冲与 `struct erofs_map_blocks` 提到**文件域静态** | 内核对 APP 线程的栈是 4 KiB 量级; 上游那几处一放就把栈用光 |
| 4 | 索引块缓存的判据从 `map->index` 改挂到 `z_erofs_maprecorder.index` + 一个"scratch 有效"标志 | 上游的缓存靠"每次构造新 map"成立; 缓冲变静态后必须显式失效 |
| 5 | 块读"整扇区读进中转缓冲再按长度拷出" | bdev 的 read 是整扇区语义; 直接给调用方缓冲会越界写 |
| 6 | `DBG_BUGON` = 打印 + 由 -EIO 收敛(不是停机) | 坏镜像不该把设备打死 |
| 7 | 多设备(extra devices)⇒ 挂载期 `-ENOTSUP` | 本件一个卷 |
| 8 | **errno 名字不带映射表**: 裸名字来自 core 的 `<br/core/br_errno.h>` | ADR-0023: 名字是 core 的(有没有 POSIX 运行时都有), 且上游的 `-EFSCORRUPTED`(`EUCLEAN`=117) **一处都没改** —— 本件不再需要 `#define` |
| 9 | 非目录 inode 也有一张 `iops`(只填 `getattr`) | 契约: "每个支持 `br_stat` 的 inode 都必须带一张 iops"; 只给目录会让 `br_stat(文件)` 得 -ENOTSUP |
| 10 | 目录项填名**按长度**复制(不是"复制到 NUL") | EROFS 目录块里的名字没有 NUL 结尾(紧挨着拼); 扫 NUL 会把后面所有名字一起吞进来 |

## 4. 名字上界:本仓库 `BR_NAME_MAX` = 32, EROFS 是 255

这是一个**取舍**, 两处口径不同且都是刻意的:

- **目录面(`readdir`)**: 名字 ≥ 32 字节的条目**跳过并计数** —— 到穷尽那一刻打一行
  `br_log_warn` 报总数。为什么不是 `-ENOSPC`: 目录迭代是"流", 一条超长名字不该让整个
  枚举失败(调用方连已经能看见的条目都拿不到了)。
- **`lookup`**: `br_dirent_t`/分量缓冲都装不下 ≥32 字节的名字 ⇒ 本件让 `lookup` 返回
  `BR_NULL`(vfs 得 `-ENOENT`)。任务书写的"lookup 上返回 `BR_ERR(BR_ENOSPC)`"在本件的
  落点是 **vfs 自己**: `resolve_norm()` 对 `clen >= BR_NAME_MAX` 的分量返回 `-ENOSPC`,
  于是同一个超长名字从路径面看仍是 `-ENOSPC`。`lookup` 的签名没有错误码通道
  (返回 inode 指针), 所以 FS 侧只能 `BR_NULL`。
- 同理, `path[UBOOT_EROFS_REL_PATH_MAX]`(256 B)放不下的相对路径也返回 `BR_NULL`。

## 5. 一致性用例(`TC-EROFS-*`, `adapter/erofs_selftest.c`)

8 条, 判据都经**公开面**(`br_stat`/`br_open`/`br_file_read`/`br_file_lseek`/`br_opendir`/
`br_readdir`/`br_unlink`/`br_mkdir`)或**盘上事实**(直接读偏移 1024 的 magic)。
夹具内容与宿主 `tools/mk-fs-images.sh` **逐字节对齐**(常量在 `erofs_internal.h`):

| id | 判据 |
|---|---|
| TC-EROFS-001 | 挂载表含 `/assets`; 盘上偏移 1024 的 magic = `0xE0F5E1E2`; `blkszbits == 12`; `root_nid` 解析出来**是目录** |
| TC-EROFS-002 | `hello.txt`(12 B)逐字节 = `"erofs hello\n"`; 布局是 0/2(未压缩) |
| TC-EROFS-003 | `dir/a.bin` 中段读(offset 1000, len 500)逐字节 = `i & 0xFF`(非零 offset 的 extent 切分) |
| TC-EROFS-004 | `comp/lz4.txt`(70000 B)逐字节 = `"ABCDEFGH"[i % 8]`(压缩块 + LZ4 解压 + 尾 extent) |
| TC-EROFS-005 | `readdir /assets` 与 `/assets/dir/sub`: `.`/`..` 在; `comp`/`dir` 是目录; `hello.txt`/`raw.bin`/`deep.txt` 是文件;**按名字断言, 不数总数** |
| TC-EROFS-006 | 不存在的名字: `br_open` 得 NULL、`br_stat` 得 `-ENOENT`、内部 `erofs_ilookup` 得 `-ENOENT`; `br_stat` 对目录/文件的类型与大小正确 |
| TC-EROFS-007 | 只读执法: `BR_O_WRONLY`/`BR_O_RDWR` 打开 ⇒ NULL; `unlink/mkdir/rmdir/rename/truncate/symlink/link` ⇒ `-EROFS`; 目录**只读打开仍然成功** |
| TC-EROFS-008 | `raw.bin`(8192, 压不动)与 `a.bin`(4096, 平面)整文件逐字节一致; `SEEK_END` 与越过 EOF 后 `read == 0` |

判据是**生成式**的: `"ABCDEFGH"[i % 8]`、`(i * 1103515245 + 12345) >> 16 & 0xFF`、
`i & 0xFF` —— 源码里**没有任何大块字面量**。

> 宿主的两个压缩夹具刻意选成两条不同的 LZ4 路径: `comp/lz4.txt` 是"压得动"
> (压缩比 5.85%, 且 70000 不是 4096 的整数倍 ⇒ 覆盖尾 extent 的 partial decoding);
> `raw.bin` 是"压不动"(压缩比 50%, 几乎全是字面量)。

## 6. 依赖与构建

| 边 | kind | 理由 |
|---|---|---|
| `framework/vfs-core` | init | 实现并注册 `br_fs_ops`/`br_inode_ops`/`br_file_ops` |
| `framework/bdev-core` | init | `br_bdev_get("emmc3")` 取块设备 |
| `third_party/uboot-mmc` | init | 它注册 `emmc3`; 这条**init 边**保证拓扑序在本插件之前 |

`src/*.c` 五件(上游派生): `erofs_super.c` / `erofs_namei.c` / `erofs_data.c` /
`erofs_zmap.c` / `erofs_decompress.c`; `adapter/*.c` 两件(本项目): `erofs_vfs.c`
(块层 shim + VFS 对接) / `erofs_selftest.c`, 另有 `adapter/erofs_compat.h`(唯一的运行环境入口)。

## 6.1 `src/` 与 `adapter/` 的拆分(单向依赖)

GPL 边界靠**依赖方向**而非目录名维持:

- `src/`(**上游派生**, GPL-2.0+): `erofs_super.c` / `erofs_namei.c` / `erofs_data.c` /
  `erofs_zmap.c` / `erofs_decompress.c` / `erofs_fs.h` / `erofs_internal.h` —— 逐字对应
  u-boot `fs/erofs/**` + `lib/lz4.c`, 不含任何 `#include <br/...>`;
- `adapter/`(**本项目**): `erofs_compat.h`(运行环境的**唯一**入口)、`erofs_vfs.c`
  (块层 shim + 四层 ops)、`erofs_selftest.c`(TC-EROFS-*)。

依赖**单向** `src/` → `adapter/`: `src/erofs_internal.h` include `adapter/erofs_compat.h`
取运行环境; `adapter/*.c` 反过来 include `src/erofs_internal.h` 是允许的(适配层当然要认识
被适配的代码)。[plugin.toml](plugin.toml) 据此收 `sources = ["src/*.c", "adapter/*.c"]` /
`includes = ["src", "adapter"]`, 门禁 `tools/check-third-party-isolation.sh`
(见 `tests/gates.toml`)机械地守住这条边界。完整理由与逐文件归属见
[`adapter/README.md`](adapter/README.md)。

## 7. 门禁契约(改日志措辞前先看这里)

`tests/gates.toml` 的 `erofs-test` 逐字要求这几条(由**调用方**维护, 本插件只负责产出):

- `uboot-erofs: /assets 就绪` —— `erofs_vfs.c` 的挂载成功行(super 与 vfs-core 各自也打
  `vfs: mount /assets ...`);
- `[EROFCONF] SUMMARY pass=N fail=0 ` —— 自检收尾行;
- **禁止**出现 `[EROFCONF] FAIL` 与 `[PANIC]`;
- 8 条 tag 逐条点名。

⇒ 挂载行的措辞是**门禁契约的一部分**(改它就会把门禁判红), 代码里也在同一处留了注释。

## 8. 欠债(如实记账)

1. **`WORKAROUND(br-wa-fs-001)`** —— 挂载点是**代码常量**: plugin.toml 的 `[[mount]].path`
   与本插件 `erofs_internal.h` 的 `BR_EROFS_MOUNT_PATH` 两处真值(工具的挂载计划尚未被
   组合器消费)。与 `fs/tmpfs` 的 `TMPFS_MOUNT_PATH` 同款。退出条件 = 组合器按挂载计划
   产出行。
2. **`br_mount_register` 的挂载点自动 mkdir 在本件用不上**: `/assets` 由 `fs/tmpfs`
   作为"挂载点在父 FS 缺失时自动 mkdir"建出来(vfs 的 `mkdir_p`)。
3. **lookup 是 O(深度) 的路径重走**: 每次 `lookup` 都把"父的相对路径 + 名字"交给上游的
   `erofs_ilookup()` 从卷根重走一遍(因为上游只认路径)。目录块内是线性扫描(上游如此,
   没有用"dirent 排序 ⇒ 二分")。在 v1 的单 APP 规模下可接受; 真正的改进是给 inode 缓存
   一个 nid → 包装的映射(v2 的 dcache, SD-3 明确推迟)。
4. **超级块 checksum 不校验**: 上游也不校验(u-boot 侧只算不查)。`sb_csum` 特性位存在时
   本件同样不验 —— 少一次 crc32c 与一次重读。
5. **`emmc3` 这个设备名是约定**: 本插件按 `br_bdev_get("emmc3")` 取设备。宿主
   `platform/qemu-aarch64/plugin.toml` 目前只挂了 `emmc0`; 第二块 16 MiB 卡
   (16 KiB 的 EROFS 镜像 ↔ 擦写区从 LBA 4096 起)需要平台侧补一个
   `-drive if=none,id=emmc1,...,file=build/emmc3.img` + `-device emmc,drive=emmc1`
   (或由 `tools/mk-fs-images.sh` 与平台声明一起产出)。**设备不在 ⇒ 挂载失败 ⇒ 首败即
   停机**(刻意的): 没有 `/assets` 的产品不该在半残命名空间里继续跑。
6. **单块 I/O 走逐扇区**: 4 KiB 块 = 8 次单扇区 `read`。满足 `max_rw ≤ 64` 的最强形式,
   代价是命令数; 将来可以让 shim 用 `max_rw` 做多扇区批量(bdev 允许 0..n 的短读),
   但那时"整扇区读进中转缓冲"的约束仍在。
7. **`map->mpage`(4 KiB)在本件没有读者**: 保留它是为了与上游结构体同形, 索引块实际读
   在共享 scratch 里。这是一个 sizeof(struct erofs_map_blocks) ≈ 4.1 KiB 的静态浪费
   (已在 `[[res]]` 的 16 KiB 预算里)。
