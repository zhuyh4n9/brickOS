/*
 * brickOS prototype v0.2.0 — PL011 UART 字符设备(cdev)驱动(实现体)
 *
 * 设计依据(权威, 逐条对应):
 *   - `1-03-roadmap.md` §1 插件清单: `io/uart-pl011` = "PL011: 早期轮询 console(M0: platform
 *     早期 console, **不注册设备**) → **中断 tty(M2: 注册 cdev)**"。本文件落地的是 M2 的
 *     **形态 A**(`8-01-device.md` §1.2): 把同一个 PL011 注册成 cdev ⇒ `/dev/uart0` 可被
 *     `br_open` 打开。"中断 tty"(RX 中断 + wait-queue)是同一 M2 的下一小步, 见下面 ②。
 *   - `8-01-device.md` §3(cdev 会话式 ops: `open/read/write/ioctl/poll/close` + D22 预留
 *     `suspend/resume`)、§4(SD-6 阻塞语义 / SD-10 负 errno / ISR 禁令)、§5(ioctl 用 Linux
 *     兼容编码, 本器件族魔数 `'u'`)
 *   - `7-01-vfs.md` §1(独占设备策略**归驱动**: uart 类在 `ops->open` 里返回 `-EBUSY`)、
 *     §3(SD-7 poll 分期: v1 = 查询就绪位; 等待侧是"轮询 + sleep")
 *   - 逐条裁定与偏离登记在 `WORKAROUND(br-wa-io-001)`; 一致性用例(TC-IO-*)在
 *     `src/uart_selftest.c`, 由 core 的自检 pass 驱动(ADR-0010); id 自编号登记在
 *     `br-wa-test-001`。
 *
 * ============================================================================
 * ① 与 platform 早期 console 的关系(同一份硬件, 两种形态 —— 设计内)
 * ============================================================================
 *   platform 的 `console_pl011.c` 是 M0 早期 console(轮询 putc, standalone, 不注册设备),
 *   它继续负责**内核日志**与 **panic 通道**。本驱动是 `/dev/uart0` 的**设备语义**面。
 *   两者共用同一个 PL011 寄存器窗口, 因此本文件对寄存器的**写入面**刻意收得很窄:
 *
 *     读: DR(数据) / FR(状态) / RSR(接收状态: overrun) / IBRD,FBRD,CR 的回读
 *     写: DR(数据) / IBRD,FBRD,CR(**仅** SET_BAUD 的"清 CR → 写除数 → 恢复 CR") / ICR(清源)
 *     绝不动: LCR_H(8N1 + FIFO 使能, console 的线路设置) / 探测期(early_init/probe)的 CR
 *
 *   为什么"注册设备"不等于"重新初始化硬件": 镜像里 console 先起来 panic 通道, 而 cdev 的
 *   CORE 相在后。若在这里重写 CR/LCR_H, 就是**第二个主人**改线路设置 —— 一旦两者不一致,
 *   panic 输出会在最需要它的时刻变成乱码。所以本驱动不 bring-up、只复用。
 *   (反过来的做法 —— 让 io/uart-pl011 拥有初始化、console 调它 —— 会让 panic 通道依赖
 *   一个可能没被组合进来的插件; 设计 `1-01` §8 的 console 双形态明确要求早期 console
 *   自持, 故不采。)
 *
 * ============================================================================
 * ② 为什么 v1 是"轮询 cdev"而不是"中断 tty"(诚实交代偏离)
 * ============================================================================
 *   设计目标是中断 tty, 但 SD-7 把事件等待分成两期: **v1 = 轮询 + br_task_sleep**,
 *   v2 才上 wait-queue 精确唤醒(必须与 preempt 同期, 否则抢占产品上轮询延迟毛刺, R-S1)。
 *   于是本文件:
 *     - **不开** UART 中断(不写 IMSC/不注册 ISR)⇒ 没有并发, 也就不需要锁;
 *     - 阻塞 read 用 `br_task_sleep(1ms)` 轮询(见 ⑤);
 *     - poll 只回答"当前就绪位"(SD-7 的 v1 口径), 不做等待。
 *   等 M2 的后半段(中断 tty)落地时, 本文件的 ops 形状**不用改** —— 变的只是 read 的等待
 *   机制(从 sleep 轮询换成 wait-queue)与 IMSC/ISR 的加入。
 *
 * ============================================================================
 * ③ 会话与独占性(7-01 §1: 策略归驱动)
 * ============================================================================
 *   会话 = `{flags, rx_dropped}`(本文件私有, 不透明地交给 cdev 适配层)。独占性**不**靠
 *   cdev-core: 谁有权利"第二个打开者被拒"是**策略**, 而 uart 是典型独占设备(两个消费者
 *   交错读同一个 FIFO 谁都不完整)。所以 `open` 在已有会话时直接 `-EBUSY`, `close` 放开。
 *   不采"按 open flags 允许多读者"的方案: v1 只有单 APP, 多读只会带来"谁吃掉这个字节"
 *   的歧义, 而歧义的代价比 -EBUSY 高得多。
 *
 * ============================================================================
 * ④ overrun(rx_dropped)的诚实来源(与任务书的差异, 已显式裁定)
 * ============================================================================
 *   任务书建议"FR bit3 = OE, 用 ICR bit3 清"。按 ARM DDI 0183(PrimeCell UART):
 *     - `UARTFR[3]` 是 **BUSY**(发送中), **不是** OE —— 拿它当丢字节计数会把"正在移位输出"
 *       误记成"RX 溢出"; UARTFR 里根本没有 OE 位。
 *     - overrun 的诚实来源是 `UARTRSR[3]`(接收状态寄存器; FIFO 模式下它是接收状态 FIFO 的
 *       读端口, 与 DR 读一一配对弹出), 与 DR 的 `[11]`=OE 同源。
 *     - 中断侧的清源位是 `UARTICR[10]`=OEIC; ICR 的 bit3 在 PL011 上是保留位。
 *   故本文件采样 RSR[3], 计数进会话的 rx_dropped, 并写 ICR.OEIC 尽"设备侧清源"义务
 *   (v1 没开 UART 中断, 这一写是幂等的; 现在写下来, M2 接中断时不必回头补)。
 *   **不采**"FR[3] + ICR[3]"的写法: 那会在真硬件上制造假计数并写保留位, 属于"照抄指令
 *   而违背硬件"的错。
 *
 * ============================================================================
 * ⑤ 阻塞语义与"可能永远等下去"(SD-6 / SD-7)
 * ============================================================================
 *   read: FIFO 空时, 非阻塞 ⇒ `-EAGAIN`; 阻塞 ⇒ `br_task_sleep(1ms)` 后重试。
 *         **v1 没有超时** —— 等的是"外部世界给不给字节", 没有期限就是没有期限(不假装
 *         知道对端什么时候打字)。代价是调用线程可能永不返回; 单独阻塞在 uart 上的线程
 *         应由上层(runtime/posix 的 poll/超时)而不是驱动来兜。
 *   write: 逐字节等 `FR.TXFF` 清零。这里是**忙等**而不是 sleep: 等的是"自己刚写进去的数据
 *         往外挪一格", 上界是 FIFO 深度 × 位时间(115200 下 16 字节 ≈ 1.4 ms), 为它睡 1 ms
 *         会把吞吐再砍一半。方向不同, 机制就不同 —— 这不是不一致, 是两种等待的性质不同。
 *
 * ============================================================================
 * ⑥ rx_available 的诚实上限
 * ============================================================================
 *   PL011 的 FR **没有** RX FIFO 水位字段(没有 16550 那种 level 寄存器), 只有 RXFE(空)/
 *   RXFF(满)。所以 GET_INFO 的 rx_available 只能给一个**下界**:
 *     RXFE ⇒ 0; RXFF ⇒ 16(FIFO 深 16); 其余 ⇒ 1(至少有 1 个, 到底几个读 FR 看不出来)。
 *   报 1 是保守可解释的("至少有 1"), 报一个猜的中间值才是撒谎。
 *   (PL011 的 FR 只有 RXFE/RXFF 两个状态位, **没有** FIFO 深度字段; 头文件已按"诚实的
 *    下界"口径写明 0/1/16 —— 报精确值需要读 FIFO, 而查询语义不该有副作用。)
 *
 * ============================================================================
 * ⑦ SET_BAUD 的共享线路风险(为什么要说清楚)
 * ============================================================================
 *   IBRD/FBRD 是**整条 PL011 线路**的参数, 而 console 与 /dev/uart0 是同一条线: 改波特率
 *   会连带改掉内核日志/panic 通道的速率。v1 仍提供这个 ioctl(设备语义要完整, 且它是
 *   "可表示性校验 + 寄存器和 console 同源常量"的最好载体), 但把风险写在明处:
 *   真产品应经 termios 层协调(console 是线路的主人), 而不是让两个消费者各写各的。
 *   恢复 CR 时用**回读值原样写回**, 不自己拼 `UARTEN|TXE|RXE` —— 线路设置的"真相"在
 *   console 侧, 本驱动只借用除数那一格。
 *
 * ============================================================================
 * ⑧ ISR 纪律
 * ============================================================================
 *   本文件全部函数都是 **thread-only**(`8-01` §4: 设备/存储域 API 禁止在 ISR 里调用,
 *   白名单为空): 会 `br_malloc`/`br_free`/`br_task_sleep`, 也会自旋等 TX。v1 不开中断,
 *   没有 ISR 会走到这里。
 */
