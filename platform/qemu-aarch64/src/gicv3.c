/*
 * brickOS prototype v0.1.0 — GICv3 driver (ISA-layer "dialect")
 *
 * Design authority: `docs/3-os-core/3-02-int.md`
 *   §4.1   `br_pic_ops` contract (ack/eoi/mask/unmask/config/CPU-side masking)
 *   §4.1.1 GICv3 EOImode obligation: **must be 0** (priority drop + deactivate in one step);
 *          `ICC_IAR1_EL1` reads have side effects => read exactly once per entry, volatile
 *   §5.3   Map the whole 1020–1023 range to BR_PIC_NO_IRQ (mapping only 1023 makes core EOI special INTIDs)
 *   §7.1   Logical priority (small = high) -> hardware encoding: significant bits at the **MSB**;
 *          when bits are short, saturate toward **lower priority**, never silently raise priority
 *   §8.4   Software trigger (SGI) = self-test channel of the interrupt subsystem
 *   §13.1  SGI/PPI are per-CPU private (not routable), SPI is routable — the boundary of set_affinity
 *
 * ★ Layer boundary (design 1-01 §8 / WORKAROUND(br-wa-isa-001)): this file describes only what
 *   changes with the controller model (register offsets/bitfields/init sequence) and holds **no
 *   QEMU/board facts** (GICD/GICR bases, frame stride, INTID binding table live in board_irq.c /
 *   plat_qemu_virt.c).
 *
 * ★ Hot-path discipline (3-02 §4.1/§14.3): ack/eoi/mask/unmask/cpu_mask/cpu_unmask touch only
 *   registers (MMIO or system registers) and never do anything that can fail or block on a bus —
 *   these must be callable from an ISR.
 */
#include <br/core/br_console.h>
#include <br/core/br_error.h>
#include <br/core/br_irq.h>      /* BR_IRQ_TRIG_*: shared trigger encoding (§8.1, interpreted by the dialect) */
#include <br/core/br_pic.h>
#include <br/platform/br_gicv3.h>

/* =====================================================================
 * 1. GICD (distributor) registers and bitfields (GICv3 arch IHI0069; offsets from GICD base)
 * ===================================================================== */
#define BR_GICD_CTLR        0x0000u  /* GICD_CTLR      Control                     */
#define BR_GICD_TYPER       0x0004u  /* GICD_TYPER     Type                        */
#define BR_GICD_ISENABLER   0x0100u  /* GICD_ISENABLER<n>  Set-Enable (4 B per 32 lines) */
#define BR_GICD_ICENABLER   0x0180u  /* GICD_ICENABLER<n>  Clear-Enable            */
#define BR_GICD_ISPENDR     0x0200u  /* GICD_ISPENDR<n>    Set-Pending             */
#define BR_GICD_ISACTIVER   0x0300u  /* GICD_ISACTIVER<n>  Set-Active              */
#define BR_GICD_IPRIORITYR  0x0400u  /* GICD_IPRIORITYR    Priority(**byte**-addressed) */
#define BR_GICD_ICFGR       0x0C00u  /* GICD_ICFGR<n>      Configuration (2 bits per line) */
#define BR_GICD_IGROUPR     0x0080u  /* GICD_IGROUPR<n>    Group (1 bit per line; SPI range) */
#define BR_GICD_IGRPMODR    0x0D00u  /* GICD_IGRPMODR<n>   Group1 secure/non-secure split */
#define BR_GICD_IROUTER     0x6000u  /* GICD_IROUTER<n>    Routing (8 B per SPI, valid with ARE) */

#define BR_GICD_CTLR_ENABLE_GRP1NS  (1u << 1)   /* EnableGrp1NS: enable Group1 non-secure */
#define BR_GICD_CTLR_ARE_NS         (1u << 4)   /* ARE_NS: affinity routing (otherwise IROUTER has no effect) */
#define BR_GICD_CTLR_RWP            (1u << 31)  /* RWP: register write not yet effective (read-only) */

#define BR_GICD_TYPER_ITLINES_SHIFT 5u           /* ITLinesNumber bitfield position */
#define BR_GICD_TYPER_ITLINES_MASK  0x1Fu

