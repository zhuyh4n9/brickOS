# 0024 — 三方件隔离**硬化**: `src/` ↔ `adapter/` 硬边界 + 审计发现的三处缺陷

> 状态: **已落地**。三个三方插件的源码目录一分为二 —— `src/`(上游派生)与
> `adapter/`(本项目适配层), 依赖方向单向; 新增适配层入口头 `mmc_compat.h`(fat/erofs
> 的入口头已有); 新增门禁 `check-third-party-isolation`(四条机械判据)进 `gates.toml`;
> 修掉审计核实的**三处缺陷**(一个错常量 / 一处缓冲错配 / 全树 SPDX 署名回归)。
> 前置: `docs/decisions/0021-bdev-pci-emmc.md`(D-3 三方隔离)、
> `docs/decisions/0022-erofs-fat.md`(D-1 两件 FS 为何只能是三方件)、
> `docs/decisions/0023-errno-in-core.md`(D-6「三方件最小改动」原则)。
> 对照基准: u-boot `nxp-imx/uboot-imx`, 分支 `lf_v2026.04`,
> commit `bb41975aeb4cbd153b49c9fd62da1d64cd65968d`(与 `NOTICE` 一致)。

## 1. 背景: 隔离政策**只写在文档里**

ADR-0021 D-3 起, 三个移植件声明 `api_type = "third_party"`, 政策是"目录级隔离":
`third_party/**` 是 GPL-2.0+, `framework/**`/`platform/**`/`core/**` 保持 WTFPL。
`NOTICE` 逐文件列了来源。

但这套政策**没有任何机械判据**。本刀的审计(以钉死 commit 的上游为基准, 逐文件 `diff`)
发现了三类问题, 都是"没有判据"的直接后果:

1. **边界不存在**: `src/` 的"上游派生件"里散着十来条 `#include <br/...>` 与上百处
   `br_*` 调用, 与 `adapter` 角色混在一个目录里 —— "目录隔离"只剩目录名。
2. **"移植"名不副实**: 见 §2 的重合度表。`uboot-mmc` 的两个 `.c` 与上游的实义行重合度
   只有 **2.5%** 与 **9.1%**, 是**重写**; 另有 459 行(26%)是纯本项目代码。
3. **署名回归**: 上游 `drivers/mmc/**`、`fs/fat/**`、`fs/erofs/**`、`lib/lz4.c`
   **每个文件**都带 `SPDX-License-Identifier`; 而移植件里只有 `erofs_fs.h` 保留了 ——
   其余 11 个上游派生文件把 SPDX 行丢了。

## 2. 审计结果(逐文件, 相对上游)

方法: 取上游 tarball(`codeload` 的 commit 快照, 41 546 项), 逐文件 `diff` +
词法归一化(去注释/空白)后算重合度。**没有任何文件逐字节等同上游。**

取证命令(可复现; 落点在 `build/`, 不进版本库):

```bash
REF=build/uboot-ref/uboot-imx-bb41975aeb4cbd153b49c9fd62da1d64cd65968d
mkdir -p build/uboot-ref && cd build/uboot-ref
curl -sL -o uboot.tar.gz \
  https://codeload.github.com/nxp-imx/uboot-imx/tar.gz/bb41975aeb4cbd153b49c9fd62da1d64cd65968d
# ↑ sha256 = 9d060f32c84b15c1ae2e9903c300d51b3088898b3ad80b95bca74c67ef6a4a52 (47 019 609 B)
tar xzf uboot.tar.gz "$(basename $REF)/{drivers/mmc,include,fs/fat,fs/erofs,lib/lz4.c}"
# 逐文件对照:
diff -u "$REF/drivers/mmc/sdhci.c" third_party/uboot-mmc/src/sdhci_host.c | head
```

| 插件 | 文件 | 上游对应物 | 重合度 | 判定 |
|---|---|---|---|---|
| mmc | `mmc_core.c` | `drivers/mmc/mmc.c` + **`mmc_write.c`** | 2.5% | 重写 |
| mmc | `sdhci_host.c` | `drivers/mmc/sdhci.c` + `sdhci-adma.c` | 9.1% | 重写 |
| mmc | `mmc_regs.h` | `include/mmc.h` + `include/sdhci.h` | 137/141 宏名一致 | 摘录 |
| fat | `fat_core.c` | `fs/fat/fat.c` | 76.4% | 派生 |
| fat | `fat_write.c` | `fs/fat/fat_write.c` | 65.9% | 派生 |
| erofs | `erofs_decompress.c`(LZ4 段) | `lib/lz4.c` | ~90% | 最贴近 |
| erofs | `erofs_zmap/data/namei/super.c` | 同名上游 | 60~75% | 派生 |
| erofs | `erofs_fs.h` | `fs/erofs/erofs_fs.h` | — | 派生(**NOTICE 说的"原样保留"不成立**) |

