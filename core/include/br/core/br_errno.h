/*
 * brickOS — **forwarding header** for POSIX errno names (legacy entry point; the truth now
 * lives in `br_error.h`)
 *
 * ============================== Why this file still exists ==============================
 * ADR-0023 once kept the "POSIX names" here (an alias table of `#define EPERM BR_EPERM`),
 * with the **values** in `br_error.h`. ADR-0027 merged the two:
 *
 *   - Names and values now live **together in `br_error.h`**, and the names are the
 *     **original POSIX names** (`EPERM`/`EIO`/…), **without a `BR_` prefix** — therefore:
 *       1) bare `-EIO`/`-EFSCORRUPTED` in third-party ports **hold as-is** (no mapping table
 *          needed);
 *       2) "errno names" no longer mix with this project's own symbol prefixes (design §11).
 *   - This file is kept as a **compatibility entry point**: existing code that includes
 *     `<br/core/br_errno.h>` needs no change.
 *
 * ★ Single source of truth: this file **defines no names** (forwarding only). Criterion in
 *   `tools/check-errno.sh`.
 */
#ifndef BR_CORE_BR_ERRNO_H
#define BR_CORE_BR_ERRNO_H

#include <br/core/br_error.h>

#endif /* BR_CORE_BR_ERRNO_H */