#include <br/io/br_uart_pl011.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>     /* br_malloc / br_free(会话分配) */
#include <br/core/br_sched.h>   /* br_task_sleep(阻塞 read 的轮询节拍) */
#include <br/core/br_time.h>    /* BR_US_PER_MS */
#include <br/core/br_types.h>
#include <br/dev/br_cdev.h>     /* br_cdev_ops_t / br_cdev_register(本文件实现与调用) */
#include <br/dev/br_dev.h>      /* BR_IOC_* 编码宏 */
#include <br/vfs/br_vfs.h>      /* BR_O_NONBLOCK / BR_POLL*(read/poll 的语义位) */

#include "pl011_internal.h"    /* 平台数据(base/refclk/baud); 与自检套件同源(见该头) */

/* 生命周期钩子的原型(名 = symbol_prefix + 相, 生成物引用; 头文件里也有,
 * 这里再写一遍是为了 -Wmissing-prototypes 的读者一眼看到"本文件的对外面就这三个"。) */
int uart_pl011_early_init(void);
int uart_pl011_init(void);
int uart_pl011_start(void);

/* =====================================================================
 * 1. 硬件寄存器(平台数据 base/refclk/baud 在 src/pl011_internal.h —— 生产与自检套件
 *    共用**一份真值**; 这里只留"只有本驱动需要"的寄存器偏移与位定义)
 *
 *    与 platform/qemu-aarch64/src/console_pl011.c 是同一块硬件的两种形态, 数值各写各的:
 *    它们属于**不同插件**, core 不提供"PL011 寄存器表"这种归属不明的共享头。数值若不一致,
 *    各自的 _Static_assert(在 pl011_internal.h)/ 用例会在编译期与启动日志里报出来。
 * ===================================================================== */

