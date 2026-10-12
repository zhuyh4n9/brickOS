/*
 * brickOS prototype v0.1.0 — interrupt subsystem conformance cases (in-image)
 *
 * Design basis: `docs/6-test/6-01-test.md` §3.7 (TC-IRQ group, target-only) and
 * `docs/3-os-core/3-02-int.md` §17.1/§17.2 (case ↔ design point mapping).
 *
 * Why **in-image** rather than host cases:
 *   design 6-01 §3.7 already fixed "interrupt group = target-only"; and the Stage 1
 *   interrupt path (exception vector stub → ack → dispatch → ISR → eoi → ERET) does not
 *   exist on host at all.
 *   So this file compiles the cases into the image, driven by `qemu_aarch64_selftest()`
 *   (src/selftest.c) after **all starts** via the core self-test pass (ADR-0010; this file
 *   used to be called from the platform start),
 *   each case prints one line `[IRQCONF] PASS/FAIL <case id> <detail>`, ending with
 *   `[IRQCONF] SUMMARY pass=N fail=M` —— so red/green can be grepped by `make smoke` /
 *   `make irq-test`, not judged by eye.
 *
 * This file is a **platform plugin** (same layer as GICv3/binding table): it needs ISA-level
 * means (software trigger SGI, hardware readback) to build scenarios; the cases themselves
 * call only core native APIs.
 *
 * Coverage (aligned with 3-02 §17.1 "existing 7 cases" + §17.2 "suggested additions"):
 *   TC-IRQ-001 register(SGI)→enable→self-trigger; arg delivery; ack/eoi pairing (INV-H)
 *   TC-IRQ-002 duplicate register → -EBUSY (INV-A)
 *   TC-IRQ-003/013 trigger while disabled does not arrive; enable catches up (CAP_LATCH); idempotent enable
 *   TC-IRQ-004 lock nesting: inner unlock keeps IRQs off; st bit layout (IR-15)
 *   TC-IRQ-008 unbound hwirq → spurious_owned, no panic, no ISR
 *   TC-IRQ-010 extable hit: illegal access degrades to -EFAULT
 *   TC-IRQ-012/021 storm protection: self-retrigger in ISR (software livelock equivalent) ⇒ windowed criterion detects it
 *   TC-IRQ-014 priority quantization: saturate toward lower priority + descriptor keeps logical value + trace (IR-7)
 *   TC-IRQ-016 extra unlock caught by the underflow guard, IRQs still recover
 *   TC-IRQ-018 direct API on a domain member → -EINVAL, parent line not wrongly masked
 *   TC-IRQ-019 lowest-priority line can still be delivered (PMR init duty, §7.1)
 *   TC-IRQ-022 after an extable hit in_fault is reset (second fault still takes the fixup path)
 *   TC-IRQ-101 FAST cascaded domain: dispatch/per-child ack/orphan child masked
 *   TC-IRQ-102 SLOW cascaded domain: demux in **bh** (status register never read in ISR) ——
 *              ★ before ADR-0011 this asserted "SLOW explicitly rejected"; after bh landed it flipped to "usable and really in bh"
 *   TC-IRQ-015 **BH direct line** (binding table declares DISPATCH_BH): handler runs in bh context,
 *              same line masked during handling, counts/trace observable
 *   TC-IRQ-016 thread-only API called in bh ⇒ -EINVAL (runtime enforcement of atomic context)
 *   TC-IRQ-017 §11.4.1: disabled before dispatch ⇒ handler suppressed (work item still consumed), enable does not replay
 *   GIC-*      dialect facts: EOImode/PRIbits/SPI line count/binding table (platform data) check
 */
#include <br/core/br_irq.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>
#include <br/core/br_fault.h>
#include <br/core/br_error.h>
#include <br/core/br_log.h>
/* Bottom half (ADR-0011): BH cases must drive a drain explicitly and assert the handler really runs in bh context. */
#include <br/core/br_work.h>

#include <br/platform/br_plat.h>
#include <br/platform/br_gicv3.h>
#include <br/board_irq.h>

/* The domain window must fit one 32-bit pending word (v0.1 static bitmap pool sized for this) */
_Static_assert(BR_IRQ_DOMAIN_WINDOW <= 32u, "domain window exceeds single-word bitmap capacity");

/*
 * "Deterministic fault" address for the extable negative control: 0x42000000 lies inside the RAM
 * section (0x40000000, -m 128M) but outside the page-table RAM mapping window
 * (0x40000000 + 8 MiB = 0x40800000) ⇒ invalid page table entry ⇒ translation fault.
 * Why not an address like 0x1000 that "looks unmapped": QEMU virt implements the low range as
 * "unassigned/flash returning 0", which never faults (measured once, P-IRQ-16).
 * This literal equals BR_PLAT_CONF_UNMAPPED in br_mmu.h; irq_conf.c does not depend on the MMU
 * header, so it carries its own copy here with provenance (change both when moving window bounds).
 */
#define CONF_UNMAPPED_ADDR   0x42000000ul

/* =====================================================================
 * Assertions and stats
 * ===================================================================== */
static br_u32 s_pass;
static br_u32 s_fail;

static void conf_report(br_bool ok, const char *tag, const char *what)
{
    if (ok) {
        s_pass++;
        br_log_info("[IRQCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[IRQCONF] FAIL %s %s", tag, what);
    }
}

/* Observe trace events (accumulate an id bitmap; drain the ring on each poll) */
static br_u32 s_trace_seen;

static void conf_trace_poll(void)
{
    br_trace_evt_t ev[16];
    br_u32 n;

    while ((n = br_trace_drain(ev, 16u)) > 0u) {
        for (br_u32 i = 0; i < n; i++) {
            if (ev[i].id < 32u) {
                s_trace_seen |= (1u << ev[i].id);
            }
        }
    }
}

static br_bool conf_trace_has(br_u32 id)
{
    conf_trace_poll();
    return (s_trace_seen & (1u << id)) != 0u;
}

