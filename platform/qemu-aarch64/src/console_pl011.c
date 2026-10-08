/*
 * brickOS prototype v0.1.0 — PL011 早期 console(轮询)
 *
 * 实现 core 拥有的"早期 console 协议(轮询 putc)"(core/include/br/core/br_console.h),
 * 符合设计 1-01 §8 的三层模式: 接口在 core, 平台给"用哪个 UART、波特率"。
 *
 * 已还清(ADR-0005): 本文件是 Platform Entry 的一部分(未做成插件 —— 这是**设计内**的形态,
 * 形态本身**不是** workaround —— 设计 1-01 §8 明确 console 双形态, 轮询版
 * 就是 M0 的早期 console(M0 的 `io/uart-pl011` 也不注册设备, 见 1-03 §1)。
 *
 * v0.1.0 不注册设备、不开中断、不用 FIFO 中断; devfs/cdev 是 M2 的事。
 */
#include <br/core/br_console.h>
#include <br/platform/br_plat.h>

/* QEMU virt 的 PL011 基址(设计侧这是"平台数据"的一部分, 将来进 platform 插件) */
#define BR_PL011_BASE        0x09000000UL

/* PL011 寄存器偏移(PrimeCell UART, ARM DDI 0183) */
#define BR_PL011_DR          0x000   /* Data Register            */
#define BR_PL011_FR          0x018   /* Flag Register            */
#define BR_PL011_IBRD        0x024   /* Integer Baud Rate Divisor*/
#define BR_PL011_FBRD        0x028   /* Fractional Baud Divisor  */
#define BR_PL011_LCR_H       0x02c   /* Line Control             */
#define BR_PL011_CR          0x030   /* Control                  */
#define BR_PL011_ICR         0x044   /* Interrupt Clear          */

#define BR_PL011_FR_TXFF     (1u << 5)   /* TX FIFO full */

#define BR_PL011_CR_UARTEN   (1u << 0)
#define BR_PL011_CR_TXE      (1u << 8)
#define BR_PL011_CR_RXE      (1u << 9)

#define BR_PL011_LCR_H_8N1_FIFO  0x70u   /* WLEN=11(8bit) | FEN=1 */

/*
 * QEMU virt 的 PL011 参考时钟 24 MHz(平台参数, 不是猜的: QEMU hw/arm/virt.c
 * 以 24MHz 驱动 PL011 模型)。115200 8N1 的除数:
 *     24e6 / (16 * 115200) = 13.0208 -> IBRD=13, FBRD=round(0.0208*64)=1
 */
#define BR_PL011_REFCLK_HZ   24000000u
#define BR_PL011_BAUD        115200u
#define BR_PL011_IBRD_VALUE  13u
#define BR_PL011_FBRD_VALUE  1u

/* 除数常量与参考时钟绑死: 改时钟忘了改除数会在编译期报出来 */
_Static_assert(BR_PL011_REFCLK_HZ / (16u * BR_PL011_BAUD) == BR_PL011_IBRD_VALUE,
               "PL011 IBRD 与参考时钟/波特率不一致");

static void pl011_write(br_u32 off, br_u32 value)
{
    *(volatile br_u32 *)(BR_PL011_BASE + off) = value;
}

static br_u32 pl011_read(br_u32 off)
{
    return *(volatile br_u32 *)(BR_PL011_BASE + off);
}

void br_console_init(void)
{
    pl011_write(BR_PL011_CR, 0);              /* 先关, 再改参数 */
    pl011_write(BR_PL011_ICR, 0x7ffu);        /* 清残留中断 */
    pl011_write(BR_PL011_IBRD, BR_PL011_IBRD_VALUE);
    pl011_write(BR_PL011_FBRD, BR_PL011_FBRD_VALUE);
    pl011_write(BR_PL011_LCR_H, BR_PL011_LCR_H_8N1_FIFO);
    pl011_write(BR_PL011_CR,
                BR_PL011_CR_UARTEN | BR_PL011_CR_TXE | BR_PL011_CR_RXE);
}

/* 不做 '\n' 转换的裸 putc: 转换必须发生在最外层一次, 否则 '\n' 会被补两次 '\r'。 */
static void pl011_putc_raw(char c)
{
    while ((pl011_read(BR_PL011_FR) & BR_PL011_FR_TXFF) != 0) {
        /* 忙等: TX FIFO 有位置。没有中断, 也就没有别的可做。 */
    }

    pl011_write(BR_PL011_DR, (br_u32)(br_u8)c);
}

void br_console_putc(char c)
{
    /* 终端友好: 输出 '\n' 时补 '\r'。串口线只认 CRLF; 少了 CR 在某些
     * 终端上表现为"阶梯状"换行。这个转换只在轮询 console 层做。 */
    if (c == '\n') {
        pl011_putc_raw('\r');
    }

    pl011_putc_raw(c);
}

void br_console_write(const char *buf, br_size_t len)
{
    for (br_size_t i = 0; i < len; i++) {
        br_console_putc(buf[i]);
    }
}

void br_console_puts(const char *s)
{
    while (*s != '\0') {
        br_console_putc(*s);
        s++;
    }
}