/* PL011 寄存器偏移(PrimeCell UART, ARM DDI 0183) */
#define BR_PL011_DR          0x000u   /* Data Register                    */
#define BR_PL011_RSR         0x004u   /* Receive Status(读)/ Error Clear(写) */
#define BR_PL011_FR          0x018u   /* Flag Register                    */
#define BR_PL011_IBRD        0x024u   /* Integer Baud Rate Divisor        */
#define BR_PL011_FBRD        0x028u   /* Fractional Baud Rate Divisor     */
#define BR_PL011_LCR_H       0x02cu   /* Line Control(★本文件**绝不**写它) */
#define BR_PL011_CR          0x030u   /* Control                          */
#define BR_PL011_ICR         0x044u   /* Interrupt Clear                  */

/* FR 状态位(只用得到这两个 + full) */
#define BR_PL011_FR_RXFE     (1u << 4)   /* RX FIFO empty */
#define BR_PL011_FR_TXFF     (1u << 5)   /* TX FIFO full  */
#define BR_PL011_FR_RXFF     (1u << 6)   /* RX FIFO full(16 字节) */

/* 接收状态位: overrun(★不是 FR[3] —— FR[3] 是 BUSY, 见抬头 ④) */
#define BR_PL011_RSR_OE      (1u << 3)

/* ICR 清源位: overrun error interrupt clear(★不是 ICR[3], 那是保留位, 见抬头 ④) */
#define BR_PL011_ICR_OEIC    (1u << 10)

/* CR 控制位。**只登记不主动拼装**: 恢复 CR 用回读值(抬头 ⑦)。这里列出来是为了让"我们
 * 知道线路由哪几位组成"成为可读的事实, 并在 SET_BAUD 的体检里当掩码用。 */