/* Bounded wait: IRQs are delivered normally meanwhile (criterion for "the ISR really ran", no fixed sleep) */
static int conf_wait_count(volatile br_u32 *counter, br_u32 target, br_time_t timeout_us)
{
    const br_time_t t0 = br_clock_now();

    while (*counter < target) {
        if ((br_clock_now() - t0) > timeout_us) {
            return BR_ERR(ETIMEDOUT);
        }
    }
    return 0;
}

/* Pure busy wait (no assert); IRQs are delivered normally — for the negative "must not deliver while masked" criterion */
static void conf_idle_us(br_time_t us)
{
    const br_time_t t0 = br_clock_now();

    while ((br_clock_now() - t0) < us) {
        /* busy wait */
    }
}

/* =====================================================================
 * ISRs and test "fake devices" (FAST domain dialect)
 * ===================================================================== */
static volatile br_u32 s_test_isr_count;
static volatile br_u32 s_test_isr_arg;

static void conf_test_isr(void *arg)
{
    s_test_isr_count++;
    s_test_isr_arg = (br_u32)(br_uintptr_t)arg;
}

/* Storm case: self-retrigger inside the ISR ⇒ software equivalent of a level livelock (3-02 §8.3).
 * Why it can be built this way: SGI is **edge** and the GIC latches pending —— writing
 * ICC_SGI1R before this Active is eoi'd makes the same line pending again immediately. */
static volatile br_u32 s_storm_isr_count;
static volatile br_u32 s_storm_left;

static void conf_storm_isr(void *arg)
{
    (void)arg;
    s_storm_isr_count++;

    if (s_storm_left > 0u) {
        s_storm_left--;
        (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI_STORM);
    }
}

/* FAST domain "fake pin controller": state bitmap in RAM, so demux is purely in-ISR (§9.3) */
static volatile br_u32 s_fake_pending;
static volatile br_u32 s_fake_masked;
static volatile br_u32 s_fake_ack_count;
static volatile br_u32 s_child_count[3];

static void fake_pending(void *priv, br_u32 *bits, br_size_t nwords)
{
    (void)priv;
    for (br_size_t i = 0; i < nwords; i++) {
        bits[i] = 0u;
    }
    /* Masked state does not appear in the pending snapshot (the dialect's own shadow) */
    bits[0] = s_fake_pending & ~s_fake_masked;
}

static void fake_mask(void *priv, br_u32 sub)
{
    (void)priv;
    s_fake_masked |= (1u << sub);
}

static void fake_unmask(void *priv, br_u32 sub)
{
    (void)priv;
    s_fake_masked &= ~(1u << sub);
}

static void fake_ack(void *priv, br_u32 sub)
{
    (void)priv;
    s_fake_ack_count++;
    s_fake_pending &= ~(1u << sub);
}

static void conf_child0_isr(void *arg) { (void)arg; s_child_count[0]++; }
static void conf_child2_isr(void *arg) { (void)arg; s_child_count[2]++; }

static const br_irq_domain_ops_t s_fake_ops = {
    .pending = fake_pending,
    .mask    = fake_mask,
    .unmask  = fake_unmask,
    .ack     = fake_ack,
};

static br_irq_domain_t *s_dom;

/* =====================================================================
 * extable probe (3-02 §10.4)
 *
 * ★ The **only source** of the return value is the register the core writes into the exception
 *   frame per the entry's `reg`/`errno_val` (arm64 x0); the fixup label itself only branches and
 *   never fabricates an error code —— otherwise this case would not verify the design conclusion
 *   "entries must carry reg + errno_val" (§10.4 pitfall).
 * ===================================================================== */
static int conf_probe_read32(br_uintptr_t addr, br_u32 *out)
{
    register br_u32       val __asm__("x1");
    register br_s32       ret __asm__("x0");
    register br_uintptr_t a   __asm__("x2");

    a = addr;
    __asm__ volatile(
        ".Lconf_insn" "%=:\n\t"
        "   ldr %w[val], [%[addr]]\n\t"
        "   mov %w[ret], wzr\n\t"
        "   b   .Lconf_done" "%=\n\t"
        ".Lconf_fix" "%=:\n\t"
        "   b   .Lconf_done" "%=\n\t"
        ".Lconf_done" "%=:\n\t"
        BR_EXTABLE_ENTRY(.Lconf_insn%=, .Lconf_fix%=, 0, -14)
        : [val] "=&r"(val), [ret] "=&r"(ret)
        : [addr] "r"(a)
        : "memory");

    if (ret == 0 && out != BR_NULL) {
        *out = val;
    }
    return ret;
}

/* =====================================================================
 * Cases
 * ===================================================================== */
static void conf_gicv3_facts(void)
{
    br_gicv3_info_t info;
    br_gicv3_info(&info);

    conf_report(info.eoimode == 0u, "GIC-EOIMODE",
                "EOImode == 0 (single eoi is complete; hard precondition of 3-02 §4.1.1)");
    conf_report(info.prio_bits >= 5u && info.prio_bits <= 8u, "GIC-PRIBITS",
                "ICC_CTLR_EL1.PRIbits decodes to a valid priority bit count");
    conf_report(info.spi_lines >= 32u, "GIC-LINES",
                "GICD_TYPER reports SPI line count (platform data check)");

    br_u32 hwirq = 0u;
    const int r = br_plat_irq_hwirq(BR_IRQ_TIMER, &hwirq);
    conf_report(r == 0 && hwirq == 30u, "GIC-TIMERID",
                "BR_IRQ_TIMER bound to INTID 30 (EL1 physical timer PPI)");
}

