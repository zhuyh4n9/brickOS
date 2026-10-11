/*
 * third_party/uboot-mmc — 通用 SDHCI 主机驱动(移植自 u-boot `drivers/mmc/sdhci.c`
 * + `sdhci-adma.c` + `include/sdhci.h`)
 *
 * 来源(GPL-2.0+; 见 ../NOTICE): `nxp-imx/uboot-imx` @ `lf_v2026.04`
 * `bb41975aeb4cbd153b49c9fd62da1d64cd65968d`。
 *
 * 移植范围与**有意偏离**(见 docs/decisions/0021-bdev-pci-emmc.md D-4):
 *   - 保留: 命令收送(sdhci_send_command)/数据传输(sdhci_transfer_data)/PIO/ADMA2 描述符构造/
 *     set_clock/set_ios/复位/上电 —— 寄存器语义逐字对齐;
 *   - **偏离 1(中断)**: u-boot 是 `udelay()` 轮询 `INT_STATUS`; 本驱动把
 *     `INT_ENABLE`/`SIGNAL_ENABLE` 打开, ISR(经 platform 的 BH 分发)只读状态/清源/给信号量,
 *     传输线程 `br_sem_take()` 等 —— 即设计 `7-02` §5 的 ISR→bh→唤醒。
 *   - **偏离 2(DMA/PIO 可选)**: 运行期可选 ADMA2 或 PIO; 小传输/缓冲未对齐自动落 PIO 兜底。
 *   - 裁掉: DM/DT(gpio/regulator/clk)/UHS 电压与 tuning/SDMA/64 位 ADMA/quirks(按本平台固定选择)。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#include "emmc_internal.h"

/* ==================================================================== 状态 */

typedef struct br_sdhci {
    br_uintptr_t base;
    br_u32       max_clk;
    br_u32       clock;
    br_u32       bus_width;
    br_u32       mode;
    br_u32       version;
    br_u32       caps;
    br_u32       path;          /* BR_MMC_PATH_* */
    br_u32       intx_virq;
    br_bool      ready;
    br_bool      dma_ok;
    br_dma_buf_t adma;
    br_sem_t     sem;
    volatile br_u32 pending;    /* ISR 累积的中断状态位, 由等待方消费 */
} sdhci_t;

static sdhci_t s_sdhci;

/* ==================================================================== 寄存器访问
 * ECAM/BAR 都在 Device-nGnRnE 背景里 ⇒ 只做对齐访问(-mstrict-align 下更是硬约束)。 */

static volatile br_u8 *sdhci_reg(br_u32 off)
{
    return (volatile br_u8 *)(s_sdhci.base + (br_uintptr_t)off);
}

static br_u32 sdhci_rd32(br_u32 off) { return *(volatile br_u32 *)sdhci_reg(off); }
static void   sdhci_wr32(br_u32 off, br_u32 v) { *(volatile br_u32 *)sdhci_reg(off) = v; }
static br_u16 sdhci_rd16(br_u32 off) { return *(volatile br_u16 *)sdhci_reg(off); }
static void   sdhci_wr16(br_u32 off, br_u16 v) { *(volatile br_u16 *)sdhci_reg(off) = v; }
static br_u8  sdhci_rd8 (br_u32 off) { return *sdhci_reg(off); }
static void   sdhci_wr8 (br_u32 off, br_u8 v) { *sdhci_reg(off) = v; }

/* ==================================================================== 中断 */

static void sdhci_isr(void *arg)
{
    (void)arg;

    const br_u32 st = sdhci_rd32(SDHCI_INT_STATUS);
    sdhci_wr32(SDHCI_INT_STATUS, st & SDHCI_INT_OURS);   /* 设备侧清源(电平型必须) */
    s_sdhci.pending |= st;
    (void)br_sem_give(&s_sdhci.sem);
}