#define BR_PL011_CR_UARTEN   (1u << 0)
#define BR_PL011_CR_TXE      (1u << 8)
#define BR_PL011_CR_RXE      (1u << 9)

/* RX FIFO 深度(PL011 固定 16; GET_INFO 的 rx_available 上界) */
#define BR_PL011_FIFO_DEPTH  16u

/* 参考时钟 / 波特率 / 除数常量与除数公式在 src/pl011_internal.h(生产与套件同源)。 */

/* =====================================================================
 * 2. 寄存器访问(volatile; 不做位域/不做 DMA —— 这是 MMIO 不是内存)
 * ===================================================================== */

static br_u32 pl011_reg_read(br_u32 off)
{
    return *(volatile br_u32 *)(BR_PL011_BASE + off);
}

static void pl011_reg_write(br_u32 off, br_u32 value)
{
    *(volatile br_u32 *)(BR_PL011_BASE + off) = value;
}

/* =====================================================================
 * 3. 驱动状态(单实例 —— v1 静态组合只有一个 PL011; 多实例要等平台数据插件化)
 * ===================================================================== */

/* 会话(设计 `8-01` §3 的"不透明 sess"): 只放真正跨调用存活的两件事。
 *   flags      = 打开时的 BR_O_*(v1 只解释 BR_O_NONBLOCK)
 *   rx_dropped = 本会话观察到的 overrun 次数(抬头 ④) */
typedef struct br_uart_pl011_sess {
    br_u32 flags;
    br_u32 rx_dropped;
} pl011_sess_t;

/* 设备私有(注册时交给 cdev-core, 之后原样回传给 open 的 dev_priv):
 *   baud   = 当前波特率(GET_INFO 回答; 探测时不该去读硬件 —— 除数寄存器可回读, 但那种
 *            "猜 console 写了什么"的回读会把"我们没改过"变成"我们认领了" —— 所以记自己的)
 *   opened = 独占标志(7-01 §1: 策略归驱动) */
typedef struct br_uart_pl011_dev {
    br_u32  baud;
    br_bool opened;
} pl011_dev_t;

static pl011_dev_t s_dev = {
    .baud   = BR_PL011_BAUD,
    .opened = BR_FALSE,
};

/* =====================================================================
 * 4. 硬件搬运(不涉及会话语义的小工具)
 * ===================================================================== */

/*
 * 采样并清 overrun。**读 RSR 即采样**(FIFO 模式下读它弹出与 DR 配对的状态条目), 因此
 * 调用点只在"确实有数据可读"或 GET_INFO 里 —— 不在空 FIFO 的阻塞循环里空转采样, 免得
 * 把状态 FIFO 的条目与 DR 读错位(见抬头 ④)。
 */
static void pl011_sample_overrun(pl011_sess_t *s)
{
    const br_u32 rsr = pl011_reg_read(BR_PL011_RSR);

    if ((rsr & BR_PL011_RSR_OE) == 0u) {
        return;
    }

    s->rx_dropped++;

    /* 设备侧清源: ICR.OEIC。v1 没使能 UART 中断 ⇒ 幂等; 但义务留在这里, M2 接中断时
     * 不必回头补(电平型中断不清源 = 活锁, 设计 3-02 §8.2 的同类纪律)。 */
    pl011_reg_write(BR_PL011_ICR, BR_PL011_ICR_OEIC);
}

/* RX FIFO 当前可读字节数的**下界**(抬头 ⑥: FR 不报水位) */
static br_u32 pl011_rx_available(void)
{
    const br_u32 fr = pl011_reg_read(BR_PL011_FR);

    if ((fr & BR_PL011_FR_RXFE) != 0u) {
        return 0u;
    }
    if ((fr & BR_PL011_FR_RXFF) != 0u) {
        return BR_PL011_FIFO_DEPTH;
    }
    return 1u;   /* 至少有 1 个; 到底几个只有读出来才知道 */
}

/* 抽干 RX FIFO, 丢弃并计入 rx_dropped(FLUSH_RX 的实现) */
static void pl011_drain_rx(pl011_sess_t *s)
{
    while ((pl011_reg_read(BR_PL011_FR) & BR_PL011_FR_RXFE) == 0u) {
        (void)pl011_reg_read(BR_PL011_DR);   /* 读出来就是扔掉: FLUSH 的语义是"不要了" */
        s->rx_dropped++;
    }
}

