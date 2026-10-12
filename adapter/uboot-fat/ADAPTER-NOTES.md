# `adapter/uboot-fat` — 三方件的**环境**与**对接**(ADR-0026 D-2/D-3)

> **一句话**: `third_party/` 是上游原件(只读, 逐字节), `compat/` 是"给上游的环境",
> `glue/` 是"我们的对接"。三者的边界不是靠约定, 而是靠**哈希锁**(vendor)、
> **相似度判据**(adapter 不许夹带副本)与**依赖方向**(上游 .c 只经 `compat/fat_env.h`)。

## 为什么会有这两个目录

上游 FAT 假设的是一整套 u-boot 环境: `<blk.h>`/`<part.h>` 的 `struct blk_desc` +
`disk_partition` + `blk_dread/blk_dwrite`、`<fat.h>` 的盘上结构、`<fs.h>` 的目录项、
`malloc_cache_aligned()`/`memalign()`、`rand()`、libc 的
`malloc/strdup/strchr/strrchr/strcasecmp/sprintf`、`printf`/`debug`/`log_err`、`assert()`。

**"适配"的定义是给上游它依赖的环境, 而不是改上游让它适应我们**(ADR-0026 D-3)。
所以本插件不写"移植版"的 FAT —— 上游 `fs/fat/fat_write.c`(它 `#include "fat.c"`)按它自己的
写法参与编译, 一行不改; 我们提供:

| 上游依赖 | 我们提供 | 放在 |
|---|---|---|
| u-boot 的**头名**(`<blk.h>`/`<part.h>`/`<log.h>`/`<malloc.h>`/`<memalign.h>`/`<rtc.h>`/`<command.h>`/`<div64.h>`/`<rand.h>`/`<config.h>`/`<exports.h>`/`<asm/*>`/`<dm/uclass.h>`/`<linux/*>`) | 每个头名一个**薄壳**, 全部接到环境实现头 | `compat/` |
| u-boot 的**类型/宏/函数**(u8…/loff_t/`__u16`/`le16_to_cpu`/`min_t`/`FATBUFSIZE` 依赖的 `CONFIG_FS_FAT_MAX_CLUSTSIZE`/`malloc_cache_aligned`/`assert`/`printf`…) | `fat_env.h`(**环境实现**) | `compat/` |
| u-boot 的**盘上格式**(`boot_sector`/`volume_info`/`dir_entry`/`dir_slot`/`nameext`/`fsdata`/属性位/`FAT2CPU*`) | **转发壳** → vendor 的 `include/fat.h`(原件) | `compat/fat.h` |
| `struct fs_dirent`/`fs_dir_stream`/`FS_DT_*` | **转发壳** → vendor 的 `include/fs.h`(+ 新引入的 `include/rtc_def.h`) | `compat/fs.h`、`compat/rtc_def.h` |
| 块读写的**落地**(`blk_dread/blk_dwrite`)、卷表、BPB 观测 | `glue/fat_blk.c` | `glue/` |
| VFS 四层 ops + 生命周期 | `glue/fat_vfs.c` | `glue/` |
| 一致性用例 | `glue/fat_selftest.c` | `glue/` |

## 依赖方向(单向)

```
   third_party/uboot-imx/fs/fat/fat_write.c   ← 只读 vendor 原件(逐字节 = 上游; sha256 锁)
                │  只经 u-boot 头名取环境
                ▼
   adapter/uboot-fat/compat/   fat_env.h(环境实现) + <头名> 薄壳 + 3 个转发壳
                ▲
                │  同一个环境头
   adapter/uboot-fat/glue/     fat_blk.c / fat_vfs.c / fat_selftest.c(本项目代码)
```

**单向**: 上游 .c **不** include 任何 `<br/...>`(它只 include u-boot 头名, 那些名字由
`compat/` 兑现); `glue/` 可以 include `<br/...>`(它就是本项目的对接层)与上游公开 API
(`<fat.h>`)。判据 = `tools/check-third-party-pristine.sh` 判据 ⑤ + 相似度门禁
`tools/check-adapter-copies.py`。

## 三类文件的归属(逐文件)

| 文件/目录 | 归属 | 上游对应物 |
|---|---|---|
| `brick.toml` 的 `sources` 里的 `fs/fat/fat_write.c` | **上游原件**(在 `third_party/`, 本目录**没有**副本) | `fs/fat/fat_write.c`(含它 include 的 `fat.c`) |
| `compat/fat_env.h` | **本项目**(环境) | 无(兑现上游对 u-boot 运行环境的依赖) |
| `compat/<头名>.h`(薄壳, 20 个) | **本项目** | 无(只是名字) |
| `compat/fat.h`、`compat/fs.h`、`compat/rtc_def.h` | **本项目**(转发壳) | `include/fat.h`、`include/fs.h`、`include/rtc_def.h` |
| `glue/fat_blk.c` | **本项目** | 无(上游的等价物是 `blk_dread/blk_dwrite` + `disk/part.c`) |
| `glue/fat_vfs.c` | **本项目** | 无(上游的入口是**路径字符串** `fs_read`, 不是四层 ops) |
| `glue/fat_selftest.c` | **本项目** | 无 |
| `glue/fat_glue.h` | **本项目**(插件内部契约) | 无 |

> ★ 上一版把 `<fat.h>` 的盘上结构**照抄**进了 `compat/fat_compat.h`(那是 ADR-0026 §5 点名要
> 消灭的"抄上游")。现在那几个结构与属性位来自 vendor 原件, 本目录只剩**转发壳** ——
> 一个字段都不抄。

## 上游代码需要环境"有实体"的三处(不是改上游, 是给它环境)

1. **`blk_dread/blk_dwrite`**(`glue/fat_blk.c`): 上游 `disk_read/disk_write` 的落地 ——
   接到 `br_bdev_ops_t.read/write`, 并按几何 `max_rw` 切块 + 越界拒绝(上游没有这两条约束)。
   返回值保持上游口径(成功 = 扇区数, 失败 < 0)。
2. **`uboot_fat_strdup/uboot_fat_sprintf/uboot_fat_rand`**(`glue/fat_blk.c`): 无 libc 的
   `strdup`/`sprintf` 与 u-boot 的 `rand()` 在 vendor 树里都没有原件 ⇒ 由环境实现, 上界与
   理由写在 `compat/fat_env.h` 的文件头(都是**上游调用点自己的构造**, 不是我们拍的数)。
3. **`struct blk_desc`/`disk_partition` 的极小 shim**(`compat/fat_env.h`): 整设备一个分区
   (起始扇区 0, 扇区数 = `geometry.n_sectors`)⇒ 上游的 `cur_part_info.start + block` 恒等于
   `block`, 偏移算术无需改动。

## 判据(机械的)

```bash
bash  tools/check-third-party-pristine.sh    # vendor 逐字节 = 上游; 无人 include 三方件的头
python3 tools/check-adapter-copies.py        # adapter/ 里没有三方件源码副本
cc -fsyntax-only <vendor>/fs/fat/fat_write.c -Iadapter/uboot-fat/compat …   # 上游原样编过
brickie test fat-test                        # TC-FAT-001..008 全绿
```

> 逐文件的**历史对照**(改写副本相对上游改了什么)仍在
> [`docs/analysis/0003-third-party-port-delta.md`](../../docs/analysis/0003-third-party-port-delta.md):
> 那正是这一刀要消灭的东西, 作为对照留下。
