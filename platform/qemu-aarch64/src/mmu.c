/*
 * brickOS prototype v0.1.0 — QEMU virt (aarch64) 页表构造(恒等映射 + region 属性)
 *
 * 设计依据:
 *   - `docs/3-os-core/3-04-memory.md` §1: v1 虚拟内存政策 = **恒等映射 + 属性隔离**;
 *   - `docs/3-os-core/3-01-core-api-list.md` §7(`br_mm_ops_t` 的四件机械通路:
 *     activate / map_region / set_attrs / query)与 §13.6(map/protect 属 P4 = machine,
 *     只有 platform 可声明 —— 这就是"页表构造在 platform"的归属依据);
 *   - CA-8: 页粒度 = 平台编译期常量; 本原型固定 4 KiB(`BR_PAGE_SIZE`), 页池/页表/L3
 *     描述符口径一致。
 *
 * 本文件只做"机械通路", 不做政策:
 *   - 政策(哪块 RAM 是什么池 / 哪段是 MMIO)在 `src/memmap.c` 的 region 表里;
 *   - 状态机(未注册 → 已注册 → 已激活)在 core 的 `br_mm_activate()`;
 *   - 本文件把 region 表**读成页表**, 并把页表**读回**成属性(query)。
 *
 * ★ `WORKAROUND(br-wa-isa-001)`: 设计把"页表构造 + cache 一致性协议的 aarch64 实现"
 *   归 **ISA 共享库(非插件)**(3-04 §2、3-01 §7 抬头), 而原型还没有这一层 ——
 *   本文件因此暂居 platform 插件目录, 靠**文件边界**保住层次: 这里只有"换架构/换粒度
 *   就会变"的描述符序列, 板级事实在 `memmap.c`。还债动作见 WORKAROUNDS.md。
 *
 * ★ 页表内存放在 .bss(镜像内), 因此**恒等映射必须覆盖页表自己**: 写 SCTLR_EL1.M 之后
 *   的那条取指、以及之后每一次走表, 都要能被新页表翻译。页表基址 = 链接器给的镜像地址,
 *   落在 8 MiB RAM 窗口内(见 `br_plat_memmap_init` 的窗口守卫)。
 */
#include <br/core/br_mm.h>
#include <br/core/br_error.h>
#include <br/core/br_log.h>

#include <br/platform/br_mmu.h>

/* =====================================================================
 * 常量与描述符编码
 * ===================================================================== */
#define MMU_ENTRIES          512u                  /* 任意一级表的条目数 */
#define MMU_GRANULE          4096u                 /* 4 KiB 粒度(TG0=0b00) */
#define MMU_L2_BLOCK_SIZE    0x00200000u           /* 2 MiB L2 块 */
#define MMU_RAM_L3           4u                    /* 8 MiB / 2 MiB = 4 张 L3 */
#define MMU_CACHE_LINE       64u                   /* aarch64 最小 cache line(维护粒度) */

/* 描述符编码(4 KiB 粒度, 39 位 VA ⇒ walk 从 L1 起, 共 4 级):
 *
 *   表描述符(L1/L2 → 下一级表):  next_table_pa | 0b11
 *   L2 块描述符(2 MiB):           pa            | 0b01 | attrs
 *   L3 页描述符(4 KiB):           pa            | 0b11 | attrs
 *
 *   公共位:   AF(bit10) = 1
 *   Normal:   SH(bits 9:8) = 0b11(内共享), AttrIndx(bits 4:2) = 0(MAIR Attr0)
 *   Device:   SH(bits 9:8) = 0b00(非共享), AttrIndx(bits 4:2) = 1(MAIR Attr1)
 *   AP[2:1](bits 7:6): 0b00 = RW, 0b10 = RO(bit7 = 1)
 *   NX:       PXN(bit53) = 1 且 UXN(bit54) = 1
 */
