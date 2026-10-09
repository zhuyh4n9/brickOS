/*
 * brickOS prototype v0.1.0 — QEMU virt (aarch64) region 表声明 + 池初始化
 *
 * 设计依据:
 *   - `docs/3-os-core/3-04-memory.md` §1/§2: region 表由 platform 在 early_init 声明;
 *     "core 不拥有 RAM, 只认领 region"(3-01 §6 原文: platform region 表把 RAM 划为
 *     heap / contig 池 / 页池, 比例 = manifest 预算);
 *   - `docs/3-os-core/3-01-core-api-list.md` §7(`br_mm_region_add` 归 Platform =
 *     map/protect 属 P4/machine, §13.6)与 §6(`br_mem_init` 按种类认领池);
 *   - `docs/6-test/6-01-test.md` §3.5/§3.6: MM-POOLS/MM-REGION 判的就是"声明的表"与
 *     "core 认领的池"一致。
 *
 * 本文件是**数据**: 哪块内存是什么(QEMU virt 的地址与尺寸)。
 * 机制在 `src/mmu.c`(页表构造)与 core 的 `br_mem`/`br_mm` 实现里。
 *
 * ★ 本文件**只声明 region 表**: 池的认领(`br_mem_init`)与页表建立(`br_mm_activate`)
 *   是 core 的四阶段启动链里的阶段 ③(见 `core/src/main.c` / ADR-0008)。
 *
 * ★ 仍登记在案的欠债(详见 WORKAROUNDS.md):
 *   - `WORKAROUND(br-wa-mem-001)`: 下面的**池比例写死在这里**(1 MiB / 256 KiB /
 *     1 MiB / 256 KiB / 16 KiB), 未经 manifest 的 `[budget]`/`[[res]]` 生成 ——
 *     设计 3-04 §2 要的是"比例 = manifest 预算", 而 v0.1 还没有那条生成链路。
 *
 * ★ region 表的两条硬约束(由 br_mm.h 的契约给出, 这里必须自己守):
 *   ① base/size 必须 4 KiB 对齐与页粒度 —— 页表在 4 KiB 粒度上表达属性隔离, 声明面
 *      比它更细就没有落点;
 *   ② 互不重叠 —— 重叠 = "同一页有两个属性真值", 那是无法执行的声明。
 */
#include <br/core/br_error.h>
#include <br/core/br_mem.h>      /* BR_PAGE_SIZE(页粒度) */
#include <br/core/br_mm.h>

#include <br/platform/br_mmu.h>
#include <br/platform/br_plat.h>

/* 链接脚本提供的镜像/栈边界(唯一真值在 platform/qemu-aarch64/src/link.ld) */
extern char __image_start[];
extern char __stack_top[];

#define MEM_ALIGN_UP(v, a)  (((v) + ((br_uintptr_t)(a) - 1ull)) & ~((br_uintptr_t)(a) - 1ull))

/* 池尺寸。为什么是这些数(原型的预算裁定, 够跑完一致性用例且都落在 8 MiB 窗口内):
 *   HEAP    1 MiB   —— TLSF 堆; 用例 TC-MEM-002 的 128 块压力 + 每次 4 KiB 往返都够
 *   CONTIG  256 KiB —— 连续池; 够 DMA/重定位演示的块级分配
 *   PAGE    1 MiB   —— 页池 = 256 页(CA-8 的位图规模)
 *   DMA     256 KiB —— DMA 缓冲池(CA-6: 建于 contig 语义之上)
 *   RESERVED 16 KiB —— 不分配: 一致性用例拿它做 RO/NX 真保护验证(改坏也有地方收拾)
 */
#define MEM_HEAP_SIZE       (1024u * 1024u)
#define MEM_CONTIG_SIZE     (256u * 1024u)
#define MEM_PAGE_POOL_SIZE  (1024u * 1024u)
#define MEM_DMA_SIZE        (256u * 1024u)
#define MEM_RESERVED_SIZE   (16u * 1024u)

/* MMIO 窗口(QEMU virt 的地址, 与 platforms/... 的 GICv3/PL011 基址一致) */
#define MEM_GICD_BASE       0x08000000ul
#define MEM_GICD_SIZE       0x00010000ul    /* GICD 分发器帧: 64 KiB */
#define MEM_GICR_BASE       0x080A0000ul
#define MEM_GICR_SIZE       0x00100000ul    /* GICR redistributor 帧: 1 MiB(到 0x081A0000) */
#define MEM_UART0_BASE      0x09000000ul
#define MEM_UART0_SIZE      0x00001000ul    /* PL011: 4 KiB */

/* 一条 region 的登记样板(靠近声明处, 免得读的人要跳来跳去) */
static int memmap_add(br_uintptr_t base, br_size_t size, br_u32 attrs)
{
    br_mm_region_t r;

    r.base  = base;
    r.size  = size;
    r.attrs = attrs;
    return br_mm_region_add(&r);
}

