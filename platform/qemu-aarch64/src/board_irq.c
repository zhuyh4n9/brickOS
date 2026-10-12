/*
 * brickOS prototype v0.1.0 — QEMU virt board IRQ data + platform IRQ init + arch timer rearm
 *
 * Design basis (authoritative): `docs/3-os-core/3-02-int.md`
 *   §3.3  binding table (virq ↔ hwirq + board static prio/trigger) — this is **platform data**
 *   §14.3 one-way init chain: steps 1–3 live in `br_plat_early_init()`, and "any step failing = boot
 *         failure, never silently degrade"; steps 4–7 (register/enable) belong to drivers/framework
 *   §8.2  level-type must "clear at source on the device side", else the line stays asserted ⇒
 *         livelock — the timer rearm is the arch timer's "clear source + arm the next deadline"
 *   §13.1 SGI/PPI/SPI kind and affinity boundaries (see the INTID comments in board_irq.h)
 *
 * ★ Layers: this file is the **platform data and specialization** layer (base addresses/bindings/
 *   frequency conversion). The controller dialect is in gicv3.c, semantics and mechanism in core
 *   (br_irq.c). The addresses here are QEMU virt facts, not part of the ISA layer.
 */
#include <br/board_irq.h>
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
/* Timer ISR → scheduler-side tick driver (3-02 §11.1 "path four": timer PPI → timeout queue). */
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/platform/br_gicv3.h>
#include <br/platform/br_plat.h>

/* =====================================================================
 * 1. Board INTIDs (single source of truth documented in br/board_irq.h; here we only name numbers)
 * ===================================================================== */
#define BR_BOARD_INTID_TIMER        30u  /* EL1 physical timer (PPI; GIC INTID 30) */
/* Heartbeat period = **one product-configured tick** (us): `product.toml [kernel].hz` arrives
 * via `-DBR_CFG_TICK_HZ`, default 200 ⇒ 5 ms (cf. Linux CONFIG_HZ; ADR-0017). ~HZ interrupts
 * per second, so the log shows at a glance that "interrupts really run". Changing the frequency
 * touches only the product declaration, never this file. */
#define BR_BOARD_TIMER_PERIOD_US    BR_CFG_TICK_PERIOD_US
#define BR_BOARD_INTID_UART0        33u  /* PL011 UART0 (SPI 1 ⇒ INTID 32+1=33) */
#define BR_BOARD_INTID_SGI_TEST      0u  /* SGI 0: software-triggered selftest (§8.4) */
#define BR_BOARD_INTID_SGI_STORM     1u  /* SGI 1: storm test carrier */
#define BR_BOARD_INTID_SGI_DOMAIN    2u  /* SGI 2: parent line of the fake FAST domain */
#define BR_BOARD_INTID_SGI_SLOW      4u  /* SGI 4: parent line of fake SLOW domain (via bh) */
#define BR_BOARD_INTID_SGI_BH        5u  /* SGI 5: direct BH line (ISR deferred to bottom half) */
/* INTID 3 (SGI 3) deliberately unbound: sample of "hardware delivery with no owner" (TC-IRQ-008) */

/*
 * Binding table (compiled into .rodata; design §3.3/§14.4).
 * Per entry: virq↔hwirq mapping, static default prio, static trigger, dispatch form.
 *   - TIMER: INTID 30, **level** (arch timer ISTATUS/condition line), prio 0x80,
 *            PERCPU (PPI is per-core; §13.1 — must never "balance onto another core")
 *   - UART0: INTID 33, level (PL011 UARTINTR is level-type), prio 0xA0; v0.1 only polls the
 *            console, the interrupt is unused — bound here to exercise the SPI mask/prio/readback path
 *   - SGI:   always **edge** (fixed by the architecture), prio 0xC0, software trigger channel
 *   - DOMAIN_PARENT: SGI 2, edge, parent line of the fake FAST domain (members in virq 56.. window)
 *   - DOMAIN_SLOW:   SGI 4, edge, parent of the fake SLOW domain — **demux via bottom half** (§9.4/ADR-0011)
 *   - BH_LINE:       SGI 5, edge, **per-line BH dispatch** (ISR deferred to bottom half; §12.4/ADR-0011)
 * dispatch is INLINE for all but BH_LINE (§12.2: default carrier is the binding table, shown here).
 * pic_id = 0 (first/only PIC in EARLY registration order); dom_id = -1 (direct, in no domain).
 */
