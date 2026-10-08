/*
 * brickOS prototype v0.1.0 — GICv3 驱动(ISA 层"方言")
 *
 * 设计依据(权威): `docs/3-os-core/3-02-int.md`
 *   §4.1   `br_pic_ops` 填表契约(ack/eoi/mask/unmask/配置/CPU 侧屏蔽)
 *   §4.1.1 GICv3 EOImode 义务: **必须为 0**(priority drop + deactivate 一步完成);
 *          `ICC_IAR1_EL1` 读有副作用 ⇒ 每次进入只读一次且 volatile
 *   §5.3   1020–1023 整段折算 BR_PIC_NO_IRQ(只折算 1023 会让 core 对特殊 INTID 写 EOI)
 *   §7.1   逻辑优先级(小 = 高)→ 硬件编码的量化: 有效位在**高位(MSB 对齐)**,
 *          位宽不足时向**低优先级**方向饱和, 绝不静默提权
 *   §8.4   软件触发(SGI)= 中断子系统的自检通道
 *   §13.1  SGI/PPI 每核私有(不可路由), SPI 可路由 —— set_affinity 的边界
 *
 * ★ 层边界(设计 1-01 §8 / WORKAROUND(br-wa-isa-001)): 本文件只描述"换控制器型号就
 *   变"的东西(寄存器偏移/位域/初始化序列), **不含任何 QEMU/板级事实**(GICD/GICR
 *   基址、帧跨度、INTID 绑定表在 board_irq.c / plat_qemu_virt.c)。
 *
 * ★ 热路径纪律(3-02 §4.1/§14.3): ack/eoi/mask/unmask/cpu_mask/cpu_unmask 只碰寄存器
 *   (MMIO 或系统寄存器), 不做任何"要过总线可能失败/阻塞"的事 —— 它们要能在 ISR 内调用。
 */
#include <br/core/br_console.h>
#include <br/core/br_error.h>
#include <br/core/br_irq.h>      /* BR_IRQ_TRIG_*: 触发方式的共享编码(§8.1, 解释权在方言) */
#include <br/core/br_pic.h>
#include <br/platform/br_gicv3.h>

/* =====================================================================
 * 1. GICD(分发器)寄存器与位域(GICv3 架构 IHI0069; 偏移相对 GICD base)
 * ===================================================================== */
#define BR_GICD_CTLR        0x0000u  /* GICD_CTLR      Control                     */
#define BR_GICD_TYPER       0x0004u  /* GICD_TYPER     Type                        */
#define BR_GICD_ISENABLER   0x0100u  /* GICD_ISENABLER<n>  Set-Enable(4 B/32 线)   */
#define BR_GICD_ICENABLER   0x0180u  /* GICD_ICENABLER<n>  Clear-Enable            */
#define BR_GICD_ISPENDR     0x0200u  /* GICD_ISPENDR<n>    Set-Pending             */
#define BR_GICD_ISACTIVER   0x0300u  /* GICD_ISACTIVER<n>  Set-Active              */
#define BR_GICD_IPRIORITYR  0x0400u  /* GICD_IPRIORITYR    Priority(**字节**寻址) */
#define BR_GICD_ICFGR       0x0C00u  /* GICD_ICFGR<n>      Configuration(2 位/线) */
#define BR_GICD_IROUTER     0x6000u  /* GICD_IROUTER<n>    Routing(8 B/SPI, ARE 下有效) */

#define BR_GICD_CTLR_ENABLE_GRP1NS  (1u << 1)   /* EnableGrp1NS: 使能 Group1 非安全 */
#define BR_GICD_CTLR_ARE_NS         (1u << 4)   /* ARE_NS: 亲和性路由(否则 IROUTER 无效) */
#define BR_GICD_CTLR_RWP            (1u << 31)  /* RWP: 寄存器写尚未生效(只读) */

#define BR_GICD_TYPER_ITLINES_SHIFT 5u           /* ITLinesNumber 位域位置 */
#define BR_GICD_TYPER_ITLINES_MASK  0x1Fu

/* GICD_IROUTER 的 IRM(bit31): 本驱动只用"路由到指定亲和性"(IRM=0) */
#define BR_GICD_IROUTER_IRM         (1ull << 31)

/* =====================================================================
 * 2. GICR(重分发器)寄存器与位域(每核一份, 2×64 KiB)
 * ===================================================================== */
#define BR_GICR_TYPER               0x0008u  /* GICR_TYPER(64 位: 0x8 低字 / 0xC 高字) */
#define BR_GICR_TYPER_LAST          (1u << 4) /* Last: 本帧是连续帧数组的最后一帧 */
#define BR_GICR_WAKER               0x0014u  /* GICR_WAKER  Control */
#define BR_GICR_WAKER_PROCESSOR_SLEEP (1u << 1) /* ProcessorSleep: 软件置位表示本核要睡 */
#define BR_GICR_WAKER_CHILDREN_ASLEEP (1u << 2) /* ChildrenAsleep: 硬件报"已睡" */