/* 等 `want` 位之一出现; 错误位优先。返回 0 / -EIO / -ETIMEDOUT。 */
static int sdhci_wait(br_u32 want, br_u32 timeout_ms, br_u32 *out_stat)
{
    const br_time_t deadline = br_deadline_from_now((br_time_t)timeout_ms * BR_US_PER_MS);

    for (;;) {
        const br_irq_state_t s = br_irq_lock();
        const br_u32 p = s_sdhci.pending;
        if ((p & SDHCI_INT_ERROR_MASK) != 0u) {
            s_sdhci.pending = 0u;
            br_irq_unlock(s);
            if (out_stat != BR_NULL) { *out_stat = p; }
            return BR_ERR(BR_EIO);
        }
        if ((p & want) != 0u) {
            s_sdhci.pending = p & ~want;
            br_irq_unlock(s);
            if (out_stat != BR_NULL) { *out_stat = p; }
            return 0;
        }
        br_irq_unlock(s);

        const br_time_t now = br_clock_now();
        if (now >= deadline) {
            br_log_error("sdhci: wait 0x%x 超时 (INT_STATUS=0x%08x pending=0x%08x present=0x%08x)",
                         (unsigned)want, (unsigned)sdhci_rd32(SDHCI_INT_STATUS),
                         (unsigned)s_sdhci.pending, (unsigned)sdhci_rd32(SDHCI_PRESENT_STATE));
            if (out_stat != BR_NULL) { *out_stat = 0u; }
            return BR_ERR(BR_ETIMEDOUT);
        }

        /* 有调度器就阻塞等信号量(ISR→bh 会给); 没有(纯启动上下文)就短睡重查。 */
        if (br_task_self() != BR_NULL) {
            const int rc = br_sem_take(&s_sdhci.sem, deadline - now);
            if (rc != 0 && br_clock_now() >= deadline) {
                br_irq_stat_t ist;
                br_irq_stats_get(s_sdhci.intx_virq, &ist);
                br_log_error("sdhci: wait 0x%x 超时[信号量] (INT_STATUS=0x%08x pending=0x%08x present=0x%08x)",
                             (unsigned)want, (unsigned)sdhci_rd32(SDHCI_INT_STATUS),
                             (unsigned)s_sdhci.pending, (unsigned)sdhci_rd32(SDHCI_PRESENT_STATE));
                br_log_error("sdhci: virq=%u irq count=%u spurious_owned=%u bh_deferred=%u flags=0x%x trig=%u",
                             (unsigned)s_sdhci.intx_virq, (unsigned)ist.count,
                             (unsigned)ist.spurious_owned, (unsigned)ist.bh_deferred,
                             (unsigned)ist.flags, (unsigned)ist.trigger);
                if (out_stat != BR_NULL) { *out_stat = 0u; }
                return BR_ERR(BR_ETIMEDOUT);
            }
        } else {
            br_delay_us(20);
        }
    }
}

/* ==================================================================== 复位/时钟/IO */

static int sdhci_reset(br_u8 mask)
{
    sdhci_wr8(SDHCI_SOFTWARE_RESET, mask);

    const br_time_t deadline = br_deadline_from_now((br_time_t)100u * BR_US_PER_MS);
    while ((sdhci_rd8(SDHCI_SOFTWARE_RESET) & mask) != 0u) {
        if (br_clock_now() >= deadline) {
            return BR_ERR(BR_ETIMEDOUT);
        }
        br_delay_us(1000);
    }
    return 0;
}