/* GICD_IROUTER IRM (bit31): this driver only uses "route to given affinity" (IRM=0) */
#define BR_GICD_IROUTER_IRM         (1ull << 31)

/* =====================================================================
 * 2. GICR (redistributor) registers and bitfields (one per CPU, 2×64 KiB)
 * ===================================================================== */
#define BR_GICR_TYPER               0x0008u  /* GICR_TYPER (64-bit: 0x8 low word / 0xC high word) */
#define BR_GICR_TYPER_LAST          (1u << 4) /* Last: this frame is the last of a contiguous frame array */
#define BR_GICR_WAKER               0x0014u  /* GICR_WAKER  Control */
#define BR_GICR_WAKER_PROCESSOR_SLEEP (1u << 1) /* ProcessorSleep: software sets it to put this CPU to sleep */
#define BR_GICR_WAKER_CHILDREN_ASLEEP (1u << 2) /* ChildrenAsleep: hardware reports "asleep" */

/* SGI_base = RD_base + 64 KiB; all SGI/PPI (INTID 0–31) config lives in this half-frame */
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

/* =====================================================================
 * 3. Generic bit-manipulation constants
 * ===================================================================== */
#define BR_GIC_INTID_PER_WORD 32u                                     /* each enable/pending register covers 32 lines */
#define BR_GIC_WORD_INDEX(id) ((id) / BR_GIC_INTID_PER_WORD)          /* which 4 B word */
#define BR_GIC_WORD_BIT(id)   (1u << ((id) % BR_GIC_INTID_PER_WORD))  /* bit within the word */
#define BR_GIC_ICFGR_WORDS    16u                                     /* each ICFGR word covers 16 lines */
#define BR_GIC_ICFGR_SHIFT(id) (2u * ((id) % BR_GIC_ICFGR_WORDS))     /* 2 bits per line */
#define BR_GIC_ICFGR_MASK      3u

/* AE (exception context/DAIF): only arch bits are used here; the br_exc.h macros are
 * not reused (keeps the ISA layer free of core headers) */
#define BR_AARCH64_DAIF_I 0x080u   /* PSTATE.I (arch value of br_exc.h BR_DAIF_I) */

/* =====================================================================
 * 4. MMIO and system register access primitives
 * ===================================================================== */

/* GIC MMIO: must be volatile (the compiler may not merge/eliminate it); GICv3 registers
 * are little-endian 32/64-bit */
static inline br_u32 br_gic_mmio_read32(br_uintptr_t base, br_u32 off)
{
    return *(volatile br_u32 *)(base + off);
}

static inline void br_gic_mmio_write32(br_uintptr_t base, br_u32 off, br_u32 val)
{
    *(volatile br_u32 *)(base + off) = val;
}

/* priority registers are **byte-addressable** (GICD_IPRIORITYR / GICR_IPRIORITYR) */
static inline br_u8 br_gic_mmio_read8(br_uintptr_t base, br_u32 off)
{
    return *(volatile br_u8 *)(base + off);
}

static inline void br_gic_mmio_write8(br_uintptr_t base, br_u32 off, br_u8 val)
{
    *(volatile br_u8 *)(base + off) = val;
}

/* GICD_IROUTER is a 64-bit register */
static inline void br_gic_mmio_write64(br_uintptr_t base, br_u32 off, br_u64 val)
{
    *(volatile br_u64 *)(base + off) = val;
}

/*
 * DSB/ISB purpose (why they are needed):
 *   - after setting `ICC_SRE_EL1`, an ISB is required: SRE decides whether later `ICC_*`
 *     accesses are system register accesses or get trapped to EL2/EL3, so it changes how
 *     instructions are interpreted and needs a context synchronization event;
 *   - after writing `GICD_CTLR`, a DSB is required: it is an MMIO write that pushes config
 *     into the distributor's internal state, and the DSB makes the write actually reach the
 *     device (only then does polling RWP mean anything);
 *   - after writing `ICC_PMR_EL1` / `ICC_CTLR_EL1`, an ISB: they affect CPU interface state
 *     that drives interrupt delivery decisions.
 */
static inline void br_gic_dsb_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void br_gic_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

/* ---- system registers (ICC_* = GICv3 CPU interface; GICv3 arch IHI0069 §11) ---- */

/* ICC_SRE_EL1: System Register Enable. SRE(bit0)=1 => ICC_* uses system registers */
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

