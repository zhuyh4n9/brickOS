/*
 * GICv3 PIC driver (ISA-layer dialect): register/bitfield access, init sequence, ack/eoi, masking
 * and prio/trigger/affinity config. Holds no board facts (GICD/GICR bases, frame stride and the
 * INTID binding table live in board_irq.c / plat_qemu_virt.c). ack/eoi/mask/unmask/cpu_mask/
 * cpu_unmask touch only registers and never block, so they are callable from an ISR. EOImode must
 * be 0 (one eoi completes); ICC_IAR1_EL1 reads have side effects and are read exactly once.
 * WORKAROUND(br-wa-isa-001): no QEMU/board facts in this file.
 */
#include <br/core/br_console.h>
#include <br/core/br_error.h>
#include <br/hal/br_irq.h>      /* BR_IRQ_TRIG_*: shared trigger encoding */
#include <br/hal/br_pic.h>
#include <br/platform/br_gicv3.h>

/* ---- 1. GICD (distributor) registers and bitfields (offsets from GICD base) ---- */
#define BR_GICD_CTLR        0x0000u  /* GICD_CTLR      Control                     */
#define BR_GICD_TYPER       0x0004u  /* GICD_TYPER     Type                        */
#define BR_GICD_ISENABLER   0x0100u  /* GICD_ISENABLER<n>  Set-Enable (4 B per 32 lines) */
#define BR_GICD_ICENABLER   0x0180u  /* GICD_ICENABLER<n>  Clear-Enable            */
#define BR_GICD_ISPENDR     0x0200u  /* GICD_ISPENDR<n>    Set-Pending             */
#define BR_GICD_ISACTIVER   0x0300u  /* GICD_ISACTIVER<n>  Set-Active              */
#define BR_GICD_IPRIORITYR  0x0400u  /* GICD_IPRIORITYR    Priority (byte-addressed) */
#define BR_GICD_ICFGR       0x0C00u  /* GICD_ICFGR<n>      Configuration (2 bits per line) */
#define BR_GICD_IGROUPR     0x0080u  /* GICD_IGROUPR<n>    Group (1 bit per line; SPI range) */
#define BR_GICD_IGRPMODR    0x0D00u  /* GICD_IGRPMODR<n>   Group1 secure/non-secure split */
#define BR_GICD_IROUTER     0x6000u  /* GICD_IROUTER<n>    Routing (8 B per SPI, valid with ARE) */

#define BR_GICD_CTLR_ENABLE_GRP1NS  (1u << 1)   /* EnableGrp1NS: enable Group1 non-secure */
#define BR_GICD_CTLR_ARE_NS         (1u << 4)   /* ARE_NS: affinity routing (otherwise IROUTER has no effect) */
#define BR_GICD_CTLR_RWP            (1u << 31)  /* RWP: register write not yet effective (read-only) */

#define BR_GICD_TYPER_ITLINES_SHIFT 5u           /* ITLinesNumber bitfield position */
#define BR_GICD_TYPER_ITLINES_MASK  0x1Fu

/* GICD_IROUTER IRM (bit31): this driver only uses "route to given affinity" (IRM=0). */
#define BR_GICD_IROUTER_IRM         (1ull << 31)

/* ---- 2. GICR (redistributor) registers and bitfields (one per CPU, 2x64 KiB) ---- */
#define BR_GICR_TYPER               0x0008u  /* GICR_TYPER (64-bit: 0x8 low word / 0xC high word) */
#define BR_GICR_TYPER_LAST          (1u << 4) /* Last: this frame is the last of a contiguous frame array */
#define BR_GICR_WAKER               0x0014u  /* GICR_WAKER  Control */
#define BR_GICR_WAKER_PROCESSOR_SLEEP (1u << 1) /* ProcessorSleep: software sets it to put this CPU to sleep */
#define BR_GICR_WAKER_CHILDREN_ASLEEP (1u << 2) /* ChildrenAsleep: hardware reports "asleep" */

/* SGI_base = RD_base + 64 KiB; INTID 0-31 config lives in this half-frame. */
#define BR_GICR_SGI_OFFSET  0x10000u
#define BR_GICR_IGROUPR0    0x0080u  /* GICR_IGROUPR0   Group for INTID 0–31 */
#define BR_GICR_ISENABLER0  0x0100u  /* GICR_ISENABLER0 INTID 0–31 enable */
#define BR_GICR_ICENABLER0  0x0180u  /* GICR_ICENABLER0 INTID 0–31 mask */
#define BR_GICR_ISPENDR0    0x0200u  /* GICR_ISPENDR0   INTID 0–31 pending */
#define BR_GICR_ISACTIVER0  0x0300u  /* GICR_ISACTIVER0 INTID 0–31 active */
#define BR_GICR_IPRIORITYR  0x0400u  /* GICR_IPRIORITYR INTID 0–31 priority (byte) */
#define BR_GICR_ICFGR0      0x0C00u  /* GICR_ICFGR0     SGI (0–15): always edge */
#define BR_GICR_ICFGR1      0x0C04u  /* GICR_ICFGR1     PPI (16–31): level/edge */
#define BR_GICR_IGRPMODR0   0x0D00u  /* GICR_IGRPMODR0  0 = Group1 NS */