/* SGI_base = RD_base + 64 KiB; SGI/PPI(INTID 0–31)的配置全在这一半帧里 */
#define BR_GICR_SGI_OFFSET  0x10000u
#define BR_GICR_IGROUPR0    0x0080u  /* GICR_IGROUPR0   INTID 0–31 的 Group */
#define BR_GICR_ISENABLER0  0x0100u  /* GICR_ISENABLER0 INTID 0–31 使能 */
#define BR_GICR_ICENABLER0  0x0180u  /* GICR_ICENABLER0 INTID 0–31 屏蔽 */
#define BR_GICR_ISPENDR0    0x0200u  /* GICR_ISPENDR0   INTID 0–31 pending */
#define BR_GICR_ISACTIVER0  0x0300u  /* GICR_ISACTIVER0 INTID 0–31 active */
#define BR_GICR_IPRIORITYR  0x0400u  /* GICR_IPRIORITYR INTID 0–31 优先级(字节) */
#define BR_GICR_ICFGR0      0x0C00u  /* GICR_ICFGR0     SGI(0–15): 恒边沿 */
#define BR_GICR_ICFGR1      0x0C04u  /* GICR_ICFGR1     PPI(16–31): 电平/边沿 */
#define BR_GICR_IGRPMODR0   0x0D00u  /* GICR_IGRPMODR0  0 = Group1 NS */

/* =====================================================================
 * 3. 通用位运算常量
 * ===================================================================== */
#define BR_GIC_INTID_PER_WORD 32u                                     /* 每个使能/pending 寄存器覆盖 32 线 */
#define BR_GIC_WORD_INDEX(id) ((id) / BR_GIC_INTID_PER_WORD)          /* 第几个 4 B 字 */
#define BR_GIC_WORD_BIT(id)   (1u << ((id) % BR_GIC_INTID_PER_WORD))  /* 字内位 */
#define BR_GIC_ICFGR_WORDS    16u                                     /* 每个 ICFGR 字覆盖 16 线 */
#define BR_GIC_ICFGR_SHIFT(id) (2u * ((id) % BR_GIC_ICFGR_WORDS))     /* 2 位/线 */
#define BR_GIC_ICFGR_MASK      3u

/* AE(异常现场/DAIF): 这里只依赖架构位, 不复用 br_exc.h 的宏(避免 ISA 层依赖 core 头) */
#define BR_AARCH64_DAIF_I 0x080u   /* PSTATE.I(br_exc.h BR_DAIF_I 的架构值) */

/* =====================================================================
 * 4. MMIO 与系统寄存器访问原语
 * ===================================================================== */

/* GIC 的 MMIO: 必须 volatile(编译器不得合并/消除); GICv3 寄存器为小端 32/64 位 */
static inline br_u32 br_gic_mmio_read32(br_uintptr_t base, br_u32 off)
{
    return *(volatile br_u32 *)(base + off);
}

static inline void br_gic_mmio_write32(br_uintptr_t base, br_u32 off, br_u32 val)
{
    *(volatile br_u32 *)(base + off) = val;
}

/* 优先级寄存器**字节可寻址**(GICD_IPRIORITYR / GICR_IPRIORITYR) */
static inline br_u8 br_gic_mmio_read8(br_uintptr_t base, br_u32 off)
{
    return *(volatile br_u8 *)(base + off);
}

static inline void br_gic_mmio_write8(br_uintptr_t base, br_u32 off, br_u8 val)
{
    *(volatile br_u8 *)(base + off) = val;
}

/* GICD_IROUTER 是 64 位寄存器 */
static inline void br_gic_mmio_write64(br_uintptr_t base, br_u32 off, br_u64 val)
{
    *(volatile br_u64 *)(base + off) = val;
}

/*
 * DSB/ISB 的用途(为什么需要):
 *   - `ICC_SRE_EL1` 置位后必须 ISB: SRE 决定后续 `ICC_*` 是系统寄存器访问还是
 *     被 trap 到 EL2/EL3, 属于"改变指令解释"的配置 ⇒ 需要上下文同步事件;
 *   - `GICD_CTLR` 写后必须 DSB: 它是把配置推给分发器内部状态的 MMIO 写,
 *     DSB 保证写真正到达设备(随后轮询 RWP 才有意义);
 *   - `ICC_PMR_EL1` / `ICC_CTLR_EL1` 写后 ISB: 影响中断投递判定的 CPU 接口状态。
 */
static inline void br_gic_dsb_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void br_gic_isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

/* ---- 系统寄存器(ICC_* = GICv3 CPU 接口; GICv3 架构 IHI0069 §11) ---- */

/* ICC_SRE_EL1: System Register Enable。SRE(bit0)=1 ⇒ ICC_* 走系统寄存器 */
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

/* ICC_PMR_EL1: 优先级屏蔽门限(小于门限=更紧急的才投递) */
static inline void br_gic_write_icc_pmr_el1(br_u64 v)
{
    __asm__ volatile("msr icc_pmr_el1, %0" ::"r"(v) : "memory");
}

/* ICC_IGRPEN1_EL1: Group1 中断的总开关 */
static inline void br_gic_write_icc_igrpen1_el1(br_u64 v)
{
    __asm__ volatile("msr icc_igrpen1_el1, %0" ::"r"(v) : "memory");
}