#define DESC_TYPE_MASK       0x3ull
#define DESC_TABLE          0x3ull        /* 表描述符 valid + table */
#define DESC_BLOCK          0x1ull        /* L2 块描述符 valid + block */
#define DESC_PAGE           0x3ull        /* L3 页描述符 valid + page */
#define DESC_AF             (1ull << 10)
#define DESC_SH_SHIFT       8u
#define DESC_SH_INNER       0x3ull        /* Normal: 内共享 */
#define DESC_SH_NONE        0x0ull        /* Device: 非共享 */
#define DESC_AP_SHIFT       6u
#define DESC_AP_RO          (0x2ull << DESC_AP_SHIFT)   /* AP[2] = bit7 = 1 ⇒ EL1 只读 */
#define DESC_ATTRIDX_SHIFT  2u
#define DESC_ATTRIDX_NORMAL 0ull          /* MAIR Attr0 = 0xFF(Normal WB RA/WA) */
#define DESC_ATTRIDX_DEVICE 1ull          /* MAIR Attr1 = 0x00(Device-nGnRnE) */
#define DESC_PXN            (1ull << 53)
#define DESC_UXN            (1ull << 54)

/* 属性位掩码: 改属性时**保留 base 与类型位**, 只翻转这些位 */
#define DESC_ATTR_MASK      (DESC_AF | (0x3ull << DESC_SH_SHIFT) | (0x3ull << DESC_AP_SHIFT) \
                             | (0x7ull << DESC_ATTRIDX_SHIFT) | DESC_PXN | DESC_UXN)

/* 输出地址掩码(按 ARMv8 的 4 KiB 粒度描述符布局) */
#define MM_ADDR_MASK        0x0000FFFFFFFFF000ull   /* bits 47:12 */
#define MM_BLOCK_MASK_L2    0x0000FFFFFFE00000ull   /* bits 47:21(2 MiB 块) */

/* MAIR_EL1: Attr0 = 0xFF(Normal WB RA/WA, 可缓存 RAM)、Attr1 = 0x00(Device-nGnRnE)、
 * Attr2 = 0x44(Normal Non-cacheable, 备用; 本原型不产生该 AttrIndx 的描述符) */
#define MMU_MAIR_VALUE      0x004400FFull

/*
 * TCR_EL1(照 3-04 §1 的页表政策):
 *   T0SZ(bits 5:0)   = 25      ⇒ VA 39 位, walk 从 L1 起
 *   IRGN0(bits 9:8)  = 0b01    ⇒ 走表: 内 WB WA(可缓存)
 *   ORGN0(bits 11:10)= 0b11    ⇒ 走表: 外 Non-cacheable
 *   SH0(bits 13:12)  = 0b11    ⇒ 走表: 内共享(单核也取它, 免得将来多核时 walk 不共享)
 *   TG0(bits 15:14)  = 0b00    ⇒ 4 KiB 粒度
 *   EPD1(bit23)      = 1       ⇒ **只用 TTBR0**; bit63 = 1 的地址一律取翻译 fault
 *   IPS(bits 34:32)  = 0b010   ⇒ 40 位物理地址(本原型的 RAM/MMIO 全在 4 GiB 内)
 */
#define MMU_TCR_VALUE       ((25ull) | (1ull << 8) | (3ull << 10) | (3ull << 12) \
                             | (1ull << 23) | (2ull << 32))

/* SCTLR_EL1 的 RES1 位: bit11/20/22/23/28/29 必须为 1(读改写时显式置上, 不假设复位值) */
#define MMU_SCTLR_RES1      ((1ull << 11) | (1ull << 20) | (1ull << 22) | (1ull << 23) \
                             | (1ull << 29) | (1ull << 28))
#define MMU_SCTLR_M         (1ull << 0)     /* MMU enable */
#define MMU_SCTLR_C         (1ull << 2)     /* data cache enable */
#define MMU_SCTLR_I         (1ull << 12)    /* instruction cache enable */

