# `adapter/` — uboot-fat 的**brickOS 适配层**

> **一句话**: `src/` 是"上游 u-boot 的代码", `adapter/` 是"我们的代码"。
> 两者的边界不是靠约定, 而是靠**依赖方向**与**门禁**(`tools/check-third-party-isolation.sh`)。

## 为什么要有这个目录

本插件是 u-boot `fs/fat/**`(GPL-2.0+)的移植件。上游 FAT 假设的是一整套 u-boot 环境
(`<fat.h>` 的盘上结构、`<part.h>` 的 `struct blk_desc`/`disk_partition`、`blk_dread`/
`blk_dwrite`、libc 的 `strdup/strchr/rand`、`printf`/`debug`、`assert`), 而本件是
**裸机 unikernel + 本项目的四层 VFS 契约**。把"接环境"写进上游派生文件里, 会让三方代码
与 brickOS 代码的边界消失(理由与判据详见 `adapter/uboot-mmc/adapter/README.md`
的同一节, 此处不重复)。

## 依赖方向(单向)

```
        ┌──────────────────── src/ (上游派生, GPL-2.0+) ─────────────────────┐
        │  fat_core.c (← fs/fat/fat.c)   fat_write.c (← fs/fat/fat_write.c)  │
        │  fat_internal.h(内部契约; 含上游 struct fat_itr 的逐字段照抄)      │
        └───────────────────────────────┬───────────────────────────────────┘
                                        │ 只经 fat_compat.h 取运行环境
                                        ▼
        ┌────────────────────────── adapter/ (本项目) ──────────────────────┐
        │  fat_compat.h   ← 唯一 include <br/...> 的**头**                  │
        │                   (u-boot/Linux 式类型与工具宏 + 字符串小工具)     │
        │  fat_vfs.c      ← br_fs_ops/br_inode_ops/br_file_ops 对接 + 钩子   │
        │  fat_selftest.c ← TC-FAT-*(经 vfs 的端到端)                        │
        └────────────────────────────────────────────────────────────────────┘
```

**`src/` → `adapter/` 单向**。`adapter/` include `src/` 的头(fat_internal.h)是允许的。

## 文件清单与归属

| 文件 | 归属 | 上游对应物 |
|---|---|---|
| `src/fat_core.c` | 上游派生(76.4% 行重合) | `fs/fat/fat.c` |
| `src/fat_write.c` | 上游派生(65.9% 行重合) | `fs/fat/fat_write.c` |
| `src/fat_internal.h` | 上游派生(部分: `struct fat_itr` 逐字段) | `fs/fat/fat.c:733-800` + 本件内部契约 |
| `adapter/fat_compat.h` | **本项目** | 盘上结构照抄 `include/fat.h`; 其余无对应物 |
| `adapter/fat_vfs.c` | **本项目** | 无(上游的入口是**路径字符串** `fs_read`, 不是四层 ops) |
| `adapter/fat_selftest.c` | **本项目** | 无 |

> ★ `src/fat_write.c` 里的 `uboot_fat_truncate()` 是**本项目新增**(上游没有 truncate,
> 只有一句 `/* truncate the rest */` 注释)。它目前**留在 `src/`** 是因为它与上游写路径
> 共用同一批静态原语(`set_fatent_value`/`flush_dir`/`find_empty_cluster`), 搬出去要么
> 导出内部符号、要么复制逻辑。这是一处**已知的边界不纯**, 登记在 ADR-0024 的"未做"节。

## 判据(机械的)

与 uboot-mmc 同一条门禁、同一套四条判据 —— 见
`adapter/uboot-mmc/adapter/README.md` 末节。