/* ---- 3. Generic bit-manipulation constants ---- */
#define BR_GIC_INTID_PER_WORD 32u                                     /* each enable/pending register covers 32 lines */
#define BR_GIC_WORD_INDEX(id) ((id) / BR_GIC_INTID_PER_WORD)          /* which 4 B word */
#define BR_GIC_WORD_BIT(id)   (1u << ((id) % BR_GIC_INTID_PER_WORD))  /* bit within the word */
#define BR_GIC_ICFGR_WORDS    16u                                     /* each ICFGR word covers 16 lines */
#define BR_GIC_ICFGR_SHIFT(id) (2u * ((id) % BR_GIC_ICFGR_WORDS))     /* 2 bits per line */
#define BR_GIC_ICFGR_MASK      3u

/* Local copy of PSTATE.I (the ISA layer avoids core headers). */
#define BR_AARCH64_DAIF_I 0x080u   /* PSTATE.I (arch value of br_exc.h BR_DAIF_I) */

/* ---- 4. MMIO and system register access primitives ---- */

/* GIC MMIO must be volatile; GICv3 registers are little-endian 32/64-bit. */
static inline br_u32 br_gic_mmio_read32(br_uintptr_t base, br_u32 off)
{
    return *(volatile br_u32 *)(base + off);
}

static inline void br_gic_mmio_write32(br_uintptr_t base, br_u32 off, br_u32 val)
{
    *(volatile br_u32 *)(base + off) = val;
}

/* Priority registers are byte-addressable. */
static inline br_u8 br_gic_mmio_read8(br_uintptr_t base, br_u32 off)
{
    return *(volatile br_u8 *)(base + off);
}

static inline void br_gic_mmio_write8(br_uintptr_t base, br_u32 off, br_u8 val)
{
    *(volatile br_u8 *)(base + off) = val;
}

/* GICD_IROUTER is 64-bit. */
static inline void br_gic_mmio_write64(br_uintptr_t base, br_u32 off, br_u64 val)
{
    *(volatile br_u64 *)(base + off) = val;
}

/*
 * Barriers: ISB after ICC_SRE_EL1 (it changes how later ICC_* accesses are interpreted); DSB
 * after GICD_CTLR (the MMIO write must reach the distributor before the RWP poll is meaningful);
 * ISB after ICC_PMR_EL1 / ICC_CTLR_EL1 (they drive interrupt delivery).
 */
static inline void br_gic_dsb_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void br_gic_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

/* ---- ICC_* system registers (GICv3 CPU interface) ---- */

/* ICC_SRE_EL1: SRE(bit0)=1 => ICC_* uses system registers. */
static inline br_u64 br_gic_read_icc_sre_el1(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(v));
    return v;
}

static inline void br_gic_write_icc_sre_el1(br_u64 v)
{
    __asm__ volatile("msr icc_sre_el1, %0" ::"r"(v) : "memory");
}

/* ICC_CTLR_EL1: EOImode(bit1) / PRIbits([10:8]). */
static inline br_u64 br_gic_read_icc_ctlr_el1(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, icc_ctlr_el1" : "=r"(v));
    return v;
}

static inline void br_gic_write_icc_ctlr_el1(br_u64 v)
{
    __asm__ volatile("msr icc_ctlr_el1, %0" ::"r"(v) : "memory");
}

/* ICC_PMR_EL1: priority mask; only priorities above the threshold are delivered. */
static inline void br_gic_write_icc_pmr_el1(br_u64 v)
{
    __asm__ volatile("msr icc_pmr_el1, %0" ::"r"(v) : "memory");
}

/* ICC_IGRPEN1_EL1: master switch for Group1 interrupts. */
static inline void br_gic_write_icc_igrpen1_el1(br_u64 v)
{
    __asm__ volatile("msr icc_igrpen1_el1, %0" ::"r"(v) : "memory");
}

/* ICC_IAR1_EL1 reads have side effects (Pending->Active). Read exactly once per entry: a second
 * read returns 1023 and corrupts the state machine. Called only from gicv3_ack(). */
static inline br_u64 br_gic_read_icc_iar1_el1(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(v));
    return v;
}

/* ICC_EOIR1_EL1: with EOImode=0, priority drop + deactivate in one step. */
static inline void br_gic_write_icc_eoir1_el1(br_u64 v)
{
    __asm__ volatile("msr icc_eoir1_el1, %0" ::"r"(v) : "memory");
}

/* ICC_SGI1R_EL1: software generated interrupt (Group1), one 64-bit store. */
static inline void br_gic_write_icc_sgi1r_el1(br_u64 v)
{
    __asm__ volatile("msr icc_sgi1r_el1, %0" ::"r"(v) : "memory");
}