/*
 * 对齐检查: 把 A / SA / SA0 三个开关全清。
 * ★ 编码更正(实测校准): 交办单里写的是 "清 SA(bit4)/SA0(bit5)", 那是 ARMv7/AArch32 的
 *   旧字段号。ARMv8-A 的 SCTLR_EL1 里:
 *     A   = bit1  —— **数据访存的**对齐检查(非对齐访存取 Alignment fault 的就是它)
 *     SA  = bit3  —— SP 对齐检查(EL1)
 *     SA0 = bit4  —— SP 对齐检查(EL0)
 *   要兑现"Normal 内存上的非对齐访问不取对齐 fault"这条意图, 需要清的是 A(bit1),
 *   而不是只有 SP 检查。bit5 是 CP15BEN(AArch32 的 CP15 barrier 开关), 与本意图无关,
 *   不去碰它 —— 清一个语义无关的控制位才是真的"另立方案"。
 */
#define MMU_SCTLR_A         (1ull << 1)
#define MMU_SCTLR_SA        (1ull << 3)
#define MMU_SCTLR_SA0       (1ull << 4)

/* =====================================================================
 * 页表内存(静态数组: L1 1 张 + Device L2 1 张 + RAM L2 1 张 + L3 4 张 = 7 张 = 28 KiB)
 *
 * 为什么静态而不是从页池分配: 页池的可用前提是"映射已生效", 而页表本身是映射生效的
 * 前提 —— 先有鸡还是先有蛋(CA-8 的 v1 消费者为零, 3-01 §6 原文)。静态数组由链接器
 * 钉在 .bss, start.S 的 BSS 清零顺带把全部表项清成 invalid。
 * ===================================================================== */
static br_u64 s_l1[MMU_ENTRIES]                 __attribute__((aligned(MMU_GRANULE)));
static br_u64 s_l2_dev[MMU_ENTRIES]             __attribute__((aligned(MMU_GRANULE)));
static br_u64 s_l2_ram[MMU_ENTRIES]             __attribute__((aligned(MMU_GRANULE)));
static br_u64 s_l3_ram[MMU_RAM_L3][MMU_ENTRIES] __attribute__((aligned(MMU_GRANULE)));

/* 激活状态: `activate` 只许成功一次(重复调用 ⇒ -EBUSY, 与 core 的状态机同义) */
static br_bool s_activated;

_Static_assert(MMU_RAM_L3 * MMU_L2_BLOCK_SIZE == BR_PLAT_RAM_WINDOW_SIZE,
               "L3 张数与 RAM 映射窗口不一致");

/* =====================================================================
 * 描述符与系统寄存器的小工具
 * ===================================================================== */

/* region 属性 → 描述符属性位。DEVICE 优先于 CACHED: 两位同置是自相矛盾的输入, 取更
 * 保守的 Device(猜错方向的后果是"外设寄存器被缓存", 比"RAM 走 Device 慢"严重得多)。 */
static br_u64 mmu_desc_attrs(br_u32 attrs)
{
    br_u64 d = DESC_AF;

    if ((attrs & BR_MM_DEVICE) != 0u) {
        d |= (DESC_SH_NONE << DESC_SH_SHIFT) | (DESC_ATTRIDX_DEVICE << DESC_ATTRIDX_SHIFT);
    } else {
        d |= (DESC_SH_INNER << DESC_SH_SHIFT) | (DESC_ATTRIDX_NORMAL << DESC_ATTRIDX_SHIFT);
    }

    if ((attrs & BR_MM_RO) != 0u) {
        d |= DESC_AP_RO;
    }
    if ((attrs & BR_MM_NX) != 0u) {
        d |= DESC_PXN | DESC_UXN;
    }
    return d;
}

/* 描述符属性位 → region 属性。★ 这里**不合成 region 表的 kind**:
 * query 的回答是"页表里此刻真实生效的属性", kind 是"这块内存被声明成什么"——
 * 前者是硬件事实(可能被 set_attrs 改过), 后者是声明事实(region 表)。两者分工在
 * `br_mm.h` 的观测契约里已经写明(见该头 §只读观测面): kind 由 br_mm_region_* 回答。
 * 合二为一会让 MM-RO 这类"改过属性的页"永远读回声明的 kind, 用例就失去意义。 */