static void conf_extable(void)
{
    static const br_u32 probe_src = 0x5A5A1234u;
    br_u32 v = 0u;

    const int r1 = conf_probe_read32((br_uintptr_t)&probe_src, &v);
    br_log_info("[IRQCONF] info probe(valid) ret=%d val=0x%x", r1, v);
    conf_report(r1 == 0 && v == probe_src, "TC-IRQ-010",
                "extable positive control: legal address reads the true value");

    /*
     * ★ How to produce a "deterministically recoverable memory fault" (P-IRQ-16; changed once
     *   along with the memory mapping subsystem):
     *   the old trick was an **unaligned access** with MMU off (all accesses as Device-nGnRnE
     *   ⇒ Alignment fault, EC=0x25/DFSC=0x21). After the platform memory mapping subsystem
     *   landed, MMU is on and SCTLR_EL1 A/SA/SA0 are explicitly cleared (see the MMU_SCTLR_A
     *   comment in mmu.c: field numbers corrected per ARMv8) ⇒ an unaligned access to Normal
     *   memory **no longer** faults (`-mstrict-align` thus degraded from a hard requirement to a
     *   defensive knob, see the Makefile comment).
     *   Now it uses a **truly unmapped address**: CONF_UNMAPPED_ADDR lies outside the page-table
     *   RAM mapping window (0x40000000 + 8 MiB), its L1/L2 entry is invalid ⇒ **Translation
     *   fault** (EC=0x25/DFSC=0x04..0x07 class), as deterministic as the old trick and likewise
     *   fixed by extable.
     */
    v = 0u;
    const int r2 = conf_probe_read32(CONF_UNMAPPED_ADDR, &v);
    br_log_info("[IRQCONF] info probe(unmapped=0x%lx) ret=%d", (br_u64)CONF_UNMAPPED_ADDR, r2);
    conf_report(r2 == BR_ERR(EFAULT), "TC-IRQ-010",
                "extable hit: unmapped address (translation fault) degrades to -EFAULT, system survives");

    /* Second fault: if in_fault were not reset, this one would be treated as a double fault (bare panic) */
    v = 0u;
    const int r3 = conf_probe_read32(CONF_UNMAPPED_ADDR, &v);
    conf_report(r3 == BR_ERR(EFAULT), "TC-IRQ-022",
                "after a fixup in_fault is reset (the second fault still takes the fixup path, not a double fault)");

    conf_report(conf_trace_has(BR_TRACE_FAULT_FIXUP), "TC-IRQ-010",
                "fixup trace (TRACE_FAULT_FIXUP)");
}

static void conf_sgi_path(void)
{
    const br_irq_attr_t attr = {
        .prio = BR_IRQ_PRIO_DEFAULT, .trigger = BR_IRQ_TRIG_EDGE_RISE, .flags = 0u
    };

    const int r = br_irq_register(BR_IRQ_TEST_SGI, conf_test_isr,
                                  (void *)(br_uintptr_t)0xC0FFEEu, &attr);
    conf_report(r == 0, "TC-IRQ-001", "register(SGI virq) -> 0");

    conf_report(br_irq_enable(BR_IRQ_TEST_SGI) == 0, "TC-IRQ-001", "enable -> 0");

    const br_u32 before = s_test_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    const br_bool ran = (conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0);
    conf_report(ran && s_test_isr_arg == 0xC0FFEEu, "TC-IRQ-001",
                "ISR ran and arg delivered correctly");

    br_irq_stat_t st;
    br_irq_stats_get(BR_IRQ_TEST_SGI, &st);
    conf_report(st.count == s_test_isr_count, "TC-IRQ-001",
                "ack/eoi exactly once each: descriptor count == ISR count (INV-H)");
}

static void conf_duplicate_register(void)
{
    const br_irq_attr_t attr = { BR_IRQ_PRIO_DEFAULT, BR_IRQ_TRIG_DEFAULT, 0u };
    const int r = br_irq_register(BR_IRQ_TEST_SGI, conf_test_isr, BR_NULL, &attr);

    conf_report(r == BR_ERR(EBUSY), "TC-IRQ-002",
                "duplicate register -> -EBUSY (exactly one owner per physical line, INV-A)");
}

static void conf_priority_readback(void)
{
    /* Use a not-yet-registered line (PL011 UART0 = SPI 33) for a "quantize + hardware readback"
     * cross-check. Requested logical prio = 0x01: GICv3 valid bits are **MSB aligned**
     * (5 bits ⇒ step 8), so it must quantize toward **lower priority** to 0x08 —— a reversed
     * direction would give 0x00 (silent privilege raise, forbidden by IR-7). */
    br_u32 hwirq = 0u;
    if (br_plat_irq_hwirq(BR_IRQ_UART0, &hwirq) != 0) {
        conf_report(BR_FALSE, "TC-IRQ-014", "failed to get UART0 hwirq (binding table entry missing)");
        return;
    }

    /*
     * ★ First handle "lowest realizable level is permanently masked by PMR" (P-IRQ-15):
     *   with 5 priority bits PMR implements only the top 5 (writing 0xFF reads back 0xF8), and the
     *   delivery test is `prio < PMR` ⇒ a line quantized to 0xF8 is **never delivered**; requesting
     *   0xFF must be judged "not expressible" ⇒ `-ENOTSUP` (§4.2/§7.1), **not** silently lowered
     *   to 0xF0 (that is a privilege raise, forbidden by IR-7).
     */
    {
        const br_irq_attr_t too_low = { .prio = BR_IRQ_PRIO_LOWEST,
                                        .trigger = BR_IRQ_TRIG_DEFAULT, .flags = 0u };
        const int rr_low = br_irq_register(BR_IRQ_UART0, conf_test_isr, BR_NULL, &too_low);
        conf_report(rr_low == BR_ERR(ENOTSUP), "TC-IRQ-014",
                    "requested logical lowest (0xFF) lands in the PMR-permanently-masked hardware level -> -ENOTSUP (no silent privilege raise)");
    }

    const br_irq_attr_t attr = { .prio = 0x01u, .trigger = BR_IRQ_TRIG_DEFAULT, .flags = 0u };
    conf_report(br_irq_register(BR_IRQ_UART0, conf_test_isr, BR_NULL, &attr) == 0,
                "TC-IRQ-014", "register(UART0, logical prio 0x01) -> 0");

    br_u8 hw_prio = 0u;
    const int rr = br_gicv3_readback_prio(hwirq, &hw_prio);
    conf_report(rr == 0 && hw_prio == 0x08u, "TC-IRQ-014",
                "hardware priority = 0x08 (saturate toward lower priority, never a silent raise)");

    br_irq_stat_t st;
    br_irq_stats_get(BR_IRQ_UART0, &st);
    conf_report(st.prio == 0x01u, "TC-IRQ-014",
                "descriptor keeps the logical priority (hardware encoding does not leak into core)");

    conf_report(conf_trace_has(BR_TRACE_IRQ_PRIO_QUANTIZED), "TC-IRQ-014",
                "quantization trace (both directions are reported, §7.1)");

    int en = 1;
    (void)br_gicv3_readback_enabled(hwirq, &en);
    conf_report(en == 0, "TC-IRQ-014",
                "after register the hardware is masked (CAP_READBACK cross-checks shadow vs hardware)");
}