/*
 * ★★ ICC_IAR1_EL1: **读有副作用**(读走即 Pending→Active)。
 *     - 必须恰好读一次(§4.1.1); 为 trace"顺手再读一次"会拿到 1023 并破坏状态机。
 *     - 因此本函数只被 gicv3_ack() 调用一处, 且带 volatile。
 */
static inline br_u64 br_gic_read_icc_iar1_el1(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(v));
    return v;
}

/* ICC_EOIR1_EL1: 写 hwirq ⇒ EOImode=0 时 priority drop + deactivate 一步完成 */
static inline void br_gic_write_icc_eoir1_el1(br_u64 v)
{
    __asm__ volatile("msr icc_eoir1_el1, %0" ::"r"(v) : "memory");
}

/* ICC_SGI1R_EL1: 软件生成中断(Group1); 64 位一次性写完 */
static inline void br_gic_write_icc_sgi1r_el1(br_u64 v)
{
    __asm__ volatile("msr icc_sgi1r_el1, %0" ::"r"(v) : "memory");
}

/* MPIDR_EL1: 本核亲和性(Aff3..Aff0) */
static inline br_u64 br_gic_read_mpidr_el1(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v;
}

/* =====================================================================
 * 5. 实例存储
 * ===================================================================== */
/*
 * ★ `br_pic_t` 在 core 是**不透明类型**(br_pic.h): core 只持有指针, 存储归 platform。
 *   所以 `struct br_pic` 的完整定义只在本文件出现。
 */
struct br_pic {
    /* 平台数据(register 时拷入; 换板子就变) */
    br_uintptr_t gicd_base;
    br_uintptr_t gicr_base;
    br_u32       gicr_frame_size;   /* 单核帧跨度(2 × 64 KiB) */
    br_u32       gicr_max_frames;   /* 亲和性扫描上限 */
    br_u32       nr_irq;            /* INTID 上界(报给 caps) */

    /* init 发现的事实 */
    br_uintptr_t gicr_frame;        /* 本核 RD_base(亲和性匹配结果) */
    br_uintptr_t gicr_sgi;          /* 本核 SGI_base = RD_base + 64 KiB */
    br_u64       mpidr;             /* MPIDR_EL1 原值(供 SGI 目标构造) */
    br_u32       spi_lines;         /* GICD_TYPER 报的 SPI 线数 */
    br_u32       caps;              /* BR_PIC_CAP_* 快照 */

    /* ICC_CTLR_EL1 读出的事实 */
    br_u8        prio_bits;
    br_u8        eoimode;
    br_u8        ready;             /* init 已完成 */
    br_u8        rsv;
};

static struct br_pic       s_gicv3;      /* 唯一实例(BSS 零初始化) */
static br_gicv3_info_t     s_info;       /* br_gicv3_info 的出参 */

/* RWP/唤醒轮询上限; 只是"不要死循环"的护栏, 不是精确时序 */
#define BR_GICV3_POLL_SPINS 1000000u

/* GICv3 架构要求"至少 5 位有效优先级"(IHI0069): PRIbits 字段异常偏小时的下限 */
#define BR_GICV3_PRIO_BITS_MIN 5u
#define BR_GICV3_PRIO_BITS_MAX 8u