/* MPIDR_EL1: this CPU's affinity (Aff3..Aff0). */
static inline br_u64 br_gic_read_mpidr_el1(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v;
}

/* ---- 5. Instance storage ---- */
/* struct br_pic is opaque in core; the full definition lives here (platform-owned storage). */
struct br_pic {
    /* platform data (copied at register time) */
    br_uintptr_t gicd_base;
    br_uintptr_t gicr_base;
    br_u32       gicr_frame_size;   /* per-CPU frame stride (2 × 64 KiB) */
    br_u32       gicr_max_frames;   /* upper bound of the affinity scan */
    br_u32       nr_irq;            /* INTID upper bound (reported to caps) */

    /* facts discovered by init */
    br_uintptr_t gicr_frame;        /* this CPU's RD_base (affinity match result) */
    br_uintptr_t gicr_sgi;          /* this CPU's SGI_base = RD_base + 64 KiB */
    br_u64       mpidr;             /* raw MPIDR_EL1 (for building SGI targets) */
    br_u32       spi_lines;         /* SPI line count reported by GICD_TYPER */
    br_u32       caps;              /* BR_PIC_CAP_* snapshot */

    /* facts read from ICC_CTLR_EL1 */
    br_u8        prio_bits;
    br_u8        eoimode;
    br_u8        ready;             /* init completed */
    br_u8        rsv;
};

static struct br_pic       s_gicv3;      /* the only instance (zero-initialized BSS) */
static br_gicv3_info_t     s_info;       /* out param of br_gicv3_info */

/* RWP/wake polling limit; guards against an infinite loop, not precise timing. */
#define BR_GICV3_POLL_SPINS 1000000u

/* Arch minimum significant priority bits; lower bound when PRIbits reads abnormally small. */
#define BR_GICV3_PRIO_BITS_MIN 5u
#define BR_GICV3_PRIO_BITS_MAX 8u