static br_u32 mmu_attrs_from_desc(br_u64 d)
{
    br_u32 a = 0u;

    if ((d & DESC_AF) == 0ull) {
        return 0u;      /* AF=0 的描述符不可用, 由 mmu_query 判成未映射 */
    }

    if (((d >> DESC_ATTRIDX_SHIFT) & 0x7ull) == DESC_ATTRIDX_DEVICE) {
        a |= BR_MM_DEVICE;
    } else {
        a |= BR_MM_CACHED;      /* Attr0 = Normal WB; Attr2(备用)也归入"非 Device" */
    }
    if ((d & DESC_AP_RO) != 0ull) {
        a |= BR_MM_RO;
    }
    if ((d & (DESC_PXN | DESC_UXN)) != 0ull) {
        a |= BR_MM_NX;
    }
    return a;
}

/*
 * 页表遍历: 返回**末级描述符的可写指针**, `*out_level` = 2(2 MiB 块)或 3(4 KiB 页)。
 * 未映射 / 表项 invalid ⇒ NULL。
 *
 * 为什么用"恒等映射"的指针算术: 本平台的政策就是恒等映射, 描述符里的物理地址可以直接
 * 当指针解引用(这正是恒等映射对页表构造最大的便利, 也是 3-04 §1 选它的理由之一)。
 */
static br_u64 *mmu_desc_ptr(br_uintptr_t va, br_u32 *out_level)
{
    const br_uintptr_t l1_index = (va >> 30) & 0x1FFull;
    const br_uintptr_t l2_index = (va >> 21) & 0x1FFull;
    const br_uintptr_t l3_index = (va >> 12) & 0x1FFull;

    br_u64 *tbl = s_l1;
    br_u64  d   = tbl[l1_index];

    if ((d & DESC_TYPE_MASK) != DESC_TABLE) {
        return BR_NULL;
    }

    tbl = (br_u64 *)(br_uintptr_t)(d & MM_ADDR_MASK);
    d   = tbl[l2_index];
    if ((d & DESC_TYPE_MASK) == DESC_BLOCK) {
        *out_level = 2u;
        return &tbl[l2_index];
    }
    if ((d & DESC_TYPE_MASK) != DESC_TABLE) {
        return BR_NULL;
    }

    tbl = (br_u64 *)(br_uintptr_t)(d & MM_ADDR_MASK);
    d   = tbl[l3_index];
    if ((d & DESC_TYPE_MASK) != DESC_PAGE) {
        return BR_NULL;
    }
    *out_level = 3u;
    return &tbl[l3_index];
}

/* 描述符 → 物理地址(页框 base, 不含页内偏移) */
static br_uintptr_t mmu_desc_pa(br_uintptr_t va, br_u64 d, br_u32 level)
{
    if (level == 2u) {
        return (va & ~((br_uintptr_t)MMU_L2_BLOCK_SIZE - 1ull)) | (br_uintptr_t)(d & MM_BLOCK_MASK_L2);
    }
    return (va & ~((br_uintptr_t)MMU_GRANULE - 1ull)) | (br_uintptr_t)(d & MM_ADDR_MASK);
}

/* 改过页表之后必须让 TLB 看见: 本原型取最粗但绝不会错的粒度(vmalle1)。
 * 逐条 `tlbi vaae1` 是优化, 不是正确性 —— 而正确性优先(4 KiB 页只有 2048 条映射)。 */
static void mmu_tlb_flush_all(void)
{
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static br_u64 mmu_read_sctlr(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    return v;
}

static void mmu_write_sctlr(br_u64 v)
{
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(v) : "memory");
}

static br_u64 mmu_read_tcr(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(v));
    return v;
}

static br_u64 mmu_read_mair(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(v));
    return v;
}

static br_u64 mmu_read_ttbr0(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(v));
    return v;
}

/* =====================================================================
 * 建表(activate 的内部动作)
 * ===================================================================== */

/* RAM 窗口的**背景映射**: 4 张 L3 逐页映射, 默认 Normal WB RW 可执行。
 * "未声明覆盖的页仍映射"是刻意的(3-04 §1): 恒等映射的价值在于"内核看自己的
 * 内存时地址不变", 若窗口内留空洞, 任何未声明的局部对象都会莫名 fault。 */
