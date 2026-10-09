/*
 * brickOS prototype v0.1.0 — QEMU virt (aarch64) 内存映射子系统的平台观测面
 *
 * 设计依据:
 *   - `docs/3-os-core/3-04-memory.md` §1: v1 虚拟内存政策 = **恒等映射 + 属性隔离**;
 *   - `docs/3-os-core/3-01-core-api-list.md` §7(br-mm 组: region 表 + map/unmap +
 *     cache 维护)与 §13.6(memory 三轴: ops × granularity × regions; map/protect 属 P4,
 *     只有 platform 可声明);
 *   - `docs/6-test/6-01-test.md` §3.5/§3.6(TC-MEM-* 与 TC-MM-* 在 target 上的期望)。
 *
 * 三层模式(设计 1-01 §8)在本子系统的落点:
 *   接口在 core(`br/core/br_mm.h`: `br_mm_ops_t` / `br_mm_register` / `br_mm_activate`),
 *   构造在本平台(`src/mmu.c` 的页表 + `src/memmap.c` 的 region 表),
 *   数据也在本平台(QEMU virt 的 RAM/MMIO 地址与尺寸)。
 *
 * 为什么本头文件的函数不在 core 的观测契约里(CA-5 的"面小"纪律):
 *   它们是**平台自检/排障面** —— 一致性用例(`src/mm_conf.c`)要拿"页表里的真实结果"
 *   与"用例的期望"对拍, 而 `br_mm_query` 只回答属性、不回答"VA 翻译到哪个 PA"。
 *   恒等映射的判据只能是"页表里的 PA == VA 页框", 所以 `br_plat_mmu_translate()` 是
 *   本平台对用例开放的**唯一可靠**翻译观测口(与 ADR-0003 登记平台增量的处置同型)。
 */
#ifndef BR_PLATFORM_BR_MMU_H
#define BR_PLATFORM_BR_MMU_H

#include <br/core/br_types.h>

/* ---------------------------------------------------------------- 平台内存布局
 * 这些常量是**页表布局与 region 表共用的唯一真值**(设计 3-04 §1: 页表布局归 platform)。
 * 放在平台对外头上而不是藏在 .c 里: 一致性用例要用同一个窗口边界造"未映射地址",
 * 若用例自己再写一份字面量, 窗口一改就会出现"用例还在测旧边界"的静默漂移。
 */

/* RAM 段起始(QEMU virt: -m 128M 时 RAM = 0x40000000..0x48000000) */
#define BR_PLAT_RAM_BASE        0x40000000ul

/* 4 KiB 页表的 **RAM 映射窗口** = 8 MiB(L1[1] → L2 → 4 张 L3)。
 * 窗口之外(本 RAM 段的其余部分)**保持 invalid**, 于是"访问未声明 RAM"取
 * translation fault —— 这是用例构造可恢复 fault 的唯一手段(TC-MM-* / MM-UNMAP)。
 * 为什么是 8 MiB: 镜像 + 启动栈 + 五块池(共 2.5 MiB 余)全落在里面, 再多映射纯属浪费
 * 页表内存。 */
#define BR_PLAT_RAM_WINDOW_SIZE 0x00800000ul
#define BR_PLAT_RAM_WINDOW_END  (BR_PLAT_RAM_BASE + BR_PLAT_RAM_WINDOW_SIZE)

/* 低 1 GiB = 整段 512 条 2 MiB Device-nGnRnE 块(L1[0])。
 * 为什么整段而不是"只映射用到的 1 MiB": QEMU virt 的外设/ROM/FLASH 全在低 1 GiB, 且
 * GICR 帧一直排到 0x081A0000 —— 逐设备映射既脆(漏一块就是一个诡异 fault)又要额外页表。 */
#define BR_PLAT_DEVICE_SPAN     0x40000000ul

/* 池基址的对齐粒度: 2 MiB。池基址取 `ALIGN_UP(&__stack_top, 2 MiB)`, 于是镜像/栈与
 * 池之间自然留出 < 2 MiB 的空隙(region 表禁重叠, 合并成一条 IMAGE region 之后靠它隔开)。 */
#define BR_PLAT_POOL_ALIGN      0x00200000ul