/* =====================================================================
 * 6. 初始化(3-02 §14.3 步 1–2; EARLY 相, 全局关中断)
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

    /* ---- 步 1: ICC_SRE_EL1.SRE = 1(读改写; 置不上就直接失败) ----
     * 若 SRE 不生效, 后续所有 ICC_* 访问都可能 trap 或被当作未定义 ⇒ 没有
     * 任何"降级"是诚实的, 只能启动失败。ISB: 该位改变后续指令的解释方式。 */
    sre = br_gic_read_icc_sre_el1();
    sre |= 1u;
    br_gic_write_icc_sre_el1(sre);
    br_gic_isb();
    if ((br_gic_read_icc_sre_el1() & 1u) == 0u) {
        return BR_ERR(BR_EIO);
    }

    /* ---- 步 2: GICD_CTLR = ARE_NS | EnableGrp1NS ----
     * ARE_NS(bit4): 打开亲和性路由(GICD_IROUTER 才有意义, 也是 GICv3 的正常形态);
     * EnableGrp1NS(bit1): 分发器对非安全 Group1 中断的总放行。
     * ★ 这是 RWP 寄存器: 写入后要等 RWP(bit31)落回 0 才算生效 —— 不等就配
     *   redistributor, 会出现"配置写了但分发器还没接受"的竞态。 */
    br_gic_mmio_write32(pic->gicd_base, BR_GICD_CTLR,
                       BR_GICD_CTLR_ARE_NS | BR_GICD_CTLR_ENABLE_GRP1NS);
    br_gic_dsb_sy();   /* 确保这次 MMIO 写真正到达分发器, 轮询才可信 */
    for (spins = 0u; spins < BR_GICV3_POLL_SPINS; spins++) {
        ctlr = br_gic_mmio_read32(pic->gicd_base, BR_GICD_CTLR);
        if (((ctlr & (BR_GICD_CTLR_ARE_NS | BR_GICD_CTLR_ENABLE_GRP1NS)) ==
             (BR_GICD_CTLR_ARE_NS | BR_GICD_CTLR_ENABLE_GRP1NS)) &&
            ((ctlr & BR_GICD_CTLR_RWP) == 0u)) {
            break;
        }
    }
    if (spins == BR_GICV3_POLL_SPINS) {
        return BR_ERR(BR_ETIMEDOUT);
    }

    /* ---- 步 3: 按 MPIDR 亲和性找**本核**的 redistributor 帧 ----
     * GICR_TYPER 的亲和性在**高 32 位**(0xC), 且编码与 MPIDR 不同:
     *   GICv3: Aff0[39:32] Aff1[47:40] Aff2[55:48] Aff3[63:56]
     *   MPIDR: Aff0[7:0]   Aff1[15:8]  Aff2[23:16]  Aff3[39:32], bit31 = U/MP
     * ⇒ 先把 MPIDR 的四个亲和字节"压平"成 32 位(Aff0 在最低字节)再比较,
     *   这样顺带屏蔽掉 bit31(单核系统恒为 1, 不参与亲和性)与其他 RES 位。 */
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
        /* Last(低字 bit4)= 连续帧数组的最后一帧 ⇒ 扫到即停 */
        if ((typer_lo & BR_GICR_TYPER_LAST) != 0u) {
            break;
        }
    }
    if (frame_found == 0u) {
        return BR_ERR(BR_ENODEV);   /* 扫描范围内没有属于本核的帧 */
    }
    pic->gicr_frame = frame;
    pic->gicr_sgi   = frame + BR_GICR_SGI_OFFSET;

    /* ---- 步 4: 唤醒本核 redistributor ----
     * ProcessorSleep=0 表示"本核要收中断"; 之后等 ChildrenAsleep 落 0
     * (硬件确认 Q/内部流水已醒)。不等待就配寄存器, 配置可能被丢弃。 */
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
            return BR_ERR(BR_ETIMEDOUT);
        }
    }

    /* ---- 步 5: SGI 帧(INTID 0–31)的组/触发初值 ----
     * IGROUPR0 全 1: 32 条私有线都归 Group1 NS(与 GICD 的 EnableGrp1NS 对应;
     *   留在 Group0 而只开 Group1 ⇒ 线永不被投递 —— 经典的"配了却收不到")。
     * IGRPMODR0 = 0: 不做 Group1 安全/非安全的二级划分。
     * ICFGR0 = 0: SGI 恒边沿(架构固定, 写它只是明确初值)。
     * ICFGR1 = 0: PPI 默认**电平** —— arch timer 就是电平线。 */
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_IGROUPR0, 0xFFFFFFFFu);
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_IGRPMODR0, 0u);
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_ICFGR0, 0u);
    br_gic_mmio_write32(pic->gicr_sgi, BR_GICR_ICFGR1, 0u);
    br_gic_dsb_sy();

    /* ---- 步 6: ICC_PMR_EL1 = 0xFF(放行全部可实现优先级)----
     * ★ 复位值为 0x00 = **屏蔽一切**。忘了这步的症状正是"PIC 初始化完毕、线也
     *   enable 了, 却永远收不到中断"(设计 §7.1 的 init 义务)。
     * ★ 实测口径(QEMU 10.2 / cortex-a53 / 5 位优先级): 写 0xFF **读回 0xF8**
     *   —— PMR 的未实现低位 RAZ/WI, 且投递判据是"优先级 >= PMR 即屏蔽"。
     *   于是**最低可实现优先级(0xF8)永远被屏蔽**: 任何被量化到 enc_max 的线
     *   都收不到中断。这是架构性质(不是本驱动的选择), 记入 P-IRQ-PLAT-1;
     *   想验证"PMR 已放行"必须用 <= 0xF0 的优先级。 */
    br_gic_write_icc_pmr_el1((br_u64)BR_GICV3_PMR_ALL);
    br_gic_isb();

    /* ---- 步 7: ICC_CTLR_EL1.EOImode = 0(§4.1.1 的硬前提)----
     * EOImode=1 时 `ICC_EOIR1_EL1` 只做 priority drop, 还要再写 ICC_DIR_EL1;
     * 而 core 的 eoi 模型是"单次 eoi 即完整"(§5.3)⇒ EOImode 必须为 0。 */
    ctlr_el1 = (br_u32)br_gic_read_icc_ctlr_el1();
    ctlr_el1 &= ~(1u << 1);            /* EOImode = 0 */
    br_gic_write_icc_ctlr_el1((br_u64)ctlr_el1);
    br_gic_isb();
    ctlr_el1 = (br_u32)br_gic_read_icc_ctlr_el1();
    pic->eoimode = (br_u8)((ctlr_el1 >> 1) & 1u);

    /* PRIbits([10:8])= "实现位数 − 1"(架构口径; Linux gic_get_pribits() 用同一
     * 公式)。★ 防御: 老 QEMU/非合规实现可能把该字段报成 0(即"1 位"), 而 GICv3
     * 架构下限是 5 位; 直接信字段会把优先级量化到 2 档 ⇒ 字段低于架构下限时按
     * 下限处理(这是本驱动对 QEMU 的**实测口径**, 见实现裁定 P-IRQ-PLAT-1)。 */
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
        /* 置不上(RO)⇒ 单次 eoi 的假设不成立; 这属"必须让人看见"的配置事故 */
        br_console_puts("[gicv3] WARN: ICC_CTLR_EL1.EOImode != 0 (single-eoi model broken)\n");
    }

    /* ---- 步 8: ICC_IGRPEN1_EL1 = 1(CPU 接口放行 Group1) ---- */
    br_gic_write_icc_igrpen1_el1(1u);
    br_gic_isb();

    /* ---- 步 9: GICD_TYPER 读 SPI 线数(诊断/信息) ----
     * ITLinesNumber([7:5]): SPI 线数 = (值 + 1) * 32(SPI 从 INTID 32 起)。 */
    typer = br_gic_mmio_read32(pic->gicd_base, BR_GICD_TYPER);
    pic->spi_lines = (((typer >> BR_GICD_TYPER_ITLINES_SHIFT) &
                       BR_GICD_TYPER_ITLINES_MASK) + 1u) * BR_GIC_INTID_PER_WORD;

    /* 能力位: 见 gicv3_caps_get() 的逐位说明; 在 init 时快照, 避免每次调用重算 */
    pic->caps = BR_PIC_CAP_MASK | BR_PIC_CAP_LATCH | BR_PIC_CAP_READBACK |
                BR_PIC_CAP_PRIO | BR_PIC_CAP_TRIG_EDGE | BR_PIC_CAP_SW_TRIGGER |
                BR_PIC_CAP_PER_CPU | BR_PIC_CAP_AFFINITY;
    pic->ready = 1u;

    /* 诊断快照(br_gicv3_info 的出参) */
    s_info.prio_bits = pic->prio_bits;
    s_info.eoimode   = pic->eoimode;
    s_info.spi_lines = pic->spi_lines;
    s_info.mpidr     = (br_u32)pic->mpidr;

    return 0;
}

