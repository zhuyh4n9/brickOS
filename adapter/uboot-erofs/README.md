# adapter/uboot-erofs — u-boot EROFS 移植(ability / fs / third_party)

> ⚠ **本插件整体以 GPL-2.0-or-later 分发**: 上游原件是 GPL-2.0+(两处例外见 [NOTICE](NOTICE)), `compat/` 与 `glue/` 与上游原件同插件分发, 静态链进同一镜像后**整镜像**在分发时仍构成 GPL 组合作品(残余风险见 `../../../WORKAROUNDS.md` 的 `br-wa-tp-001`)。它与 [uboot-mmc](../uboot-mmc/README.md) 一起是本仓库内的 GPL 代码; 来源归属见 [NOTICE](NOTICE), 许可全文见 [LICENSE.GPL-2.0](LICENSE.GPL-2.0), 设计史见 [ADAPTER-NOTES.md](ADAPTER-NOTES.md)。

## 1. 它是什么

把 u-boot(NXP 分支)的 **EROFS 只读压缩文件系统**接进 brickOS, 出口是 `framework/vfs-core` 的
`br_fs_ops` / `br_inode_ops` / `br_file_ops` 契约。上游 `.c` **原样参与编译, 没有副本**:
[brick.toml](brick.toml) 的 `[build].sources` 直指 `third_party/uboot-imx/` 下的原件; 本件只提供
`compat/`(上游依赖的**运行环境**)与 `glue/`(**块层与 VFS 出口**)。本目录**没有 `src/`**。

```
emmc3(16 MiB 的 eMMC 卡, 镜像从扇区 0 开始; 由 adapter/uboot-mmc 注册)
        │  br_bdev_get("emmc3") + geometry() → {512 B 扇区, n_sectors, max_rw=64}
        ▼
  glue/erofs_vfs.c §①  块层 shim: 上游 erofs_dev_read()/erofs_blk_read() 的**实现**
        │               字节偏移 → 扇区; 逐扇区下发; 整扇区读进中转缓冲再按长度拷出
        ▼
  third_party/.../fs/erofs/super.c       偏移 1024 处找 magic 0xE0F5E1E2 → blkszbits/meta_blkaddr/root_nid
  third_party/.../fs/erofs/namei.c       inode 解析(32 B COMPACT / 64 B EXTENDED)+ 目录块内名字查找
  third_party/.../fs/erofs/data.c        平面/stub 映射 + chunk 映射 + 读路径(erofs_pread)
  third_party/.../fs/erofs/zmap.c        压缩映射迭代(4B/2B 紧凑索引、big pcluster、lookbehind)
  third_party/.../fs/erofs/decompress.c  INTERLACED / SHIFTED / LZ4 分派
  third_party/.../lib/lz4.c              LZ4_decompress_safe / LZ4_decompress_safe_partial
        │                (以上 6 个 .c 是**逐字节上游原件**; 环境由 compat/ 兑现)
        ▼
  glue/erofs_vfs.c §②  br_fs_ops.mount → 根 inode; lookup → 瞬态路径包装; read → erofs_pread
        ▼
  framework/vfs-core  /assets/hello.txt 等路径对 APP 可见
```

`compat/erofs_env.h` 是运行环境的**唯一实现**(类型/宏/日志/字符串/分配器接地);
`compat/` 其余头是 u-boot 头名的**薄壳**(每个 `<linux/...>` / `<asm/...>` / `<u-boot/...>` 名一个,
只 include 环境实现)。`compat/u-boot/lz4.h` 特殊 —— **相对路径转发**到 vendor 原件, 整个文件只有一行
相对路径 `#include`, 不复制内容(原因见 [ADAPTER-NOTES.md](ADAPTER-NOTES.md))。

## 2. 目录结构(实际在场的东西)

| 路径 | 归属 | 内容 |
|---|---|---|
| `brick.toml` | 本项目的声明面 | `[build].sources` 直指 vendor 原件 + `glue/*.c`; `[build].includes = ["compat", "glue", "<vendor>/fs/erofs"]`; `[build].cflags` = vendor 告警口径; `[selftest].cases = 8` |
| `compat/` | 本项目(适配层) | `erofs_env.h`(运行环境实现)+ 每个 u-boot 头名一个薄壳头 |
| `glue/` | 本项目(适配层) | `erofs_glue.h`(适配层内部契约)、`erofs_vfs.c`(块层 shim + VFS 四层 ops + `sbi` 存储 + on-disk 静态判据)、`erofs_selftest.c`(TC-EROFS-*) |
| `LICENSE.GPL-2.0` `NOTICE` `README.md` `ADAPTER-NOTES.md` | 本项目文档 | 许可全文 / 来源归属 / 本文件 / 适配层设计史 |