static const br_irq_binding_t s_board_irq[BR_IRQ_BOARD_NR] = {
    [BR_IRQ_TIMER] = {
        .virq     = BR_IRQ_TIMER,
        .pic_id   = 0u,
        .flags    = BR_IRQB_F_PERCPU,
        .hwirq    = BR_BOARD_INTID_TIMER,
        .dom_id   = -1,
        .prio     = 0x80u,
        .trigger  = BR_IRQ_TRIG_LEVEL_HIGH,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_UART0] = {
        .virq     = BR_IRQ_UART0,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_UART0,
        .dom_id   = -1,
        .prio     = 0xA0u,
        .trigger  = BR_IRQ_TRIG_LEVEL_HIGH,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_TEST_SGI] = {
        .virq     = BR_IRQ_TEST_SGI,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_SGI_TEST,
        .dom_id   = -1,
        .prio     = 0xC0u,
        .trigger  = BR_IRQ_TRIG_EDGE_RISE,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_TEST_SGI_STORM] = {
        .virq     = BR_IRQ_TEST_SGI_STORM,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_SGI_STORM,
        .dom_id   = -1,
        .prio     = 0xC0u,
        .trigger  = BR_IRQ_TRIG_EDGE_RISE,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_DOMAIN_PARENT] = {
        .virq     = BR_IRQ_DOMAIN_PARENT,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_SGI_DOMAIN,
        .dom_id   = -1,
        .prio     = 0xC0u,
        .trigger  = BR_IRQ_TRIG_EDGE_RISE,
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_DOMAIN_SLOW] = {
        .virq     = BR_IRQ_DOMAIN_SLOW,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_SGI_SLOW,
        .dom_id   = -1,
        .prio     = 0xC0u,
        .trigger  = BR_IRQ_TRIG_EDGE_RISE,
        /* ★ The parent line itself stays INLINE (the domain dispatch form comes from
         *   `BR_IRQ_DOMAIN_F_SLOW`, §9.4); it can do only "mask + submit" in the ISR precisely
         *   because the child status register is not read here. */
        .dispatch = BR_IRQ_DISPATCH_INLINE,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_BH_LINE] = {
        .virq     = BR_IRQ_BH_LINE,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = BR_BOARD_INTID_SGI_BH,
        .dom_id   = -1,
        .prio     = 0xC0u,
        .trigger  = BR_IRQ_TRIG_EDGE_RISE,
        /* ★ Statically declared dispatch form (§12.2: default carrier = binding table ⇒ no RAM). */
        .dispatch = BR_IRQ_DISPATCH_BH,
        .rsv      = {0u, 0u, 0u},
    },
    /*
     * PCIe INTx A..D (INTID 35..38; ADR-0021).
     *   - trigger = **level** (QEMU PCIe INTx is LEVEL_HIGH at the GIC side) ⇒ device must clear at source;
     *   - dispatch = **BH** (design `7-02` §5: the ISR does the minimum; completion/error handed to the
     *     waiting thread in bh);
     *   - prio = 0xB0 (between UART 0xA0 and SGI 0xC0; storage completion must not outrank timer/serial);
     *   - with no ISR registered they are just "bound but unused" lines (same as UART0).
     */
    [BR_IRQ_PCIE_INTX_A] = {
        .virq     = BR_IRQ_PCIE_INTX_A,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = 35u,
        .dom_id   = -1,
        .prio     = 0xB0u,
        .trigger  = BR_IRQ_TRIG_LEVEL_HIGH,
        .dispatch = BR_IRQ_DISPATCH_BH,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_PCIE_INTX_B] = {
        .virq     = BR_IRQ_PCIE_INTX_B,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = 36u,
        .dom_id   = -1,
        .prio     = 0xB0u,
        .trigger  = BR_IRQ_TRIG_LEVEL_HIGH,
        .dispatch = BR_IRQ_DISPATCH_BH,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_PCIE_INTX_C] = {
        .virq     = BR_IRQ_PCIE_INTX_C,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = 37u,
        .dom_id   = -1,
        .prio     = 0xB0u,
        .trigger  = BR_IRQ_TRIG_LEVEL_HIGH,
        .dispatch = BR_IRQ_DISPATCH_BH,
        .rsv      = {0u, 0u, 0u},
    },
    [BR_IRQ_PCIE_INTX_D] = {
        .virq     = BR_IRQ_PCIE_INTX_D,
        .pic_id   = 0u,
        .flags    = 0u,
        .hwirq    = 38u,
        .dom_id   = -1,
        .prio     = 0xB0u,
        .trigger  = BR_IRQ_TRIG_LEVEL_HIGH,
        .dispatch = BR_IRQ_DISPATCH_BH,
        .rsv      = {0u, 0u, 0u},
    },
};