/* ICC_CTLR_EL1: EOImode(bit1) / PRIbits([10:8]) */
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

/* ICC_PMR_EL1: priority mask threshold (only priorities more urgent than the threshold
 * are delivered) */
static inline void br_gic_write_icc_pmr_el1(br_u64 v)
{
    __asm__ volatile("msr icc_pmr_el1, %0" ::"r"(v) : "memory");
}

/* ICC_IGRPEN1_EL1: master switch for Group1 interrupts */
static inline void br_gic_write_icc_igrpen1_el1(br_u64 v)
{
    __asm__ volatile("msr icc_igrpen1_el1, %0" ::"r"(v) : "memory");
}

/*
 * ★★ ICC_IAR1_EL1: **reads have side effects** (a read takes Pending->Active).
 *     - must be read exactly once (§4.1.1); an extra "just for tracing" read returns 1023
 *       and corrupts the state machine.
 *     - therefore this function is called from exactly one place, gicv3_ack(), and is volatile.
 */
static inline br_u64 br_gic_read_icc_iar1_el1(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(v));
    return v;
}

/* ICC_EOIR1_EL1: writing hwirq => with EOImode=0, priority drop + deactivate in one step */
static inline void br_gic_write_icc_eoir1_el1(br_u64 v)
{
    __asm__ volatile("msr icc_eoir1_el1, %0" ::"r"(v) : "memory");
}

/* ICC_SGI1R_EL1: software generated interrupt (Group1); written in one 64-bit store */
static inline void br_gic_write_icc_sgi1r_el1(br_u64 v)
{
    __asm__ volatile("msr icc_sgi1r_el1, %0" ::"r"(v) : "memory");
}

/* MPIDR_EL1: this CPU's affinity (Aff3..Aff0) */
static inline br_u64 br_gic_read_mpidr_el1(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v;
}

/* =====================================================================
 * 5. Instance storage
 * ===================================================================== */
/*
 * ★ `br_pic_t` is an **opaque type** in core (br_pic.h): core only holds a pointer, the
 *   storage belongs to platform. So the full definition of `struct br_pic` appears only here.
 */
struct br_pic {
    /* platform data (copied at register time; changes per board) */
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

/* RWP/wake polling limit; only a guard against an infinite loop, not precise timing */
#define BR_GICV3_POLL_SPINS 1000000u

/* GICv3 arch requires "at least 5 significant priority bits" (IHI0069): lower bound used
 * when the PRIbits field is abnormally small */
#define BR_GICV3_PRIO_BITS_MIN 5u
#define BR_GICV3_PRIO_BITS_MAX 8u

/* =====================================================================
 * 6. Initialization (3-02 §14.3 steps 1–2; EARLY phase, interrupts globally disabled)
 * ===================================================================== */
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

    /* ---- step 1: ICC_SRE_EL1.SRE = 1 (read-modify-write; fail hard if it will not set) ----
     * If SRE does not take effect, every later ICC_* access may trap or be undefined, so no
     * "degraded" mode is honest: boot must fail. ISB: this bit changes how later
     * instructions are interpreted. */
    sre = br_gic_read_icc_sre_el1();
    sre |= 1u;
    br_gic_write_icc_sre_el1(sre);
    br_gic_isb();
    if ((br_gic_read_icc_sre_el1() & 1u) == 0u) {
        return BR_ERR(EIO);
    }

    /* ---- step 2: GICD_CTLR = ARE_NS | EnableGrp1NS ----
     * ARE_NS(bit4): enable affinity routing (only then does GICD_IROUTER mean anything, and
     * it is the normal GICv3 form); EnableGrp1NS(bit1): distributor-wide admission of
     * non-secure Group1 interrupts.
     * ★ This is an RWP register: after the write, wait until RWP(bit31) falls back to 0;
     *   configuring the redistributor before that races "config written but distributor has
     *   not accepted it yet". */
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