static int memmap_declare_regions(void)
{
    const br_uintptr_t img_base  = (br_uintptr_t)&__image_start[0];
    const br_uintptr_t stack_top = (br_uintptr_t)&__stack_top[0];
    int rc;

    /*
     * ① IMAGE: 镜像(text/rodata/data/bss)+ 启动栈。
     *
     * 为什么合成一条(而不是 IMAGE / STACK 两条): link.ld 里 `__stack_bottom` 紧跟在
     * `__image_end` 之后、只隔一个 16 字节对齐缝 —— 两条 region 各自按 4 KiB 向上取整
     * 之后必然覆盖同一个页框, 直接撞上 region 表的**重叠禁则**。这是"页粒度声明"带来
     * 的真实约束, 不是偷懒: 栈与镜像在同一批页里, 属性也只能是同一套。
     * 尺寸 = 从镜像基址到 __stack_top 向上取整到页(栈顶之上到池基址的空隙由
     * BR_PLAT_POOL_ALIGN 的 2 MiB 对齐自然留出, 不声明 ⇒ 那几页也不会被谁用)。
     */
    rc = memmap_add(img_base, MEM_ALIGN_UP(stack_top - img_base, BR_PAGE_SIZE),
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_IMAGE));
    if (rc != 0) {
        return rc;
    }

    /*
     * ② 池基址: 2 MiB 对齐(留 < 2 MiB 余量, 并保证五块池整体落在 8 MiB RAM 映射窗口内)。
     * 下面五条 region 依次紧排, 互不重叠。
     */
    const br_uintptr_t pool_base     = MEM_ALIGN_UP(stack_top, BR_PLAT_POOL_ALIGN);
    const br_uintptr_t heap_base     = pool_base;
    const br_uintptr_t contig_base   = heap_base + MEM_HEAP_SIZE;
    const br_uintptr_t page_base     = contig_base + MEM_CONTIG_SIZE;
    const br_uintptr_t dma_base      = page_base + MEM_PAGE_POOL_SIZE;
    const br_uintptr_t reserved_base = dma_base + MEM_DMA_SIZE;
    const br_uintptr_t pool_end      = reserved_base + MEM_RESERVED_SIZE;

    /* 守卫: 池窗口必须整个落在页表的 4 KiB 页区里。溢出 ⇒ 声明面与页表布局不一致,
     * 明确失败(ENOSPC)而不是"映射到一半"或静默截断。 */
    if (pool_end > BR_PLAT_RAM_WINDOW_END) {
        return BR_ERR(BR_ENOSPC);
    }

    /* TLSF 字节堆: malloc/calloc/realloc/free 的唯一来源(3-01 §6) */
    rc = memmap_add(heap_base, MEM_HEAP_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_HEAP));
    if (rc != 0) {
        return rc;
    }

    /* 物理连续池(CA-7): 不走 TLSF, 供 DMA 引擎与 v2 重定位的大块搬运 */
    rc = memmap_add(contig_base, MEM_CONTIG_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_CONTIG));
    if (rc != 0) {
        return rc;
    }

    /* 页池(CA-8): 静态位图 + 连续 run 首次适配; 256 页 @ 4 KiB */
    rc = memmap_add(page_base, MEM_PAGE_POOL_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_PAGE));
    if (rc != 0) {
        return rc;
    }

    /* DMA 缓冲池(CA-6): 保证物理连续(v1 恒等 ⇒ dma_addr == vaddr) */
    rc = memmap_add(dma_base, MEM_DMA_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_DMA));
    if (rc != 0) {
        return rc;
    }

    /* 保留区: 不交给任何池; 一致性用例在这里做 RO/NX 真保护验证 */
    rc = memmap_add(reserved_base, MEM_RESERVED_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));
    if (rc != 0) {
        return rc;
    }

    /*
     * ③ MMIO 三条: 全部落在低 1 GiB 的 Device 背景映射里(L1[0] 的 2 MiB 块),
     *    所以页表侧无需再落表 —— 声明的作用是**给 core 一张可查的属性表**
     *    (cache 维护判"是不是可缓存"、dump/ramdump 判"这块能不能读", 都靠它)。
     *    尺寸取帧的**实际大小**, 不取"能覆盖它的 2 MiB 块": region 表是给软件查的,
     *    多声明出来的地址会让"未声明地址"的判据(MM-REGION/MM-UNMAP)失真。
     */
    rc = memmap_add(MEM_GICD_BASE, MEM_GICD_SIZE,
                    BR_MM_DEVICE | BR_MM_WITH_KIND(0u, BR_MM_KIND_MMIO));
    if (rc != 0) {
        return rc;
    }

    rc = memmap_add(MEM_GICR_BASE, MEM_GICR_SIZE,
                    BR_MM_DEVICE | BR_MM_WITH_KIND(0u, BR_MM_KIND_MMIO));
    if (rc != 0) {
        return rc;
    }

    rc = memmap_add(MEM_UART0_BASE, MEM_UART0_SIZE,
                    BR_MM_DEVICE | BR_MM_WITH_KIND(0u, BR_MM_KIND_MMIO));
    return rc;
}

int br_plat_memmap_init(void)
{
    /*
     * ★ 本函数**只声明** region 表(哪块内存是什么)。
     *   池的**认领**(`br_mem_init()`)与页表**建立**(`br_mm_activate()`)都是 core.init
     *   的一格(设计 1-01 §9: "堆 · 中断框架 · 注册表 · 调度框架对象"), 由 core 的入口
     *   `br_core_main()` 在 platform 插件初始化之后执行 —— 见
     *   `docs/decisions/0008-core-main-boot-chain.md` 与 `core/src/main.c` 的阶段 ③。
     *   这样 platform 只提供"数据 + 换型号就变的机制", memory 管理/分配与地址映射的
     *   建立动作归 core。
     *
     * 声明失败(重叠 / 粒度不合 / 表满)= 启动失败(3-02 §14.3 的错误处理义务, 与
     * board_irq.c 的 IRQ 初始化同一纪律)。
     */
    return memmap_declare_regions();
}