static void mmu_fill_ram_window(void)
{
    const br_u64 normal = mmu_desc_attrs(BR_MM_CACHED);

    for (br_u32 t = 0; t < MMU_RAM_L3; t++) {
        s_l2_ram[t] = (br_u64)(br_uintptr_t)&s_l3_ram[t][0] | DESC_TABLE;

        for (br_u32 i = 0; i < MMU_ENTRIES; i++) {
            const br_uintptr_t pa = (br_uintptr_t)BR_PLAT_RAM_BASE
                                    + ((br_uintptr_t)t * MMU_L2_BLOCK_SIZE)
                                    + ((br_uintptr_t)i * MMU_GRANULE);
            s_l3_ram[t][i] = (br_u64)pa | DESC_PAGE | normal;
        }
    }
    s_l1[1] = (br_u64)(br_uintptr_t)&s_l2_ram[0] | DESC_TABLE;
}

/* 低 1 GiB 的**背景映射**: 512 条 2 MiB Device-nGnRnE 块(L1[0])。
 * 覆盖 QEMU virt 的全部外设/ROM/FLASH(GICD 0x08000000、GICR 0x080A0000..0x081A0000、
 * PL011 0x09000000 都在里面), 所以 region 表里的 MMIO 声明**不需要**再落表 —— 背景
 * 已经是对的(见 mmu_region_apply 对 Device 区"属性一致即无操作"的处置)。 */
static void mmu_fill_device_span(void)
{
    const br_u64 device = mmu_desc_attrs(BR_MM_DEVICE);

    for (br_u32 i = 0; i < MMU_ENTRIES; i++) {
        const br_uintptr_t pa = (br_uintptr_t)i * MMU_L2_BLOCK_SIZE;
        s_l2_dev[i] = (br_u64)pa | DESC_BLOCK | device;
    }
    s_l1[0] = (br_u64)(br_uintptr_t)&s_l2_dev[0] | DESC_TABLE;
}

/*
 * 把一条 region 的属性落进页表(activate 期间与激活后的 `map_region` 共用)。
 *
 * 两个区间、两种粒度:
 *   - RAM 窗口(4 KiB 页区): 逐页改属性, 这是"属性隔离"真正发生的地方;
 *   - 低 1 GiB(2 MiB Device 块区): QEMU virt 的 MMIO 帧(GICD 64 KiB / GICR 1 MiB /
 *     PL011 4 KiB)都**小于一个块**, 页表在 2 MiB 粒度上根本表达不了"只改这 4 KiB 的
 *     属性"。所以边界是硬的: 子块范围的改属性 ⇒ `-ENOTSUP`(见 MM-NXDESC 的注释:
 *     不假装支持硬件表达不了的事); 属性与背景一致 ⇒ 无操作返回 0; 整块对齐的范围 ⇒
 *     可以整块改。
 *   - 两个区间之外 ⇒ `-EINVAL`(声明面与页表布局不一致, 不能静默忽略)。
 */
static int mmu_region_apply(const br_mm_region_t *r)
{
    const br_uintptr_t base = r->base;
    const br_size_t    size = r->size;

    if (base < BR_PLAT_RAM_WINDOW_END && (base + size) > BR_PLAT_RAM_BASE) {
        const br_u64 attrs = mmu_desc_attrs(r->attrs);

        if (base < BR_PLAT_RAM_BASE || (base + size) > BR_PLAT_RAM_WINDOW_END) {
            return BR_ERR(BR_EINVAL);       /* 跨窗口边界: 4 KiB 页区表达不了 */
        }

        for (br_size_t off = 0u; off < size; off += MMU_GRANULE) {
            br_u32 level = 0u;
            br_u64 *d = mmu_desc_ptr(base + off, &level);

            if (d == BR_NULL) {
                return BR_ERR(BR_EINVAL);   /* 页表没覆盖到 ⇒ 布局与声明不一致 */
            }
            if (level != 3u) {
                return BR_ERR(BR_ENOTSUP);  /* 块粒度上做子块改属性: 表达不了 */
            }
            *d = (*d & ~DESC_ATTR_MASK) | attrs;
        }
        mmu_tlb_flush_all();
        return 0;
    }

    if (base < BR_PLAT_DEVICE_SPAN && (base + size) <= BR_PLAT_DEVICE_SPAN) {
        const br_u64 want = mmu_desc_attrs(r->attrs);

        if (want == mmu_desc_attrs(BR_MM_DEVICE)) {
            return 0;       /* 背景已是 Device ⇒ 声明成立, 无需改动页表 */
        }
        if ((base % MMU_L2_BLOCK_SIZE) == 0ull && (size % MMU_L2_BLOCK_SIZE) == 0u) {
            for (br_size_t off = 0u; off < size; off += MMU_L2_BLOCK_SIZE) {
                br_u32 level = 0u;
                br_u64 *d = mmu_desc_ptr(base + off, &level);

                if (d == BR_NULL || level != 2u) {
                    return BR_ERR(BR_EINVAL);
                }
                *d = (*d & ~DESC_ATTR_MASK) | want;
            }
            mmu_tlb_flush_all();
            return 0;
        }
        return BR_ERR(BR_ENOTSUP);
    }

    return BR_ERR(BR_EINVAL);
}