**没有 `src/`**, `compat/` 与 `glue/` 里也没有上游源码副本(唯一的"转发"是
`compat/u-boot/lz4.h` 的那一行 `#include`)。

## 3. 卷与接口(私有契约, 不导出)

| 项 | 值 |
|---|---|
| 块设备 | `emmc3`(16 MiB 镜像从**扇区 0** 开始; 镜像由宿主 `tools/mk-fs-images.sh` 造) |
| 挂载点 | `/assets`(只读; `brick.toml` 的 `[[mount]]` 与 `glue/erofs_glue.h` 的 `BR_EROFS_MOUNT_PATH` 两处真值) |
| 块大小 | 4096 B(`glue/erofs_vfs.c` 挂载期要求 `erofs_blksiz() == EROFS_MAX_BLOCK_SIZE`, 否则 `-ENOTSUP`) |
| 支持的数据布局 | 0 `FLAT_PLAIN` / 2 `FLAT_INLINE`(未压缩) · 3 `COMPRESSED_COMPACT` / 1 `COMPRESSED_FULL`(压缩) · 4 `CHUNK_BASED`(走上游 `data.c` 的 chunk 映射) |
| 支持的算法 | LZ4(含 0padding、SHIFTED、INTERLACED 的未压缩面); LZMA/DEFLATE 在 `z_erofs_decompress()` 处 `-EOPNOTSUPP` |
| inode 模型 | **瞬态路径包装**(设计 SD-3): 每次 lookup `br_malloc` 一个 `{相对路径, struct erofs_inode}` |
| 只读 | 名字空间变更 8 个槽位全填, 一律 `-EROFS`; 写权限打开也 `-EROFS` |
| 多设备 | 盘上有 device table 时被上游 `erofs_init_devices()` 判 `-EINVAL`(本件不设 `sbi.extra_devices`), 所以块层的 `device_id` 恒为 0 |

## 4. 与上游的关系: 适配在哪(逐条落点)

总口径: 上游 `.c` 一行不改, 下面每条都是**给它环境**或**在本项目侧对接**。