/* Table length must match BR_IRQ_BOARD_NR in br/board_irq.h: the table must not silently drift */
_Static_assert((br_size_t)BR_ARRAY_SIZE(s_board_irq) == (br_size_t)BR_IRQ_BOARD_NR,
               "binding table length drifted from BR_IRQ_BOARD_NR");

/* =====================================================================
 * 2. Platform IRQ init (design §14.3 steps 1–3; EARLY phase, IRQs globally disabled)
 * ===================================================================== */
int br_plat_irq_init(void)
{
    /* QEMU virt GICv3 layout (platform data):
     *   GICD 0x0800_0000; GICR 0x080A_0000; per-core frame span 2 × 64 KiB = 0x2_0000.
     *   nr_irq = 1024 = INTID upper bound (not used as an allocation table; §14.2 says no direct index table). */
    static const br_gicv3_cfg_t cfg = {
        .gicd_base       = 0x08000000ul,
        .gicr_base       = 0x080A0000ul,
        .gicr_frame_size = 0x20000u,
        .gicr_max_frames = 8u,
        .nr_irq          = 1024u,
    };
    int pic_id;
    int rc;

    /* Steps 1–2: register the PIC instance (internally initializes the GICv3 CPU interface/
     * distributor/redistributor). Failure = boot failure, negative errno propagated up
     * (never "silently return 0 while unconfigured", §14.3 obligation). */
    pic_id = br_gicv3_register(&cfg);
    if (pic_id < 0) {
        return pic_id;
    }

    /* Step 3: submit the binding table — only then can virq↔hwirq be resolved.
     * A duplicate submit is reported by core as -EBUSY (EARLY submits exactly once). */
    rc = br_irq_bindings_set(s_board_irq, BR_ARRAY_SIZE(s_board_irq));
    if (rc != 0) {
        return rc;
    }
    return 0;
}

/* =====================================================================
 * 3. Arm/rearm of the arch timer (EL1 physical timer, PPI INTID 30)
 *   (ISR and "let interrupts run" in §4; period constant BR_BOARD_TIMER_PERIOD_US)
 *
 * Why this counts as "clear at source on the device side" (§8.2): the arch timer condition line is
 * **level**-type — an ISR that only acks/eois without re-arming TVAL leaves the condition holding
 * ⇒ immediately pending again ⇒ livelock. So rearm (rewriting CNTP_TVAL_EL0) is not an optional
 * optimization but an obligation of a level interrupt.
 * ===================================================================== */
#define BR_CNTP_CTL_ENABLE  0x1u   /* CNTP_CTL_EL0.ENABLE (bit0); IMASK (bit1)=0 means unmasked */
#define BR_CNTP_CTL_DISABLE 0x0u   /* ENABLE=0 */

static void br_timer_write_tval(br_u64 ticks)
{
    __asm__ volatile("msr cntp_tval_el0, %0" ::"r"(ticks) : "memory");
}

static void br_timer_write_ctl(br_u64 ctl)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" ::"r"(ctl) : "memory");
}

/*
 * Microseconds → ticks. **Round up**: `((us * freq) + 999999) / 1000000`.
 * Rounding down would shorten the deadline (expire earlier than requested) — the unsafe direction
 * for tickless "earliest deadline" semantics (may wake early), so rather wait less than 1 tick too long.
 * freq comes from CNTFRQ_EL0 (QEMU virt = 62.5 MHz), provided by br_plat_ticks_freq().
 */
static br_u64 br_timer_us_to_ticks(br_u64 period_us)
{
    br_u64 freq = br_plat_ticks_freq();
    return ((period_us * freq) + 999999u) / 1000000u;
}

static void br_timer_rearm(br_u64 period_us)
{
    /* Called from the ISR: rearm TVAL = clear the current condition + schedule the next deadline (§8.2) */
    br_timer_write_tval(br_timer_us_to_ticks(period_us));
}

void br_plat_timer_stop(void)
{
    br_timer_write_ctl(BR_CNTP_CTL_DISABLE);
}

/* ---------------------------------------------------------------------
 * 4. "Let interrupts run": timer PPI ISR + enable + arm + global IRQ enable
 *
 * ★ Why the ISR lives in **platform** and not APP (P-IRQ-17, decided by the privilege model):
 *   Design 3-01 §13.6 ranks "**interrupt control**" as **P3** (only
 *   `ability.subkind=scheduler` and `platform` may declare it) and "IRQ lines" as P1;
 *   APP is **P0** (only the ordinary native APIs: task/time/lock/service registry/memory allocation).
 *   ⇒ letting app/hello call `br_irq_register` would be a **declaration-vs-implementation mismatch**:
 *     it would do a P3 job from a P0 position, and the priv domain of `brickie check` (declaration
 *     legality) does not see source code and so would not flag it — such "invisible to the tool"
 *     overreach must be avoided by **placement**.
 *   Design 3-02 §11.1 assigns the timer PPI ISR to core.init; per **P-IRQ-17's placement ruling**
 *   the prototype keeps it in the platform start phase (reason: the privilege model above);
 *   `docs/decisions/0002` records this.
 *   After ADR-0008 core.init is a separate entry point, but this "where the ISR lives" ruling is
 *   **unchanged** (core.init does not take over device-level ISR registration).
 * ------------------------------------------------------------------- */
