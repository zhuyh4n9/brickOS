/*
 * provider/core — the core layer's single outward door (ADR-0038).
 *
 * This skin re-exports, one unit per header, the whole public surface of `core/c_core`:
 *   log console mem mm sched sync work irq pic fault exc time trace svc plugin types main error errno version
 *
 * A consumer declares `[[dep]] provider/core` and includes this header (or a specific
 * `<br/core/…>`, which this header pulls in). The **specification** of the core layer -- naming
 * (`br_` prefix), the negative-errno convention, "a header is the only declaration surface",
 * "one header = one unit", "no global variables", "no `__`-prefixed identifiers" -- is in
 * `provider/core/README.md`; each rule names the brickie code that enforces it.
 *
 * The declarations below are the providers'; a skin adds no implementation and renames nothing.
 */
#ifndef BR_RUNTIME_CORE_CORE_H
#define BR_RUNTIME_CORE_CORE_H

/* Lifecycle hooks (referenced by the generated build/gen/provider/core/plugin_desc.c). */
int runtime_core_early_init(void);
int runtime_core_init(void);
int runtime_core_start(void);

/* ---- the 20 units ---- */
#include <br/core/br_types.h>
#include <br/core/br_error.h>
#include <br/core/br_errno.h>
#include <br/core/br_version.h>
#include <br/core/br_log.h>
#include <br/core/br_console.h>
#include <br/core/br_main.h>
#include <br/core/br_time.h>
#include <br/hal/br_mem.h>
#include <br/hal/br_mm.h>
#include <br/hal/br_irq.h>
#include <br/hal/br_pic.h>
#include <br/hal/br_fault.h>
#include <br/hal/br_exc.h>
#include <br/hal/br_sched.h>
#include <br/hal/br_sync.h>
#include <br/hal/br_work.h>
#include <br/core/br_trace.h>
#include <br/core/br_svc.h>
#include <br/core/br_plugin.h>

#endif /* BR_RUNTIME_CORE_CORE_H */
