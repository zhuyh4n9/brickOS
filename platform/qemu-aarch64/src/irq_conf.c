/*
 * Interrupt subsystem conformance cases (in-image; the interrupt path does not exist on host).
 * Driven by qemu_aarch64_selftest() after all starts. Each case prints one line
 * `[IRQCONF] PASS/FAIL <case id> <detail>` and the run ends with `[IRQCONF] SUMMARY pass=N fail=M`,
 * so red/green is greppable by make smoke / make irq-test. Cases need ISA means (SGI trigger,
 * hardware readback); the case bodies call core APIs only. Coverage:
 *   TC-IRQ-001 register/enable/self-trigger, arg delivery, ack/eoi pairing
 *   TC-IRQ-002 duplicate register -> -EBUSY; TC-IRQ-003/013 masked trigger, enable catch-up
 *   TC-IRQ-004 lock nesting; TC-IRQ-008 unbound hwirq -> spurious; TC-IRQ-010 extable -EFAULT
 *   TC-IRQ-012/021 storm criterion; TC-IRQ-014 priority quantization; TC-IRQ-015 BH direct line
 *   TC-IRQ-016 underflow guard; TC-IRQ-017 disabled before dispatch => suppressed, no replay
 *   TC-IRQ-018 direct API on a domain member -> -EINVAL; TC-IRQ-019 lowest-priority delivered
 *   TC-IRQ-022 in_fault reset after a fixup; TC-IRQ-101 FAST domain orphan child masked
 *   TC-IRQ-102 SLOW domain demux in bh; GIC-* facts: EOImode/PRIbits/lines/binding table
 */
#include <br/core/br_irq.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>
#include <br/core/br_fault.h>
#include <br/core/br_error.h>
#include <br/core/br_log.h>
/* BH cases must drive a drain explicitly and assert the handler really runs in bh context. */
#include <br/core/br_work.h>

#include <br/platform/br_plat.h>
#include <br/platform/br_gicv3.h>
#include <br/board_irq.h>

/* The domain window must fit one 32-bit pending word. */
_Static_assert(BR_IRQ_DOMAIN_WINDOW <= 32u, "domain window exceeds single-word bitmap capacity");

/*
 * Deterministic fault address for the extable negative control: inside RAM (0x40000000, -m 128M)
 * but outside the page-table RAM window (0x40000000 + 8 MiB) => translation fault. The low range
 * never faults on QEMU virt (unassigned/flash reads 0). Equals BR_PLAT_CONF_UNMAPPED in br_mmu.h;
 * change both together.
 */
#define CONF_UNMAPPED_ADDR   0x42000000ul

/* ---- Assertions and stats ---- */
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

/* Trace id bitmap; the ring is drained on each poll. */
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

/* Bounded wait; IRQs are delivered meanwhile (criterion for "the ISR really ran"). */
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

/* Busy wait; IRQs delivered normally -- negative "must not deliver while masked" criterion. */
static void conf_idle_us(br_time_t us)
{
    const br_time_t t0 = br_clock_now();

    while ((br_clock_now() - t0) < us) {
        /* busy wait */
    }
}

/* ---- ISRs and test fake devices (FAST domain dialect) ---- */
static volatile br_u32 s_test_isr_count;
static volatile br_u32 s_test_isr_arg;

static void conf_test_isr(void *arg)
{
    s_test_isr_count++;
    s_test_isr_arg = (br_u32)(br_uintptr_t)arg;
}

/* Storm case: self-retrigger inside the ISR, the software equivalent of a level livelock. SGI is
 * edge and the GIC latches pending, so triggering before this Active is eoi'd re-pends it. */
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

/* FAST domain fake pin controller: state bitmap in RAM, demux purely in-ISR. */
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
    /* Masked state does not appear in the pending snapshot. */
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

/* ---- extable probe: the return value comes only from the register core writes into the exception
 * frame per the entry's reg/errno_val (arm64 x0); the fixup label never invents an error code. */
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