NOTICE 的三处不准确(已在 `NOTICE` 修订): ① 漏列上游来源 `drivers/mmc/mmc_write.c`
(写路径的真实出处); ② 把 `struct mmc_cmd`/`struct mmc_data` 记到 `mmc_private.h`
(实际在 `include/mmc.h`); ③ `erofs_fs.h` 记作"原样保留", 实际裁剪了 xattr 家族等。

**本刀不改"重写"这个事实**——把 `uboot-mmc` 按上游重写成逐字节等同, 要连同 DM/uclass、
SD/UHS/HS200/HS400/tuning/RPMB/分区、`blk_desc` 一起搬回来, 与本项目的 bdev 形态、
中断驱动、静态实例表直接冲突, 且本环境**无法验证运行期行为**(见 §5)。本刀做的是
**把边界变成可判定的**, 而不是重写。

## 3. 决策

### D-1 每个三方插件的源码一分为二: `src/` 与 `adapter/`

- **`src/`** = **只放有上游对应物的文件**(上游派生件 + 插件的内部契约头);
- **`adapter/`** = **本项目写的全部代码**: 兼容层/适配层入口头、bdev-vfs 对接、
  一致性用例。

| 插件 | `src/` | `adapter/` |
|---|---|---|
| uboot-mmc | `mmc_core.c` `sdhci_host.c` `mmc_regs.h` `emmc_internal.h` | `mmc_compat.h`(新) `emmc_bdev.c` `emmc_selftest.c` |
| uboot-fat | `fat_core.c` `fat_write.c` `fat_internal.h` | `fat_compat.h` `fat_vfs.c` `fat_selftest.c` |
| uboot-erofs | `erofs_{super,namei,data,zmap,decompress}.c` `erofs_fs.h` `erofs_internal.h` | `erofs_compat.h` `erofs_vfs.c` `erofs_selftest.c` |

### D-2 依赖方向**单向**: `src/` → `adapter/`

`src/` 里的上游派生件**不许** `#include <br/...>`; 运行环境只能经 `adapter/` 的
适配层入口头取(`mmc_compat.h` / `fat_compat.h` / `erofs_compat.h`)。
反向(`adapter/` include `src/` 的头)是**允许**的 —— 适配层当然要认识被适配的代码。

于是"三方代码 ↔ brickOS 代码"的边界从"散在两个目录的十来条 include"
收敛成**一个文件**: 适配层入口头是 `src/` 唯一能看见 brickOS 的地方。

> 为什么不是"给每个 OS 服务做一个函数指针表"(u-boot 的 `struct mmc_ops` 那种形态):
> 那会**改动上游代码的结构**(把 `udelay()` 换成 `ops->delay()`), 与"三方件最小改动"
> (ADR-0023 D-6)直接冲突。入口头的做法让上游派生件里的**调用点拼写不变**, 只把
> "这些名字从哪来"集中到一处。

### D-3 三方件是**叶子**: 全树不许反向 include

`third_party/` 之外(框架/平台/core/APP)**不许** `#include "third_party/...`。
与规则 `api_type_edge_reason` 的 `native → third_party` 禁则同旨 —— 但那条只管
**插件依赖**, 管不到裸 `#include`; 本刀把后者也纳入判据。

### D-4 署名是判据, 不是习惯

`src/` 每个文件必须带 `SPDX-License-Identifier`, 标识符逐文件取自对应上游文件:

| 文件 | 标识符 |
|---|---|
| mmc 各 `.c`; fat 各 `.c`; erofs 各 `.c`(除 `erofs_fs.h`) | `GPL-2.0+` |
| `mmc_regs.h` `emmc_internal.h` `fat_internal.h` `erofs_internal.h` | `GPL-2.0+` |
| `erofs_fs.h` | `GPL-2.0-only OR Apache-2.0`(上游原样) |
| `erofs_decompress.c` 的 LZ4 段 | 段标识 `GPL 2.0+ OR BSD-2-Clause`(上游 `lib/lz4.c`) |

`erofs_decompress.c` 是**两个上游文件的合并**(`decompress.c` + `lib/lz4.c`), 两者许可
不同, 所以用 REUSE 口径的**段标识**: 文件头挂 `GPL-2.0+`, LZ4 段之前挂它自己的
`GPL 2.0+ OR BSD-2-Clause`(BSD 全文本来就在该段内)。

