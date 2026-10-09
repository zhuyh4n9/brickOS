/*
 * brickOS prototype v0.2.0 — io/uart-pl011 **插件私有**内部契约(不导出, 不在 include/)
 *
 * 给谁看: 本插件自己的 `src/uart_selftest.c`(生产实现 `src/uart_pl011.c` 也 include 它)。
 * 放在 `src/`(而不是 `include/`)是刻意的 —— `include/` 下的一切都是**插件对外面**
 * (会被 `brickie check` 的接口域治理); 而这里是**平台数据真值**, 不构成器件契约。
 *
 * ## 为什么自检要读内部
 *   TC-IO-003 断言 GET_INFO 回答的平台事实: `info.base == 0x0900_0000`、
 *   `info.refclk_hz == 24 MHz`、`info.baud == 115200`。这三个数**就是**被测对象 ——
 *   若套件里另抄一份, 判据就变成"复制品与复制品一致", 生产文件改了值它也照样绿。
 *   ⇒ 真值只写一处(本头), **生产与套件都 include 它**: 一个来源, 两个消费者。
 *
 * ## 为什么这里**没有**寄存器访问器(与 cdev-core 的 `cdev_internal.h` 的差别)
 *   自检不碰 MMIO: 它对器件的全部观察都经 `/dev/uart0` 的公开文件面(ioctl 的回答 /
 *   读写往返), 所以寄存器偏移、FR/RSR/ICR 位定义与 `pl011_reg_read/write` 全都留在
 *   `uart_pl011.c` 里保持 static —— 本件没有"被测对象就是内部机制本身"的判据。
 */
#ifndef BR_IO_PL011_INTERNAL_H
#define BR_IO_PL011_INTERNAL_H

#include <br/core/br_types.h>

/* =====================================================================
 * 平台数据(QEMU virt; 与 platform/qemu-aarch64/src/console_pl011.c 同源)
 *
 * 数值若与 console 侧不一致, 各自的 _Static_assert 会在编译期报出来 —— 重复但可自证,
 * 比"抽一个谁都能改的公共常量"安全(两者属不同插件, core 不提供"PL011 寄存器表"这种
 * 归属不明的共享头)。
 * ===================================================================== */

/* QEMU virt 的 PL011 基址(设计侧属"平台数据"; v1 直写常量, 平台数据插件化是后续刀) */
#define BR_PL011_BASE        0x09000000UL

/*
 * QEMU virt 的 PL011 参考时钟 24 MHz(QEMU hw/arm/virt.c 以 24MHz 驱动 PL011 模型)。
 * 115200 8N1 的除数: 24e6 / (16 × 115200) = 13.0208… ⇒ IBRD=13, FBRD=round(0.0208×64)=1
 */
#define BR_PL011_REFCLK_HZ   24000000u
#define BR_PL011_BAUD        115200u
#define BR_PL011_IBRD_VALUE  13u
#define BR_PL011_FBRD_VALUE  1u

/*
 * 除数公式(一份真值, 编译期与运行期共用):
 *   divisor = refclk / (16 × baud); IBRD = 整数部分; FBRD = round(小数部分 × 64)
 * 分数的定点是 1/64 —— 所以 FBRD 的"四舍五入"要写成 `(rem×64 + den/2) / den`(den = 16×baud),
 * 而不是先算浮点(本镜像 -mgeneral-regs-only, 没有 FP)。
 */
#define BR_PL011_DIV_DEN(baud)   ((br_u64)16u * (br_u64)(baud))
#define BR_PL011_DIV_IBRD(clk, baud) \
    ((br_u32)((br_u64)(clk) / BR_PL011_DIV_DEN(baud)))
#define BR_PL011_DIV_FBRD(clk, baud) \
    ((br_u32)((((br_u64)(clk) % BR_PL011_DIV_DEN(baud)) * 64u \
               + (BR_PL011_DIV_DEN(baud) / 2u)) / BR_PL011_DIV_DEN(baud)))

/* 除数常量与参考时钟绑死: 改时钟忘了改除数会在编译期报出来(与 console_pl011.c 同法)。
 * 断言放在本头 = 生产与套件两个编译单元各验一次, 而真值只有这一份。 */
_Static_assert(BR_PL011_DIV_IBRD(BR_PL011_REFCLK_HZ, BR_PL011_BAUD) == BR_PL011_IBRD_VALUE,
               "PL011 IBRD 与参考时钟/波特率不一致");
_Static_assert(BR_PL011_DIV_FBRD(BR_PL011_REFCLK_HZ, BR_PL011_BAUD) == BR_PL011_FBRD_VALUE,
               "PL011 FBRD 与参考时钟/波特率的小数分频不一致");

#endif /* BR_IO_PL011_INTERNAL_H */