/* =====================================================================
 * 5. 波特率(SET_BAUD 的全部机制 + 一处 console 共享线路的风险说明)
 * ===================================================================== */

/*
 * 可表示性判定: 算出 IBRD/FBRD; 不可表示 ⇒ -EINVAL。
 *   baud == 0        ⇒ 除数为 0(不先挡掉就是除零, 不是"参数不好"而是"程序会崩")
 *   IBRD == 0        ⇒ 除数 < 1 ⇒ baud > refclk/16(24MHz 下 1.5Mbaud), PL011 表示不了
 *   IBRD > 0xFFFF    ⇒ 除数寄存器只有 16 位(极低波特率)
 *   FBRD 舍入到 64   ⇒ 进位成 IBRD+1、FBRD=0(否则写进 6 位字段会截断成 0, 速率差 1/64)
 */
static int pl011_divisors_for(br_u32 baud, br_u32 *ibrd_out, br_u32 *fbrd_out)
{
    if (baud == 0u) {
        return BR_ERR(BR_EINVAL);
    }

    br_u32 ibrd = BR_PL011_DIV_IBRD(BR_PL011_REFCLK_HZ, baud);
    br_u32 fbrd = BR_PL011_DIV_FBRD(BR_PL011_REFCLK_HZ, baud);

    if (fbrd >= 64u) {
        ibrd++;
        fbrd -= 64u;
    }
    if ((ibrd == 0u) || (ibrd > 0xffffu)) {
        return BR_ERR(BR_EINVAL);
    }

    *ibrd_out = ibrd;
    *fbrd_out = fbrd;
    return 0;
}

/*
 * 落盘除数。ARM 的要求(DDI 0183): 改 IBRD/FBRD 前先清 CR.UARTEN, 写完再恢复 CR ——
 * 运行中改除数会让线路上出现一个位宽不确定的字符。
 *
 * ★ 恢复值是**回读的 CR**, 不是自己拼 `UARTEN|TXE|RXE`: CR 是 console 与 /dev/uart0 的
 *   共享线路设置(抬头 ⑦)。自己拼常量 = 把 console 的配置悄悄改写成"我以为的样子";
 *   读-改-写连"UARTEN 本来是关的"这种状态也如实保留。
 * ★ 不等 FR.TXFE 再关: v1 的发送者是单线程日志, 调 SET_BAUD 时没有在途数据; 加一个无上界
 *   的等待反而在流控/对端不回读时把 ioctl 挂死 —— 那是在驱动里自造活锁面(与 read 的
 *   无限等待不同: read 等的是外部输入, 那是语义)。
 */
static void pl011_program_divisors(br_u32 ibrd, br_u32 fbrd)
{
    const br_u32 cr   = pl011_reg_read(BR_PL011_CR);
    const br_u32 want = BR_PL011_CR_UARTEN | BR_PL011_CR_TXE | BR_PL011_CR_RXE;

    pl011_reg_write(BR_PL011_CR, 0u);
    pl011_reg_write(BR_PL011_IBRD, ibrd);
    pl011_reg_write(BR_PL011_FBRD, fbrd);
    pl011_reg_write(BR_PL011_CR, cr);

    /* 体检(不改变行为): 线路三使能位不齐说明 console 还没 bring-up 或有人关了 UART。
     * 用 debug 级 —— 这不是错误, 是给 -v 排障留的线索, 不该在正常启动里刷屏。 */
    if ((cr & want) != want) {
        br_log_debug("uart-pl011: SET_BAUD 时 CR=0x%08x 未含 UARTEN|TXE|RXE 全体; "
                     "除数已改, 线路使能状态按 console 原样保留", cr);
    }
}

/* =====================================================================
 * 6. cdev ops 实现(设计 `8-01` §3 的会话式形状)
 * ===================================================================== */

/*
 * open(dev_priv, flags, &sess)
 *   ① 独占执法(先查再分配: 被拒的调用不该产生任何分配);
 *   ② 会话用 br_malloc 分配 —— 不塞进任何静态池: 会话生命周期与"打开"绑定, 而独占保证
 *      同一时刻最多一个 ⇒ 分配次数天然有界(不构成碎片风险)。
 *   错误: -EINVAL(参数/私有不对) / -EBUSY(已被打开) / -ENOMEM(堆尽)。
 */