    /* ---- step 3: find **this CPU's** redistributor frame by MPIDR affinity ----
     * GICR_TYPER affinity is in the **high 32 bits** (0xC) and is encoded differently from
     * MPIDR:
     *   GICv3: Aff0[39:32] Aff1[47:40] Aff2[55:48] Aff3[63:56]
     *   MPIDR: Aff0[7:0]   Aff1[15:8]  Aff2[23:16]  Aff3[39:32], bit31 = U/MP
     * => first "flatten" the four MPIDR affinity bytes into 32 bits (Aff0 in the lowest
     *    byte) and compare, which also masks off bit31 (always 1 on single-core systems and
     *    not part of the affinity) and the other RES bits. */
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
        /* Last (low word bit4) = last frame of the contiguous frame array => stop when seen */
        if ((typer_lo & BR_GICR_TYPER_LAST) != 0u) {
            break;
        }
    }
    if (frame_found == 0u) {
        return BR_ERR(ENODEV);   /* no frame for this CPU within the scan range */
    }
    pic->gicr_frame = frame;
    pic->gicr_sgi   = frame + BR_GICR_SGI_OFFSET;

    /* ---- step 4: wake this CPU's redistributor ----
     * ProcessorSleep=0 means "this CPU wants interrupts"; then wait for ChildrenAsleep to
     * fall to 0 (hardware confirms the Q/internal pipeline is awake). Configuring registers
     * without waiting may drop the configuration. */
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

    /* ---- step 5: initial group/trigger for the SGI frame (INTID 0–31) ----
     * IGROUPR0 all ones: all 32 private lines belong to Group1 NS (matching GICD's
     *   EnableGrp1NS; leaving them in Group0 while only enabling Group1 means the lines are
     *   never delivered -- the classic "configured but never received").
     * IGRPMODR0 = 0: no Group1 secure/non-secure split.
     * ICFGR0 = 0: SGI is always edge (fixed by arch; writing it just states the init value).
     * ICFGR1 = 0: PPI defaults to **level** -- the arch timer is a level line. */
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_IGROUPR0, 0xFFFFFFFFu);
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_IGRPMODR0, 0u);
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_ICFGR0, 0u);
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_ICFGR1, 0u);

    /*
     * ---- step 5b: the SPI range (INTID 32+) must also be Group1NS ----
     * ★ This is the other half of the **same trap** as IGROUPR0: GICD_IGROUPR resets to 0
     *   (Group0), and this CPU admits only Group1 (GICD_CTLR.EnableGrp1NS + ICC_IGRPEN1 = 1)
     *   => **no SPI is ever delivered**. The prototype previously had no SPI source that
     *   actually raised interrupts (the timer is a PPI, the UART is only bound/read back and
     *   never generates interrupts), so this gap went unnoticed -- eMMC over PCIe INTx
     *   (SPI 38) is the first real SPI consumer, and it showed "hardware status bit set,
     *   handler never entered".
     * GICD_IGROUPR/IGRPMODR have one 32-bit register per 32 INTIDs; n=0 covers the private
     * lines (managed by GICR), so start at n=1 and cover nr_irq (writes beyond the
     * implemented range are RAZ/WI).
     */
    for (br_u32 n = 1u; n < (pic->nr_irq / 32u); n++) {
        br_gic_mmio_write32(pic->gicd_base, BR_GICD_IGROUPR + n * 4u, 0xFFFFFFFFu);
        br_gic_mmio_write32(pic->gicd_base, BR_GICD_IGRPMODR + n * 4u, 0u);
    }
    br_gic_dsb_sy();

    /* ---- step 6: ICC_PMR_EL1 = 0xFF (admit all implemented priorities) ----
     * ★ The reset value is 0x00 = **mask everything**. Forgetting this step looks exactly
     *   like "PIC initialized, line enabled, but interrupts never arrive" (the init
     *   obligation of design §7.1).
     * ★ Measured behavior (QEMU 10.2 / cortex-a53 / 5 priority bits): writing 0xFF **reads
     *   back 0xF8** -- the unimplemented low PMR bits are RAZ/WI, and the delivery rule is
     *   "priority >= PMR is masked". So the **lowest implementable priority (0xF8) is always
     *   masked**: any line quantized to enc_max never gets its interrupt. This is an
     *   architectural property (not a choice of this driver), recorded as P-IRQ-PLAT-1; to
     *   verify "PMR admits it" one must use a priority <= 0xF0. */
    br_gic_write_icc_pmr_el1((br_u64)BR_GICV3_PMR_ALL);
    br_gic_isb();

    /* ---- step 7: ICC_CTLR_EL1.EOImode = 0 (hard prerequisite of §4.1.1) ----
     * With EOImode=1, `ICC_EOIR1_EL1` only does priority drop and ICC_DIR_EL1 must be
     * written too; core's eoi model is "a single eoi is complete" (§5.3) => EOImode must
     * be 0. */
    ctlr_el1 = (br_u32)br_gic_read_icc_ctlr_el1();
    ctlr_el1 &= ~(1u << 1);            /* EOImode = 0 */
    br_gic_write_icc_ctlr_el1((br_u64)ctlr_el1);
    br_gic_isb();
    ctlr_el1 = (br_u32)br_gic_read_icc_ctlr_el1();
    pic->eoimode = (br_u8)((ctlr_el1 >> 1) & 1u);

    /* PRIbits([10:8]) = "implemented bits − 1" (arch definition; Linux
     * gic_get_pribits() uses the same formula). ★ Defensive: old QEMU/non-conformant
     * implementations may report 0 (i.e. "1 bit"), while the GICv3 arch minimum is 5 bits;
     * trusting the field would quantize priorities to 2 levels => when it is below the arch
     * minimum, use the minimum (this is this driver's **measured position** on QEMU, see
     * implementation decision P-IRQ-PLAT-1). */
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
        /* cannot be set (RO) => the single-eoi assumption is broken; this is a config
         * accident that must be visible */
        br_console_puts("[gicv3] WARN: ICC_CTLR_EL1.EOImode != 0 (single-eoi model broken)\n");
    }

    /* ---- step 8: ICC_IGRPEN1_EL1 = 1 (CPU interface admits Group1) ---- */
    br_gic_write_icc_igrpen1_el1(1u);
    br_gic_isb();

    /* ---- step 9: read the SPI line count from GICD_TYPER (diagnostic/informational) ----
     * ITLinesNumber([7:5]): SPI lines = (value + 1) * 32 (SPI starts at INTID 32). */
    typer = br_gic_mmio_read32(pic->gicd_base, BR_GICD_TYPER);
    pic->spi_lines = (((typer >> BR_GICD_TYPER_ITLINES_SHIFT) &
                       BR_GICD_TYPER_ITLINES_MASK) + 1u) * BR_GIC_INTID_PER_WORD;

    /* capability bits: see the per-bit notes in gicv3_caps_get(); snapshotted at init so
     * each call need not recompute them */
    pic->caps = BR_PIC_CAP_MASK | BR_PIC_CAP_LATCH | BR_PIC_CAP_READBACK |
                BR_PIC_CAP_PRIO | BR_PIC_CAP_TRIG_EDGE | BR_PIC_CAP_SW_TRIGGER |
                BR_PIC_CAP_PER_CPU | BR_PIC_CAP_AFFINITY;
    pic->ready = 1u;

    /* diagnostic snapshot (out param of br_gicv3_info) */
    s_info.prio_bits = pic->prio_bits;
    s_info.eoimode   = pic->eoimode;
    s_info.spi_lines = pic->spi_lines;
    s_info.mpidr     = (br_u32)pic->mpidr;

    return 0;
}

