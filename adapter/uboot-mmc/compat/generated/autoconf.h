/* adapter/uboot-mmc — **u-boot 的配置矩阵**(扮演真实 u-boot 构建里 `generated/autoconf.h`)
 *
 * ============================== 本文件是什么 ==============================
 * 上游 `drivers/mmc/` 下的原件 里满地的 `CONFIG_IS_ENABLED(X)` / `IS_ENABLED(CONFIG_X)` /
 * `#ifdef CONFIG_X` 不是"可选的装饰", 它们**决定上游代码的形状**(哪些字段、哪些函数、
 * 哪条数据通路参与编译)。要"上游一个字都不改地编过", 就必须像 u-boot 的真实构建一样,
 * 提供一个 `generated/autoconf.h` —— 本文件就是它(`include/linux/kconfig.h` 的第一行
 * 就是 `#include <generated/autoconf.h>`, 那是 u-boot 里这份矩阵的**权威读者**)。
 *
 * ## ★ 配置矩阵的关键陷阱(逐条都是实测踩出来的)
 *
 * u-boot 的 `#ifdef` 认的是"**定义没定义**", 不是"值是几" ⇒
 *   - 要**启用**的: `#define CONFIG_X 1`(`CONFIG_IS_ENABLED(X)` 靠"粘贴后是不是 1"
 *     判定, 见 `linux/kconfig.h` 的 `__ARG_PLACEHOLDER_1` 机制);
 *   - 要**关闭**的: **什么都不定义**。`CONFIG_IS_ENABLED` 的粘贴机制把"未定义"当 0,
 *     而 `#ifdef` 也自然为假 —— 两边都对。
 *     ⚠ **绝不能**写成 `#define CONFIG_DM_MMC 0`: 那会让 `sdhci.c:200/646/759` 的
 *     `#ifdef CONFIG_DM_MMC` 变成**真**, 于是上游按 DM(udevice)形态编译 —— 直接编不过。
 *   - "**值型**"配置(不是开关, 是数值)必须给: 它们以裸标识符出现在表达式里,
 *     不定义就是 `-Wundef` + `-Werror` 直接红, 且语义上也缺真值。
 *
 * ## 本件的矩阵(每一条都写"为什么是它")
 *
 * | 配置 | 值 | 为什么 |
 * |---|---|---|
 * | `CONFIG_MMC_WRITE` | 1 | `mmc_write.c`(块写/擦除)进了 `sources`。不给它 `struct mmc` 里 `write_bl_len`/`erase_grp_size`/`ssr` 被裁掉(见 mmc.h 的 `#if CONFIG_IS_ENABLED(MMC_WRITE)`), 上游 mmc_write.c 立刻编不过 |
 * | `CONFIG_MMC_SDHCI_ADMA` | 1 | 本项目要求 ADMA2 数据通路(ADR-0021 D-4)。置 1 后 `sdhci_setup_cfg()` 才会在 caps 允许时给 `host->flags` 置 `USE_ADMA`, 且 `struct sdhci_host` 才有 `adma_desc_table` 字段 |
 * | `CONFIG_MMC_SDHCI_ADMA_HELPERS` | 1 | `sdhci-adma.c` 在 u-boot 里**只在这个开关打开时才编**(drivers/mmc/Makefile)。我们把它列进了 `sources`, 于是 `struct sdhci_ops::adma_write_desc` 必须存在(`sdhci-adma.c:36` 无条件读它) |
 * | `CONFIG_MMC_QUIRKS` | 1 | 上游 `mmc.c:2965` 的 `#ifdef` 打开后, `mmc_get_op_cond()` 才给 `mmc->quirks` 置上三条重试位(set_blocklen/send_cid/app_cmd)。QEMU 的 SDHCI 模型对同一命令的偶发 NAK 靠它重试;**关掉**等于把上游自带的容错也裁掉 |
 * | `CONFIG_SYS_MMC_MAX_BLK_COUNT` | 64 | **值型**。它同时决定 `cfg->b_max`(上游单次传输的块数上界)与 `ADMA_TABLE_SZ`(描述符表大小, 见 `include/sdhci.h`)。★ 与本插件 `glue/emmc_internal.h` 的 `BR_EMMC_MAX_RW_SECTORS` 是**同一个数**的**同一处真值**的两个名字(见那里的注释) |
 * | `CONFIG_LOGLEVEL` | 4 | **值型**。`mmc.c:175` 的 `CONFIG_VAL(LOGLEVEL)` 会展开成裸标识符 `CONFIG_LOGLEVEL`, 不定义 ⇒ `-Wundef` 报错。取值 4 = `LOGL_WARNING`(u-boot 默认口径): 于 `>= LOGL_DEBUG` 为假, 上游那段"逐命令 trace 打印"不参与编译(与"不刷屏"一致) |
 * | `CONFIG_FS_FAT_MAX_CLUSTSIZE` | 32768 | **值型**。它属**接口面**而非本件功能: 上游 `include/part.h`(真实形态)经 `include/fat.h` 间接依赖它, 而 `part.h` 是所有块设备的公共接口面。本件不编 FAT, 但把接口面依赖的"值型配置"补齐, 与 `adapter/uboot-fat` 同源(同为 32768) |
 *
 * ## 有意**不定义**的(写下来免得下次有人"顺手打开")
 *
 * `DM_MMC`/`DM_REGULATOR`/`DM_GPIO`/`BLK`/`DM`/`CYCLIC`/`MMC_TINY`/`MMC_UHS_SUPPORT`/
 * `MMC_HS200_SUPPORT`/`MMC_HS400_SUPPORT`/`MMC_HS400_ES_SUPPORT`/`MMC_SUPPORTS_TUNING`/
 * `MMC_HW_PARTITIONING`/`MMC_IO_VOLTAGE`/`MMC_SDHCI_SDMA`/`MMC_SDHCI_ADMA_64BIT`/
 * `MMC_SDHCI_IO_ACCESSORS`/`MMC_PWRSEQ`/`MMC_SPEED_MODE_SET`/`MMC_VERBOSE`/`MMC_TRACE`/
 * `XPL_BUILD`/`SYS_64BIT_LBA`/`PARTITION_UUIDS`/`FIXED_SDHCI_ALIGNED_BUFFER`。
 * 前四个是"形态"开关: 打开就把上游推向 DM/udevice 或 uclass, 而本件按 ADR-0026 的
 * "上游原样编译"走 u-boot 的 **legacy(!DM_MMC)** 路径(`struct mmc_config::ops` +
 * `add_sdhci()`), 那是 u-boot 自己就支持的形态(见任务书 §2.1)。
 * `MMC_SDHCI_SDMA` 关掉是因为本件只做 ADMA2/PIO; `MMC_SDHCI_ADMA_64BIT` 关掉使
 * `struct sdhci_adma_desc` 保持 8 字节(32 位地址, 恒等映射下足够)。
 *
 * 判据: 矩阵的"启用"一侧与 `plugin.toml` 的 `sources` 一一对应; "关闭"一侧的每一条
 * 都在上面的表里写了理由。
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_MMC_GENERATED_AUTOCONF_H
#define BR_ADAPTER_UBOOT_MMC_GENERATED_AUTOCONF_H

/* ---- 启用(只列"是 1"的; 不在这里的 = 关闭) ---- */
#define CONFIG_MMC_WRITE                 1
#define CONFIG_MMC_SDHCI_ADMA            1
#define CONFIG_MMC_SDHCI_ADMA_HELPERS    1
#define CONFIG_MMC_QUIRKS                1

/* ---- 值型(必须存在; 见文件头的矩阵表) ---- */
#define CONFIG_SYS_MMC_MAX_BLK_COUNT     64
#define CONFIG_LOGLEVEL                  4
#define CONFIG_FS_FAT_MAX_CLUSTSIZE      32768

#endif /* BR_ADAPTER_UBOOT_MMC_GENERATED_AUTOCONF_H */