static void conf_mask_depth(void)
{
    br_u32 hwirq = 0u;
    (void)br_plat_irq_hwirq(BR_IRQ_TEST_SGI, &hwirq);

    const br_u32 before = s_test_isr_count;
    conf_report(br_irq_disable(BR_IRQ_TEST_SGI) == 0, "TC-IRQ-003", "disable -> 0");

    int en = 1;
    (void)br_gicv3_readback_enabled(hwirq, &en);
    conf_report(en == 0, "TC-IRQ-003", "after disable the hardware is masked (hardware written only on depth 0→1)");

    /* Trigger while masked: hardware pending latches (CAP_LATCH=1), but the ISR must not **start** */
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    conf_idle_us(2000u);
    conf_report(s_test_isr_count == before, "TC-IRQ-003",
                "trigger while masked does not arrive (INV-D: no new handler starts after disable returns)");

    conf_report(br_irq_enable(BR_IRQ_TEST_SGI) == 0, "TC-IRQ-003", "enable -> 0");
    conf_report(conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0,
                "TC-IRQ-003", "after enable the latched pending is caught up once (CAP_LATCH)");

    conf_report(br_irq_enable(BR_IRQ_TEST_SGI) == 0, "TC-IRQ-013",
                "repeat enable in enabled state is idempotent, returns 0 (§6.2)");
    conf_idle_us(1000u);
    conf_report(s_test_isr_count == before + 1u, "TC-IRQ-013",
                "repeat enable produces no extra unmask/delivery");
}

static void conf_lock_nesting(void)
{
    const br_irq_state_t st1 = br_irq_lock();
    const br_irq_state_t st2 = br_irq_lock();

    conf_report((st1 & BR_IRQ_ST_I_MASK) == 0u, "TC-IRQ-004",
                "I bit was open before entry (token bit 0 == 0)");
    conf_report(((st2 >> BR_IRQ_ST_DEPTH_SHIFT) & 0x3FFFu) == 1u, "TC-IRQ-004",
                "depth copy in the token == 1 (IR-15 golden bit layout)");

    br_irq_unlock(st2);   /* inner: never enables IRQs */
    const br_u32 before = s_test_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    conf_idle_us(2000u);
    conf_report(s_test_isr_count == before, "TC-IRQ-004",
                "after the inner unlock IRQs remain masked (no delivery)");

    br_irq_unlock(st1);   /* outermost: restore the I bit from before entry */
    conf_report(conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0,
                "TC-IRQ-004", "delivery resumes after the outermost unlock");
}

static void conf_unlock_underflow(void)
{
    const br_irq_state_t st = br_irq_lock();

    br_irq_unlock(st);   /* correct pairing */
    br_irq_unlock(st);   /* ★ one extra: must be stopped by the underflow guard (else depth wraps ⇒ IRQs off forever) */

    conf_report(conf_trace_has(BR_TRACE_IRQ_UNLOCK_UNDERFLOW), "TC-IRQ-016",
                "the extra unlock is caught by the underflow guard and traced");

    const br_u32 before = s_test_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    conf_report(conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0,
                "TC-IRQ-016", "IRQs still recover after the guard (no permanent IRQ-off)");
}

static void conf_spurious_unbound(void)
{
    br_irq_stat_t st;
    br_irq_stats_get(BR_IRQ_TIMER, &st);
    const br_u32 owned_before = st.spurious_owned;
    const br_u32 isr_before   = s_test_isr_count;

    /*
     * INTID 3 is deliberately left **unbound**. But for the core to see "no owner", the line must
     * first be **enabled** in hardware —— otherwise it never reaches the CPU (an unbound/unregistered
     * line is disabled anyway, §6.2 "naturally safe before registration"). Here the conformance-only
     * raw hook emulates "firmware left an enabled line that nobody claims", exactly the
     * integration-time fault spurious_owned is meant to catch.
     */
    conf_report(br_gicv3_raw_line_enable(BR_IRQ_TEST_SGI_UNBOUND, 1) == 0, "TC-IRQ-008",
                "conformance hook: enable the ownerless line in hardware (emulate firmware leftover)");
    conf_report(br_gicv3_sgi_trigger(BR_IRQ_TEST_SGI_UNBOUND) == 0, "TC-IRQ-008",
                "trigger the unbound SGI (INTID 3)");

    conf_idle_us(3000u);
    br_irq_stats_get(BR_IRQ_TIMER, &st);
    conf_report(st.spurious_owned > owned_before, "TC-IRQ-008",
                "unbound hwirq ⇒ counted in spurious_owned (a real bug signal)");
    conf_report(s_test_isr_count == isr_before, "TC-IRQ-008",
                "ownerless delivery calls no ISR");
    conf_report(conf_trace_has(BR_TRACE_IRQ_SPURIOUS_OWNED), "TC-IRQ-008",
                "spurious trace");
    (void)br_gicv3_raw_line_enable(BR_IRQ_TEST_SGI_UNBOUND, 0);   /* cleanup: clear the hardware enable of this line */
}

