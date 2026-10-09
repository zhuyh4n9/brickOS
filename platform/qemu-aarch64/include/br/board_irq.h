/*
 * brickOS prototype v0.1.0 — 板级中断号头(virq 的**唯一真值**)
 *
 * 设计依据: `docs/3-os-core/3-02-int.md` §3.1(IR-2「双层号模型」)——
 *   三层分离: **逻辑线名**(manifest 声明) / **virq**(组合期确定, 驱动只见这个) /
 *   **hwirq**(PIC 域内号, 只在 ISA 层/PIC ops 内部)。
 *   驱动源码里**零数字、零板级知识**: `#include <br/board_irq.h>` 后写
 *   `br_irq_register(BR_IRQ_TIMER, ...)`。
 *
 * ★ 为什么这个头是**手写静态头**而不是组合器生成物:
 *   3-02 §3.1 明确给了退路 —— "若 2-01/4-03 的生成头机制未落地: platform 插件
 *   导出静态头, 内含同型宏。语义等价, 仅'谁生成'不同 ⇒ 本篇设计不阻塞工具链进度"。
 *   `brickie` v0.1 只生成插件描述符(`build/gen/<plugin>/plugin_desc.c`), **不生成
 *   board_irq.h**(该机制属 2-01 的开放项 O-4)⇒ 本原型走这条**设计内**的退路。
 *
 * 号空间布局(3-02 §3.2): 0..47 = 直连 PIC 线; 48..55 = IPI(v2b 预留);
 * 56..63 = 级联域子中断窗口(`BR_IRQ_DOMAIN_BASE`, 由 domain_create 切片分配)。
 */
#ifndef BR_BOARD_IRQ_H
#define BR_BOARD_IRQ_H

#include <br/core/br_types.h>

/* ---- 直连 PIC 线(virq 0..4; 绑定表见 board_irq.c) ---- */

/* INTID 30 = EL1 物理 timer(PPI)。M0 唯一的"真的在做事"的 ISR(3-02 §11.1 路径四)。 */
#define BR_IRQ_TIMER          0u
/* INTID 33 = PL011 UART0(SPI 1)。v0.1 只有轮询 console, 中断未使用; 绑定在表里,
 * 用于验证 SPI 的 mask/prio/readback 路径。 */
#define BR_IRQ_UART0          1u
/* INTID 0  = SGI 0: 自测/一致性用例的软件触发通道(3-02 §8.4)。
 * SGI 是**每核**线, 只能在 CPU 接口上触发 —— 正好是"软件触发走完全相同的入口路径"
 * 的最小载体。 */
#define BR_IRQ_TEST_SGI       2u
/* INTID 1  = SGI 1: 风暴保护用例的载体(ISR 内自我重触发 ⇒ 电平活锁的软件等价物)。 */
#define BR_IRQ_TEST_SGI_STORM 3u
/* INTID 2  = SGI 2: 假 FAST 级联域的**父线**(域成员在 virq 56.. 窗口)。 */
#define BR_IRQ_DOMAIN_PARENT  4u
/* INTID 4  = SGI 4: 假 **SLOW** 级联域的父线(ADR-0011: demux 走下半部 bh)。 */
#define BR_IRQ_DOMAIN_SLOW    5u
/* INTID 5  = SGI 5: **BH 直连线** —— 该线以 `BR_IRQ_F_DISPATCH_BH` 注册, 于是它的 ISR
 * 不在中断上下文里跑, 而是被 core 推迟到下半部(设计 §11.1 路径一 / §12.4)。
 * 为什么单列一条线而不是复用 FAST 域的父线: 这一条验的是**按线的分发形态**,
 * 与"一根线带 N 个子源"的域机制是两回事(§11.4 的"两种入口", 不该混成一个)。 */
#define BR_IRQ_BH_LINE        6u

/* 直连线条目数(绑定表长度; 域成员不需要绑定条目 —— 见 README 的实现裁定 P-IRQ-1) */
#define BR_IRQ_BOARD_NR       7u

/* 故意**不绑定**的 SGI(INTID 3): "无属主却有硬件投递"的样本,
 * 用于 TC-IRQ-008(spurious_owned 路径)。 */
#define BR_IRQ_TEST_SGI_UNBOUND   3u

#endif /* BR_BOARD_IRQ_H */