/* =====================================================================
 * br_mm_ops_t 的六件实现
 * ===================================================================== */

static int mmu_activate(const br_mm_region_t *regions, br_u32 count)
{
    if (s_activated) {
        return BR_ERR(BR_EBUSY);
    }
    if (regions == BR_NULL && count != 0u) {
        return BR_ERR(BR_EINVAL);
    }

    /* ① 背景映射: 低 1 GiB 的 Device 块 + RAM 窗口的 Normal 页。
     *    (BSS 已由 start.S 清零, 这里只写"用得到的项", 其余保持 invalid ——
     *     窗口之外与 1 GiB..4 GiB 之间都是翻译 fault 的来源。) */
    mmu_fill_device_span();
    mmu_fill_ram_window();

    /* ② region 表覆盖: 逐条把声明的属性写进页表(表与页表不留第二真值) */
    for (br_u32 i = 0; i < count; i++) {
        const int rc = mmu_region_apply(&regions[i]);
        if (rc != 0) {
            return rc;
        }
    }

    /* ③ 装页表基址与内存属性寄存器 */
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((br_u64)(br_uintptr_t)&s_l1[0]) : "memory");
    __asm__ volatile("msr tcr_el1, %0"   :: "r"(MMU_TCR_VALUE)  : "memory");
    __asm__ volatile("msr mair_el1, %0"  :: "r"(MMU_MAIR_VALUE) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* ④ 读改写 SCTLR_EL1 并开 MMU。
     *    保留原值(不覆盖固件/QEMU 留下的设置), 显式:
     *      - 置 M|C|I(bit0/2/12);
     *      - 清 A/SA/SA0(bit1/3/4) —— 否则 Normal 内存上的非对齐访问仍可能取对齐 fault,
     *        而本原型的页表政策恰恰是"Normal 内存允许非对齐"(见 Makefile 对 -mstrict-align
     *        的注释: 它是防御性旋钮, 不是硬要求了; 字段号更正见上面的 MMU_SCTLR_A 注释);
     *      - 把 RES1 位(bit11/20/22/23/28/29)置 1 —— 这些位必须为 1, 不假设复位值。 */
    {
        br_u64 sctlr = mmu_read_sctlr();

        sctlr |= (MMU_SCTLR_M | MMU_SCTLR_C | MMU_SCTLR_I | MMU_SCTLR_RES1);
        sctlr &= ~(MMU_SCTLR_A | MMU_SCTLR_SA | MMU_SCTLR_SA0);
        mmu_write_sctlr(sctlr);
        __asm__ volatile("isb" ::: "memory");
    }

    s_activated = BR_TRUE;

    /*
     * 平台侧对自己的硬件事实留痕(页数/块数/寄存器回读)。core 的 `br_mem_init()` 已
     * 单独打了"堆/池"摘要(见 ADR-0008 的阶段 ③) —— 两边各说自己的事实, 不互相代打。
     */
    {
        br_plat_mmu_info_t info;

        br_plat_mmu_info(&info);
        br_log_info("mem: identity map on (SCTLR=0x%lx, regions=%u, 4 KiB pages=%u, 2 MiB device blocks=%u)",
                    info.sctlr, info.regions, info.mapped_pages, info.device_blocks);
        br_log_info("mem: TTBR0=0x%lx TCR=0x%lx MAIR=0x%lx",
                    info.ttbr0, info.tcr, info.mair);
    }
    return 0;
}