## 4. 审计核实并修掉的三处缺陷

三处都是**独立于本重构**的真实缺陷, 对着上游逐一核实过:

### R-1 `EXT_CSD_CMD_SET_NORMAL` 值错(`mmc_regs.h`)

- 移植件: `0u`; 上游 `include/mmc.h:249`: `(1 << 0)` = 1。
- 该宏当时**无引用**(潜伏), 但它与同文件"数值逐字对齐"的自述直接矛盾。
- 修: 改为 `(1u << 0)` 并标注上游出处。

### R-2 EROFS 压缩索引路径**缓冲错配**(`erofs_zmap.c`) —— 本刀最重的一处

- `z_erofs_reload_indexes()` 把索引块读进 **`map->mpage`**;
  而两个解析器(`legacy_load_cluster_from_disk` / `compact_load_cluster_from_disk`)
  都从 **`uboot_erofs_scratch()`** 取字节 —— 两个**不同**的 4 KiB 缓冲, 中间**没有拷贝**。
- `map->mpage` 全文只有这一处写、**零处读**; 文件头注释却自称"语义完全一样"。
- 后果: 压缩镜像(LZ4)走这条路时, 解析到的是共享 scratch 里的**残留内容**。
  ★ 运行期影响**未能在本环境证实**(见 §5: 本机 QEMU 无 `-device emmc`, `erofs-test`
  门禁跑不起来, 与本次改动无关)。
- 修: 读点改为 `uboot_erofs_scratch()`, 与解析点同一个缓冲; 并把 `mpage` 无读者这件事
  写进注释(而不是留一句与代码相反的"语义完全一样")。

### R-3 SPDX 署名回归(11 个文件)

见 D-4。修: 按上游逐文件补回标识符。

## 5. 未做与欠债

| 项 | 为什么没做 |
|---|---|
| `uboot-mmc` 的 `.c` 按上游重写 | 见 §2 末: 要连 DM/SD/UHS/RPMB/电源整族搬回来, 与本项目形态冲突; 且**本环境无法验证运行期行为** |
| `fat_write.c` 的 `uboot_fat_truncate()`(上游没有, 160 行)搬去 `adapter/` | 它与上游写路径共用同一批 **static** 原语(`set_fatent_value`/`flush_dir`/`find_empty_cluster`), 搬出去要么导出内部符号、要么复制逻辑 —— 后者更坏。**已知的边界不纯**, 登记在此 |
| mmc 的两处行为偏离: `MMC_RSP_BUSY` 未等 `DATA_END`(上游 `sdhci.c:262` 会等)、136 位响应缺 `<<8` 与字节拼接(上游 `sdhci.c:46-53`) | CID/CSD 目前被丢弃 ⇒ 后者是潜伏; 前者影响 CMD6/CMD12 完成判定。**改了无法验证**(见下), 单独立项 |
| 审计发现的 min 级问题: `uboot_fat_fls`/`ilog2`/`get_unaligned_le32` 死代码; `check_path_prefix` 疑似 `fs.fatbuf` 泄漏; `LOGC_FS` 未定义 | 与本刀主题(隔离)无关, 无运行期判据 |

**★ 本刀的验证边界(必须如实说明)**: 本机的 QEMU 是 8.2.2, **没有 `-device emmc`**
(`qemu-system-aarch64: -device emmc,drive=emmc0: 'emmc' is not a valid device model name`),
所以 `emmc/fat/erofs` 三个设备侧门禁在**本刀之前就已经是红的**。
本刀能验证到的是: **交叉编译 + 链接全绿**、`brickie check` 全绿、**宿主用例 7 件全绿**、
**脚本门禁全绿**; 设备侧的运行期行为**未验证**。

## 6. 判据

新门禁 `tools/check-third-party-isolation.sh`(注册于 `tests/gates.toml`),
四条机械判据:

1. 每个三方插件都有**非空**的 `src/` 与 `adapter/`;
2. `src/**` 里**不出现** `#include <br/...>`;
3. `third_party/` 之外**无人** include 三方件的头;
4. `src/**` 每个文件都带 `SPDX-License-Identifier`。

> ★ 门禁只判**边界在不在**, 不判"代码写得好不好"。它拦不住"把 brickOS 逻辑用宏伪装进
> `src/`"; 那道防线是 review, 判据是**适配层入口头里只该有"环境"**。