| # | 落点 | 事实 |
|---|---|---|
| 1 | `compat/erofs_env.h` | **无 libc / 无 u-boot**: 类型(u8/u64/loff_t/`__leN`/`IS_ERR` 家族)、`min_t`/`round_up`/`ilog2`/`BUILD_BUG_ON` 等工具宏在这里接地; 字符串自研(`uboot_erofs_str*` + 宏把上游裸名字接过来)、`malloc`→`br_malloc`、`memcpy` 等用 core 实现、`pr_err/info/debug`→`br_log_write` |
| 2 | `compat/erofs_env.h` | **`IS_ENABLED(CONFIG_*)` 按上游保留为编译期开关**: env 给 `CONFIG_LZ4 1` / `CONFIG_ZLIB 0`, 于是上游 `decompress.c` 的 `#if IS_ENABLED(...)` 原样成立; 不支持的算法收在 `z_erofs_decompress()` 的 `-EOPNOTSUPP`。旧的"拆掉开关改运行期分派"是 `src/` 时代的改写, 已被 ADR-0026 取代 |
| 3 | `glue/erofs_vfs.c` | **栈纪律的落点是适配层自己的缓冲**: 共享 4 KiB 中转缓冲 `s_scratch_words` 与目录块缓冲 `s_dir_blk_words` 都是**文件域静态**(裸机 APP 线程栈是 4 KiB 量级); 上游自身的局部量按上游原样编译 —— 旧 `src/` 里"把上游栈上 4 KiB 搬到文件域"的改写已随 ADR-0026 取消 |
| 4 | `glue/erofs_vfs.c` §① | **块层换 bdev**: 上游 `erofs_dev_read()`/`erofs_blk_read()` 的原型仍由上游 `internal.h` 给, **实现**在本件 —— `fs_devread`/`blk_desc` → `br_bdev_ops_t`; 逐扇区下发(恒 ≤ `max_rw`), **整扇区读进中转缓冲再按长度拷出**(直接交给设备会越界写); 越界/短读 ⇒ `-EIO` |
| 5 | `glue/erofs_vfs.c` §② | **出口是 VFS 四层 ops**: `br_fs_ops.mount` 产出根 inode, `lookup` 拼相对路径交给上游 `erofs_ilookup()`, 目录流在本件按 EROFS 的"块头 dirent 数组 + 块尾名字区"解析 |
| 6 | `compat/erofs_env.h` | **`DBG_BUGON` 不停机**: 上游 `internal.h` 把 `DBG_BUGON` 映射到 `BUG_ON`; 本件的 `BUG_ON` 只**打印**(`uboot_erofs_log_err_inline`), 由调用方紧接着的 `-EIO`/`-EFSCORRUPTED` 收敛(坏镜像不该把设备打死) |
| 7 | `compat/erofs_env.h` | **errno 名字归 core**(ADR-0023): 裸名字来自 `<br/core/br_errno.h>`, 本件**不带映射表**; `EFSCORRUPTED` 照抄上游 `internal.h` 的 `#ifdef EUCLEAN` 构造 |
| 8 | `glue/erofs_vfs.c` | **只读执法**: 名字空间变更槽位**也填**, 但一律 `BR_ERR(EROFS)`(比空槽位的 `-ENOTSUP` 更准); `br_file_ops.write` 与写权限/CREAT/TRUNC 打开同样 `-EROFS` |
| 9 | `glue/erofs_vfs.c` 文件域 | **on-disk 布局判据由适配层持有**: 一串 `_Static_assert(sizeof(struct …) == N)`。上游 `erofs_fs.h` 末尾那份 `static inline` 自检**上游自己从不调用** ⇒ `static inline` 不实例化 ⇒ 那些 `BUILD_BUG_ON` 永不诊断, **实际是死的** |
| 10 | `glue/erofs_vfs.c` | **全局 `sbi` 的存储**: 上游把 `struct erofs_sb_info sbi;` 放在 `fs/erofs/fs.c`(u-boot 的 blk/DM 胶水层), 本插件**不编译** `fs.c` ⇒ 存储由本件定义(上游 `internal.h` 已 `extern`) |

## 5. 名字上界:本仓库 `BR_NAME_MAX` = 32, EROFS 是 255

- **目录面(`readdir`)**: ≥32 字节的名字**跳过并计数**, 到穷尽时打一行 `br_log_warn` 报总数
  (目录迭代是"流", 所以不用 `-ENOSPC`)。
- **`lookup`**: `br_dirent_t`/分量缓冲装不下 ≥32 字节的名字 ⇒ 返回 `BR_NULL`(vfs 得 `-ENOENT`);
  路径面上的 `-ENOSPC` 由 vfs 的 `resolve_norm()` 给(`clen >= BR_NAME_MAX`), 该名字从路径面看仍是
  `BR_ERR(ENOSPC)`(`lookup` 的签名没有错误码通道, 所以 FS 侧只能 `BR_NULL`)。
- `path[UBOOT_EROFS_REL_PATH_MAX]`(256 B, 定义在 `glue/erofs_glue.h`)放不下的相对路径同样
  返回 `BR_NULL`。

## 6. 一致性用例(`TC-EROFS-*`, `glue/erofs_selftest.c`)

8 条, 判据都经**公开面**(`br_stat`/`br_open`/`br_file_read`/`br_file_lseek`/`br_opendir`/
`br_readdir`/`br_unlink`/`br_mkdir`)或**盘上事实**(直接读偏移 1024 的 magic); 夹具内容与宿主
`tools/mk-fs-images.sh` **逐字节对齐**(常量在 `glue/erofs_glue.h`):

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

判据是**生成式**的: `"ABCDEFGH"[i % 8]`、`(i * 1103515245 + 12345) >> 16 & 0xFF`、`i & 0xFF`
—— 源码里**没有任何大块字面量**。

> 两个压缩夹具刻意走两条 LZ4 路径: `comp/lz4.txt` 压得动(压缩比 5.85%, 70000 不是 4096 的
> 整数倍 ⇒ 覆盖尾 extent 的 partial decoding); `raw.bin` 压不动(压缩比 50%)。

## 7. 依赖与构建