/* ---- 6. Initialization (EARLY phase, interrupts globally disabled) ---- */
static int gicv3_init(br_pic_t *pic)
{
    br_u64 sre;
    br_u32 ctlr;
    br_u32 spins;
    br_u32 i;
    br_u32 packed_aff;
    br_u32 typer;
    br_u32 ctlr_el1;
    br_uintptr_t frame = 0u;
    br_u32 frame_found = 0u;

    /* Step 1: ICC_SRE_EL1.SRE=1. Without it every later ICC_* access may trap, so there is no
     * honest degraded mode: fail with -EIO. ISB: this bit changes instruction interpretation. */
    sre = br_gic_read_icc_sre_el1();
    sre |= 1u;
    br_gic_write_icc_sre_el1(sre);
    br_gic_isb();
    if ((br_gic_read_icc_sre_el1() & 1u) == 0u) {
        return BR_ERR(EIO);
    }

    /* Step 2: GICD_CTLR = ARE_NS | EnableGrp1NS (affinity routing, distributor-wide non-secure
     * Group1). RWP register: wait for RWP=0, else redistributor config races unaccepted writes. */
    br_gic_mmio_write32(pic->gicd_base, BR_GICD_CTLR,
                       BR_GICD_CTLR_ARE_NS | BR_GICD_CTLR_ENABLE_GRP1NS);
    br_gic_dsb_sy();   /* make sure this MMIO write reached the distributor before polling */
    for (spins = 0u; spins < BR_GICV3_POLL_SPINS; spins++) {
        ctlr = br_gic_mmio_read32(pic->gicd_base, BR_GICD_CTLR);
        if (((ctlr & (BR_GICD_CTLR_ARE_NS | BR_GICD_CTLR_ENABLE_GRP1NS)) ==
             (BR_GICD_CTLR_ARE_NS | BR_GICD_CTLR_ENABLE_GRP1NS)) &&
            ((ctlr & BR_GICD_CTLR_RWP) == 0u)) {
            break;
        }
    }
    if (spins == BR_GICV3_POLL_SPINS) {
        return BR_ERR(ETIMEDOUT);
    }

    /* Step 3: match this CPU's redistributor frame by affinity. GICR_TYPER packs it in the high
     * word (Aff0[39:32]..Aff3[63:56]); flatten MPIDR's Aff0..Aff3 bytes plus bit31 and compare. */
    pic->mpidr = br_gic_read_mpidr_el1();
    packed_aff = (br_u32)(((pic->mpidr & 0xFF00000000ull) >> 8) |
                          (pic->mpidr & 0xFFFFFFull));

    for (i = 0u; i < pic->gicr_max_frames; i++) {
        br_uintptr_t base = pic->gicr_base + (br_uintptr_t)i * pic->gicr_frame_size;
        br_u32 typer_lo = br_gic_mmio_read32(base, BR_GICR_TYPER);
        br_u32 typer_hi = br_gic_mmio_read32(base, BR_GICR_TYPER + 4u);

        if (typer_hi == packed_aff) {
            frame = base;
            frame_found = 1u;
        }
        /* Last = final frame of the contiguous array => stop. */
        if ((typer_lo & BR_GICR_TYPER_LAST) != 0u) {
            break;
        }
    }
    if (frame_found == 0u) {
        return BR_ERR(ENODEV);   /* no frame for this CPU within the scan range */
    }
    pic->gicr_frame = frame;
    pic->gicr_sgi   = frame + BR_GICR_SGI_OFFSET;

    /* Step 4: wake this CPU's redistributor (ProcessorSleep=0) and wait for ChildrenAsleep=0;
     * configuring before the wake completes may drop the configuration. */
    {
        br_u32 waker = br_gic_mmio_read32(frame, BR_GICR_WAKER);
        waker &= ~BR_GICR_WAKER_PROCESSOR_SLEEP;
        br_gic_mmio_write32(frame, BR_GICR_WAKER, waker);
        br_gic_dsb_sy();
        for (spins = 0u; spins < BR_GICV3_POLL_SPINS; spins++) {
            if ((br_gic_mmio_read32(frame, BR_GICR_WAKER) &
                 BR_GICR_WAKER_CHILDREN_ASLEEP) == 0u) {
                break;
            }
        }
        if (spins == BR_GICV3_POLL_SPINS) {
            return BR_ERR(ETIMEDOUT);
        }
    }

    /* Step 5: SGI frame init. IGROUPR0=all ones (private lines in Group1 NS, matching
     * EnableGrp1NS; Group0 lines are never delivered). IGRPMODR0=0 (no secure split). ICFGR0=0
     * (SGI always edge). ICFGR1=0: PPI defaults to level -- the arch timer is a level line. */
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_IGROUPR0, 0xFFFFFFFFu);
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_IGRPMODR0, 0u);
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_ICFGR0, 0u);
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_ICFGR1, 0u);

    /* Step 5b: the SPI range must also be Group1 NS. GICD_IGROUPR resets to Group0 while this CPU
     * admits only Group1 => no SPI is ever delivered (exposed by eMMC over PCIe INTx, SPI 38).
     * n=0 covers the private GICR lines, so start at n=1; out-of-range writes are RAZ/WI. */
    for (br_u32 n = 1u; n < (pic->nr_irq / 32u); n++) {
        br_gic_mmio_write32(pic->gicd_base, BR_GICD_IGROUPR + n * 4u, 0xFFFFFFFFu);
        br_gic_mmio_write32(pic->gicd_base, BR_GICD_IGRPMODR + n * 4u, 0u);
    }
    br_gic_dsb_sy();

    /* Step 6: ICC_PMR_EL1 = 0xFF (admit all implemented priorities); reset 0x00 masks everything.
     * PMR implements only the top prio_bits (0xFF reads back 0xF8 with 5 bits) and delivery needs
     * priority < PMR, so the 0xF8 level is permanently masked -- use a priority <= 0xF0. */
    br_gic_write_icc_pmr_el1((br_u64)BR_GICV3_PMR_ALL);
    br_gic_isb();

    /* Step 7: EOImode=0; core's eoi model is "a single eoi completes" (EOImode=1 would also
     * require ICC_DIR_EL1 to be written). */
    ctlr_el1 = (br_u32)br_gic_read_icc_ctlr_el1();
    ctlr_el1 &= ~(1u << 1);            /* EOImode = 0 */
    br_gic_write_icc_ctlr_el1((br_u64)ctlr_el1);
    br_gic_isb();
    ctlr_el1 = (br_u32)br_gic_read_icc_ctlr_el1();
    pic->eoimode = (br_u8)((ctlr_el1 >> 1) & 1u);

    /* PRIbits([10:8]) = implemented bits - 1; clamp to the arch minimum because non-conformant
     * implementations may report 0 ("1 bit"), which would quantize priorities to 2 levels. */
    {
        br_u32 pribits = ((ctlr_el1 >> 8) & 0x7u) + 1u;
        if (pribits < BR_GICV3_PRIO_BITS_MIN) {
            pribits = BR_GICV3_PRIO_BITS_MIN;
        }
        if (pribits > BR_GICV3_PRIO_BITS_MAX) {
            pribits = BR_GICV3_PRIO_BITS_MAX;
        }
        pic->prio_bits = (br_u8)pribits;
    }
    if (pic->eoimode != 0u) {
        /* RO and nonzero => the single-eoi assumption is broken; must be visible. */
        br_console_puts("[gicv3] WARN: ICC_CTLR_EL1.EOImode != 0 (single-eoi model broken)\n");
    }

    /* Step 8: ICC_IGRPEN1_EL1 = 1 (CPU interface admits Group1). */
    br_gic_write_icc_igrpen1_el1(1u);
    br_gic_isb();

    /* Step 9: SPI line count from GICD_TYPER ITLinesNumber: (value + 1) * 32. */
    typer = br_gic_mmio_read32(pic->gicd_base, BR_GICD_TYPER);
    pic->spi_lines = (((typer >> BR_GICD_TYPER_ITLINES_SHIFT) &
                       BR_GICD_TYPER_ITLINES_MASK) + 1u) * BR_GIC_INTID_PER_WORD;

    /* Capability snapshot; per-bit rationale in gicv3_caps_get(). */
    pic->caps = BR_PIC_CAP_MASK | BR_PIC_CAP_LATCH | BR_PIC_CAP_READBACK |
                BR_PIC_CAP_PRIO | BR_PIC_CAP_TRIG_EDGE | BR_PIC_CAP_SW_TRIGGER |
                BR_PIC_CAP_PER_CPU | BR_PIC_CAP_AFFINITY;
    pic->ready = 1u;

    /* Diagnostic snapshot (br_gicv3_info out param). */
    s_info.prio_bits = pic->prio_bits;
    s_info.eoimode   = pic->eoimode;
    s_info.spi_lines = pic->spi_lines;
    s_info.mpidr     = (br_u32)pic->mpidr;

    return 0;
}