static int pl011_open(void *dev_priv, br_u32 flags, void **sess)
{
    /* v1 只有一个实例: CORE 相注册时给的私有必然是 &s_dev。核验而不是静默忽略 ——
     * 框架送来别的私有, 说明注册/查找配错了对, 早失败比"打开了一个影子设备"好。 */
    if ((dev_priv != (void *)&s_dev) || (sess == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }

    if (s_dev.opened != BR_FALSE) {
        return BR_ERR(BR_EBUSY);   /* 独占: 策略在驱动, 见抬头 ③ */
    }

    pl011_sess_t *s = (pl011_sess_t *)br_malloc(sizeof(pl011_sess_t));
    if (s == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }

    s->flags      = flags;   /* 原样存: v1 只解释 BR_O_NONBLOCK, 其余留给后续(不假装支持) */
    s->rx_dropped = 0u;

    s_dev.opened = BR_TRUE;
    *sess        = (void *)s;
    return 0;
}

/*
 * read(sess, buf, n): 从 RX FIFO 抽最多 n 字节。
 *   非阻塞且一个字节都没有 ⇒ -EAGAIN; 阻塞 ⇒ 每 1ms 醒一次重试(**无超时**, 抬头 ⑤)。
 *   已拿到一些字节后 FIFO 空 ⇒ 立即**短读返回**: "抽干当前可读"比"凑满 n"更符合字符设备
 *   语义(数据什么时候来不由调用方决定), 也让非阻塞读不会因为要凑 n 而变成阻塞读。
 */
static br_s64 pl011_read(void *sess, void *buf, br_size_t n)
{
    if ((sess == BR_NULL) || ((buf == BR_NULL) && (n > 0u))) {
        return BR_ERR(BR_EINVAL);
    }

    pl011_sess_t *s   = (pl011_sess_t *)sess;
    br_u8        *out = (br_u8 *)buf;
    br_size_t     got = 0u;

    while (got < n) {
        const br_u32 fr = pl011_reg_read(BR_PL011_FR);

        if ((fr & BR_PL011_FR_RXFE) == 0u) {
            pl011_sample_overrun(s);                                   /* 与本次 DR 读配对 */
            out[got] = (br_u8)(pl011_reg_read(BR_PL011_DR) & 0xffu);        /* DR[7:0] = 数据 */
            got++;
            continue;
        }

        /* FIFO 空 */
        if (got > 0u) {
            break;                                          /* 短读返回 */
        }
        if ((s->flags & BR_O_NONBLOCK) != 0u) {
            return BR_ERR(BR_EAGAIN);                       /* SD-7 v1: 非阻塞 = 问一次 */
        }

        /* 阻塞等: sleep 而不是自旋 —— 等的是外部世界, 可能等很久, 不能占着 CPU 空转
         * (coop 调度下自旋就是"整机不动")。无超时(抬头 ⑤)。 */
        (void)br_task_sleep((br_time_t)BR_US_PER_MS);
    }

    return (br_s64)got;
}

/*
 * write(sess, buf, n): 逐字节等 TX FIFO 腾位后写入。返回写入字节数(= n)。
 *   忙等 TXFF(抬头 ⑤ 解释了为什么这里不是 sleep)。写 DR 就是"发一个字节", 不做任何
 *   转换 —— '\n' → CRLF 是 console/终端的策略, 设备面必须字节透明, 否则同一份数据经
 *   两条路径出去会不一样。
 */
static br_s64 pl011_write(void *sess, const void *buf, br_size_t n)
{
    if ((sess == BR_NULL) || ((buf == BR_NULL) && (n > 0u))) {
        return BR_ERR(BR_EINVAL);
    }

    const br_u8 *p = (const br_u8 *)buf;

    for (br_size_t i = 0u; i < n; i++) {
        while ((pl011_reg_read(BR_PL011_FR) & BR_PL011_FR_TXFF) != 0u) {
            /* 忙等: TX FIFO 有空位。没有 TX 中断的 v1, 除了等没有别的可做。 */
        }
        pl011_reg_write(BR_PL011_DR, (br_u32)p[i]);
    }

    return (br_s64)n;
}

/*
 * ioctl(sess, cmd, arg)。
 *   分发维度 = **type(族魔数) + nr**; dir/size 不做强校验: 设计 `8-01` §5 说编码的用途是
 *   "静态查表/生成清单", 而 v1 的调用方是自己人 —— 因为 size 位对不上就拒绝一个语义完全
 *   明确的命令, 只会把"文档里的编码"变成"运行期的地雷"。类型/nr 不认 = -ENOTSUP。
 *   错误: -ENOTSUP(不是本器件的命令/未实现) / -EINVAL(参数缺失或波特率不可表示)。
 */
static int pl011_ioctl(void *sess, br_u32 cmd, void *arg)
{
    if (sess == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    pl011_sess_t *s = (pl011_sess_t *)sess;

    /* 族魔数不对 ⇒ 这条命令不归本器件管。用 -ENOTSUP 而不是 -EINVAL: 命令本身可能是合法的
     * (别的器件族), 只是没人实现 —— 与"给我的参数不对"必须分开(SD-10)。 */
    if (BR_IOC_TYPE(cmd) != (br_u32)BR_UART_IOC_MAGIC) {
        return BR_ERR(BR_ENOTSUP);
    }

    switch (BR_IOC_NR(cmd)) {
    case BR_UART_IOC_GET_INFO: {
        if (arg == BR_NULL) {
            return BR_ERR(BR_EINVAL);
        }

        /* 顺带采样一次 overrun(抬头 ④): 查询时把"上次读之后又丢过字节"如实反映出来。
         * 代价是可能多弹一个状态条目 —— v1 不暴露逐字符错误状态, 所以不构成信息损失。 */
        pl011_sample_overrun(s);

        br_uart_pl011_info_t *info = (br_uart_pl011_info_t *)arg;
        info->base         = (br_u32)BR_PL011_BASE;
        info->refclk_hz    = BR_PL011_REFCLK_HZ;
        info->baud         = s_dev.baud;
        info->rx_available = pl011_rx_available();
        info->rx_dropped   = s->rx_dropped;
        return 0;
    }

    case BR_UART_IOC_FLUSH_RX:
        /* 无参数命令: arg 可为空, 不校验(将来若加方向/尺寸校验, 也不该在这里要求 arg) */
        pl011_drain_rx(s);
        return 0;

    case BR_UART_IOC_SET_BAUD: {
        if (arg == BR_NULL) {
            return BR_ERR(BR_EINVAL);
        }

        /* arg = br_u32 波特率(值传递, 不是结构体)。解引用由调用方保证对齐(vfs 传的是
         * br_u32 对象; -mstrict-align 只约束编译器生成的访问, 不替调用方对齐指针)。 */
        const br_u32 baud = *(const br_u32 *)arg;
        br_u32       ibrd = 0u;
        br_u32       fbrd = 0u;

        const int rc = pl011_divisors_for(baud, &ibrd, &fbrd);
        if (rc != 0) {
            return rc;   /* -EINVAL: 不可表示(含 baud=0), 硬件**一个字节都没动** */
        }

        pl011_program_divisors(ibrd, fbrd);
        s_dev.baud = baud;   /* 只在成功落盘后才更新"当前波特率" */
        return 0;
    }

    default:
        /* 未知 nr(wrong magic 已在上面挡掉): 明确地"不支持", 不静默成功 */
        return BR_ERR(BR_ENOTSUP);
    }
}

/*
 * poll(sess, events): v1 = **查询**就绪位(SD-7; 没有等待, 也不注册 wait-queue)。
 *   置位规则: RX 有数据 ⇒ BR_POLLIN; TX 不满 ⇒ BR_POLLOUT。
 *   未置位的位**必须清掉** ⇒ 这里用赋值而不是或(调用方的 events 可能是复用缓冲;
 *   "我只回答我看到的状态" —— 沿用旧值会把"没数据"说成"有数据")。
 */
static int pl011_poll(void *sess, br_u32 *events)
{
    if ((sess == BR_NULL) || (events == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }

    const br_u32 fr = pl011_reg_read(BR_PL011_FR);
    br_u32       ev = 0u;

    if ((fr & BR_PL011_FR_RXFE) == 0u) {
        ev |= BR_POLLIN;
    }
    if ((fr & BR_PL011_FR_TXFF) == 0u) {
        ev |= BR_POLLOUT;
    }

    *events = ev;
    return 0;
}

/*
 * close(sess): 释放会话 = 放开独占。
 * 顺序是"先销毁再放开": 反过来的话, 并发场景下第二个 open 可能拿到一个"前一个会话刚被
 * 释放、但还没从设备状态里摘掉"的窗口。v1 是单线程, 这条顺序只是把不变量写在明显处。
 */
static int pl011_close(void *sess)
{
    if (sess == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    br_free(sess);
    s_dev.opened = BR_FALSE;
    return 0;
}

/*
 * suspend/resume: **恒 -ENOTSUP**(D22 预留槽位, 设计 `8-01` §4 末段 / O-S6)。
 *   为什么不是"返回 0 假装成功": 返回 0 等于声称"掉电/时钟门控后线路设置与 FIFO 状态由我
 *   保存并恢复" —— v1 既没有 PM 调用方, 也没有保存/恢复任何寄存器状态, 那是谎报能力。
 *   槽位存在(= 非 NULL)是因为 ops 布局入 golden(D14): 后补字段 = 二进制不兼容, 预留即免破坏。
 *   错误码取 -ENOTSUP 而不是 -EINVAL: 命令合法、场合合法, 只是本版本没这个能力(SD-10)。
 */
static int pl011_suspend(void *dev_priv)
{
    (void)dev_priv;
    return BR_ERR(BR_ENOTSUP);
}

static int pl011_resume(void *dev_priv)
{
    (void)dev_priv;
    return BR_ERR(BR_ENOTSUP);
}

/* ops 表(静态 const: 布局与内容都是契约; 全槽位显式给出, 不靠 C 的零初始化) */
static const br_cdev_ops_t s_ops = {
    .open    = pl011_open,
    .read    = pl011_read,
    .write   = pl011_write,
    .ioctl   = pl011_ioctl,
    .poll    = pl011_poll,
    .close   = pl011_close,
    .suspend = pl011_suspend,
    .resume  = pl011_resume,
};

/* =====================================================================
 * 7. 生命周期钩子(名 = symbol_prefix + 相; 由生成物 plugin_desc.c 引用)
 * ===================================================================== */

/*
 * EARLY 相: **无动作**。此刻无线程(不能 br_task_sleep)、无堆(不能 br_malloc), 而设备的
 * 注册要等 CORE 相(子分类框架先就位, 堆也就位)。更重要的是: PL011 的线路设置此刻归
 * platform 的早期 console —— 在这里"初始化 UART"会把 panic 通道的配置改掉(抬头 ①)。
 */
int uart_pl011_early_init(void)
{
    return 0;
}

/*
 * CORE 相: 注册 cdev。注册成功即出现在 /dev(devfs 枚举 dev-core 注册表)。
 * rc **原样上抛**: -EINVAL(名/ops 非法)/ -EEXIST(重名)/ -ENOSPC(注册表满)都是启动期
 * 必须炸出来的错; 吞掉它 = "设备看起来在, 其实没注册", 是最难查的一类静默失败。
 */
int uart_pl011_init(void)
{
    const int rc = br_cdev_register(BR_UART_PL011_DEV_NAME, &s_ops, &s_dev);

    if (rc != 0) {
        br_log_error("uart-pl011: cdev 注册失败 rc=%d (name=%s)",
                     rc, BR_UART_PL011_DEV_NAME);
        return rc;
    }

    br_log_info("uart-pl011: /dev/%s 已注册 (cdev; PL011 @ 0x%08x, %u baud, %u MHz refclk)",
                BR_UART_PL011_DEV_NAME, (br_u32)BR_PL011_BASE,
                BR_PL011_BAUD, BR_PL011_REFCLK_HZ / 1000000u);
    return 0;
}

/*
 * START 相: 本器件的一致性用例**不在这里跑** —— ADR-0010 之后由 core 的
 * `br_plugin_manager_selftest()` 经描述符的 `.selftest` 钩子统一驱动(见 src/uart_selftest.c)。
 * 这里只留一行设备摘要作启动证据(此时堆/线程/devfs 都在, 设备已在 CORE 相注册)。
 * 返回 0 而**不是**失败数: 红绿由门禁看自检套件打的日志判定(见 src/uart_selftest.c)——
 * 让 start 返回非 0 会把插件管理器判成"启动失败", 后面的相位与别的插件的证据可能一起丢。
 */
int uart_pl011_start(void)
{
    br_log_info("uart-pl011: /dev/%s 就绪 (PL011 @ 0x%08x, %u baud)",
                BR_UART_PL011_DEV_NAME, (br_u32)BR_PL011_BASE, BR_PL011_BAUD);
    return 0;
}
