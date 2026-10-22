/*
 * QEMU virt board IRQ data: INTID numbers, the binding table (virq<->hwirq, static prio/trigger,
 * dispatch form), platform IRQ init (EARLY phase), arch timer arm/rearm and conformance hooks.
 * The addresses and bindings here are QEMU virt facts; the controller dialect is gicv3.c and the
 * mechanism/semantics are core (br_irq.c).
 */
#include <br/board_irq.h>
#include <br/core/br_error.h>
#include <br/hal/br_irq.h>
/* Timer ISR -> scheduler-side tick driver. */
#include <br/hal/br_sched.h>
#include <br/core/br_time.h>
#include <br/runtime/br_gicv3.h>
#include <br/runtime/br_plat.h>

/* ---- 1. Board INTIDs ---- */
#define BR_BOARD_INTID_TIMER        30u  /* EL1 physical timer (PPI; GIC INTID 30) */
/* Heartbeat period = one product-configured tick, from -DBR_CFG_TICK_HZ (default 200 => 5 ms). */
#define BR_BOARD_TIMER_PERIOD_US    BR_CFG_TICK_PERIOD_US
#define BR_BOARD_INTID_UART0        33u  /* PL011 UART0 (SPI 1 => INTID 32+1=33) */
#define BR_BOARD_INTID_SGI_TEST      0u  /* SGI 0: software-triggered selftest */
#define BR_BOARD_INTID_SGI_STORM     1u  /* SGI 1: storm test carrier */
#define BR_BOARD_INTID_SGI_DOMAIN    2u  /* SGI 2: parent line of the fake FAST domain */
#define BR_BOARD_INTID_SGI_SLOW      4u  /* SGI 4: parent line of fake SLOW domain (via bh) */
#define BR_BOARD_INTID_SGI_BH        5u  /* SGI 5: direct BH line (ISR deferred to bottom half) */
/* INTID 3 (SGI 3) deliberately unbound: sample of "hardware delivery with no owner" (TC-IRQ-008) */

/*
 * Binding table in .rodata; per entry: virq<->hwirq, static default prio, static trigger and
 * dispatch form. TIMER/PPI is level and PERCPU (never balanced); UART0 is level and unused in
 * v0.1 (bound to exercise the SPI mask/prio/readback path); SGI is always edge. DOMAIN_PARENT
 * (SGI 2) parents the fake FAST domain, DOMAIN_SLOW (SGI 4) demuxes via bh, BH_LINE (SGI 5) is
 * per-line BH dispatch; all others INLINE. pic_id = 0 (first/only PIC); dom_id = -1 (no domain).
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
        /* Parent line stays INLINE (the domain's dispatch form comes from BR_IRQ_DOMAIN_F_SLOW);
         * the ISR only masks + submits because it never reads the child status register. */
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
        /* Static dispatch form in the binding table => no RAM needed. */
        .dispatch = BR_IRQ_DISPATCH_BH,
        .rsv      = {0u, 0u, 0u},
    },
    /*
     * PCIe INTx A..D (INTID 35..38). Level at the GIC side, so the device must clear at source;
     * BH dispatch (ISR does the minimum, completion/error goes to the waiting thread). prio 0xB0
     * sits between UART 0xA0 and SGI 0xC0; without an ISR these stay bound-but-unused lines.
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

/* Table length must match BR_IRQ_BOARD_NR in br/board_irq.h. */
_Static_assert((br_size_t)BR_ARRAY_SIZE(s_board_irq) == (br_size_t)BR_IRQ_BOARD_NR,
               "binding table length drifted from BR_IRQ_BOARD_NR");

/* ---- 2. Platform IRQ init (EARLY phase, IRQs globally disabled) ---- */
int br_plat_irq_init(void)
{
    /* QEMU virt GICv3 layout: GICD 0x0800_0000, GICR 0x080A_0000, per-core frame stride
     * 0x2_0000. nr_irq = 1024 is the INTID upper bound, not an index table. */
    static const br_gicv3_cfg_t cfg = {
        .gicd_base       = 0x08000000ul,
        .gicr_base       = 0x080A0000ul,
        .gicr_frame_size = 0x20000u,
        .gicr_max_frames = 8u,
        .nr_irq          = 1024u,
    };
    int pic_id;
    int rc;

    /* Register the PIC (internally initializes the GICv3 CPU interface/distributor/
     * redistributor); failure = boot failure, negative errno propagated. */
    pic_id = br_gicv3_register(&cfg);
    if (pic_id < 0) {
        return pic_id;
    }

    /* Submit the binding table, after which virq<->hwirq resolves. A duplicate submit is
     * reported by core as -EBUSY (EARLY submits exactly once). */
    rc = br_irq_bindings_set(s_board_irq, BR_ARRAY_SIZE(s_board_irq));
    if (rc != 0) {
        return rc;
    }
    return 0;
}

/* ---- 3. Arch timer arm/rearm (EL1 physical timer, PPI INTID 30) ----
 * The condition line is level: an ISR that acks/eois without rewriting TVAL leaves the condition
 * asserted => pending again => livelock, so rearm is mandatory rather than an optimization. */
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
 * Microseconds -> ticks, rounded up: ((us * freq) + 999999) / 1000000. Rounding down would expire
 * earlier than requested, the unsafe direction for tickless earliest-deadline semantics.
 * freq comes from CNTFRQ_EL0 (QEMU virt = 62.5 MHz).
 */
static br_u64 br_timer_us_to_ticks(br_u64 period_us)
{
    br_u64 freq = br_plat_ticks_freq();
    return ((period_us * freq) + 999999u) / 1000000u;
}

static void br_timer_rearm(br_u64 period_us)
{
    /* Called from the ISR: rewrite TVAL to clear the condition and schedule the next deadline. */
    br_timer_write_tval(br_timer_us_to_ticks(period_us));
}

void br_plat_timer_stop(void)
{
    br_timer_write_ctl(BR_CNTP_CTL_DISABLE);
}

/* ---- 4. Timer PPI ISR + enable + arm + global IRQ enable ----
 * ISR lives in platform: interrupt control outranks APP (P0), so APP must not register lines. */
static void br_plat_timer_isr(void *arg)
{
    (void)arg;
    /* Heartbeat count belongs to core (platform only notifies): account first, then rearm. */
    br_clock_tick_notify();
    br_sched_on_tick(br_clock_now());
    br_timer_rearm(BR_BOARD_TIMER_PERIOD_US);
}

br_u32 br_plat_timer_ticks(void)
{
    /* Forwards to core's single source of truth; there is no second count here. */
    return br_clock_tick_count();
}

int br_plat_irq_start(void)
{
    const br_irq_attr_t attr = {
        .prio    = BR_IRQ_PRIO_DEFAULT,   /* use the binding table's static default */
        .trigger = BR_IRQ_TRIG_DEFAULT,   /* likewise (PPI is level-type, set by board data) */
        /* PERCPU must match the binding table. NO_STORM_GUARD: the timer is a known legitimate
         * periodic high-frequency line and would otherwise exceed the 25% storm share. */
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

    /* Global IRQ enable, the last init step; the manager already did it, this is idempotent. */
    br_irq_cpu_enable();
    return 0;
}

/* ---- 5. Conformance selftest channel (implemented in irq_conf.c) ---- */

/* Software-trigger a virq bound to an SGI: no binding -> -ENODEV; not an SGI -> -ENOTSUP. */
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

/* virq -> hwirq lookup, for the conformance shadow-vs-hardware cross-check. */
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
