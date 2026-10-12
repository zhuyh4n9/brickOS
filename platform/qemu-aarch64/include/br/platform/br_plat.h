/*
 * brickOS prototype v0.1.0 — Platform Entry contract (aarch64)
 *
 * ============================ WORKAROUND notice ============================
 * This header is the **public face of the Platform plugin `platform/qemu-aarch64`**
 * (design side 1-01 §6.3 / §8 three-layer pattern: one platform plugin per SoC,
 * providing reset assembly, early console, clock, region table, interrupt controller
 * implementation).
 *
 * Repayment progress (`repaid (ADR-0005)`; that ledger entry was retired when the
 * plugin manager landed):
 *   (1) `brickie` can now discover/validate plugin layout (design 4-02) and generate
 *       descriptors (4-03) — **repaid**;
 *   (2) this directory has converged into the plugin `platform/qemu-aarch64`
 *       (manifest = `brick.toml`, `BR_PLUGIN` descriptor = generated
 *       `build/gen/platform/qemu-aarch64/plugin_desc.c`) — **repaid**;
 *   (3) **still owed**: Start.S's call site is not yet driven by enumeration of the
 *       `.br_plugins` section (design 3-05 §2.2's
 *       __br_plugins_start/__br_plugins_stop) — that belongs to M0 runtime (once the
 *       plugin manager + scheduler are in place); it is still compiled directly by
 *       the Makefile.
 * See WORKAROUNDS.md and docs/decisions/0001-platform-plugin-manifest.md.
 * ========================================================================
 *
 * Layering (where design 1-01 §8 "platform capability three-layer pattern" lands in
 * this prototype):
 *   core interface      -> platform/qemu-aarch64/include/br/platform/br_plat.h (this file)
 *   ISA-shared impl     -> read arch timer directly with inline asm (src/timer_arch.c)
 *   platform-provided data -> PL011 base / baud rate / clock frequency / region table (QEMU virt values)
 */
#ifndef BR_PLATFORM_BR_PLAT_H
#define BR_PLATFORM_BR_PLAT_H

#include <br/core/br_types.h>

/*
 * Platform early initialization. **Called by the platform plugin's `early_init`
 * hook**, which is **stage (2)** of the core boot chain `br_core_main()` (see
 * `core/include/br/core/br_main.h` / ADR-0008):
 *
 *   reset/BSS (asm) → core platform-independent init → **[this function]** → core heap/address mapping → plugin phases
 *
 * This function does only the three **platform-side** things (in this order):
 *   1. early console ready (polled PL011) — must be first, observability after it;
 *   2. the platform-side three steps of the interrupt subsystem (PIC register / caps /
 *      binding table, `br_plat_irq_init`);
 *   3. memory mapping **data and mechanism**: declare the region table + register the
 *      page-table construction ops.
 *
 * ★ Pool claiming (`br_mem_init`) and page-table **activation** (`br_mm_activate`)
 *   belong to core's stage (3) — platform only provides "which memory is what" and
 *   "how page tables are built", core performs the actual setup (ADR-0008).
 *   Once built, the v1 policy "MMU used for region attributes" takes effect, and
 *   `-mstrict-align` degrades from a "hard requirement" to a "defensive knob" (see
 *   the Makefile comment).
 */
void br_plat_early_init(void);

/* Platform identity, used for the boot banner and diagnostics (design side: the platform plugin's name). */
const char *br_plat_name(void);
const char *br_plat_isa(void);

/* ---- arch timer (generic aarch64, provided by platform) ----
 * Why platform and not core: design 1-01 §8 table — `br_clock` is core's tickless
 * semantic interface, while "which counter to read, at what frequency" belongs to
 * platform (ISA-shared library + platform data). */
br_u64 br_plat_ticks_freq(void);   /* CNTFRQ_EL0, Hz */
br_u64 br_plat_ticks_now(void);    /* CNTPCT_EL0, monotonically increasing count */

/* ---- interrupt subsystem (design 3-02 §14.3's one-way init chain) ----
 *
 * Steps 1–3 inside `br_plat_early_init()` (platform side):
 *   br_pic_register(driver) → caps_get → br_irq_bindings_set(binding table)
 * Any step failing ⇒ negative errno and **no degradation** (an EARLY-phase failure =
 * boot failure: interrupts are globally off here, and "silently return 0 but not
 * configured" would put symptoms far from the root cause, 3-02 §14.3 error-handling
 * obligation).
 *
 * Why these belong to platform: they deliver **data and specialization** (PIC
 * instance/base/priority bits, virq↔hwirq bindings, board-level static
 * prio&trigger), while mechanism and semantics live in core (3-02 §1.3).
 */