static void conf_storm(void)
{
    /* Lowest-priority registration: whether it can be delivered = whether the PMR init duty is met (§7.1 / TC-IRQ-019) */
    /*
     * ★ Use the "lowest **deliverable** level" 0xF0 (P-IRQ-15): with 5 priority bits PMR implements
     *   only the top 5 (writing 0xFF reads back 0xF8), and the delivery test is `prio < PMR` ⇒ the
     *   0xF8 level is always masked. TC-IRQ-019 verifies "init opens PMR for **all deliverable**
     *   priorities", hence 0xF0; "0xFF is rejected" is covered by the -ENOTSUP assertion in
     *   TC-IRQ-014.
     */
    const br_irq_attr_t attr = {
        .prio = 0xF0u, .trigger = BR_IRQ_TRIG_EDGE_RISE, .flags = 0u
    };

    conf_report(br_irq_register(BR_IRQ_TEST_SGI_STORM, conf_storm_isr, BR_NULL, &attr) == 0,
                "TC-IRQ-019", "register(logical prio = lowest deliverable level 0xF0) -> 0");
    conf_report(br_irq_enable(BR_IRQ_TEST_SGI_STORM) == 0, "TC-IRQ-019", "enable -> 0");

    {
        br_u32 shw = 0u;
        br_u8  sprio = 0u;
        int    sen = -1;
        (void)br_plat_irq_hwirq(BR_IRQ_TEST_SGI_STORM, &shw);
        (void)br_gicv3_readback_prio(shw, &sprio);
        (void)br_gicv3_readback_enabled(shw, &sen);
        br_log_info("[IRQCONF] info storm line intid=%u prio=0x%x enabled=%d", shw, (br_u32)sprio, sen);
        conf_report(sprio == 0xF0u && sen != 0, "TC-IRQ-019",
                    "hardware readback: prio==0xF0 (lowest deliverable) and the line is enabled");
    }

    s_storm_left = 1u;
    const br_u32 base = s_storm_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI_STORM);
    conf_report(conf_wait_count(&s_storm_isr_count, base + 1u, 20000u) == 0, "TC-IRQ-019",
                "lowest-priority line delivered (PMR open for all realizable priorities)");

    /* Storm: self-retrigger 80 times inside the ISR (software equivalent of a level livelock) */
    const br_u32 storm_base = s_storm_isr_count;
    s_storm_left = 80u;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI_STORM);
    conf_report(conf_wait_count(&s_storm_isr_count, storm_base + 81u, 200000u) == 0,
                "TC-IRQ-012", "in-ISR self-retrigger chain completed (80 software triggers)");

    br_irq_stat_t st;
    br_irq_stats_get(BR_IRQ_TEST_SGI_STORM, &st);
    conf_report(st.storm > (br_u16)BR_IRQ_STORM_LIMIT, "TC-IRQ-012",
                "in-window count crosses BR_IRQ_STORM_LIMIT");
    conf_report(conf_trace_has(BR_TRACE_IRQ_STORM), "TC-IRQ-021",
                "storm trace: windowed criterion (ratio, independent of frequency/adjacency)");
    br_log_info("[IRQCONF] info trace ring: emitted=%u overrun=%u seen=0x%x (storm=%u)",
                br_trace_total(), br_trace_overrun(), s_trace_seen, (br_u32)st.storm);

    /* Default action = trace + continue (no auto-mask): other lines keep working */
    const br_u32 before = s_test_isr_count;
    (void)br_plat_irq_trigger(BR_IRQ_TEST_SGI);
    conf_report(conf_wait_count(&s_test_isr_count, before + 1u, 20000u) == 0, "TC-IRQ-012",
                "the system keeps running after the storm (other lines deliver normally)");
}

/* ------------------------------------------------------------------ bottom half (bh) */

/*
 * Handler of the BH direct line. **The criterion lives in its body**: it must observe
 *   - `br_irq_in_isr() == 0`  —— really not in interrupt context (§12.1 shape definition);
 *   - `br_work_in_bh() != 0`  —— really in the bottom half (ADR-0011 context flag);
 *   - `br_irq_in_atomic() != 0` —— still the atomic context where thread-only APIs are banned.
 * If someone ever implements DISPATCH_BH as "just call it directly in the ISR", the first two go red.
 */
static volatile br_u32 s_bh_count;
static volatile int    s_bh_saw_isr;
static volatile int    s_bh_saw_bh;
static volatile int    s_bh_saw_atomic;
static volatile int    s_bh_disable_rc = -12345;

static void conf_bh_isr(void *arg)
{
    s_bh_saw_isr    = (br_irq_in_isr() != BR_FALSE) ? 1 : 0;
    s_bh_saw_bh     = (br_work_in_bh() != BR_FALSE) ? 1 : 0;
    s_bh_saw_atomic = (br_irq_in_atomic() != BR_FALSE) ? 1 : 0;
    s_bh_count++;
    (void)arg;
}

/* Work item body for (a): call a thread-only API in **bh** ⇒ must be rejected (bh ≠ thread context). */
static void conf_bh_disable_from_bh(void *arg)
{
    (void)arg;
    s_bh_disable_rc = br_irq_disable(BR_IRQ_BH_LINE);
}

/*
 * §11.4.1: for a work item "queued but not yet started", if the line was disabled before dispatch
 * ⇒ **skip the handler**, the work item is still consumed from the queue (queue depth not held),
 * and after enable it is **not replayed**.
 *
 * ★ How to make this window **really appear** (not just in a comment) —— use the real mechanism of
 *   **exhausting this round's drain budget** (`BR_WORK_BH_BUDGET` exists for the production path)
 *   plus the **non-reentrant** property of `br_work_drain` (W3):
 *
 *     thread: submit(W1 = "trigger the line")  → queue [W1]
 *     thread: drain(max=1)                     → takes W1 and runs it (s_in_bh = true now)
 *       W1: trigger(line)                      → SGI delivered immediately (IRQs on in thread ctx)
 *         IRQ entry: ack → DISPATCH_BH → submit br_irq_bh_run (queued after W1)
 *                    exit drain(BUDGET) → **non-reentrant ⇒ returns 0, item stays queued**
 *       W1 returns
 *     thread: drain loops until ran == max(1)  ⇒ exits, the handler item **still undispatched**
 *     thread: br_irq_disable(line)             ← legal (thread context)
 *     thread: drain(8)                         → pops br_irq_bh_run
 *       bh: line depth > 0                     → **suppress branch hits**: handler not called, count +1
 *
 *   No state in this chain was made up "just for the test": budget exhaustion + thread-side disable
 *   both happen in production, and §11.4.1 asks exactly "what about the one already queued".
 */
static void conf_bh_trigger(void *arg)
{
    (void)arg;
    (void)br_plat_irq_trigger(BR_IRQ_BH_LINE);
}

