/*
 * brickOS prototype v0.1.0 — GICv3 驱动契约(ISA 层方言)
 *
 * 设计依据:
 *   `docs/1-01-architecture.md` §8「平台能力三层模式」——
 *     中断控制器 | `br_pic`(mask/unmask/ack/prio/eoi) | **GICv3 驱动** | 中断号绑定、路由策略
 *   `docs/3-02-int.md` §4.1(`br_pic_ops` 填表)/§4.1.1(EOImode 义务)/§7.1(优先级编码归方言)
 *
 * ⚠ **层次说明(原型的诚实边界, WORKAROUND br-wa-isa-001)**: 设计侧 GICv3 的寄存器
 *   序列属 **ISA 共享库(可复用库, 非插件)**。本原型还没有独立的 ISA 库层, 因此
 *   `src/gicv3.c` 与 platform 插件同目录 —— 但**文件边界就是层边界**: 本头与
 *   `gicv3.c` 只描述"换控制器型号就变"的东西(寄存器偏移/位域/序列), 不含任何
 *   QEMU/板级事实(基址/绑定表/引脚在 `plat_qemu_virt.c` + `board_irq.c`)。
 *
 * ★ 本驱动必须自己吞掉的三件方言差异(core 不知道):
 *   1. `ICC_IAR1_EL1` 的 **1020–1023 整个区间**折算为 BR_PIC_NO_IRQ(§5.3);
 *   2. `ICC_CTLR_EL1.EOImode` 必须为 0(否则 eoi 需要两步)(§4.1.1);
 *   3. 逻辑优先级(小 = 高)→ 硬件编码的**量化**: GICv3 的有效位在**高位
 *      (MSB 对齐)**, 位宽不足时向**低优先级**方向饱和(§7.1, 绝不静默提权)。
 */
#ifndef BR_PLATFORM_BR_GICV3_H
#define BR_PLATFORM_BR_GICV3_H

#include <br/core/br_types.h>
#include <br/core/br_irq.h>     /* BR_IRQ_TRIG_* / BR_IRQ_PRIO_*(方言要按这些逻辑编码解释请求) */
#include <br/core/br_pic.h>

/* GICv3 的 INTID 分界(架构常量, 与板子无关) */
#define BR_GICV3_INTID_SGI_MAX   15u     /* 0–15   软件生成中断(每核) */
#define BR_GICV3_INTID_PPI_BASE  16u     /* 16–31  每核私有外设中断(arch timer = 30) */
#define BR_GICV3_INTID_PPI_MAX   31u
#define BR_GICV3_INTID_SPI_BASE  32u     /* 32+    共享外设中断 */
#define BR_GICV3_INTID_SPECIAL   1020u   /* 1020–1023: core 必须折算为 BR_PIC_NO_IRQ */
#define BR_GICV3_INTID_MAX       1023u

/* PMR 放行全部可实现优先级(§7.1 的 init 义务) */
#define BR_GICV3_PMR_ALL         0xFFu

/* 平台数据: GICv3 实例的地址与规模(换板子就变 ⇒ 归 platform) */
typedef struct {
    br_uintptr_t gicd_base;       /* 分发器(GICD) */
    br_uintptr_t gicr_base;       /* 重分发器(GICR)帧数组基址 */
    br_u32       gicr_frame_size; /* 单核帧跨度(2 × 64 KiB) */
    br_u32       gicr_max_frames; /* 亲和性匹配的扫描上限 */
    br_u32       nr_irq;          /* INTID 上界(报告给 caps; 不用于分配表) */
} br_gicv3_cfg_t;

/*
 * 初始化本核的 GICv3 CPU 接口并注册 PIC 实例(EARLY 相, 全局关中断)。
 * 内部顺序(3-02 §14.3 步 1–2): `ICC_SRE_EL1` → GICD_CTLR(ARE_NS + Grp1) →
 * 按 MPIDR 亲和性找本核 GICR 帧 → 唤醒 → 设 IGROUPR0/IPRIORITYR0 →
 * `ICC_PMR_EL1 = 0xFF` → `ICC_IGRPEN1_EL1 = 1` → 断言 `EOImode == 0`。
 * 返回 pic_id(≥ 0)或负 errno。
 */
int br_gicv3_register(const br_gicv3_cfg_t *cfg);

/* 记录本次 init 读到的 GICv3 事实(诊断/自检; 均为 0 表示尚未 init) */
typedef struct {
    br_u8  prio_bits;      /* 有效优先级位数(ICC_CTLR_EL1.PRIbits + 5) */
    br_u8  eoimode;        /* 配好之后读回的值(必须为 0) */
    br_u32 spi_lines;      /* GICD_TYPER 报的 SPI 线数 */
    br_u32 mpidr;          /* 本核亲和性 */
} br_gicv3_info_t;

void br_gicv3_info(br_gicv3_info_t *out);

/*
 * 软件触发一条 SGI(仅 INTID 0–15; CAP_SW_TRIGGER)。
 * 走**完全相同的入口路径**(ack → ISR → eoi), 因此它是中断子系统的自检通道(§8.4)。
 */
int br_gicv3_sgi_trigger(br_u32 intid);

/* 交叉校验用的硬件回读(CAP_READBACK; 只允许 conformance/debug 用, §4.3 第 5 条) */
int br_gicv3_readback_enabled(br_u32 intid, int *out);
int br_gicv3_readback_prio   (br_u32 intid, br_u8 *out);
int br_gicv3_readback_pending(br_u32 intid, int *out);

/*
 * conformance 专用: 直接按 INTID 改硬件使能位(绕过 core 的绑定/影子)。
 * 用途只有一个 —— 构造"硬件会投递但 core 无属主"的场景(TC-IRQ-008 的 spurious_owned,
 * 模拟"固件留下一条已使能却无人认领的线")。**产品代码不得调用**(见 gicv3.c 的注释)。
 */
int br_gicv3_raw_line_enable(br_u32 intid, int enable);

#endif /* BR_PLATFORM_BR_GICV3_H */