static int mmu_map_region(const br_mm_region_t *r)
{
    if (r == BR_NULL || r->size == 0u) {
        return BR_ERR(BR_EINVAL);
    }
    if (!s_activated) {
        return BR_ERR(BR_ENODEV);       /* 页表还没建; activate 会按快照一次构造 */
    }
    return mmu_region_apply(r);
}

static int mmu_set_attrs(br_uintptr_t addr, br_size_t size, br_u32 attrs)
{
    if ((addr & ((br_uintptr_t)MMU_GRANULE - 1ull)) != 0ull || size == 0u) {
        return BR_ERR(BR_EINVAL);
    }

    /* 长度向下取整到页(跨末页的部分不做半页保护: 4 KiB 粒度上没有"半页"这回事) */
    br_size_t remaining = size & ~((br_size_t)MMU_GRANULE - 1ull);
    br_uintptr_t cur = addr;
    const br_u64 new_attrs = mmu_desc_attrs(attrs);

    while (remaining > 0u) {
        br_u32 level = 0u;
        br_u64 *d = mmu_desc_ptr(cur, &level);

        if (d == BR_NULL) {
            return BR_ERR(BR_EINVAL);       /* 未映射的区间不许改属性 */
        }

        if (level == 2u) {
            /* 2 MiB 块: 只接受"整块对齐 + 覆盖整块"的请求(Device 区边界见文件头) */
            if ((cur & ((br_uintptr_t)MMU_L2_BLOCK_SIZE - 1ull)) != 0ull
                || remaining < (br_size_t)MMU_L2_BLOCK_SIZE) {
                return BR_ERR(BR_ENOTSUP);
            }
            *d = (*d & ~DESC_ATTR_MASK) | new_attrs;
            cur += MMU_L2_BLOCK_SIZE;
            remaining -= MMU_L2_BLOCK_SIZE;
        } else {
            *d = (*d & ~DESC_ATTR_MASK) | new_attrs;
            cur += MMU_GRANULE;
            remaining -= MMU_GRANULE;
        }
    }

    mmu_tlb_flush_all();
    return 0;
}

static int mmu_query(br_uintptr_t addr, br_size_t size, br_u32 *out_attrs)
{
    if (out_attrs == BR_NULL || size == 0u) {
        return BR_ERR(BR_EINVAL);
    }
    if ((addr + size) < addr) {
        return BR_ERR(BR_EINVAL);       /* 区间回绕 */
    }

    /*
     * 区间按"覆盖到的页"逐个走表: 全映射 + 全同属性才给答案;
     * 属性不齐(半页 RO / 半页 NX)不是"一个属性", 报 -EINVAL 而不是挑第一页蒙混。
     * ★ addr 不要求页对齐 —— 调用方问的是"这个地址/这段字节所在页此刻什么属性"
     *   (MM-IDENT 抽样的正是变量与栈上的**非对齐**地址), 只要求内部页边界对齐即可。
     */
    const br_uintptr_t lo = addr & ~((br_uintptr_t)MMU_GRANULE - 1ull);
    const br_uintptr_t hi = (addr + size - 1ull) & ~((br_uintptr_t)MMU_GRANULE - 1ull);
    br_u32 first = 0u;
    br_u32 index = 0u;

    for (br_uintptr_t cur = lo; ; cur += MMU_GRANULE) {
        br_u32 level = 0u;
        const br_u64 *d = mmu_desc_ptr(cur, &level);

        if (d == BR_NULL || (*d & DESC_AF) == 0ull) {
            return BR_ERR(BR_EFAULT);
        }

        const br_u32 a = mmu_attrs_from_desc(*d);
        if (index == 0u) {
            first = a;
        } else if (a != first) {
            return BR_ERR(BR_EINVAL);
        }
        index++;

        if (cur >= hi) {
            break;
        }
    }

    *out_attrs = first;     /* ★ 只有页表事实, 不含 BR_MM_KIND_*(见 mmu_attrs_from_desc) */
    return 0;
}