/* 一致性用例用的"RAM 段窗口外、页表 invalid"地址: 0x42000000 在 8 MiB 窗口
 * (0x40000000+0x800000 = 0x40800000)之外, 但在 128 MiB RAM 段之内 —— QEMU 不会因为
 * "访问不存在的内存"给总线错误, 页表的 invalid 项才是确定性 fault 的来源。 */
#define BR_PLAT_CONF_UNMAPPED   0x42000000ul

/* ---------------------------------------------------------------- MMU 观测面 */

/* 页表/寄存器的回读快照。`sctlr/tcr/mair/ttbr0` 由 `mrs` 直接读硬件(不是我们写的影子
 * 变量的副本)—— "装进去了"与"硬件真的收下了"是两件事, 用例判的是后者。 */
typedef struct {
    br_u64 sctlr;           /* SCTLR_EL1: M(bit0)=1 表示 MMU 已开 */
    br_u64 tcr;             /* TCR_EL1: T0SZ/EPD1/IPS/TG0 等 */
    br_u64 mair;            /* MAIR_EL1: Attr0=Normal WB / Attr1=Device-nGnRnE */
    br_u64 ttbr0;           /* TTBR0_EL1: L1 表物理基址 */
    br_u32 mapped_pages;    /* 当前有效的 4 KiB 页数(RAM 窗口内) */
    br_u32 device_blocks;   /* 当前有效的 2 MiB Device 块数(低 1 GiB 背景) */
    br_u32 regions;         /* core region 表条目数(br_mm_region_count) */
} br_plat_mmu_info_t;

/* 回读上面那张快照。`out == NULL` 时无操作(调用方不必先判)。 */
void br_plat_mmu_info(br_plat_mmu_info_t *out);

/* 单页属性探测: `br_mm_query(addr, 4 KiB, out_attrs)` 的薄封装。
 * 为什么要有这一层: 用例读的是"页表真实属性", 走 core 的 query 派发即可, 但用例里
 * 反复出现 `br_mm_query(a, BR_PAGE_SIZE, &v)` 会让意图淹没在样板里(见 6-01 §3.6)。 */
int br_plat_mmu_probe(br_uintptr_t addr, br_u32 *out_attrs);

/* 页表翻译: 返回 `va` 对应页框的物理地址(base 部分, 不含页内偏移)。
 * 未映射 / 表项 invalid ⇒ `-EFAULT`; `pa == NULL` ⇒ `-EINVAL`。
 * ★ 这是"恒等映射"这条政策的**唯一判据**(MM-IDENT): `br_mm_query` 只说"已映射且是什么
 *   属性", 说不出"翻到哪"; 只有真的走一遍页表并把 base 取出来, 才能断言 PA == VA 页框。 */
int br_plat_mmu_translate(br_uintptr_t va, br_uintptr_t *pa);

/* ---------------------------------------------------------------- 启动接线
 * `br_plat_early_init()` 按此顺序调这两步(**只做平台侧的数据与机制**)。
 *
 * ★ 池的**认领**(`br_mem_init`)与页表的**建立**(`br_mm_activate`)不在这里 ——
 *   它们是 core.init 的一格(设计 1-01 §9: "堆 · 中断框架 · 注册表 · 调度框架对象"),
 *   由 core 的入口 `br_core_main()` 在 platform 插件初始化之后执行。分工:
 *   platform 提供 region 数据 + 页表构造机制, core 提供抽象并执行建立动作。
 *   见 `docs/decisions/0008-core-main-boot-chain.md` 与 `core/src/main.c` 的阶段 ③。
 */

/* ① 声明 region 表(镜像/栈/五块池/MMIO)。**只登记, 不认领池、不碰页表**。
 * 返回 0 或负 errno(重叠 / 粒度不合 / 表满 = 启动失败, 调用方负责停机)。 */
int br_plat_memmap_init(void);

/* ② 注册平台页表 ops(`br_mm_register`)—— 只填"建表 + 开 MMU"的机制。
 * 返回 0 或负 errno(重复注册 ⇒ core 报 -EBUSY)。 */
int br_plat_mmu_ops_register(void);

#endif /* BR_PLATFORM_BR_MMU_H */