static int sdhci_set_clock(br_u32 clock)
{
    br_u32 div;

    /* 等 CMD/DATA inhibit 落下去(最多 20 ms; u-boot 同款) */
    br_time_t deadline = br_deadline_from_now((br_time_t)20u * BR_US_PER_MS);
    while ((sdhci_rd32(SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) != 0u) {
        if (br_clock_now() >= deadline) {
            return BR_ERR(BR_EBUSY);
        }
        br_delay_us(100);
    }

    sdhci_wr16(SDHCI_CLOCK_CONTROL, 0u);
    if (clock == 0u) {
        return 0;
    }

    /* SDHCI 3.0: 分频必须是偶数; 寄存器里的 div 是"分频值/2" */
    if (s_sdhci.max_clk <= clock) {
        div = 1u;
    } else {
        for (div = 2u; div < SDHCI_MAX_DIV_SPEC_300; div += 2u) {
            if ((s_sdhci.max_clk / div) <= clock) {
                break;
            }
        }
    }
    div >>= 1;

    br_u16 clk = (br_u16)(SDHCI_CLOCK_INT_EN |
                          ((div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT) |
                          (((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN) << SDHCI_DIVIDER_HI_SHIFT));
    sdhci_wr16(SDHCI_CLOCK_CONTROL, clk);

    deadline = br_deadline_from_now((br_time_t)20u * BR_US_PER_MS);
    for (;;) {
        clk = sdhci_rd16(SDHCI_CLOCK_CONTROL);
        if ((clk & SDHCI_CLOCK_INT_STABLE) != 0u) {
            break;
        }
        if (br_clock_now() >= deadline) {
            return BR_ERR(BR_EBUSY);
        }
        br_delay_us(1000);
    }

    clk |= SDHCI_CLOCK_CARD_EN;
    sdhci_wr16(SDHCI_CLOCK_CONTROL, clk);
    return 0;
}

static int sdhci_set_ios(void *host, br_u32 clock, br_u32 bus_width, br_u32 mode)
{
    (void)host;

    if (clock != s_sdhci.clock) {
        const int rc = sdhci_set_clock(clock);
        if (rc != 0) {
            return rc;
        }
        s_sdhci.clock = clock;
    }
    s_sdhci.bus_width = bus_width;
    s_sdhci.mode = mode;

    br_u8 ctrl = sdhci_rd8(SDHCI_HOST_CONTROL);
    if (bus_width == 8u) {
        ctrl &= (br_u8)~SDHCI_CTRL_4BITBUS;
        ctrl |= (br_u8)SDHCI_CTRL_8BITBUS;
    } else {
        ctrl &= (br_u8)~SDHCI_CTRL_8BITBUS;
        if (bus_width == 4u) {
            ctrl |= (br_u8)SDHCI_CTRL_4BITBUS;
        } else {
            ctrl &= (br_u8)~SDHCI_CTRL_4BITBUS;
        }
    }

    if (mode == BR_MMC_MODE_HS) {
        ctrl |= (br_u8)SDHCI_CTRL_HISPD;
    } else {
        ctrl &= (br_u8)~SDHCI_CTRL_HISPD;
    }
    sdhci_wr8(SDHCI_HOST_CONTROL, ctrl);
    return 0;
}

/* ==================================================================== 数据传输 */

static void sdhci_prepare_adma(void *buf, br_u32 len)
{
    br_adma2_desc_t *d = (br_adma2_desc_t *)s_sdhci.adma.vaddr;

    d->attr = (br_u8)(ADMA_DESC_ATTR_VALID | ADMA_DESC_ATTR_ACT2 | ADMA_DESC_ATTR_END);
    d->reserved = 0u;
    d->len = (br_u16)len;
    d->addr_lo = (br_u32)(br_uintptr_t)buf;

    br_mm_cache_flush(d, (br_size_t)sizeof(*d));

    br_u8 ctrl = sdhci_rd8(SDHCI_HOST_CONTROL);
    ctrl = (br_u8)((ctrl & ~SDHCI_CTRL_DMA_MASK) | SDHCI_CTRL_ADMA32);
    sdhci_wr8(SDHCI_HOST_CONTROL, ctrl);

    sdhci_wr32(SDHCI_ADMA_ADDRESS, (br_u32)(br_uintptr_t)d);
}

/* PIO: 块内按 4 字节搬运; 逐字节拼装 ⇒ 与调用方缓冲的对齐无关(-mstrict-align 安全)。 */
static void sdhci_pio_block(void *buf, br_u32 blocksize, br_u32 dir)
{
    br_u8 *p = (br_u8 *)buf;

    for (br_u32 i = 0u; i < blocksize; i += 4u) {
        if (dir == MMC_DATA_READ) {
            const br_u32 w = sdhci_rd32(SDHCI_BUFFER);
            p[i + 0u] = (br_u8)(w & 0xFFu);
            p[i + 1u] = (br_u8)((w >> 8) & 0xFFu);
            p[i + 2u] = (br_u8)((w >> 16) & 0xFFu);
            p[i + 3u] = (br_u8)((w >> 24) & 0xFFu);
        } else {
            const br_u32 w = (br_u32)p[i + 0u] | ((br_u32)p[i + 1u] << 8) |
                             ((br_u32)p[i + 2u] << 16) | ((br_u32)p[i + 3u] << 24);
            sdhci_wr32(SDHCI_BUFFER, w);
        }
    }
}

static int sdhci_send_command(void *host, br_mmc_cmd_t *cmd, br_mmc_data_t *data)
{
    (void)host;

    /* ① 等 inhibit 落下 */
    br_time_t deadline = br_deadline_from_now((br_time_t)1000u * BR_US_PER_MS);
    br_u32 inhibit_mask = SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT;
    if (cmd->cmdidx == MMC_CMD_STOP_TRANSMISSION) {
        inhibit_mask &= ~SDHCI_DATA_INHIBIT;
    }
    while ((sdhci_rd32(SDHCI_PRESENT_STATE) & inhibit_mask) != 0u) {
        if (br_clock_now() >= deadline) {
            return BR_ERR(BR_EBUSY);
        }
        br_delay_us(1000);
    }

    sdhci_wr32(SDHCI_INT_STATUS, 0xFFFFFFFFu);      /* 清残留 */
    {
        const br_irq_state_t s = br_irq_lock();
        s_sdhci.pending = 0u;
        br_irq_unlock(s);
    }

    /* ② 命令标志 */
    br_u8 flags = 0u;
    if ((cmd->resp_type & MMC_RSP_PRESENT) == 0u) {
        flags = SDHCI_CMD_RESP_NONE;
    } else if ((cmd->resp_type & MMC_RSP_136) != 0u) {
        flags = SDHCI_CMD_RESP_LONG;
    } else if ((cmd->resp_type & MMC_RSP_BUSY) != 0u) {
        flags = SDHCI_CMD_RESP_SHORT_BUSY;
    } else {
        flags = SDHCI_CMD_RESP_SHORT;
    }
    if ((cmd->resp_type & MMC_RSP_CRC) != 0u)    { flags |= SDHCI_CMD_CRC; }
    if ((cmd->resp_type & MMC_RSP_OPCODE) != 0u) { flags |= SDHCI_CMD_INDEX; }
    if (data != BR_NULL)                         { flags |= SDHCI_CMD_DATA; }

    /* ③ 数据通路: ADMA2 或 PIO */
    br_bool use_dma = BR_FALSE;
    br_u32  trans_bytes = 0u;

    if (data != BR_NULL) {
        trans_bytes = data->blocks * data->blocksize;
        use_dma = ((s_sdhci.path == BR_MMC_PATH_DMA) && (s_sdhci.dma_ok == BR_TRUE) &&
                   (data->blocks >= BR_SDHCI_PIO_THRESHOLD_SECTORS) &&
                   (((br_uintptr_t)data->buf & 0x3u) == 0u) &&
                   (trans_bytes <= ADMA_MAX_LEN)) ? BR_TRUE : BR_FALSE;

        sdhci_wr8(SDHCI_TIMEOUT_CONTROL, 0x0Eu);

        br_u16 mode = SDHCI_TRNS_BLK_CNT_EN;
        if (data->blocks > 1u) { mode |= SDHCI_TRNS_MULTI; }
        if (data->flags == MMC_DATA_READ) { mode |= SDHCI_TRNS_READ; }

        if (use_dma == BR_TRUE) {
            if (data->flags == MMC_DATA_WRITE) {
                br_mm_cache_flush(data->buf, trans_bytes);   /* clean: 让设备看见新数据 */
            }
            sdhci_prepare_adma(data->buf, trans_bytes);
            mode |= SDHCI_TRNS_DMA;
        } else {
            br_u8 ctrl = sdhci_rd8(SDHCI_HOST_CONTROL);
            ctrl = (br_u8)(ctrl & ~SDHCI_CTRL_DMA_MASK);
            sdhci_wr8(SDHCI_HOST_CONTROL, ctrl);
        }

        sdhci_wr16(SDHCI_BLOCK_SIZE, (br_u16)SDHCI_MAKE_BLKSZ(0u, data->blocksize));
        sdhci_wr16(SDHCI_BLOCK_COUNT, (br_u16)data->blocks);
        sdhci_wr16(SDHCI_TRANSFER_MODE, mode);
    } else if ((cmd->resp_type & MMC_RSP_BUSY) != 0u) {
        sdhci_wr8(SDHCI_TIMEOUT_CONTROL, 0x0Eu);
    }

    /* ④ 发命令 */
    sdhci_wr32(SDHCI_ARGUMENT, cmd->cmdarg);
    sdhci_wr16(SDHCI_COMMAND, (br_u16)SDHCI_MAKE_CMD(cmd->cmdidx, flags));

    /* ⑤ 等结果 */
    br_u32 st = 0u;
    int rc;

    if (data == BR_NULL) {
        rc = sdhci_wait(SDHCI_INT_RESPONSE, 1000u, &st);
        if (rc != 0) {
            (void)sdhci_reset((br_u8)(SDHCI_RESET_CMD | SDHCI_RESET_DATA));
            return rc;
        }
    } else if (use_dma == BR_TRUE) {
        rc = sdhci_wait(SDHCI_INT_DATA_END, 2000u, &st);
        if (rc != 0) {
            (void)sdhci_reset((br_u8)(SDHCI_RESET_CMD | SDHCI_RESET_DATA));
            return rc;
        }
        if (data->flags == MMC_DATA_READ) {
            br_mm_cache_invalidate(data->buf, trans_bytes);  /* 丢弃陈旧 cache */
        }
    } else {
        /* PIO: 逐块搬运 —— 每次只消费一类事件, 搬完当前块 */
        br_u8 *p = (br_u8 *)data->buf;
        br_u32 moved = 0u;
        for (;;) {
            const br_u32 want = (data->flags == MMC_DATA_READ)
                              ? (SDHCI_INT_DATA_AVAIL | SDHCI_INT_DATA_END)
                              : (SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_END);
            rc = sdhci_wait(want, 2000u, &st);
            if (rc != 0) {
                (void)sdhci_reset((br_u8)(SDHCI_RESET_CMD | SDHCI_RESET_DATA));
                return rc;
            }
            const br_bool buf_evt = (data->flags == MMC_DATA_READ)
                                  ? ((st & SDHCI_INT_DATA_AVAIL) != 0u)
                                  : ((st & SDHCI_INT_SPACE_AVAIL) != 0u);
            if (buf_evt && (moved < data->blocks)) {
                sdhci_pio_block(p, data->blocksize, data->flags);
                p += data->blocksize;
                moved++;
            }
            if (((st & SDHCI_INT_DATA_END) != 0u) && (moved >= data->blocks)) {
                break;
            }
        }
    }

    /* ⑥ 取响应 */
    if ((cmd->resp_type & MMC_RSP_PRESENT) != 0u) {
        if ((cmd->resp_type & MMC_RSP_136) != 0u) {
            for (br_u32 i = 0u; i < 4u; i++) {
                cmd->response[i] = sdhci_rd32(SDHCI_RESPONSE + (i * 4u));
            }
        } else {
            cmd->response[0] = sdhci_rd32(SDHCI_RESPONSE);
            cmd->response[1] = 0u;
            cmd->response[2] = 0u;
            cmd->response[3] = 0u;
        }
    }

    sdhci_wr32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    return 0;
}

/* ==================================================================== 探测/对外 */

int br_sdhci_probe(br_uintptr_t base, br_u32 intx_virq)
{
    if (s_sdhci.ready == BR_TRUE) {
        return 0;
    }
    if (base == 0u || intx_virq == 0u) {
        return BR_ERR(BR_EINVAL);
    }

    s_sdhci.base = base;
    s_sdhci.clock = 0u;
    s_sdhci.bus_width = 1u;
    s_sdhci.mode = BR_MMC_MODE_LEGACY;
    s_sdhci.path = BR_MMC_PATH_DMA;
    s_sdhci.intx_virq = intx_virq;
    s_sdhci.pending = 0u;
    s_sdhci.dma_ok = BR_FALSE;
    s_sdhci.ready = BR_FALSE;

    s_sdhci.caps = sdhci_rd32(SDHCI_CAPABILITIES);
    s_sdhci.version = (br_u32)(sdhci_rd16(SDHCI_HOST_VERSION) & 0xFFu);
    s_sdhci.max_clk = ((s_sdhci.caps & SDHCI_CLOCK_V3_BASE_MASK) >> SDHCI_CLOCK_BASE_SHIFT) * 1000000u;
    if (s_sdhci.max_clk == 0u) {
        s_sdhci.max_clk = 52000000u;      /* 平台事实兜底(QEMU 默认 0x34 = 52 MHz) */
    }

    int rc = sdhci_reset((br_u8)SDHCI_RESET_ALL);
    if (rc != 0) {
        return rc;
    }

    sdhci_wr8(SDHCI_POWER_CONTROL, (br_u8)(SDHCI_POWER_ON | SDHCI_POWER_330));
    br_delay_us(2000);

    /* ADMA2 描述符表(DMA 池, 4 KiB 对齐) */
    rc = br_dma_alloc(2u * (br_size_t)sizeof(br_adma2_desc_t), BR_DMA_F_ALIGN4K, &s_sdhci.adma);
    if (rc == 0) {
        s_sdhci.dma_ok = BR_TRUE;
    } else {
        br_log_warn("uboot-mmc: ADMA2 描述符分配失败 rc=%d ⇒ 只用 PIO", rc);
    }

    /* 中断: 注册 → 打开信号 → 清残留(顺序不能反: 先打开信号会把旧状态当新事件) */
    rc = br_sem_init(&s_sdhci.sem, 0u);
    if (rc != 0) {
        return rc;
    }
    const br_irq_attr_t attr = {
        .prio    = BR_IRQ_PRIO_DEFAULT,
        .trigger = BR_IRQ_TRIG_DEFAULT,
        .flags   = 0u,
    };
    rc = br_irq_register(intx_virq, sdhci_isr, BR_NULL, &attr);
    if (rc != 0) {
        br_log_error("uboot-mmc: br_irq_register(virq=%u) 失败 rc=%d", (unsigned)intx_virq, rc);
        return rc;
    }
    sdhci_wr32(SDHCI_INT_STATUS, 0xFFFFFFFFu);
    sdhci_wr32(SDHCI_INT_ENABLE, SDHCI_INT_OURS);
    sdhci_wr32(SDHCI_SIGNAL_ENABLE, SDHCI_INT_OURS);
    rc = br_irq_enable(intx_virq);
    if (rc != 0) {
        return rc;
    }

    s_sdhci.ready = BR_TRUE;
    br_log_info("uboot-mmc: SDHCI @ 0x%lx (caps=0x%08x, max_clk=%u Hz, intx virq=%u, path=%s)",
                (unsigned long)base, (unsigned)s_sdhci.caps, (unsigned)s_sdhci.max_clk,
                (unsigned)intx_virq,
                (s_sdhci.path == BR_MMC_PATH_DMA) ? "ADMA2" : "PIO");
    return 0;
}

void br_sdhci_set_path(br_u32 path)
{
    s_sdhci.path = (path == BR_MMC_PATH_PIO) ? BR_MMC_PATH_PIO : BR_MMC_PATH_DMA;
}

br_u32 br_sdhci_get_path(void)
{
    return s_sdhci.path;
}

const br_mmc_host_ops_t *br_sdhci_host_ops(void)
{
    static const br_mmc_host_ops_t ops = {
        .send_cmd = sdhci_send_command,
        .set_ios  = sdhci_set_ios,
    };
    return &ops;
}