/* ---- 7. caps_get ---- */
static int gicv3_caps_get(br_pic_t *pic, br_pic_caps_t *out)
{
    if (pic == BR_NULL || out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    out->prio_bits = pic->prio_bits;
    out->nr_irq    = pic->nr_irq;
    /* Hardware facts: MASK (GICR/GICD ICENABLER), LATCH (GIC latches pending while masked),
     * READBACK (ISPENDR/ISACTIVER/ISENABLER readable), PRIO, TRIG_EDGE, SW_TRIGGER (SGI), PER_CPU,
     * AFFINITY (SPI routable via GICD_IROUTER). Not reported: NEST, TRIG_BOTH, RESYNC. */
    out->caps = pic->caps;
    return 0;
}

/* ---- 8. Acknowledge and end (hot path) ---- */
static br_u32 gicv3_ack(br_pic_t *pic)
{
    br_u32 iar;

    (void)pic;   /* ack has no parameter source: the only entry is this CPU's interface */

    /* Only read of ICC_IAR1_EL1 in the driver; a second read returns 1023 and corrupts state. */
    iar = (br_u32)br_gic_read_icc_iar1_el1();

    /* Map the whole 1020-1023 range to BR_PIC_NO_IRQ: otherwise core looks 1020-1022 up as a
     * legitimate hwirq, misses, takes the "already claimed" branch and writes ICC_EOIR1_EL1 for
     * a special INTID, which is undefined. */
    if (iar >= BR_GICV3_INTID_SPECIAL && iar <= BR_GICV3_INTID_MAX) {
        return BR_PIC_NO_IRQ;
    }
    return iar;
}

static void gicv3_eoi(br_pic_t *pic, br_u32 hwirq)
{
    (void)pic;

    /* EOImode=0 (asserted by init) => one write does priority drop + deactivate. No DSB needed:
     * ICC_EOIR1_EL1 is a system register, not an MMIO device write. The ISB orders the priority
     * drop before later instructions. */
    br_gic_write_icc_eoir1_el1((br_u64)hwirq);
    br_gic_isb();
}

/* ---- 9. Single-line masking (MMIO only => ISR-callable) ---- */
static void gicv3_mask(br_pic_t *pic, br_u32 hwirq)
{
    if (hwirq >= pic->nr_irq) {
        return;   /* void hot path: cannot report out-of-range, but never writes out of range */
    }
    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_ICENABLER0, BR_GIC_WORD_BIT(hwirq));
    } else {
        br_gic_mmio_write32(pic->gicd_base,
                            BR_GICD_ICENABLER + BR_GIC_WORD_INDEX(hwirq) * 4u,
                            BR_GIC_WORD_BIT(hwirq));
    }
}

static void gicv3_unmask(br_pic_t *pic, br_u32 hwirq)
{
    if (hwirq >= pic->nr_irq) {
        return;
    }
    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_ISENABLER0, BR_GIC_WORD_BIT(hwirq));
    } else {
        br_gic_mmio_write32(pic->gicd_base,
                            BR_GICD_ISENABLER + BR_GIC_WORD_INDEX(hwirq) * 4u,
                            BR_GIC_WORD_BIT(hwirq));
    }
}

/* ---- 10. State queries (diagnostic, not hot path) ---- */
static int gicv3_pending(br_pic_t *pic, br_u32 hwirq, int *out)
{
    br_u32 val;

    if (pic == BR_NULL || out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(EINVAL);
    }
    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        val = br_gic_mmio_read32(pic->gicr_sgi, BR_GICR_ISPENDR0);
    } else {
        val = br_gic_mmio_read32(pic->gicd_base,
                                 BR_GICD_ISPENDR + BR_GIC_WORD_INDEX(hwirq) * 4u);
    }
    *out = ((val & BR_GIC_WORD_BIT(hwirq)) != 0u) ? 1 : 0;
    return 0;
}