/*
 * cache 维护(设计 3-01 §7; 驱动 DMA 前后, 风险 R4)。
 * core 已经判过"这段是不是可缓存"以及"MMU 是否已开"(见 br_mm.h 的语义), 所以这里
 * 只做机械动作: 逐 64 B 行发维护指令 + 一次 dsb。
 *   - flush     = `dc cvac`(clean 到 PoC; 保留副本, 送出去)
 *   - invalidate= `dc ivac`(invalidate; 丢掉本地副本, 下次读从内存取)
 */
static void mmu_cache_flush(void *addr, br_size_t size)
{
    const br_uintptr_t base = (br_uintptr_t)addr;

    for (br_size_t off = 0u; off < size; off += MMU_CACHE_LINE) {
        __asm__ volatile("dc cvac, %0" :: "r"(base + off) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
}

static void mmu_cache_invalidate(void *addr, br_size_t size)
{
    const br_uintptr_t base = (br_uintptr_t)addr;

    for (br_size_t off = 0u; off < size; off += MMU_CACHE_LINE) {
        __asm__ volatile("dc ivac, %0" :: "r"(base + off) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
}

static const br_mm_ops_t s_mm_ops = {
    .activate         = mmu_activate,
    .map_region       = mmu_map_region,
    .set_attrs        = mmu_set_attrs,
    .query            = mmu_query,
    .cache_flush      = mmu_cache_flush,
    .cache_invalidate = mmu_cache_invalidate,
};

/* =====================================================================
 * 平台观测面(br_mmu.h)
 * ===================================================================== */

void br_plat_mmu_info(br_plat_mmu_info_t *out)
{
    if (out == BR_NULL) {
        return;
    }

    out->sctlr = mmu_read_sctlr();
    out->tcr   = mmu_read_tcr();
    out->mair  = mmu_read_mair();
    out->ttbr0 = mmu_read_ttbr0();

    /* 计数走页表本身(不是"我们打算映射多少"的影子计数) */
    out->mapped_pages = 0u;
    for (br_u32 t = 0; t < MMU_RAM_L3; t++) {
        for (br_u32 i = 0; i < MMU_ENTRIES; i++) {
            if ((s_l3_ram[t][i] & DESC_TYPE_MASK) == DESC_PAGE) {
                out->mapped_pages++;
            }
        }
    }

    out->device_blocks = 0u;
    for (br_u32 i = 0; i < MMU_ENTRIES; i++) {
        if ((s_l2_dev[i] & DESC_TYPE_MASK) == DESC_BLOCK) {
            out->device_blocks++;
        }
    }

    out->regions = br_mm_region_count();
}

int br_plat_mmu_probe(br_uintptr_t addr, br_u32 *out_attrs)
{
    return br_mm_query(addr, (br_size_t)MMU_GRANULE, out_attrs);
}

int br_plat_mmu_translate(br_uintptr_t va, br_uintptr_t *pa)
{
    if (pa == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (!s_activated) {
        return BR_ERR(BR_ENODEV);
    }

    br_u32 level = 0u;
    const br_u64 *d = mmu_desc_ptr(va, &level);

    if (d == BR_NULL || (*d & DESC_AF) == 0ull) {
        return BR_ERR(BR_EFAULT);
    }
    *pa = mmu_desc_pa(va, *d, level);
    return 0;
}

/* =====================================================================
 * 启动接线(由 br_plat_early_init 调用, 见 br_plat.h / br_mmu.h 的顺序说明)
 *
 * ★ 本函数只**注册**平台页表 ops; "建表 + 开 MMU"(br_mm_activate)是 core.init 的
 *   一格(设计 1-01 §9), 由 core 的入口 `br_core_main()` 在 platform 插件初始化之后
 *   执行 —— 机制在 platform, 编排/状态机在 core(三层模式的边界)。见 ADR-0008。
 * ===================================================================== */
int br_plat_mmu_ops_register(void)
{
    return br_mm_register(&s_mm_ops);
}