int br_plat_irq_init(void);

/*
 * EL1 physical timer (PPI INTID 30) as M0's **actual interrupt source** (3-02 §11.1
 * path four).
 *
 * ★ `br_plat_irq_start()` is the single entry for "start devices and interrupts
 *   together": register the timer ISR → enable that line → arm the first deadline →
 *   **globally enable interrupts** (the last step of the init chain in design §14.3).
 *   Why these are in platform and not in the APP: design 3-01 §13.6 assigns
 *   "interrupt control" to **P3** and "IRQ lines" to P1, while the APP is **P0** ⇒ the
 *   APP must not touch `br_irq_register/enable`.
 *   Returns 0 or negative errno (failure = boot failure: no silent degradation).
 */
int br_plat_irq_start(void);

/* Heartbeat count (maintained by the timer PPI ISR).
 *
 * ★ **Single source of truth is in core**: the platform ISR calls
 *   `br_clock_tick_notify()` every tick, the count is held by core, so this function
 *   is now only a forwarding port that "reads core's same count". Consumers
 *   (especially the P0 APP) should read `br_clock_tick_count()`
 *   (`<br/core/br_time.h>`) — an APP reading platform directly would leave an
 *   `app → platform` edge in the declared surface, and that M0 bootstrap exception
 *   has been repaid (see `docs/decisions/0016-core-timer-heartbeat.md`).
 *   This function is kept as a **platform-side access port**: its interface surface
 *   is unchanged (removal must follow the §6.3 deprecation cycle). */
br_u32 br_plat_timer_ticks(void);

/* Stop the timer (diagnostics / future PM). */
void br_plat_timer_stop(void);

/*
 * Interrupt consistency case entry (design 6-01 §3.7's TC-IRQ-*, run on QEMU).
 * Returns the **number of failures** (0 = all green) and prints each item's verdict
 * as a `[IRQCONF] PASS/FAIL <case name>` line — so red/green can be judged by
 * `make smoke` / CI grep, not by eye.
 */
int br_plat_irq_conformance(void);

/*
 * Software-trigger a virq "bound as SGI" (self-test/consistency case channel, design
 * 3-02 §8.4).
 * Goes through the **exact same entry path** (ack → ISR → eoi), so it is the
 * interrupt subsystem's self-check channel.
 * Returns 0 or negative errno (not bound / not an SGI / controller lacks software
 * trigger).
 */
int br_plat_irq_trigger(br_u32 virq);

/*
 * virq → hwirq in the binding table (the **static** mapping committed in the EARLY
 * phase).
 * Restricted use: only conformance/debug may do the "shadow vs hardware" cross-check
 * (design 3-02 §4.3 item 5: a read-back-capable controller may be cross-checked in
 * conformance).
 * Returns 0 or -ENODEV.
 */
int br_plat_irq_hwirq(br_u32 virq, br_u32 *out);

/* ---- memory mapping subsystem (design 3-04; page tables/region table/three pools) ----
 *
 * The platform side **provides only two things** (see the `br_plat_early_init`
 * comment and ADR-0008):
 *   (1) data: declare the region table (`br_mm_region_add`: image/stack/three
 *       pools/MMIO/reserved);
 *   (2) mechanism: `br_mm_register(ops)` registers page-table construction.
 * The core side performs two things (core boot chain stage (3), `core/src/main.c`):
 *   (3) `br_mem_init()` (core claims pools by region kind);
 *   (4) `br_mm_activate()` (core performs setup: build a 4 KiB identity mapping from
 *       the same region table and enable the MMU).
 *
 * `br_plat_mem_conformance()` = memory/MMU consistency case entry (design 6-01
 * §3.5/§3.6's `TC-MEM-*` and `TC-MM-*`, run on QEMU). Returns the **number of
 * failures** (0 = all green), prints a
 * `[MEMCONF] PASS/FAIL <case name> <description>` line per item, and at the end
 * `[MEMCONF] SUMMARY pass=N fail=0 total=N` — so red/green is judged by
 * `make dbg-test` / CI grep, not by eye.
 */
int br_plat_mem_conformance(void);

/* Halt (never returns). There is no scheduler, so there is no "idle thread", only a WFE spin loop. */
BR_NORETURN void br_plat_park_forever(void);

/*
 * Unhandled exceptions from the vector table all land here (design side: 3-02 §8.3.1's
 * bare path: do not touch possibly-corrupt core facilities, just poll the console for
 * one line and halt).
 * `vector` = exception vector index 0..15. Does not return.
 */
BR_NORETURN void br_plat_unexpected_exception(br_u64 vector);

#endif /* BR_PLATFORM_BR_PLAT_H */
