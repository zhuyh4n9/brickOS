/*
 * brickOS prototype v0.2.0 — PL011 UART 字符设备(io/uart-pl011 插件的对外面)
 *
 * 设计依据:
 *   - `1-03-roadmap.md` §1 插件清单: `io/uart-pl011` = "PL011: 早期轮询 console
 *     (M0: platform 早期 console, **不注册设备**) → **中断 tty(M2: 注册 cdev)**"
 *   - `8-01-device.md` §1.1(体系分层: 器件 → 设备类 → dev-core →(devfs)→ VFS)、
 *     §6(驱动编写者契约: 实现 ops 表 + 三条纪律 + 声明资源)
 *   - `8-01-device.md` §5(ioctl 用 Linux 兼容编码; 本器件的族魔数 = `'u'`)
 * 逐条裁定(尤其"v1 为什么是轮询 cdev 而不是中断 tty")见
 * `docs/decisions/0009-vfs-storage-stack.md` §3 裁定 7, 并登记 `WORKAROUND(br-wa-io-001)`。
 *
 * ## 与 platform 早期 console 的关系
 *   platform 的 `console_pl011.c` 是 **M0 早期 console**(轮询 putc, 形态 C standalone:
 *   不注册设备) —— 那是设计**内**的形态(`1-01` §8 的 console 双形态)。本插件是它的
 *   **M2 形态 A** 侧: 把同一个 PL011 注册成 `cdev`, 于是 `/dev/uart0` 可被 `br_open` 打开。
 *   两者共用同一份硬件: console 继续负责内核日志(含 panic 通道), `/dev/uart0` 供
 *   消费者做设备语义的读写(设计 `8-01` §1.2 的分工, 不是重复实现)。
 *
 * ## 寄存器与时钟事实(QEMU virt)
 *   基址 0x0900_0000; 参考时钟 24 MHz; 115200 8N1 ⇒ IBRD=13 FBRD=1(与 console 同源常量 ——
 *   两处都写是因为它们属不同插件; 数值不一致会在各自的 _Static_assert 上报出来)。
 */
#ifndef BR_IO_BR_UART_PL011_H
#define BR_IO_BR_UART_PL011_H

#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

/* /dev/<name>(设计 `8-01` §2 的命名规则: [a-z][a-z0-9]*) */
#define BR_UART_PL011_DEV_NAME   "uart0"

/* ioctl 族魔数(设计 `8-01` §5: 每驱动族一个) */
#define BR_UART_IOC_MAGIC        'u'

/* 本器件支持的 ioctl(cmd = BR_IOR/IOW/IOWR(BR_UART_IOC_MAGIC, nr, type)) */
#define BR_UART_IOC_GET_INFO     0u   /* BR_IOR('u', 0, br_uart_pl011_info_t) */
#define BR_UART_IOC_FLUSH_RX     1u   /* BR_IO ('u', 1): 丢弃 RX FIFO 里的残留 */
#define BR_UART_IOC_SET_BAUD     2u   /* BR_IOW('u', 2, br_u32): 改波特率(Hz) */

/* `BR_UART_IOC_GET_INFO` 的回答(会话级查询; 只读)。 */
typedef struct br_uart_pl011_info {
    br_u32 base;           /* 寄存器基址(平台数据, 供诊断) */
    br_u32 refclk_hz;      /* 参考时钟 */
    br_u32 baud;           /* 当前波特率 */
    /* ★ PL011 的 FR 只有 RXFE/RXFF 两个状态位, **没有** FIFO 深度字段 ⇒ 这里报的是
     * 诚实的下界: 0(RXFE 置位 = 空) / 16(RXFF 置位 = 满, FIFO 深 16) / 其余 1。
     * 报"精确可读字节数"需要读 FIFO(有副作用), 查询语义不该有副作用。 */
    br_u32 rx_available;   /* 0 / 1 / 16 —— 见上注 */
    /* 累计 overrun 次数。★ 来源是 **UARTRSR[3]**(ARM DDI 0183): PL011 的 FR 里
     * bit3 是 BUSY 而不是 OE, 清 overrun 走 UARTICR[10](OEIC)—— ICR[3] 是保留位。
     * 用错位会把 BUSY 当成"丢了字节"并误写保留位。 */
    br_u32 rx_dropped;
} br_uart_pl011_info_t;

/*
 * 生命周期钩子(名 = `symbol_prefix` + 相; 生成物引用)。
 *   `early_init` EARLY: 无动作(console 已由 platform 建好; 此刻无线程无堆)
 *   `init`       CORE: `br_cdev_register("uart0", …)` —— 注册即出现在 /dev
 *   `start`      START: 只报一行设备摘要(一致性用例不在这里跑, 见下面的说明)
 */
int uart_pl011_early_init(void);
int uart_pl011_init(void);
int uart_pl011_start(void);

/* ==================================================================== 一致性用例 */

/* 本器件的一致性用例(自编号 `TC-IO-*`; 登记在 `br-wa-test-001`)已移到
 * `src/uart_selftest.c`(实现 `uart_pl011_selftest()`), 由 core 的
 * `br_plugin_manager_selftest()` 经描述符的 `.selftest` 钩子统一驱动(ADR-0010)。
 * 为什么从对外头里删掉: 测试入口**不是**器件的 golden 接口面 —— 它是"本器件自证"的机制,
 * 消费者不该依赖它, 它也不该参与接口 hash/版本治理(加一条用例不该是接口变更)。 */

#endif /* BR_IO_BR_UART_PL011_H */