static int gicv3_active(br_pic_t *pic, br_u32 hwirq, int *out)
{
    br_u32 val;

    if (pic == BR_NULL || out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(EINVAL);
    }
    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        val = br_gic_mmio_read32(pic->gicr_sgi, BR_GICR_ISACTIVER0);
    } else {
        val = br_gic_mmio_read32(pic->gicd_base,
                                 BR_GICD_ISACTIVER + BR_GIC_WORD_INDEX(hwirq) * 4u);
    }
    *out = ((val & BR_GIC_WORD_BIT(hwirq)) != 0u) ? 1 : 0;
    return 0;
}

/* ---- 11. Configuration (thread context only) ---- */
static int gicv3_set_prio(br_pic_t *pic, br_u32 hwirq, br_u8 prio)
{
    br_u32 step;
    br_u32 enc;

    if (pic == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(EINVAL);
    }
    if (pic->prio_bits == 0u) {
        return BR_ERR(ENOTSUP);   /* this is the path when CAP_PRIO was not reported */
    }

    /*
     * Logical priority (small = high) -> hardware encoding with significant bits at the MSB
     * (5 bits => step 8). Quantize toward lower priority, never silently raise:
     * enc = (prio + step - 1) & ~(step - 1). The cap is the lowest deliverable level: delivery
     * needs prio < PMR, PMR implements only the top prio_bits and 0xFF reads back 0xF8 with
     * 5 bits, so the 0xF8 encoding is permanently masked; lowering it to 0xF0 would be a raise,
     * hence -ENOTSUP. Deliverable lines need a logical priority <= 0xF0.
     */
    step = 1u << (BR_GICV3_PRIO_BITS_MAX - pic->prio_bits);
    enc  = ((br_u32)prio + step - 1u) & ~(step - 1u);

    const br_u32 deliverable_max = (0xFFu & ~(step - 1u)) - step;
    if (enc > deliverable_max) {
        return BR_ERR(ENOTSUP);
    }

    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        /* INTID 0-31 priorities live in the GICR SGI frame (byte-addressed). */
        br_gic_mmio_write8(pic->gicr_sgi, BR_GICR_IPRIORITYR + hwirq, (br_u8)enc);
    } else {
        /* GICD_IPRIORITYR is byte-addressed by INTID; no 4-byte grouping needed. */
        br_gic_mmio_write8(pic->gicd_base, BR_GICD_IPRIORITYR + hwirq, (br_u8)enc);
    }
    return 0;
}

static int gicv3_set_trigger(br_pic_t *pic, br_u32 hwirq, br_u8 trigger)
{
    br_u32 edge;
    br_u32 base;
    br_u32 off;
    br_u32 shift;
    br_u32 cfg;

    if (pic == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(EINVAL);
    }

    /* DEFAULT = core already resolved it from the binding table. */
    if (trigger == BR_IRQ_TRIG_DEFAULT) {
        return 0;
    }

    /* SGI (0-15) is always edge and ICFGR0 has no effect on it. */
    if (hwirq <= BR_GICV3_INTID_SGI_MAX) {
        return (trigger == BR_IRQ_TRIG_EDGE_RISE) ? 0 : BR_ERR(ENOTSUP);
    }

    switch (trigger) {
    case BR_IRQ_TRIG_LEVEL_HIGH:
    case BR_IRQ_TRIG_LEVEL_LOW:
        /* ICFGR expresses only "level/edge", not polarity; polarity is board data
         * (BR_IRQB_F_ACTIVE_LOW) => both LEVEL variants map to level. */
        edge = 0u;
        break;
    case BR_IRQ_TRIG_EDGE_RISE:
        edge = 1u;
        break;
    default:
        /* EDGE_FALL / EDGE_BOTH are unsupported by GICv3: never silently rewritten. */
        return BR_ERR(ENOTSUP);
    }

    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        /* PPI (16-31): GICR_ICFGR1, bit = 2 * (intid - 16). */
        base  = pic->gicr_sgi;
        off   = BR_GICR_ICFGR1;
        shift = BR_GIC_ICFGR_SHIFT(hwirq - BR_GICV3_INTID_PPI_BASE);
    } else {
        /* SPI: GICD_ICFGR<n>, one word per 16 lines. */
        base  = pic->gicd_base;
        off   = BR_GICD_ICFGR + (hwirq / BR_GIC_ICFGR_WORDS) * 4u;
        shift = BR_GIC_ICFGR_SHIFT(hwirq);
    }

    cfg  = br_gic_mmio_read32(base, off);
    cfg &= ~(BR_GIC_ICFGR_MASK << shift);
    cfg |= (edge << shift);
    br_gic_mmio_write32(base, off, cfg);
    return 0;
}