static void br_plat_timer_isr(void *arg)
{
    (void)arg;
    /* The heartbeat count belongs to **core** (repays `br-wa-boot-001` ②): platform only "notifies",
     * core owns the count, so APP need not know the platform plugin interface to read a diagnostic
     * count. Order: account first (count), then advance time/rearm — the count is the
     * "this tick arrived" observation point. */
    br_clock_tick_notify();
    br_sched_on_tick(br_clock_now());
    br_timer_rearm(BR_BOARD_TIMER_PERIOD_US);
}

br_u32 br_plat_timer_ticks(void)
{
    /* Platform-side accessor (kept on the interface: removal must follow design §6.3's
     * deprecated → deprecation window → REMOVED, see `docs/decisions/0016-core-timer-heartbeat.md`).
     * The single source of truth is now core ⇒ this only forwards, there is no second count. */
    return br_clock_tick_count();
}

int br_plat_irq_start(void)
{
    const br_irq_attr_t attr = {
        .prio    = BR_IRQ_PRIO_DEFAULT,   /* use the binding table's static default */
        .trigger = BR_IRQ_TRIG_DEFAULT,   /* likewise (PPI is level-type, set by board data) */
        /* PERCPU must match the binding table (§3.4); NO_STORM_GUARD = design §8.3's "escape hatch" —
         * the arch timer is a **known and legitimate** periodic high-frequency line, and this prototype
         * produces almost nothing else ⇒ its share of the "last 256 interrupts" far exceeds 25% and
         * would periodically trip the storm warning. A per-line exemption (rather than tuning the
         * threshold or disabling the detector) expresses "this is a known legitimate high-frequency line". */
        .flags   = BR_IRQ_F_PERCPU | BR_IRQ_F_NO_STORM_GUARD,
    };

    int r = br_irq_register(BR_IRQ_TIMER, br_plat_timer_isr, BR_NULL, &attr);
    if (r != 0) {
        return r;
    }
    r = br_irq_enable(BR_IRQ_TIMER);
    if (r != 0) {
        return r;
    }

    br_timer_rearm(BR_BOARD_TIMER_PERIOD_US);
    br_timer_write_ctl(BR_CNTP_CTL_ENABLE);   /* IMASK=0: raise the interrupt when the condition holds */

    /* Global IRQ enable = the last step of design 3-02 §14.3's init chain ("after all plugins init").
     * The plugin manager already does it after the LATE phase and before the START phase (see the phase
     * driver in plugin_mgr.c); keeping one call here is the **idempotent** platform-side bring-up
     * (matching the literal order of 3-02 §14.3). */
    br_irq_cpu_enable();
    return 0;
}

/* =====================================================================
 * 5. Conformance selftest channel (design §8.4; implemented in irq_conf.c, only two primitives here)
 * ===================================================================== */

/* Software-trigger a virq bound to an SGI: no binding ⇒ -ENODEV; not an SGI ⇒ -ENOTSUP */
int br_plat_irq_trigger(br_u32 virq)
{
    for (br_size_t i = 0; i < BR_ARRAY_SIZE(s_board_irq); i++) {
        if ((br_u32)s_board_irq[i].virq == virq) {
            if (s_board_irq[i].hwirq > BR_GICV3_INTID_SGI_MAX) {
                return BR_ERR(ENOTSUP);   /* only SGIs (0–15) can be software-triggered */
            }
            return br_gicv3_sgi_trigger(s_board_irq[i].hwirq);
        }
    }
    return BR_ERR(ENODEV);
}

/* virq → hwirq from the binding table (only for conformance shadow-vs-hardware cross-check, §4.3 item 5) */
int br_plat_irq_hwirq(br_u32 virq, br_u32 *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    for (br_size_t i = 0; i < BR_ARRAY_SIZE(s_board_irq); i++) {
        if ((br_u32)s_board_irq[i].virq == virq) {
            *out = s_board_irq[i].hwirq;
            return 0;
        }
    }
    return BR_ERR(ENODEV);
}