static void conf_bh_disable_then_dispatch(void)
{
    br_irq_stat_t st0, st1;

    /* (a) thread-only API in bh ⇒ reject. Mechanical criterion that "bh ≠ thread context". */
    s_bh_count = 0u;
    s_bh_disable_rc = -12345;

    const int sub1 = br_work_submit(conf_bh_disable_from_bh, BR_NULL);
    const br_u32 ran = br_work_drain(8u);      /* explicit drain (equivalent to the one at IRQ exit) */

    br_irq_stats_get(BR_IRQ_BH_LINE, &st0);
    br_log_info("[IRQCONF] info bh: submit(disable_in_bh)=%d drained=%u rc=%d suppressed=%u",
                sub1, ran, s_bh_disable_rc, st0.suppressed_dispatch);

    conf_report(sub1 == 0 && ran >= 1u && s_bh_disable_rc == BR_ERR(EINVAL),
                "TC-IRQ-016", "thread-only API (br_irq_disable) in bh ⇒ -EINVAL (atomic context enforcement)");

    /* (b) Suppress branch: build "the handler item is queued while the line is disabled" per the timeline above. */
    (void)br_irq_enable(BR_IRQ_BH_LINE);       /* precondition: the line must be enabled, else the trigger does not arrive */
    s_bh_count = 0u;
    s_bh_disable_rc = -12345;

    const int sub2 = br_work_submit(conf_bh_trigger, BR_NULL);
    const br_u32 ran_a = br_work_drain(1u);    /* ★ budget 1: push the handler item past this round */

    const int dis = br_irq_disable(BR_IRQ_BH_LINE);   /* thread context ⇒ legal */
    const br_u32 ran_b = br_work_drain(8u);           /* dispatch now ⇒ the line is already disabled */

    br_irq_stats_get(BR_IRQ_BH_LINE, &st1);
    br_log_info("[IRQCONF] info bh: submit(trigger)=%d ran_a=%u disable=%d ran_b=%u "
                "handler_ran=%u suppressed=%u",
                sub2, ran_a, dis, ran_b, s_bh_count, st1.suppressed_dispatch);

    conf_report(sub2 == 0 && dis == 0 && s_bh_count == 0u &&
                st1.suppressed_dispatch > st0.suppressed_dispatch,
                "TC-IRQ-017",
                "BH shape: disabled before dispatch ⇒ handler not called (work item still consumed, count +1)");

    conf_report(conf_trace_has(BR_TRACE_IRQ_DISPATCH_SUPPRESSED), "TC-IRQ-017",
                "suppression trace (TRACE_IRQ_DISPATCH_SUPPRESSED)");

    /* (c) enable does not replay: after re-enabling, the suppressed work does **not** run by itself (§11.4.1). */
    s_bh_count = 0u;
    (void)br_irq_enable(BR_IRQ_BH_LINE);
    const br_u32 ran3 = br_work_drain(8u);
    conf_report(ran3 == 0u && s_bh_count == 0u, "TC-IRQ-017",
                "suppressed work is not replayed after enable (§11.4.1)");
}

static void conf_bh_direct_line(void)
{
    /* This line's DISPATCH_BH comes from the **binding table** (static declaration, §12.2) and must not be overridden by attr at register time. */
    const br_irq_attr_t attr = {
        .prio = BR_IRQ_PRIO_DEFAULT, .trigger = BR_IRQ_TRIG_DEFAULT, .flags = 0u
    };

    const int r = br_irq_register(BR_IRQ_BH_LINE, conf_bh_isr, BR_NULL, &attr);
    conf_report(r == 0, "TC-IRQ-015", "register(BH direct line) -> 0");

    br_irq_stat_t st;
    br_irq_stats_get(BR_IRQ_BH_LINE, &st);
    conf_report((st.flags & (br_u16)BR_IRQ_F_DISPATCH_BH) != 0u, "TC-IRQ-015",
                "binding table DISPATCH_BH folded into descriptor flags (static dispatch shape = truth)");

    conf_report(br_irq_enable(BR_IRQ_BH_LINE) == 0, "TC-IRQ-015", "enable(BH line) -> 0");

    s_bh_count    = 0u;
    s_bh_saw_isr  = -1;
    s_bh_saw_bh   = -1;
    s_bh_saw_atomic = -1;

    const br_u32 before_defer = st.bh_deferred;
    (void)br_plat_irq_trigger(BR_IRQ_BH_LINE);

    /* The IRQ exit (after eoi) pops and runs bh ⇒ what is awaited here is **that** execution. */
    conf_report(conf_wait_count(&s_bh_count, 1u, 20000u) == 0, "TC-IRQ-015",
                "the BH line's ISR is deferred to the bottom half (not lost)");
    conf_report(s_bh_saw_isr == 0, "TC-IRQ-015",
                "in the handler br_irq_in_isr()==0 (really not in interrupt context)");
    conf_report(s_bh_saw_bh == 1, "TC-IRQ-015",
                "in the handler br_work_in_bh()!=0 (bh context flag visible)");
    conf_report(s_bh_saw_atomic == 1, "TC-IRQ-015",
                "in the handler br_irq_in_atomic()!=0 (still under the thread-only ban)");

    br_irq_stats_get(BR_IRQ_BH_LINE, &st);
    conf_report(st.bh_deferred > before_defer, "TC-IRQ-015",
                "CPU-local count bh_deferred incremented (independent of the trace ring)");
    conf_report(conf_trace_has(BR_TRACE_IRQ_BH_DEFER), "TC-IRQ-015",
                "deferral trace (TRACE_IRQ_BH_DEFER)");

    /* Mask the same line during handling (§12.4): after demux/handler the line must be enabled again. */
    {
        br_u32 hw = 0u;
        int en = 0;
        const int g1 = br_plat_irq_hwirq(BR_IRQ_BH_LINE, &hw);
        const int g2 = (g1 == 0) ? br_gicv3_readback_enabled(hw, &en) : -1;
        conf_report(g1 == 0 && g2 == 0 && en != 0, "TC-IRQ-015",
                    "after bh the hardware line is enabled again (skeleton masks the same line while handling)");
    }

    conf_bh_disable_then_dispatch();
}

/* ------------------------------------------------------------------ cascaded domains */