/*
 * Software-triggered SGI (INTID 0-15 only). ICC_SGI1R_EL1: [15:0] TargetList (Aff0 bitmap),
 * [23:16] Aff1, [27:24] INTID, [39:32] Aff2, [55:48] Aff3, [63] IRM (0 = by affinity).
 * RS is deprecated (written 0); no range selector is used.
 */
static void gicv3_sgi1r_write(const struct br_pic *pic, br_u32 intid)
{
    br_u64 val;

    val  = ((br_u64)((pic->mpidr >> 8) & 0xFFull)) << 16;  /* Aff1 */
    val |= ((br_u64)((pic->mpidr >> 16) & 0xFFull)) << 32; /* Aff2 */
    val |= ((br_u64)((pic->mpidr >> 32) & 0xFFull)) << 48; /* Aff3 */
    val |= ((br_u64)intid) << 24;                          /* INTID */
    /* TargetList: this CPU only. The bitmap is 16 bits (Aff0 = 0..15), so masking with 0xF is
     * arch-conformant and avoids an out-of-range shift. */
    val |= (br_u64)1u << (pic->mpidr & 0xFull);
    br_gic_write_icc_sgi1r_el1(val);
    br_gic_isb();   /* get the SGI delivered as early as possible (self-tests need it now) */
}

