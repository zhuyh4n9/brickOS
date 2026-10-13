/* driver/uboot-mmc -- the u-boot configuration matrix; `include/linux/kconfig.h` includes it
 * first, and it fixes the shape of the upstream `drivers/mmc/` sources.
 * `#ifdef` tests definedness, not value:
 *   - enable: `#define CONFIG_X 1` (`CONFIG_IS_ENABLED` pastes and checks for 1);
 *   - disable: define nothing. **Never** `#define CONFIG_DM_MMC 0` -- `#ifdef CONFIG_DM_MMC` then
 *     becomes true and upstream takes the DM (udevice) shape;
 *   - value-type configs (numbers) must be defined: they appear as bare identifiers, so omitting
 *     them fails under `-Wundef -Werror`.
 *
 * Enabled (matches brick.toml `sources`): `MMC_WRITE` (mmc_write.c; adds `write_bl_len`/
 * `erase_grp_size`/`ssr` to `struct mmc`), `MMC_SDHCI_ADMA` + `MMC_SDHCI_ADMA_HELPERS` (ADMA2
 * path, `adma_desc_table`, `sdhci-adma.c` built), `MMC_QUIRKS` (`mmc_get_op_cond()` sets the retry
 * bits QEMU's SDHCI model relies on). Value type: `CONFIG_SYS_MMC_MAX_BLK_COUNT` (per-transfer
 * limit and `ADMA_TABLE_SZ`), `CONFIG_LOGLEVEL` = 4 = LOGL_WARNING (u-boot default; `< LOGL_DEBUG`
 * cuts the per-command trace and `mmc_mode_name()`), `CONFIG_FS_FAT_MAX_CLUSTSIZE`
 * (block-interface surface, as uboot-fat).
 *
 * Deliberately not defined: `DM_MMC`/`DM_REGULATOR`/`DM_GPIO`/`BLK`/`DM`/`CYCLIC`/`MMC_TINY`/
 * `MMC_UHS_SUPPORT`/`MMC_HS200_SUPPORT`/`MMC_HS400_SUPPORT`/`MMC_HS400_ES_SUPPORT`/
 * `MMC_SUPPORTS_TUNING`/`MMC_HW_PARTITIONING`/`MMC_IO_VOLTAGE`/`MMC_SDHCI_SDMA`/
 * `MMC_SDHCI_ADMA_64BIT` (keeps the descriptor at 8 bytes)/`MMC_SDHCI_IO_ACCESSORS`/`MMC_PWRSEQ`/
 * `MMC_SPEED_MODE_SET`/`MMC_VERBOSE`/`MMC_TRACE`/`XPL_BUILD`/`SYS_64BIT_LBA`/`PARTITION_UUIDS`/
 * `FIXED_SDHCI_ALIGNED_BUFFER` -- the shape switches are off so upstream compiles on its legacy
 * (`!DM_MMC`) path.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_GENERATED_AUTOCONF_H
#define BR_ADAPTER_UBOOT_MMC_GENERATED_AUTOCONF_H

/* enabled; anything absent = disabled */
#define CONFIG_MMC_WRITE                 1
#define CONFIG_MMC_SDHCI_ADMA            1
#define CONFIG_MMC_SDHCI_ADMA_HELPERS    1
#define CONFIG_MMC_QUIRKS                1

/* value type (must exist) */
#define CONFIG_SYS_MMC_MAX_BLK_COUNT     64
#define CONFIG_LOGLEVEL                  4
#define CONFIG_FS_FAT_MAX_CLUSTSIZE      32768

#endif /* BR_ADAPTER_UBOOT_MMC_GENERATED_AUTOCONF_H */