/* =====================================================================
 * 7. caps_get(3-02 §4.2 IR-3)
 * ===================================================================== */
static int gicv3_caps_get(br_pic_t *pic, br_pic_caps_t *out)
{
    if (pic == BR_NULL || out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    out->prio_bits = pic->prio_bits;
    out->nr_irq    = pic->nr_irq;
    /*
     * 逐位(每个"有"或"没有"都是硬件事实, 不是选择):
     *   MASK        有 GICR/GICD 的 ICENABLER(单线硬件屏蔽)⇒ 影子只作诊断(§4.3)
     *   LATCH       屏蔽期间 Pending 锁存由 GIC 完成
     *   READBACK    ISPENDR/ISACTIVER/ISENABLER 可回读(conformance 交叉校验)
     *   PRIO        GICv3 必带优先级
     *   TRIG_EDGE   支持上升沿(SPI/PPI 的 ICFGR 有边沿位)
     *   SW_TRIGGER  SGI(ICC_SGI1R_EL1)⇒ 自检通道(§8.4)
     *   PER_CPU     有 PPI/SGI(INTID 0–31 每核私有)
     *   AFFINITY    SPI 可经 GICD_IROUTER 路由(§13.1)
     * ★ 刻意**不报**:
     *   NEST         v1 不嵌套(IR-6); 报了就与 core 的断言前提矛盾
     *   TRIG_BOTH    GICv3 的 SPI/PPI 只有"电平/上升沿", 没有双边沿
     *   RESYNC       save/restore 属 v2 PM
     */
    out->caps = pic->caps;
    return 0;
}

/* =====================================================================
 * 8. 取走与结束(热路径; 3-02 §4.1 / §5.3)
 * ===================================================================== */
static br_u32 gicv3_ack(br_pic_t *pic)
{
    br_u32 iar;

    (void)pic;   /* ack 无参可依: 唯一入口 = 本核 CPU 接口 */

    /* ★★★ 全驱动里对 ICC_IAR1_EL1 的**唯一**一次读(volatile, 有副作用)。
     * 不许为了 trace/断言再读一次: 第二次会返回 1023(spurious)并破坏状态机。 */
    iar = (br_u32)br_gic_read_icc_iar1_el1();

    /* ★ 1020–1023 **整段**折算(§5.3 的方言义务): 只折算 1023 的话, 1020–1022
     *   会被 core 当合法 hwirq 查绑定表 → 未命中 → 按"已领到"分支写 EOI, 而对
     *   特殊 INTID 写 ICC_EOIR1_EL1 是未定义/被弃用的行为。 */
    if (iar >= BR_GICV3_INTID_SPECIAL && iar <= BR_GICV3_INTID_MAX) {
        return BR_PIC_NO_IRQ;
    }
    return iar;
}

static void gicv3_eoi(br_pic_t *pic, br_u32 hwirq)
{
    (void)pic;

    /* EOImode=0(init 已断言)⇒ 一次写即 priority drop + deactivate, §4.1.1。
     * 这里**不需要 DSB**: ICC_EOIR1_EL1 是系统寄存器写, 不是 MMIO 设备写。
     * 保留一条 ISB 作上下文同步(与 Linux gic_eoi_irq 一致), 使优先级跌落对
     * 后续指令可见 —— 极廉价, 且避免"eoi 后立刻判定仍看到旧 running priority"。 */
    br_gic_write_icc_eoir1_el1((br_u64)hwirq);
    br_gic_isb();
}

/* =====================================================================
 * 9. 单线屏蔽(只碰 MMIO ⇒ ISR 内可调用; 3-02 §14.3)
 * ===================================================================== */
static void gicv3_mask(br_pic_t *pic, br_u32 hwirq)
{
    if (hwirq >= pic->nr_irq) {
        return;   /* void 热路径: 越界无法报错, 但绝不写越界寄存器 */
    }
    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        /* INTID 0–31: 本核 GICR SGI 帧的 ICENABLER0(写 1 屏蔽) */
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
 * 10. 状态查询(诊断, 非热路径; 3-02 §4.3)
 * ===================================================================== */
static int gicv3_pending(br_pic_t *pic, br_u32 hwirq, int *out)
{
    br_u32 val;

    if (pic == BR_NULL || out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(BR_EINVAL);
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
        return BR_ERR(BR_EINVAL);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(BR_EINVAL);
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
 * 11. 配置(thread-only; 3-02 §7.1 / §8.1 / §13.1)
 * ===================================================================== */
static int gicv3_set_prio(br_pic_t *pic, br_u32 hwirq, br_u8 prio)
{
    br_u32 step;
    br_u32 enc;

    if (pic == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(BR_EINVAL);
    }
    if (pic->prio_bits == 0u) {
        return BR_ERR(BR_ENOTSUP);   /* CAP_PRIO 未报时就该走这里 */
    }

    /*
     * 逻辑优先级(小 = 高)→ GICv3 硬件编码: 有效位在**高位(MSB 对齐)**。
     * 例: 5 位有效 ⇒ 可达 {0x00, 0x08, ..., 0xF8}(step = 1 << (8-5) = 8)。
     *
     * ★ 量化方向 = **向低优先级(数值变大)取整**, 绝不静默提权(IR-7):
     *     enc = ceil(prio / step) * step = (prio + step - 1) & ~(step - 1)
     *   请求 0 仍是 0(最高优先级不被降级)。
     *
     * ★★ 上限 = **最低可投递**档, 不是"最低可实现"档(P-IRQ-15, 实测确认):
     *   投递判据是 `prio < PMR`(严格小于), 而 PMR 只实现高 prio_bits 位 ——
     *   写 0xFF 读回 0xF8(5 位时), 于是"逻辑最低"量化到的 0xF8 与 PMR **相等**
     *   ⇒ 该线永远不会被投递(QEMU 实测: 0xF8 不投递, 0xF0 投递)。
     *   此时只有两条路:
     *     (a) 把 0xF8 降到 0xF0 —— 那是**提权**(数值更小 = 更紧急), IR-7 明确禁止;
     *     (b) `-ENOTSUP` —— 设计 §4.2/§7.1 的"无法表达所请求的紧迫度 ⇒ 直接拒绝"。
     *   取 (b): 拒绝比"静默让这条线永远收不到中断"或"静默提权"都诚实。
     *   需要"一定能收到"的线请用 <= 最低可投递档(5 位 = 0xF0)的逻辑优先级;
     *   绑定表里的静态 prio 也是同一约束。 */
    step = 1u << (BR_GICV3_PRIO_BITS_MAX - pic->prio_bits);
    enc  = ((br_u32)prio + step - 1u) & ~(step - 1u);

    const br_u32 deliverable_max = (0xFFu & ~(step - 1u)) - step;
    if (enc > deliverable_max) {
        return BR_ERR(BR_ENOTSUP);
    }

    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        /* INTID 0–31 的优先级寄存器在 GICR SGI 帧(0x400 起, 字节寻址) */
        br_gic_mmio_write8(pic->gicr_sgi, BR_GICR_IPRIORITYR + hwirq, (br_u8)enc);
    } else {
        /* GICD_IPRIORITYR 就按 INTID 字节寻址 ⇒ 不需要按 4 字节分组 */
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
        return BR_ERR(BR_EINVAL);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(BR_EINVAL);
    }

    /* DEFAULT = "core 已按绑定表解析过, 这里无需改动"(父 agent 的澄清) */
    if (trigger == BR_IRQ_TRIG_DEFAULT) {
        return 0;
    }

    /* SGI(0–15)**恒为边沿**(架构固定, ICFGR0 对它无效) */
    if (hwirq <= BR_GICV3_INTID_SGI_MAX) {
        return (trigger == BR_IRQ_TRIG_EDGE_RISE) ? 0 : BR_ERR(BR_ENOTSUP);
    }

    switch (trigger) {
    case BR_IRQ_TRIG_LEVEL_HIGH:
    case BR_IRQ_TRIG_LEVEL_LOW:
        /* GIC 的 ICFGR 只表达"电平/边沿", 不表达极性; 极性是板级数据
         * (绑定表 BR_IRQB_F_ACTIVE_LOW)⇒ 两种 LEVEL 都落到"电平" */
        edge = 0u;
        break;
    case BR_IRQ_TRIG_EDGE_RISE:
        edge = 1u;
        break;
    default:
        /* EDGE_FALL / EDGE_BOTH: GICv3 不支持 ⇒ 绝不静默改写(§4.2 铁律) */
        return BR_ERR(BR_ENOTSUP);
    }

    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        /* PPI(16–31): GICR_ICFGR1, 位 = 2 * (intid - 16) */
        base  = pic->gicr_sgi;
        off   = BR_GICR_ICFGR1;
        shift = BR_GIC_ICFGR_SHIFT(hwirq - BR_GICV3_INTID_PPI_BASE);
    } else {
        /* SPI: GICD_ICFGR<n>, 每字覆盖 16 线 */
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
 * 软件触发 SGI(只有 INTID 0–15; CAP_SW_TRIGGER)。
 * ICC_SGI1R_EL1 的目标编码(Aff3..Aff0 来自 MPIDR; TargetList 位图):
 *   [15:0]  TargetList —— 目标核的 Aff0 位图(1 << Aff0)
 *   [23:16] Aff1
 *   [27:24] INTID
 *   [39:32] Aff2
 *   [55:48] Aff3
 *   [63]    IRM(0 = 按亲和性; 1 = 任意核, 本驱动不用)
 * RS 域在当前架构已弃用(置 0); 我们不做"range selector"。
 */
static void gicv3_sgi1r_write(const struct br_pic *pic, br_u32 intid)
{
    br_u64 val;

    val  = ((br_u64)((pic->mpidr >> 8) & 0xFFull)) << 16;  /* Aff1 */
    val |= ((br_u64)((pic->mpidr >> 16) & 0xFFull)) << 32; /* Aff2 */
    val |= ((br_u64)((pic->mpidr >> 32) & 0xFFull)) << 48; /* Aff3 */
    val |= ((br_u64)intid) << 24;                          /* INTID */
    /* TargetList: 只打本核。位图只有 16 位(Aff0 = 0..15), 低位以外的 Aff0 无意义,
     * 这里与 0xF 掩码既合架构也避免移位计数越界(UB)。 */
    val |= (br_u64)1u << (pic->mpidr & 0xFull);
    br_gic_write_icc_sgi1r_el1(val);
    br_gic_isb();   /* 让 SGI 尽早被投递(自检用例要立刻看到中断) */
}

static int gicv3_trigger(br_pic_t *pic, br_u32 hwirq)
{
    if (pic == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (hwirq > BR_GICV3_INTID_SGI_MAX) {
        return BR_ERR(BR_ENOTSUP);   /* 非 SGI 不能软件触发 */
    }
    gicv3_sgi1r_write(pic, hwirq);
    return 0;
}

/* =====================================================================
 * 12. CPU 侧屏蔽(PSTATE.I 的方言包装 = L2; 3-02 §6.4)
 * ===================================================================== */
static br_u32 gicv3_cpu_mask(br_pic_t *pic)
{
    br_u64 daif;

    (void)pic;

    /* 返回**原始 DAIF**(core 的 br_irq_state_t 自己解释位布局, §6.4) */
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");   /* #2 = bit1 = PSTATE.I */
    return (br_u32)daif;
}

static void gicv3_cpu_unmask(br_pic_t *pic, br_u32 state)
{
    (void)pic;

    /* v1**只管理 I**(§6.4): F/A/D 原样不动, 不"顺手恢复"。
     * state 是 cpu_mask() 拿到的原始 DAIF; bit7 = I。 */
    if ((state & BR_AARCH64_DAIF_I) != 0u) {
        __asm__ volatile("msr daifset, #2" ::: "memory");
    } else {
        __asm__ volatile("msr daifclr, #2" ::: "memory");
    }
}

/* =====================================================================
 * 13. 亲和性(仅 CAP_AFFINITY; 3-02 §13.1/§13.2)
 * ===================================================================== */
static int gicv3_set_affinity(br_pic_t *pic, br_u32 hwirq, br_u32 cpu_mask)
{
    br_u32 aff0 = 0u;

    if (pic == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    /* SGI 每次发送自带目标; PPI 每核私有 —— 二者都不可路由(§13.1) */
    if (hwirq < BR_GICV3_INTID_SPI_BASE) {
        return BR_ERR(BR_ENOTSUP);
    }
    if (hwirq >= pic->nr_irq) {
        return BR_ERR(BR_EINVAL);
    }
    if (cpu_mask == 0u) {
        return BR_ERR(BR_EINVAL);
    }
    while (aff0 < 32u && ((cpu_mask >> aff0) & 1u) == 0u) {
        aff0++;
    }
    if (aff0 >= 32u) {
        return BR_ERR(BR_EINVAL);
    }

    /* GICD_IROUTER<n>(8 B/SPI): 本原型只表达"Aff0 = 目标核下标, Aff1/2/3 = 0"
     * (单簇 QEMU virt)。IRM=0(按亲和性路由, 不是"任意核")。 */
    br_gic_mmio_write64(pic->gicd_base, BR_GICD_IROUTER + hwirq * 8u, (br_u64)aff0);
    return 0;
}

/* =====================================================================
 * 14. ops 填表
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

    /* 仅当 CAP_NEST; v1 恒不嵌套(IR-6), core 不会调用 ⇒ 留 NULL 并注明 */
    .set_running_prio = BR_NULL,

    .set_affinity = gicv3_set_affinity,

    /* 仅当 CAP_RESYNC; PM 属 v2(§14.5)⇒ 留 NULL 并注明 */
    .save = BR_NULL,
    .restore = BR_NULL,
};

/* =====================================================================
 * 15. 对外面(br_gicv3.h)
 * ===================================================================== */
int br_gicv3_register(const br_gicv3_cfg_t *cfg)
{
    int pic_id;
    int rc;

    if (cfg == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (cfg->gicd_base == 0u || cfg->gicr_base == 0u) {
        return BR_ERR(BR_EINVAL);
    }
    if (cfg->gicr_frame_size == 0u || cfg->gicr_max_frames == 0u) {
        return BR_ERR(BR_EINVAL);
    }
    if (cfg->nr_irq == 0u || cfg->nr_irq > (BR_GICV3_INTID_MAX + 1u)) {
        return BR_ERR(BR_EINVAL);
    }

    /* 平台数据拷入实例(ISA 层不保留指向 platform 数据的指针) */
    s_gicv3.gicd_base       = cfg->gicd_base;
    s_gicv3.gicr_base       = cfg->gicr_base;
    s_gicv3.gicr_frame_size = cfg->gicr_frame_size;
    s_gicv3.gicr_max_frames = cfg->gicr_max_frames;
    s_gicv3.nr_irq          = cfg->nr_irq;
    s_gicv3.gicr_frame      = 0u;
    s_gicv3.gicr_sgi        = 0u;
    s_gicv3.ready           = 0u;

    /* EARLY 相: 先按 §14.3 的序列把硬件配好; 失败 = 启动失败(负 errno 上抛) */
    rc = gicv3_init(&s_gicv3);
    if (rc != 0) {
        return rc;
    }

    pic_id = br_pic_register(&s_gicv3, &s_gicv3_ops);
    if (pic_id < 0) {
        return pic_id;   /* -ENOSPC / -EBUSY / -EINVAL 原样上抛 */
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
        return BR_ERR(BR_ENODEV);
    }
    if (intid > BR_GICV3_INTID_SGI_MAX) {
        return BR_ERR(BR_EINVAL);
    }
    gicv3_sgi1r_write(&s_gicv3, intid);
    return 0;
}

/* ---- 交叉校验回读(§4.3 第 5 条: 只允许 conformance/debug 用) ---- */
int br_gicv3_readback_enabled(br_u32 intid, int *out)
{
    br_u32 val;

    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (s_gicv3.ready == 0u) {
        return BR_ERR(BR_ENODEV);
    }
    if (intid >= s_gicv3.nr_irq) {
        return BR_ERR(BR_EINVAL);
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
        return BR_ERR(BR_EINVAL);
    }
    if (s_gicv3.ready == 0u) {
        return BR_ERR(BR_ENODEV);
    }
    if (intid >= s_gicv3.nr_irq) {
        return BR_ERR(BR_EINVAL);
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
        return BR_ERR(BR_EINVAL);
    }
    if (s_gicv3.ready == 0u) {
        return BR_ERR(BR_ENODEV);
    }
    if (intid >= s_gicv3.nr_irq) {
        return BR_ERR(BR_EINVAL);
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
 * conformance/debug 专用: 直接按 INTID 改硬件使能位(**绕过 core 的绑定/描述符**)
 * =====================================================================
 *
 * 存在的唯一理由(TC-IRQ-008): 要验证 core 的"领到 hwirq 却查不到 virq ⇒
 * spurious_owned"分支, 需要一个"**硬件会投递但 core 无属主**"的场景。而这个场景在
 * 正常路径下**构造不出来** —— 没有绑定/注册的线在 GIC 里本来就是 disabled 的,
 * 谁也不会去 unmask 它(这也正是"注册前天然安全"的来源, §6.2)。
 * ⇒ 由一致性用例自己把硬件打开: 它模拟的是"固件/引导器留下一条已使能但无人认领的线"
 *   —— 恰恰是 spurious_owned 想抓的那类集成期故障。
 *
 * ⚠ 产品代码**不得**调用: 它绕过 core 的影子/深度模型(§4.3), 会让 spurious_owned
 *   失去"真 bug 信号"的意义。审计时它应只出现在 platform 的 conformance 文件里。
 */
int br_gicv3_raw_line_enable(br_u32 intid, int enable)
{
    if (s_gicv3.ready == 0u) {
        return BR_ERR(BR_ENODEV);
    }
    if (intid >= s_gicv3.nr_irq) {
        return BR_ERR(BR_EINVAL);
    }

    if (enable != 0) {
        gicv3_unmask(&s_gicv3, intid);
    } else {
        gicv3_mask(&s_gicv3, intid);
    }
    return BR_OK;
}
