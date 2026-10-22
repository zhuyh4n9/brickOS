/*
 * Platform Entry contract (aarch64) - public face of the platform plugin runtime/qemu-aarch64.
 * Layering: core interface here, arch timer read directly in src/timer_arch.c, platform data
 * (PL011 base/baud/clock, region table) in the sources.
 * Still owed (see WORKAROUNDS.md): Start.S's call site is not yet driven by enumeration of the
 * .br_plugins section; the Makefile compiles it directly.
 */
#ifndef BR_PLATFORM_BR_PLAT_H
#define BR_PLATFORM_BR_PLAT_H

#include <br/core/br_types.h>

/*
 * Platform early initialization, called by the plugin's early_init hook (stage 2 of the core boot
 * chain br_core_main()). Does three platform-side things in order: early polled console ready (must
 * be first, for observability); PIC register / caps / binding table; memory mapping data and
 * mechanism (region table + page-table ops). Pool claiming and MMU activation are core stage 3.
 */
void br_plat_early_init(void);

/* Platform identity, used for the boot banner and diagnostics. */
const char *br_plat_name(void);
const char *br_plat_isa(void);

/* ---- arch timer (provided by platform) ---- */
br_u64 br_plat_ticks_freq(void);   /* CNTFRQ_EL0, Hz */
br_u64 br_plat_ticks_now(void);    /* CNTPCT_EL0, monotonically increasing count */

/* ---- interrupt subsystem ----
 * br_plat_early_init does br_pic_register(driver) -> caps_get -> br_irq_bindings_set(table).
 * Any step failing returns a negative errno with no degradation: an EARLY-phase failure while
 * interrupts are globally off is a boot failure, not a silently unconfigured subsystem. */
int br_plat_irq_init(void);

/*
 * EL1 physical timer (PPI INTID 30) as M0's interrupt source. br_plat_irq_start() is the single
 * entry for "start devices and interrupts together": register timer ISR -> enable that line -> arm
 * the first deadline -> globally enable interrupts. Returns negative errno on failure.
 */
int br_plat_irq_start(void);

/* Heartbeat count, maintained by the timer PPI ISR; core holds the same count, so prefer
 * br_clock_tick_count() (<br/core/br_time.h>). Kept as a platform-side access port. */
br_u32 br_plat_timer_ticks(void);

/* Stop the timer (diagnostics / future PM). */
void br_plat_timer_stop(void);

/* Interrupt consistency case entry (TC-IRQ-*). Returns the number of failures (0 = all green) and
 * prints a "[IRQCONF] PASS/FAIL <case name>" line per item. */
int br_plat_irq_conformance(void);

/* Software-trigger a virq bound as an SGI; same entry path (ack -> ISR -> eoi). Returns 0 or
 * negative errno (not bound / not an SGI / controller lacks software trigger). */
int br_plat_irq_trigger(br_u32 virq);

/* virq -> hwirq in the static EARLY-phase binding table; conformance/debug only (shadow vs hardware
 * cross-check). Returns 0 or -ENODEV. */
int br_plat_irq_hwirq(br_u32 virq, br_u32 *out);

/* ---- memory mapping subsystem ----
 * Platform provides data (region table: image/stack/pools/MMIO/reserved) and mechanism
 * (br_mm_register of page-table ops); core claims pools and activates the MMU.
 * br_plat_mem_conformance() = consistency case entry (TC-MEM-* and TC-MM-*): returns the number of
 * failures, printing "[MEMCONF] PASS/FAIL <case name> <description>" lines and a final SUMMARY. */
int br_plat_mem_conformance(void);

/* Halt (never returns). No scheduler, so no "idle thread": only a WFE spin loop. */
BR_NORETURN void br_plat_park_forever(void);

/* Unhandled exceptions from the vector table land here; `vector` = exception vector index 0..15.
 * Bare path: touch no possibly-corrupt core facility; print one line and halt. Does not return. */
BR_NORETURN void br_plat_unexpected_exception(br_u64 vector);

#endif /* BR_PLATFORM_BR_PLAT_H */