/* =====================================================================
 * 7. caps_get (3-02 §4.2 IR-3)
 * ===================================================================== */
static int gicv3_caps_get(br_pic_t *pic, br_pic_caps_t *out)
{
    if (pic == BR_NULL || out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    out->prio_bits = pic->prio_bits;
    out->nr_irq    = pic->nr_irq;
    /*
     * Bit by bit (each "have" or "have not" is a hardware fact, not a choice):
     *   MASK        GICR/GICD ICENABLER (per-line hardware mask) => the shadow is only
     *               diagnostic (§4.3)
     *   LATCH       pending latching during masking is done by the GIC
     *   READBACK    ISPENDR/ISACTIVER/ISENABLER can be read back (conformance cross-check)
     *   PRIO        GICv3 always has priorities
     *   TRIG_EDGE   supports rising edge (SPI/PPI ICFGR has an edge bit)
     *   SW_TRIGGER  SGI (ICC_SGI1R_EL1) => self-test channel (§8.4)
     *   PER_CPU     has PPI/SGI (INTID 0–31 private per CPU)
     *   AFFINITY    SPI can be routed via GICD_IROUTER (§13.1)
     * ★ Deliberately **not reported**:
     *   NEST        v1 does not nest (IR-6); reporting it would contradict core's assertions
     *   TRIG_BOTH   GICv3 SPI/PPI only have "level/rising edge", no both-edges
     *   RESYNC      save/restore belongs to v2 PM
     */
    out->caps = pic->caps;
    return 0;
}

/* =====================================================================
 * 8. Acknowledge and end (hot path; 3-02 §4.1 / §5.3)
 * ===================================================================== */
static br_u32 gicv3_ack(br_pic_t *pic)
{
    br_u32 iar;

    (void)pic;   /* ack has no parameter source: the only entry is this CPU's interface */

    /* ★★★ The **only** read of ICC_IAR1_EL1 in the whole driver (volatile, side effects).
     * Never read it again for trace/assert: the second read returns 1023 (spurious) and
     * corrupts the state machine. */
    iar = (br_u32)br_gic_read_icc_iar1_el1();

    /* ★ Map the **whole** 1020–1023 range (dialect obligation of §5.3): mapping only 1023
     *   would let core look 1020–1022 up in the binding table as a legitimate hwirq -> miss
     *   -> take the "already claimed" branch and write EOI, while writing ICC_EOIR1_EL1 for
     *   a special INTID is undefined/deprecated behavior. */
    if (iar >= BR_GICV3_INTID_SPECIAL && iar <= BR_GICV3_INTID_MAX) {
        return BR_PIC_NO_IRQ;
    }
    return iar;
}

static void gicv3_eoi(br_pic_t *pic, br_u32 hwirq)
{
    (void)pic;

    /* EOImode=0 (asserted by init) => one write does priority drop + deactivate, §4.1.1.
     * **No DSB needed** here: ICC_EOIR1_EL1 is a system register write, not an MMIO device
     * write. One ISB is kept for context synchronization (as in Linux gic_eoi_irq) so the
     * priority drop is visible to later instructions -- very cheap, and it avoids "right
     * after eoi the running priority still looks stale". */
    br_gic_write_icc_eoir1_el1((br_u64)hwirq);
    br_gic_isb();
}

/* =====================================================================
 * 9. Single-line masking (MMIO only => callable from an ISR; 3-02 §14.3)
 * ===================================================================== */
static void gicv3_mask(br_pic_t *pic, br_u32 hwirq)
{
    if (hwirq >= pic->nr_irq) {
        return;   /* void hot path: cannot report out-of-range, but never writes out of range */
    }
    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        /* INTID 0–31: ICENABLER0 of this CPU's GICR SGI frame (write 1 to mask) */
        br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_ICENABLER0, BR_GIC_WORD_BIT(hwirq));
    } else {
        /* INTID >= 32: GICD_ICENABLER<n>, n = intid/32 */
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

/* =====================================================================
 * 10. State queries (diagnostic, not hot path; 3-02 §4.3)
 * ===================================================================== */
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

/* =====================================================================
 * 11. Configuration (thread-only; 3-02 §7.1 / §8.1 / §13.1)
 * ===================================================================== */
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
     * Logical priority (small = high) -> GICv3 hardware encoding: significant bits are at
     * the **MSB**.
     * Example: 5 significant bits => reachable {0x00, 0x08, ..., 0xF8} (step = 1 << (8-5) = 8).
     *
     * ★ Quantization direction = **round toward lower priority (larger value)**, never
     *   silently raise priority (IR-7):
     *     enc = ceil(prio / step) * step = (prio + step - 1) & ~(step - 1)
     *   A request of 0 still encodes 0 (the highest priority is not demoted).
     *
     * ★★ The cap is the **lowest deliverable** level, not the "lowest implementable" one
     *   (P-IRQ-15, confirmed by measurement):
     *   The delivery rule is `prio < PMR` (strictly less), and PMR implements only the top
     *   prio_bits bits -- writing 0xFF reads back 0xF8 (with 5 bits), so the 0xF8 that
     *   "logical lowest" quantizes to is **equal** to PMR => that line is never delivered
     *   (QEMU measurement: 0xF8 not delivered, 0xF0 delivered).
     *   There are only two options:
     *     (a) lower 0xF8 to 0xF0 -- that is a **priority raise** (smaller value = more
     *         urgent), explicitly forbidden by IR-7;
     *     (b) `-ENOTSUP` -- design §4.2/§7.1's "cannot express the requested urgency =>
     *         reject outright".
     *   We take (b): rejecting is more honest than "silently make this line never receive
     *   interrupts" or "silently raise priority".
     *   Lines that must be deliverable should use a logical priority <= the lowest
     *   deliverable level (5 bits = 0xF0); static prios in the binding table face the same
     *   constraint. */
    step = 1u << (BR_GICV3_PRIO_BITS_MAX - pic->prio_bits);
    enc  = ((br_u32)prio + step - 1u) & ~(step - 1u);

    const br_u32 deliverable_max = (0xFFu & ~(step - 1u)) - step;
    if (enc > deliverable_max) {
        return BR_ERR(ENOTSUP);
    }

    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        /* INTID 0–31 priority registers live in the GICR SGI frame (from 0x400, byte
         * addressed) */
        br_gic_mmio_write8(pic->gicr_sgi, BR_GICR_IPRIORITYR + hwirq, (br_u8)enc);
    } else {
        /* GICD_IPRIORITYR is byte-addressed by INTID => no 4-byte grouping needed */
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

    /* DEFAULT = "core already resolved it from the binding table, nothing to change here"
     * (clarified by the parent agent) */
    if (trigger == BR_IRQ_TRIG_DEFAULT) {
        return 0;
    }

    /* SGI (0–15) is **always edge** (fixed by arch; ICFGR0 has no effect on it) */
    if (hwirq <= BR_GICV3_INTID_SGI_MAX) {
        return (trigger == BR_IRQ_TRIG_EDGE_RISE) ? 0 : BR_ERR(ENOTSUP);
    }

    switch (trigger) {
    case BR_IRQ_TRIG_LEVEL_HIGH:
    case BR_IRQ_TRIG_LEVEL_LOW:
        /* GIC ICFGR expresses only "level/edge", not polarity; polarity is board data
         * (binding table BR_IRQB_F_ACTIVE_LOW) => both LEVEL variants map to "level" */
        edge = 0u;
        break;
    case BR_IRQ_TRIG_EDGE_RISE:
        edge = 1u;
        break;
    default:
        /* EDGE_FALL / EDGE_BOTH: unsupported by GICv3 => never silently rewrite (§4.2 iron rule) */
        return BR_ERR(ENOTSUP);
    }

    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        /* PPI (16–31): GICR_ICFGR1, bit = 2 * (intid - 16) */
        base  = pic->gicr_sgi;
        off   = BR_GICR_ICFGR1;
        shift = BR_GIC_ICFGR_SHIFT(hwirq - BR_GICV3_INTID_PPI_BASE);
    } else {
        /* SPI: GICD_ICFGR<n>, each word covers 16 lines */
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
 * Software-triggered SGI (INTID 0–15 only; CAP_SW_TRIGGER).
 * ICC_SGI1R_EL1 target encoding (Aff3..Aff0 from MPIDR; TargetList bitmap):
 *   [15:0]  TargetList -- Aff0 bitmap of target CPUs (1 << Aff0)
 *   [23:16] Aff1
 *   [27:24] INTID
 *   [39:32] Aff2
 *   [55:48] Aff3
 *   [63]    IRM (0 = by affinity; 1 = any CPU, unused by this driver)
 * The RS field is deprecated in the current architecture (set to 0); we do not do
 * "range selector".
 */
static void gicv3_sgi1r_write(const struct br_pic *pic, br_u32 intid)
{
    br_u64 val;

    val  = ((br_u64)((pic->mpidr >> 8) & 0xFFull)) << 16;  /* Aff1 */
    val |= ((br_u64)((pic->mpidr >> 16) & 0xFFull)) << 32; /* Aff2 */
    val |= ((br_u64)((pic->mpidr >> 32) & 0xFFull)) << 48; /* Aff3 */
    val |= ((br_u64)intid) << 24;                          /* INTID */
    /* TargetList: hit only this CPU. The bitmap is 16 bits wide (Aff0 = 0..15); an Aff0
     * outside the low bits is meaningless, so masking with 0xF is both arch-conformant and
     * avoids an out-of-range shift count (UB). */
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

/* =====================================================================
 * 12. CPU-side masking (dialect wrapper over PSTATE.I = L2; 3-02 §6.4)
 * ===================================================================== */
static br_u32 gicv3_cpu_mask(br_pic_t *pic)
{
    br_u64 daif;

    (void)pic;

    /* return the **raw DAIF** (core's br_irq_state_t interprets the bit layout, §6.4) */
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");   /* #2 = bit1 = PSTATE.I */
    return (br_u32)daif;
}

static void gicv3_cpu_unmask(br_pic_t *pic, br_u32 state)
{
    (void)pic;

    /* v1 manages **I only** (§6.4): F/A/D are left untouched, not "helpfully restored".
     * state is the raw DAIF obtained from cpu_mask(); bit7 = I. */
    if ((state & BR_AARCH64_DAIF_I) != 0u) {
        __asm__ volatile("msr daifset, #2" ::: "memory");
    } else {
        __asm__ volatile("msr daifclr, #2" ::: "memory");
    }
}

/* =====================================================================
 * 13. Affinity (CAP_AFFINITY only; 3-02 §13.1/§13.2)
 * ===================================================================== */
static int gicv3_set_affinity(br_pic_t *pic, br_u32 hwirq, br_u32 cpu_mask)
{
    br_u32 aff0 = 0u;

    if (pic == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    /* SGI carries its target per send; PPI is per-CPU private -- neither is routable (§13.1) */
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

    /* GICD_IROUTER<n> (8 B per SPI): this prototype only expresses "Aff0 = target CPU
     * index, Aff1/2/3 = 0" (single-cluster QEMU virt). IRM=0 (route by affinity, not
     * "any CPU"). */
    br_gic_mmio_write64(pic->gicd_base, BR_GICD_IROUTER + hwirq * 8u, (br_u64)aff0);
    return 0;
}

/* =====================================================================
 * 14. ops table
 * ===================================================================== */
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

    /* only with CAP_NEST; v1 never nests (IR-6) so core will not call it => left NULL, noted */
    .set_running_prio = BR_NULL,

    .set_affinity = gicv3_set_affinity,

    /* only with CAP_RESYNC; PM is v2 (§14.5) => left NULL, noted */
    .save = BR_NULL,
    .restore = BR_NULL,
};

/* =====================================================================
 * 15. Public surface (br_gicv3.h)
 * ===================================================================== */
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

    /* copy platform data into the instance (the ISA layer keeps no pointer to platform data) */
    s_gicv3.gicd_base       = cfg->gicd_base;
    s_gicv3.gicr_base       = cfg->gicr_base;
    s_gicv3.gicr_frame_size = cfg->gicr_frame_size;
    s_gicv3.gicr_max_frames = cfg->gicr_max_frames;
    s_gicv3.nr_irq          = cfg->nr_irq;
    s_gicv3.gicr_frame      = 0u;
    s_gicv3.gicr_sgi        = 0u;
    s_gicv3.ready           = 0u;

    /* EARLY phase: program the hardware per the §14.3 sequence; failure = boot failure
     * (negative errno propagates) */
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

/* ---- cross-check readback (§4.3 item 5: allowed for conformance/debug only) ---- */
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
 * =====================================================================
 * conformance/debug only: poke hardware enable bits directly by INTID (**bypassing core's
 * binding/descriptor layer**)
 * =====================================================================
 *
 * The one reason it exists (TC-IRQ-008): verifying core's "claimed hwirq but no virq =>
 * spurious_owned" branch needs a scenario where "**hardware delivers but core has no
 * owner**". That scenario **cannot be constructed** on the normal path -- an unbound or
 * unregistered line is disabled in the GIC anyway and nobody would unmask it (which is
 * exactly where "safe by default before registration" comes from, §6.2).
 * => the conformance case turns the hardware on itself: it simulates "firmware/bootloader
 *   left an enabled but unclaimed line" -- precisely the integration-time fault that
 *   spurious_owned is meant to catch.
 *
 * ! Product code **must not** call this: it bypasses core's shadow/depth model (§4.3) and
 *   would rob spurious_owned of its meaning as a "real bug signal". In audits it should
 *   appear only in platform conformance files.
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
