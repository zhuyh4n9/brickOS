/* adapter/uboot-mmc — shim for `<mmc.h>` (→ **the vendor original**).
 *
 * ★ Why forward to the vendor instead of writing an "interface type shim": `struct mmc` is
 *   the upstream protocol core's **own state** (~30 fields, trimmed by 6 config switches);
 *   upstream `mmc.c` and `sdhci.c` read and write it directly — it is not an interface
 *   between us and upstream but an upstream **internal data type**. Copying it means copying
 *   upstream (and syncing it with the config matrix). ⇒ Use the vendor copy byte for byte.
 *
 * ★ It includes `<linux/bitops.h> <linux/list.h> <linux/dma-direction.h> <cyclic.h>
 *   <part.h>` itself — each is satisfied by a same-named shim in `compat/`, and
 *   `linux/list.h` forwards to the vendor.
 *
 * ★ Dependency direction: this file belongs to `compat/` (it serves upstream), yet it
 *   includes an upstream header — that is allowed (`compat/`'s job is to connect upstream
 *   originals to our environment). The criterion is that no direct form naming the
 *   third_party tree path between angle brackets or quotes appears (gate
 *   `check-third-party-pristine.sh` criterion 5); this plugin always uses relative-path
 *   forwarding shims, as in the uboot-erofs compat `u-boot/lz4.h`.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_MMC_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_MMC_H
#include "../../../third_party/uboot-imx/include/mmc.h"
#endif