/*
 * SLOW domain "fake PMIC": its status register may **only be read in bh** —— that is the entire
 * reason the SLOW domain exists (bus transactions, §9.4). To turn this into a **mechanical
 * criterion**, the dialect's `pending()` sets `s_slow_violation` and returns "no bits" when called in
 * ISR context; the case asserts demux really happens in bh.
 */
static volatile br_u32 s_slow_pending_seen;   /* how many times read */
static volatile int    s_slow_violation;      /* 1 = someone read status in ISR context */
static volatile br_u32 s_slow_child;
static volatile int    s_slow_child_in_bh;
static volatile br_u32 s_slow_pending;
static br_irq_domain_t *s_slow_dom;

static void slow_pending(void *priv, br_u32 *bits, br_size_t nwords)
{
    (void)priv;
    for (br_size_t i = 0; i < nwords; i++) {
        bits[i] = 0u;
    }
    if (br_irq_in_isr() != BR_FALSE) {
        s_slow_violation = 1;      /* ★ this domain's status register must not be read in an ISR */
        return;
    }
    s_slow_pending_seen++;
    bits[0] = s_slow_pending;
}

static void slow_child_isr(void *arg)
{
    (void)arg;
    s_slow_child_in_bh = (br_work_in_bh() != BR_FALSE) ? 1 : 0;
    s_slow_child++;
    s_slow_pending = 0u;           /* read-clear status: cleared once handled */
}

static const br_irq_domain_ops_t s_slow_ops = {
    .pending = slow_pending,
    .mask    = fake_mask,
    .unmask  = fake_unmask,
    .ack     = fake_ack,
};

/* Reset the fake dialect shadow state (used by the SLOW case itself; the domain pool is a core static object, the case clears only its own shadow) */
static void fake_reset(void)
{
    s_fake_pending    = 0u;
    s_fake_masked     = 0u;
    s_fake_ack_count  = 0u;
}

static void conf_domain_slow(void)
{
    fake_reset();

    s_slow_pending_seen = 0u;
    s_slow_violation    = 0;
    s_slow_child        = 0u;
    s_slow_child_in_bh  = -1;
    s_slow_pending      = 0u;

    /* §9.4 / §1.1.1: the SLOW domain presupposes bh —— after ADR-0011 bh has landed, so it must be **usable**. */
    s_slow_dom = br_irq_domain_create("conf-slow", BR_IRQ_DOMAIN_SLOW, 4u,
                                      BR_IRQ_DOMAIN_F_SLOW, &s_slow_ops, BR_NULL);
    conf_report(s_slow_dom != BR_NULL, "TC-IRQ-102",
                "SLOW cascaded domain usable (bh landed; it was rejected in Stage 1)");

    if (s_slow_dom == BR_NULL) {
        return;
    }

    conf_report(br_irq_register_child(s_slow_dom, 1u, slow_child_isr, BR_NULL, BR_NULL) == 0,
                "TC-IRQ-102", "register_child(sub=1) -> 0");
    conf_report(br_irq_enable_child(s_slow_dom, 1u) == 0, "TC-IRQ-102", "enable_child(1)");
    conf_report(br_irq_enable(BR_IRQ_DOMAIN_SLOW) == 0, "TC-IRQ-102",
                "enable(parent line: PIC direct line)");

    br_irq_stat_t st0, st1;
    br_irq_stats_get(BR_IRQ_DOMAIN_SLOW, &st0);

    s_fake_ack_count = 0u;
    s_slow_pending   = (1u << 1);

    (void)br_plat_irq_trigger(BR_IRQ_DOMAIN_SLOW);

    conf_report(conf_wait_count(&s_slow_child, 1u, 20000u) == 0, "TC-IRQ-102",
                "SLOW domain child handler called (demux delivered via bh)");
    conf_report(s_slow_child_in_bh == 1, "TC-IRQ-102",
                "child handler runs in bh context (br_work_in_bh()!=0)");
    conf_report(s_slow_violation == 0, "TC-IRQ-102",
                "status register **never** read in ISR context (INV-F: the whole reason for the SLOW domain)");
    conf_report(s_slow_pending_seen > 0u, "TC-IRQ-102",
                "pending() really was called (the criterion is not a no-op 'nothing happened')");
    conf_report(s_fake_ack_count == 1u, "TC-IRQ-102",
                "core acks after each child handler returns (per child, §9.3)");
    conf_report((s_fake_masked & (1u << 1)) == 0u, "TC-IRQ-102",
                "after demux the child IRQ is unmasked (mask/unmask pairs complete)");

    br_irq_stats_get(BR_IRQ_DOMAIN_SLOW, &st1);
    conf_report(st1.bh_deferred > st0.bh_deferred, "TC-IRQ-102",
                "the parent ISR only did 'one-shot + mask + submit' (count is CPU-local)");

    /* The parent line must be enabled after bh (else the domain silently dies —— exactly what IR-10 guards). */
    {
        br_u32 hw = 0u;
        int en = 0;
        const int g1 = br_plat_irq_hwirq(BR_IRQ_DOMAIN_SLOW, &hw);
        const int g2 = (g1 == 0) ? br_gicv3_readback_enabled(hw, &en) : -1;
        conf_report(g1 == 0 && g2 == 0 && en != 0, "TC-IRQ-102",
                    "after bh the parent line is enabled (neither a missing owner nor a submit failure leaves a permanent mask)");
    }
}