/* ---- Cases ---- */
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
     * Deterministic memory fault: the old unaligned-access trick no longer faults (MMU on,
     * SCTLR_EL1 A/SA/SA0 cleared, so -mstrict-align is only defensive now). CONF_UNMAPPED_ADDR is
     * outside the page-table RAM window, its L1/L2 entry is invalid => translation fault
     * (EC=0x25/DFSC=0x04..0x07 class), equally deterministic and likewise fixed by extable.
     */
    v = 0u;
    const int r2 = conf_probe_read32(CONF_UNMAPPED_ADDR, &v);
    br_log_info("[IRQCONF] info probe(unmapped=0x%lx) ret=%d", (br_u64)CONF_UNMAPPED_ADDR, r2);
    conf_report(r2 == BR_ERR(EFAULT), "TC-IRQ-010",
                "extable hit: unmapped address (translation fault) degrades to -EFAULT, system survives");

    /* Second fault: if in_fault were not reset, this would look like a double fault and panic. */
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
    /* PL011 UART0 (SPI 33) is not yet registered. Requested prio 0x01: valid bits are MSB aligned
     * (5 bits => step 8), so it quantizes toward lower priority to 0x08 (0x00 would be a raise). */
    br_u32 hwirq = 0u;
    if (br_plat_irq_hwirq(BR_IRQ_UART0, &hwirq) != 0) {
        conf_report(BR_FALSE, "TC-IRQ-014", "failed to get UART0 hwirq (binding table entry missing)");
        return;
    }

    /* The lowest realizable level is rejected: with 5 priority bits PMR implements only the top 5
     * (0xFF reads back 0xF8) and delivery needs prio < PMR, so 0xF8 is never delivered => -ENOTSUP,
     * not a silent lowering to 0xF0 (a privilege raise). */
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

    /* Trigger while masked: hardware pending latches (CAP_LATCH), but no handler may start. */
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
    br_irq_unlock(st);   /* one extra: the underflow guard must stop it, else depth wraps */

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

    /* INTID 3 is deliberately unbound, but core only sees "no owner" once the line is enabled in
     * hardware; the conformance-only raw hook emulates a firmware leftover that nobody claims. */
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
    /* Lowest deliverable level 0xF0: with 5 priority bits PMR implements only the top 5, so the
     * 0xF8 level is always masked; 0xF0 verifies "PMR open for all realizable priorities". */
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
 * BH direct-line handler; the criterion is in the body: br_irq_in_isr()==0, br_work_in_bh()!=0 and
 * br_irq_in_atomic()!=0. Implementing DISPATCH_BH as a direct ISR call makes the first two go red.
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

/* Work item body: a thread-only API called in bh must be rejected (bh is not thread context). */
static void conf_bh_disable_from_bh(void *arg)
{
    (void)arg;
    s_bh_disable_rc = br_irq_disable(BR_IRQ_BH_LINE);
}

/*
 * A work item queued but not yet started is skipped if the line was disabled before dispatch; the
 * item is still consumed and enable does not replay it. The window comes from the real mechanism:
 * exhausting this round's drain budget (BR_WORK_BH_BUDGET) plus the non-reentrant br_work_drain.
 * submit(W1 = trigger the line) -> drain(max=1) runs W1 -> the IRQ entry submits br_irq_bh_run
 * (queued after W1) -> the re-entrant drain returns 0, item stays queued -> br_irq_disable(line)
 * in thread context -> drain(8) pops it while depth > 0 => suppressed: no handler, count +1.
 */
static void conf_bh_trigger(void *arg)
{
    (void)arg;
    (void)br_plat_irq_trigger(BR_IRQ_BH_LINE);
}

static void conf_bh_disable_then_dispatch(void)
{
    br_irq_stat_t st0, st1;

    /* (a) thread-only API in bh is rejected: bh is not thread context. */
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
    const br_u32 ran_a = br_work_drain(1u);    /* budget 1: push the handler item past this round */

    const int dis = br_irq_disable(BR_IRQ_BH_LINE);   /* thread context, legal */
    const br_u32 ran_b = br_work_drain(8u);           /* dispatch now: line already disabled */

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

    /* (c) enable does not replay: the suppressed work does not run by itself after re-enabling. */
    s_bh_count = 0u;
    (void)br_irq_enable(BR_IRQ_BH_LINE);
    const br_u32 ran3 = br_work_drain(8u);
    conf_report(ran3 == 0u && s_bh_count == 0u, "TC-IRQ-017",
                "suppressed work is not replayed after enable (§11.4.1)");
}

static void conf_bh_direct_line(void)
{
    /* DISPATCH_BH comes from the binding table, not from attr at register time. */
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

    /* The IRQ exit (after eoi) pops and runs bh; that execution is what is awaited here. */
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

    /* The same line is masked during handling; after the handler it must be enabled again. */
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
 * SLOW domain fake PMIC: its status register may only be read in bh (bus transactions), the whole
 * reason the SLOW domain exists. pending() records a violation and returns no bits in ISR context.
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
        s_slow_violation = 1;      /* this domain's status register must not be read in an ISR */
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

/* Reset the fake dialect shadow state (the core domain pool is static and untouched here). */
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

    /* SLOW presupposes bh; bh has landed, so the domain must be usable. */
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

    /* The parent line must be enabled after bh, else the domain silently dies. */
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

    /* Children 0/2 owned, child 1 ownerless => demux must mask it (storm gate). */
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

/* ---- Entry: START phase, global IRQs on ---- */
int br_plat_irq_conformance(void)
{
    s_pass = 0u;
    s_fail = 0u;
    s_trace_seen = 0u;
    s_dom = BR_NULL;

    br_log_info("[IRQCONF] int framework conformance (Stage 1, GICv3, no scheduler)");

    /* Idempotent global IRQ enable (the manager already did it); a software SGI needs IRQs open. */
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
    conf_bh_direct_line();          /* per-line BH dispatch + suppress/no-replay */
    conf_domain_fast();
    conf_domain_slow();             /* SLOW domain demux goes through bh */
    conf_domain_api_misuse();
    conf_stats();

    br_log_info("[IRQCONF] SUMMARY pass=%u fail=%u total=%u",
                s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}
