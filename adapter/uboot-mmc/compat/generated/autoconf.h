/* adapter/uboot-mmc — the **u-boot configuration matrix** (plays the role of `generated/autoconf.h` in a real u-boot build)
 *
 * ============================== what this file is ==============================
 * The upstream sources under `drivers/mmc/` are full of `CONFIG_IS_ENABLED(X)` / `IS_ENABLED(CONFIG_X)` /
 * `#ifdef CONFIG_X`, which are not "optional decoration" but **determine the shape of the upstream code**
 * (which fields, which functions, which data path take part in the build). To "compile upstream without
 * changing a single character", one must provide a `generated/autoconf.h` just like a real u-boot build —
 * this file is it (the first line of `include/linux/kconfig.h` is `#include <generated/autoconf.h>`, the
 * **authoritative reader** of this matrix in u-boot).
 *
 * ## ★ Key pitfalls of the config matrix (each one hit in practice)
 *
 * u-boot's `#ifdef` tests "**defined or not**", not "what value" ⇒
 *   - to **enable**: `#define CONFIG_X 1` (`CONFIG_IS_ENABLED(X)` decides by "is it 1 after
 *     pasting", see the `__ARG_PLACEHOLDER_1` mechanism in `linux/kconfig.h`);
 *   - to **disable**: **define nothing**. The `CONFIG_IS_ENABLED` paste mechanism treats "undefined"
 *     as 0, and `#ifdef` is naturally false too — both agree.
 *     ⚠ **Never** write `#define CONFIG_DM_MMC 0`: that makes `#ifdef CONFIG_DM_MMC` at
 *     `sdhci.c:200/646/759` **true**, so upstream compiles in DM (udevice) shape — it simply fails to build.
 *   - "**value-type**" configs (not switches, but numbers) must be given: they appear as bare
 *     identifiers in expressions, so defining none is an immediate `-Wundef` + `-Werror` failure,
 *     and semantically the true value is missing too.
 *
 * ## This artifact's matrix (each entry states "why this")
 *
 * | config | value | why |
 * |---|---|---|
 * | `CONFIG_MMC_WRITE` | 1 | `mmc_write.c` (block write/erase) is in `sources`. Without it, `write_bl_len`/`erase_grp_size`/`ssr` in `struct mmc` are cut (see mmc.h's `#if CONFIG_IS_ENABLED(MMC_WRITE)`) and upstream mmc_write.c immediately fails to build |
 * | `CONFIG_MMC_SDHCI_ADMA` | 1 | This project requires the ADMA2 data path (ADR-0021 D-4). Only with 1 does `sdhci_setup_cfg()` set `USE_ADMA` in `host->flags` when caps allow, and only then does `struct sdhci_host` have the `adma_desc_table` field |
 * | `CONFIG_MMC_SDHCI_ADMA_HELPERS` | 1 | In u-boot `sdhci-adma.c` is **built only with this switch on** (drivers/mmc/Makefile). We listed it in `sources`, so `struct sdhci_ops::adma_write_desc` must exist (`sdhci-adma.c:36` reads it unconditionally) |
 * | `CONFIG_MMC_QUIRKS` | 1 | With the `#ifdef` at upstream `mmc.c:2965` on, `mmc_get_op_cond()` sets the three retry bits in `mmc->quirks` (set_blocklen/send_cid/app_cmd). QEMU's SDHCI model relies on them to retry an occasional NAK of the same command; **turning it off** also cuts upstream's built-in fault tolerance |
 * | `CONFIG_SYS_MMC_MAX_BLK_COUNT` | 64 | **Value type**. It determines both `cfg->b_max` (upstream per-transfer block upper bound) and `ADMA_TABLE_SZ` (descriptor table size, see `include/sdhci.h`). ★ It and `BR_EMMC_MAX_RW_SECTORS` in this plugin's `glue/emmc_internal.h` are two names for **the same number** at the **same source of truth** (see the comment there) |
 * | `CONFIG_LOGLEVEL` | 4 | **Value type**. `CONFIG_VAL(LOGLEVEL)` at `mmc.c:175` expands to the bare identifier `CONFIG_LOGLEVEL`, so not defining it ⇒ `-Wundef` error. Value 4 = `LOGL_WARNING` (u-boot default stance): ">= LOGL_DEBUG" is false, so upstream's "per-command trace print" block is not built (consistent with "no log spam") |
 * | `CONFIG_FS_FAT_MAX_CLUSTSIZE` | 32768 | **Value type**. It belongs to the **interface surface**, not this artifact's features: upstream `include/part.h` (real shape) depends on it indirectly through `include/fat.h`, and `part.h` is the common interface surface of all block devices. This artifact does not build FAT, but completes the value-type configs the interface surface depends on, same source as `adapter/uboot-fat` (also 32768) |
 *
 * ## Deliberately **not defined** (written down so nobody "switches them on casually" next time)
 *
 * `DM_MMC`/`DM_REGULATOR`/`DM_GPIO`/`BLK`/`DM`/`CYCLIC`/`MMC_TINY`/`MMC_UHS_SUPPORT`/
 * `MMC_HS200_SUPPORT`/`MMC_HS400_SUPPORT`/`MMC_HS400_ES_SUPPORT`/`MMC_SUPPORTS_TUNING`/
 * `MMC_HW_PARTITIONING`/`MMC_IO_VOLTAGE`/`MMC_SDHCI_SDMA`/`MMC_SDHCI_ADMA_64BIT`/
 * `MMC_SDHCI_IO_ACCESSORS`/`MMC_PWRSEQ`/`MMC_SPEED_MODE_SET`/`MMC_VERBOSE`/`MMC_TRACE`/
 * `XPL_BUILD`/`SYS_64BIT_LBA`/`PARTITION_UUIDS`/`FIXED_SDHCI_ALIGNED_BUFFER`.
 * The first four are "shape" switches: turning them on pushes upstream toward DM/udevice or uclass,
 * while this artifact follows ADR-0026's "compile upstream as-is" using u-boot's **legacy(!DM_MMC)**
 * path (`struct mmc_config::ops` + `add_sdhci()`), a shape u-boot itself supports (see task doc §2.1).
 * `MMC_SDHCI_SDMA` is off because this artifact only does ADMA2/PIO; `MMC_SDHCI_ADMA_64BIT` off keeps
 * `struct sdhci_adma_desc` at 8 bytes (32-bit addresses, sufficient under identity mapping).
 *
 * Criterion: the "enabled" side of the matrix corresponds one-to-one with `brick.toml`'s `sources`;
 * every entry on the "disabled" side has its reason written in the table above.
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_MMC_GENERATED_AUTOCONF_H
#define BR_ADAPTER_UBOOT_MMC_GENERATED_AUTOCONF_H

/* ---- enabled (only the "= 1" ones are listed; anything absent = disabled) ---- */
#define CONFIG_MMC_WRITE                 1
#define CONFIG_MMC_SDHCI_ADMA            1
#define CONFIG_MMC_SDHCI_ADMA_HELPERS    1
#define CONFIG_MMC_QUIRKS                1

/* ---- value type (must exist; see the matrix table at the top of the file) ---- */
#define CONFIG_SYS_MMC_MAX_BLK_COUNT     64
#define CONFIG_LOGLEVEL                  4
#define CONFIG_FS_FAT_MAX_CLUSTSIZE      32768

#endif /* BR_ADAPTER_UBOOT_MMC_GENERATED_AUTOCONF_H */