static void conf_domain_fast(void)
{
    s_dom = br_irq_domain_create("conf-gpio", BR_IRQ_DOMAIN_PARENT, 3u,
                                BR_IRQ_DOMAIN_F_FAST, &s_fake_ops, BR_NULL);
    conf_report(s_dom != BR_NULL, "TC-IRQ-101", "domain_create(FAST, n_sub=3) succeeded");
    if (s_dom == BR_NULL) {
        return;
    }

    {
        const int rc0 = br_irq_register_child(s_dom, 0u, conf_child0_isr, BR_NULL, BR_NULL);
        const int rc2 = br_irq_register_child(s_dom, 2u, conf_child2_isr, BR_NULL, BR_NULL);
        br_log_info("[IRQCONF] info register_child rc(sub0)=%d rc(sub2)=%d", rc0, rc2);
        conf_report(rc0 == 0, "TC-IRQ-101", "register_child(sub=0) -> 0");
        conf_report(rc2 == 0, "TC-IRQ-101", "register_child(sub=2) -> 0");
    }
    conf_report(br_irq_register_child(s_dom, 0u, conf_child0_isr, BR_NULL, BR_NULL) == BR_ERR(EBUSY),
                "TC-IRQ-101", "duplicate register_child -> -EBUSY");
    conf_report(br_irq_register_child(s_dom, 9u, conf_child0_isr, BR_NULL, BR_NULL) == BR_ERR(EINVAL),
                "TC-IRQ-101", "sub out of range -> -EINVAL");

    conf_report(br_irq_enable_child(s_dom, 0u) == 0, "TC-IRQ-101", "enable_child(0)");
    conf_report(br_irq_enable_child(s_dom, 2u) == 0, "TC-IRQ-101", "enable_child(2)");
    conf_report(br_irq_enable(BR_IRQ_DOMAIN_PARENT) == 0, "TC-IRQ-101",
                "enable(parent line: it is a PIC direct line)");

    /* Bitmap: children 0/2 owned, child 1 **ownerless** ⇒ demux must mask it (storm gate, §9.3) */
    s_fake_masked    = 0u;
    s_fake_ack_count = 0u;
    s_child_count[0] = 0u;
    s_child_count[2] = 0u;
    s_fake_pending   = (1u << 0) | (1u << 1) | (1u << 2);

    (void)br_plat_irq_trigger(BR_IRQ_DOMAIN_PARENT);

    conf_report(conf_wait_count(&s_child_count[0], 1u, 20000u) == 0, "TC-IRQ-101",
                "child 0 handler runs inside demux (ISR context)");
    conf_report(s_child_count[2] == 1u, "TC-IRQ-101",
                "child 2 handler runs in the same demux");
    conf_report(s_fake_ack_count == 3u, "TC-IRQ-101",
                "core acks per child **after each child handler returns** (once per set bit, 3 total)");
    conf_report((s_fake_masked & (1u << 1)) != 0u, "TC-IRQ-101",
                "the ownerless child IRQ gets masked (no 'ownerless yet retriggering' storm gate left)");
    conf_report(conf_trace_has(BR_TRACE_IRQ_DOMAIN_ORPHAN), "TC-IRQ-101",
                "orphan trace (TRACE_IRQ_DOMAIN_ORPHAN)");
}

static void conf_domain_api_misuse(void)
{
    if (s_dom == BR_NULL) {
        conf_report(BR_FALSE, "TC-IRQ-018", "domain not created, cannot verify the two API families are mutually exclusive");
        return;
    }

    const br_u32 child_virq = BR_IRQ_DOMAIN_BASE;   /* first child IRQ of the window */
    const br_irq_attr_t attr = { BR_IRQ_PRIO_DEFAULT, BR_IRQ_TRIG_DEFAULT, 0u };

    conf_report(br_irq_register(child_virq, conf_child0_isr, BR_NULL, &attr) == BR_ERR(EINVAL),
                "TC-IRQ-018", "direct register on a domain member -> -EINVAL (reported at register time)");
    conf_report(br_irq_disable(child_virq) == BR_ERR(EINVAL),
                "TC-IRQ-018", "direct disable on a domain member -> -EINVAL");
    conf_report(br_irq_enable_child(s_dom, 7u) == BR_ERR(EINVAL),
                "TC-IRQ-018", "domain API reverse: sub out of range -> -EINVAL");

    br_u32 parent_hw = 0u;
    int  en = 0;
    const int got_hw = br_plat_irq_hwirq(BR_IRQ_DOMAIN_PARENT, &parent_hw);
    const int got_en = (got_hw == 0) ? br_gicv3_readback_enabled(parent_hw, &en) : -1;
    conf_report(got_hw == 0 && got_en == 0 && en != 0, "TC-IRQ-018",
                "hardware readback: the parent line is still enabled (the misuse did not mask an unrelated physical line)");
}

static void conf_stats(void)
{
    br_irq_stat_t st;

    br_irq_stats_get(BR_IRQ_TEST_SGI, &st);
    conf_report(st.count > 0u && st.depth == 0u && st.flags == 0u, "TC-IRQ-STATS",
                "stats_get snapshot self-consistent (count>0 / depth==0 / flags==0)");

    br_irq_stats_get(BR_IRQ_INVALID, &st);
    conf_report(st.count == 0u && st.depth == 0u, "TC-IRQ-STATS",
                "out-of-range virq snapshot zeroed (no out-of-bounds read)");

    br_irq_stats_get(BR_IRQ_UART0, &st);
    conf_report(st.depth == 1u && st.prio == 0x01u, "TC-IRQ-STATS",
                "UART0: after register depth==1 (masked) and the logical prio is kept");
}

/* =====================================================================
 * Entry (called by the platform's `qemu_aarch64_start()` in the START phase —— global IRQs are on then; see br_plat.h)
 * ===================================================================== */
int br_plat_irq_conformance(void)
{
    s_pass = 0u;
    s_fail = 0u;
    s_trace_seen = 0u;
    s_dom = BR_NULL;

    br_log_info("[IRQCONF] int framework conformance (Stage 1, GICv3, no scheduler)");

    /* Enable IRQs globally: the last step of design §14.3 (after all plugin init).
     * The manager already did it after LATE (see plugin_mgr.c); this is an **idempotent** backstop
     * —— the cases themselves need the premise "IRQs really open" (only then is a software SGI
     * received). */
    br_irq_cpu_enable();

    conf_gicv3_facts();
    conf_extable();
    conf_sgi_path();
    conf_duplicate_register();
    conf_priority_readback();
    conf_mask_depth();
    conf_lock_nesting();
    conf_unlock_underflow();
    conf_spurious_unbound();
    conf_storm();
    conf_bh_direct_line();          /* ADR-0011: per-line BH dispatch + §11.4.1 suppress/no-replay */
    conf_domain_fast();
    conf_domain_slow();             /* ADR-0011: SLOW domain demux goes through bh */
    conf_domain_api_misuse();
    conf_stats();

    br_log_info("[IRQCONF] SUMMARY pass=%u fail=%u total=%u",
                s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}
