/*
 * brickOS prototype v0.1.0 — panic 通道的 core 私有接口(不进对外契约头)
 *
 * 为什么是私有头: `br_panic_bare`/`br_panic` 是 core 的对外观测契约(`br_fault.h`),
 * 而这里两个符号只服务于 **core 内部的一条重入护栏**(fault 路径判定"我现在是不是
 * 正在 panic 输出中")。把它放进 `br/core/` 会让它变成插件可见面, 而它没有消费方。
 *
 * 背景(实测发现, 见 ADR-0003 §5): panic 的输出走轮询 console; 如果 console 本身
 * 已经不可访问, "panic → fault → 再 panic"会变成**无界递归**, 每次压 0x140 B 异常帧,
 * 64 KiB 启动栈被吃穿后异常帧落进 .bss(实测 SP 掉进页表, 页表被覆盖后连取指都翻译
 * 不过去) —— 结果是**原始 fault 现场彻底丢失**。有了这两个符号, fault 路径就能在
 * "panic 输出期间再 fault"时**静默停机**: 第一句 panic 打得出就打, 打不出就安静停住。
 */
#ifndef BR_CORE_PANIC_INTERNAL_H
#define BR_CORE_PANIC_INTERNAL_H

#include <br/core/br_types.h>

/* 是否正在 panic 输出中(panic 入口置位, 永不复位 —— 它是终止性的)。 */
br_bool br_panic_in_progress(void);

/* 不打印、不取锁、不依赖任何栈上资源的就地停机(panic 的最简形态)。不返回。 */
BR_NORETURN void br_panic_halt(void);

#endif /* BR_CORE_PANIC_INTERNAL_H */
