# `adapter/` — uboot-erofs 的**brickOS 适配层**

> **一句话**: `src/` 是"上游 u-boot 的代码", `adapter/` 是"我们的代码"。
> 两者的边界不是靠约定, 而是靠**依赖方向**与**门禁**(`tools/check-third-party-isolation.sh`)。

## 为什么要有这个目录

本插件是 u-boot `fs/erofs/**` + `lib/lz4.c`(GPL-2.0+ / 部分 GPL 2.0+ OR BSD-2-Clause)
的移植件。上游 EROFS 假设的是内核/u-boot 环境(`linux/compat.h`、`list_head`、
`IS_ERR/ERR_PTR`、`printf`/`pr_err`、libc 的 `malloc/strnlen`、`fs.c` 的块层),
而本件是裸机 unikernel + 本项目的 bdev/vfs 契约。把"接环境"写进上游派生文件里, 会让
三方代码与 brickOS 代码的边界消失(理由与判据详见
`third_party/uboot-mmc/adapter/README.md` 的同一节)。

## 依赖方向(单向)

```
        ┌──────────────────── src/ (上游派生, GPL-2.0+) ─────────────────────┐
        │  erofs_super.c  erofs_namei.c  erofs_data.c  erofs_zmap.c          │
        │  erofs_decompress.c(含 LZ4; 段标识 GPL 2.0+ OR BSD-2-Clause)       │
        │  erofs_fs.h(盘上格式唯一真值)   erofs_internal.h(内部契约)          │
        └───────────────────────────────┬───────────────────────────────────┘
                                        │ 只经 erofs_compat.h 取运行环境
                                        ▼
        ┌────────────────────────── adapter/ (本项目) ──────────────────────┐
        │  erofs_compat.h ← 唯一 include <br/...> 的**头**(类型/宏/字符串)   │
        │  erofs_vfs.c    ← 块层 shim(字节偏移→扇区)+ 四层 ops + 生命周期    │
        │  erofs_selftest.c← TC-EROFS-*(经 vfs 的端到端)                     │
        └────────────────────────────────────────────────────────────────────┘
```

**`src/` → `adapter/` 单向**: `src/erofs_internal.h` include `adapter/erofs_compat.h`;
`adapter/*.c` include `src/erofs_internal.h`(允许)。

## 文件清单与归属

| 文件 | 归属 | 上游对应物 |
|---|---|---|
| `src/erofs_super.c` | 上游派生 | `fs/erofs/super.c` |
| `src/erofs_namei.c` | 上游派生 | `fs/erofs/namei.c` |
| `src/erofs_data.c` | 上游派生 | `fs/erofs/data.c` |
| `src/erofs_zmap.c` | 上游派生 | `fs/erofs/zmap.c` |
| `src/erofs_decompress.c` | 上游派生(**两个上游文件的合并**) | `fs/erofs/decompress.c` + `lib/lz4.c` |
| `src/erofs_fs.h` | 上游派生(裁剪; SPDX 与版权逐字节保留) | `fs/erofs/erofs_fs.h` |
| `src/erofs_internal.h` | 上游派生(大幅裁剪) | `fs/erofs/internal.h` |
| `adapter/erofs_compat.h` | **本项目** | 无(吸收 upstream `internal.h` 的少量片段) |
| `adapter/erofs_vfs.c` | **本项目**(含 2 个 `fs.c` 的对应物: 块层) | `fs/erofs/fs.c` 的块层部分 |
| `adapter/erofs_selftest.c` | **本项目** | 无 |

> ★ `adapter/erofs_vfs.c` 里有**两个**上游 `fs.c` 的对应函数(`erofs_dev_read`/
> `erofs_blk_read`); 其余(四层 ops、目录流、共享 scratch)是本项目代码。按"文件归属"
> 它整件算 `adapter/` —— 因为它同时是 bdev 与 vfs 两位读者的对接面。逐函数归属见
> `NOTICE` 与 ADR-0024。

## 判据(机械的)

与 uboot-mmc 同一条门禁、同一套四条判据 —— 见
`third_party/uboot-mmc/adapter/README.md` 末节。