static int gicv3_trigger(br_pic_t *pic, br_u32 hwirq)
{
    if (pic == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (hwirq > BR_GICV3_INTID_SGI_MAX) {
        return BR_ERR(ENOTSUP);   /* only SGI can be software triggered */
    }
    gicv3_sgi1r_write(pic, hwirq);
    return 0;
}

/* ---- 12. CPU-side masking (dialect wrapper over PSTATE.I) ---- */
static br_u32 gicv3_cpu_mask(br_pic_t *pic)
{
    br_u64 daif;

    (void)pic;

    /* Raw DAIF; core's br_irq_state_t interprets the bit layout. */
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");   /* #2 = bit1 = PSTATE.I */
    return (br_u32)daif;
}

static void gicv3_cpu_unmask(br_pic_t *pic, br_u32 state)
{
    (void)pic;

    /* v1 manages I only; F/A/D untouched. state is the raw DAIF from cpu_mask(); bit7 = I. */
    if ((state & BR_AARCH64_DAIF_I) != 0u) {
        __asm__ volatile("msr daifset, #2" ::: "memory");
    } else {
        __asm__ volatile("msr daifclr, #2" ::: "memory");
    }
}

/* ---- 13. Affinity (CAP_AFFINITY only) ---- */
static int gicv3_set_affinity(br_pic_t *pic, br_u32 hwirq, br_u32 cpu_mask)
{
    br_u32 aff0 = 0u;

    if (pic == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    /* SGI carries its target per send and PPI is per-CPU private: neither is routable. */
    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        return BR_ERR(ENOTSUP);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(EINVAL);
    }
    if (cpu_mask == 0u) {
        return BR_ERR(EINVAL);
    }
    while (aff0 < 32u && ((cpu_mask >> aff0) & 1u) == 0u) {
        aff0++;
    }
    if (aff0 >= 32u) {
        return BR_ERR(EINVAL);
    }

    /* GICD_IROUTER<n> (8 B per SPI): this prototype only expresses "Aff0 = target CPU index,
     * Aff1/2/3 = 0" (single-cluster QEMU virt), IRM=0 (route by affinity). */
    br_gic_mmio_write64(pic->gicd_base, BR_GICD_IROUTER + hwirq * 8u, (br_u64)aff0);
    return 0;
}

/* ---- 14. ops table ---- */
static const br_pic_ops_t s_gicv3_ops = {
    .name = "gicv3",
    .init = gicv3_init,
    .caps_get = gicv3_caps_get,

    .ack = gicv3_ack,
    .eoi = gicv3_eoi,

    .mask = gicv3_mask,
    .unmask = gicv3_unmask,

    .pending = gicv3_pending,
    .active = gicv3_active,

    .set_prio = gicv3_set_prio,
    .set_trigger = gicv3_set_trigger,
    .trigger = gicv3_trigger,

    .cpu_mask = gicv3_cpu_mask,
    .cpu_unmask = gicv3_cpu_unmask,

    /* set_running_prio: only with CAP_NEST, which v1 never reports. */
    .set_running_prio = BR_NULL,

    .set_affinity = gicv3_set_affinity,

    /* save/restore: only with CAP_RESYNC (v2 PM). */
    .save = BR_NULL,
    .restore = BR_NULL,
};

/* ---- 15. Public surface (br_gicv3.h) ---- */
int br_gicv3_register(const br_gicv3_cfg_t *cfg)
{
    int pic_id;
    int rc;

    if (cfg == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (cfg->gicd_base == 0u || cfg->gicr_base == 0u) {
        return BR_ERR(EINVAL);
    }
    if (cfg->gicr_frame_size == 0u || cfg->gicr_max_frames == 0u) {
        return BR_ERR(EINVAL);
    }
    if (cfg->nr_irq == 0u || cfg->nr_irq > (BR_GICV3_INTID_MAX + 1u)) {
        return BR_ERR(EINVAL);
    }

    /* Copy platform data; the ISA layer keeps no pointer to it. */
    s_gicv3.gicd_base       = cfg->gicd_base;
    s_gicv3.gicr_base       = cfg->gicr_base;
    s_gicv3.gicr_frame_size = cfg->gicr_frame_size;
    s_gicv3.gicr_max_frames = cfg->gicr_max_frames;
    s_gicv3.nr_irq          = cfg->nr_irq;
    s_gicv3.gicr_frame      = 0u;
    s_gicv3.gicr_sgi        = 0u;
    s_gicv3.ready           = 0u;

    /* EARLY phase hardware init; failure = boot failure (negative errno propagates). */
    rc = gicv3_init(&s_gicv3);
    if (rc != 0) {
        return rc;
    }

    pic_id = br_pic_register(&s_gicv3, &s_gicv3_ops);
    if (pic_id < 0) {
        return pic_id;   /* -ENOSPC / -EBUSY / -EINVAL propagate as-is */
    }
    return pic_id;
}

void br_gicv3_info(br_gicv3_info_t *out)
{
    if (out == BR_NULL) {
        return;
    }
    *out = s_info;
}

int br_gicv3_sgi_trigger(br_u32 intid)
{
    if (s_gicv3.ready == 0u) {
        return BR_ERR(ENODEV);
    }
    if (intid > BR_GICV3_INTID_SGI_MAX) {
        return BR_ERR(EINVAL);
    }
    gicv3_sgi1r_write(&s_gicv3, intid);
    return 0;
}

/* ---- Cross-check readback (conformance/debug only) ---- */
int br_gicv3_readback_enabled(br_u32 intid, int *out)
{
    br_u32 val;

    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (s_gicv3.ready == 0u) {
        return BR_ERR(ENODEV);
    }
    if (intid >= s_gicv3.nr_irq) {
        return BR_ERR(EINVAL);
    }
    if (intid < BR_GICV3_INTID_SPI_BASE) {
        val = br_gic_mmio_read32(s_gicv3.gicr_sgi, BR_GICR_ISENABLER0);
    } else {
        val = br_gic_mmio_read32(s_gicv3.gicd_base,
                                 BR_GICD_ISENABLER + BR_GIC_WORD_INDEX(intid) * 4u);
    }
    *out = ((val & BR_GIC_WORD_BIT(intid)) != 0u) ? 1 : 0;
    return 0;
}

int br_gicv3_readback_prio(br_u32 intid, br_u8 *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (s_gicv3.ready == 0u) {
        return BR_ERR(ENODEV);
    }
    if (intid >= s_gicv3.nr_irq) {
        return BR_ERR(EINVAL);
    }
    if (intid < BR_GICV3_INTID_SPI_BASE) {
        *out = br_gic_mmio_read8(s_gicv3.gicr_sgi, BR_GICR_IPRIORITYR + intid);
    } else {
        *out = br_gic_mmio_read8(s_gicv3.gicd_base, BR_GICD_IPRIORITYR + intid);
    }
    return 0;
}

int br_gicv3_readback_pending(br_u32 intid, int *out)
{
    br_u32 val;

    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (s_gicv3.ready == 0u) {
        return BR_ERR(ENODEV);
    }
    if (intid >= s_gicv3.nr_irq) {
        return BR_ERR(EINVAL);
    }
    if (intid < BR_GICV3_INTID_SPI_BASE) {
        val = br_gic_mmio_read32(s_gicv3.gicr_sgi, BR_GICR_ISPENDR0);
    } else {
        val = br_gic_mmio_read32(s_gicv3.gicd_base,
                                 BR_GICD_ISPENDR + BR_GIC_WORD_INDEX(intid) * 4u);
    }
    *out = ((val & BR_GIC_WORD_BIT(intid)) != 0u) ? 1 : 0;
    return 0;
}

/*
 * Conformance/debug only: poke the hardware enable bit directly by INTID, bypassing core's
 * binding/descriptor layer. Exists for TC-IRQ-008, which needs "hardware delivers but core has
 * no owner" -- impossible on the normal path, since an unbound or unregistered line is disabled
 * in the GIC and nobody unmask it. The case therefore enables the line itself, emulating a
 * firmware/bootloader leftover that spurious_owned is meant to catch.
 * Product code must not call this: it bypasses the shadow/depth model.
 */
int br_gicv3_raw_line_enable(br_u32 intid, int enable)
{
    if (s_gicv3.ready == 0u) {
        return BR_ERR(ENODEV);
    }
    if (intid >= s_gicv3.nr_irq) {
        return BR_ERR(EINVAL);
    }

    if (enable != 0) {
        gicv3_unmask(&s_gicv3, intid);
    } else {
        gicv3_mask(&s_gicv3, intid);
    }
    return BR_OK;
}