| 边 | kind | 理由 |
|---|---|---|
| `framework/vfs-core` | init | 实现并注册 `br_fs_ops`/`br_inode_ops`/`br_file_ops` |
| `framework/bdev-core` | init | `br_bdev_get("emmc3")` 取块设备 |
| `adapter/uboot-mmc` | init | 它注册 `emmc3`; 这条**init 边**保证拓扑序在本插件之前 |

[brick.toml](brick.toml) 的编译面: `[build].sources` = vendor 的
`fs/erofs/{super,namei,data,zmap,decompress}.c` + `lib/lz4.c`(**原样编译, 无副本**) + `glue/*.c`;
`[build].includes` = `compat`、`glue`、vendor 的 `fs/erofs`(glue 与上游都要 include 上游
`internal.h`); `[build].cflags` = **vendor 的告警口径**(`-Wno-shadow` 等, 编 vendor 代码时关掉
本项目的 `-Wshadow`/`-Wpointer-arith`/…), **不为过告警去改上游**。

纪律的机械判据: `tools/check-third-party-pristine.sh`(`third_party/` 只读 + 逐字节对
`adapter/third-party-upstream.lock`)与 `tests/gates.toml` 的 `erofs-test`(见下节)。
> 已废弃: `tools/check-third-party-isolation.sh` **文件不存在**; 旧的 `src/` ↔ `adapter/`
> 边界判据也随之作废。

## 8. 门禁契约(改日志措辞前先看这里)

`tests/gates.toml` 的 `erofs-test` 逐字要求这几条(由**调用方**维护, 本插件只负责产出):

- `uboot-erofs: /assets 就绪` —— `glue/erofs_vfs.c` 的挂载成功行(super 与 vfs-core 各自也打
  `vfs: mount /assets ...`);
- `[EROFCONF] SUMMARY pass=N fail=0 ` —— 自检收尾行;
- **禁止**出现 `[EROFCONF] FAIL` 与 `[PANIC]`;
- 8 条 tag 逐条点名。

⇒ 挂载行的措辞是**门禁契约的一部分**(改它就会把门禁判红), 代码里也在同一处留了注释。

## 9. 欠债(如实记账)

1. **挂载点已归 APP 的 `[[mount]]`**: 本插件只 `br_fs_driver_register("erofs", …)` 登记驱动, 挂载点与**源设备**由 APP 的 `[[mount]]` 经 `cfg->path`/`cfg->dev` 传进 `mount` 槽位; `glue/erofs_glue.h` 的 `BR_EROFS_MOUNT_PATH`/`BR_EROFS_DEV_NAME` 退化为**自检夹具名**(判据 = `tools/check-mount-ownership.sh`)。
2. **`br_mount_register` 的挂载点自动 mkdir 在本件用不上**: `/assets` 由 `fs/tmpfs` 的"挂载点在父 FS 缺失时自动 mkdir"(vfs 的 `mkdir_p`)建出来。
3. **lookup 是 O(深度) 的路径重走**: 每次 `lookup` 都把"父的相对路径 + 名字"交给上游 `erofs_ilookup()` 从卷根重走(上游只认路径), 目录块内是线性扫描; v1 单 APP 规模可接受, 改进方向是 inode 缓存 nid → 包装的映射(v2 的 dcache, SD-3 明确推迟)。
4. **超级块 checksum 不校验**: 上游 `super.c` 只把 `checksum` 读进 `sbi.checksum` 而不验(`sb_csum` 特性位存在时同样不验)。
5. **`emmc3` 这个设备名是约定**: 本插件按 `br_bdev_get("emmc3")` 取设备; 平台侧(`platform/qemu-aarch64/brick.toml` 的 `[build.target.qemu].extra`)挂了**四块** `-device sdhci-pci` + `-device emmc`(`emmc0/1/2` = 三卷 FAT, `emmc3` = EROFS 卡), 后备文件与分工见 `tools/mk-fs-images.sh`。**设备不在 ⇒ 挂载失败 ⇒ 首败即停机**(刻意的); 这条与 `br-wa-fs-001`(挂载点/设备名是代码常量)是同一处欠债的两个侧面。
6. **单块 I/O 走逐扇区**: 4 KiB 块 = 8 次单扇区 `read`, 满足 `max_rw ≤ 64` 的最强形式, 代价是命令数; 将来可用 `max_rw` 做多扇区批量, 但"整扇区读进中转缓冲"的约束仍在。
