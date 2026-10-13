/* fs/uboot-fat - forwarding shim for <fat.h>: the on-disk FAT structures come from the
 * vendor original (hash-pinned in third_party/upstream.lock), not a local copy.
 * fat_env.h must come first: vendor fat.h pulls <fs.h>/<asm/byteorder.h>, whose FAT2CPU16/32
 * and __u16 come from this environment. Relative include path on purpose -- brickie sorts -I
 * flags lexicographically, so a vendor -I would shadow this directory's same-named shims.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_FAT_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_FAT_H
#include "fat_env.h"
#include "../../../third_party/uboot-imx/include/fat.h"
#endif
