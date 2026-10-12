// SPDX-License-Identifier: GPL-2.0+
/*
 * third_party/uboot-mmc — MMC 协议核心(移植自 u-boot `drivers/mmc/mmc.c`)
 *
 * 来源(GPL-2.0+; 见 ../NOTICE): `nxp-imx/uboot-imx` @ `lf_v2026.04`
 * `bb41975aeb4cbd153b49c9fd62da1d64cd65968d` `drivers/mmc/mmc.c`。
 * 移植范围(见 docs/analysis/0001-emmc-bdev-gap.md §2): 保留 eMMC 识别/EXT_CSD/块读写/
 * 总线宽度/HS 时序; **裁掉** SD 专用(UHS/SCR/SSR/SD switch)、HS200/HS400/tuning、
 * RPMB/分区/BKOPS、SPI 模式、DM/blk 管道(用 host ops + bdev 取代)。
 *
 * 对照位置(便于回溯): mmc_go_idle / mmc_send_op_cond(_iter) / mmc_send_ext_csd /
 * __mmc_switch / mmc_set_card_speed / mmc_read_blocks / mmc_bread / mmc_set_blocklen。
 */
#include "mmc_compat.h"   /* 适配层: u-boot 运行环境 → brickOS(唯一入口) */

#include "emmc_internal.h"

#define MMC_OP_TIMEOUT_MS    1000u
#define MMC_CMD6_TIMEOUT_MS  1000u
#define MMC_POLL_US          100u

static br_u32 mmc_le32(const br_u8 *p)
{
    return (br_u32)p[0] | ((br_u32)p[1] << 8) | ((br_u32)p[2] << 16) | ((br_u32)p[3] << 24);
}

static int mmc_send_cmd(br_mmc_t *m, br_mmc_cmd_t *c, br_mmc_data_t *d)
{
    return m->ops->send_cmd(m->host, c, d);
}

/* ---------------------------------------------------------------- 状态与忙等待 */

static int mmc_send_status(br_mmc_t *m, br_u32 *status)
{
    br_mmc_cmd_t c;
    c.cmdidx = MMC_CMD_SEND_STATUS;
    c.resp_type = MMC_RSP_R1;
    c.cmdarg = (br_u32)m->rca << 16;
    c.response[0] = 0u;

    const int rc = mmc_send_cmd(m, &c, BR_NULL);
    if (rc != 0) {
        return rc;
    }
    if (status != BR_NULL) {
        *status = c.response[0];
    }
    return 0;
}

static int mmc_poll_for_busy(br_mmc_t *m, br_u32 timeout_ms)
{
    const br_time_t deadline = br_deadline_from_now((br_time_t)timeout_ms * BR_US_PER_MS);

    for (;;) {
        br_u32 st = 0u;
        const int rc = mmc_send_status(m, &st);
        if (rc != 0) {
            return rc;
        }
        if ((st & MMC_STATUS_SWITCH_ERROR) != 0u) {
            return BR_ERR(BR_EIO);
        }
        if (((st & MMC_STATUS_RDY_FOR_DATA) != 0u) &&
            ((st & MMC_STATUS_CURR_STATE) != MMC_STATE_PRG)) {
            return 0;
        }
        if (br_clock_now() >= deadline) {
            return BR_ERR(BR_ETIMEDOUT);
        }
        br_delay_us(MMC_POLL_US);
    }
}

/* ---------------------------------------------------------------- 识别序列 */

static int mmc_go_idle(br_mmc_t *m)
{
    br_mmc_cmd_t c;

    br_delay_us(1000);                   /* 卡上电后需要一段稳定时间(u-boot 同款) */

    c.cmdidx = MMC_CMD_GO_IDLE_STATE;
    c.resp_type = MMC_RSP_NONE;
    c.cmdarg = 0u;

    const int rc = mmc_send_cmd(m, &c, BR_NULL);
    if (rc != 0) {
        return rc;
    }
    br_delay_us(2000);
    return 0;
}

static int mmc_send_op_cond(br_mmc_t *m)
{
    br_u32       ocr = 0u;
    const br_time_t deadline = br_deadline_from_now((br_time_t)MMC_OP_TIMEOUT_MS * BR_US_PER_MS);

    const int grc = mmc_go_idle(m);
    if (grc != 0) {
        return grc;
    }

    for (br_u32 i = 0u; ; i++) {
        br_mmc_cmd_t c;
        c.cmdidx = MMC_CMD_SEND_OP_COND;
        c.resp_type = MMC_RSP_R3;
        c.cmdarg = 0u;
        if (i != 0u) {
            /* 第一轮只问能力(arg=0); 之后把卡回的能力窗口原样送回去 */
            c.cmdarg = OCR_HCS | (ocr & OCR_VOLTAGE_MASK) | (ocr & OCR_ACCESS_MODE);
        }
        c.response[0] = 0u;

        const int rc = mmc_send_cmd(m, &c, BR_NULL);
        if (rc != 0) {
            return rc;
        }
        ocr = c.response[0];

        if ((ocr & OCR_BUSY) != 0u) {
            m->high_capacity = ((ocr & OCR_HCS) == OCR_HCS) ? BR_TRUE : BR_FALSE;
            return 0;
        }
        if (br_clock_now() >= deadline) {
            return BR_ERR(BR_ETIMEDOUT);
        }
        br_delay_us(MMC_POLL_US);
    }
}

static int mmc_all_send_cid(br_mmc_t *m)
{
    br_mmc_cmd_t c;
    c.cmdidx = MMC_CMD_ALL_SEND_CID;
    c.resp_type = MMC_RSP_R2;
    c.cmdarg = 0u;
    return mmc_send_cmd(m, &c, BR_NULL);
}

static int mmc_set_relative_addr(br_mmc_t *m)
{
    br_mmc_cmd_t c;
    c.cmdidx = MMC_CMD_SET_RELATIVE_ADDR;
    c.resp_type = MMC_RSP_R6;
    c.cmdarg = (br_u32)m->rca << 16;
    return mmc_send_cmd(m, &c, BR_NULL);
}

static int mmc_send_csd(br_mmc_t *m)
{
    br_mmc_cmd_t c;
    c.cmdidx = MMC_CMD_SEND_CSD;
    c.resp_type = MMC_RSP_R2;
    c.cmdarg = (br_u32)m->rca << 16;
    return mmc_send_cmd(m, &c, BR_NULL);
}

static int mmc_select_card(br_mmc_t *m)
{
    br_mmc_cmd_t c;
    c.cmdidx = MMC_CMD_SELECT_CARD;
    c.resp_type = MMC_RSP_R1;
    c.cmdarg = (br_u32)m->rca << 16;
    return mmc_send_cmd(m, &c, BR_NULL);
}

static int mmc_send_ext_csd(br_mmc_t *m, br_u8 *out)
{
    br_mmc_cmd_t  c;
    br_mmc_data_t d;

    c.cmdidx = MMC_CMD_SEND_EXT_CSD;
    c.resp_type = MMC_RSP_R1;
    c.cmdarg = 0u;

    d.buf = out;
    d.flags = MMC_DATA_READ;
    d.blocks = 1u;
    d.blocksize = MMC_MAX_BLOCK_LEN;

    return mmc_send_cmd(m, &c, &d);
}

static int mmc_set_blocklen(br_mmc_t *m, br_u32 len)
{
    br_mmc_cmd_t c;
    c.cmdidx = MMC_CMD_SET_BLOCKLEN;
    c.resp_type = MMC_RSP_R1;
    c.cmdarg = len;
    return mmc_send_cmd(m, &c, BR_NULL);
}

/* CMD6 WRITE_BYTE: 改 EXT_CSD 的一个字节, 再轮询到卡回到 TRANS */
static int mmc_switch(br_mmc_t *m, br_u32 index, br_u32 value)
{
    br_mmc_cmd_t c;

    c.cmdidx = MMC_CMD_SWITCH;
    c.resp_type = MMC_RSP_R1b;
    c.cmdarg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) | (index << 16) | (value << 8);

    const int rc = mmc_send_cmd(m, &c, BR_NULL);
    if (rc != 0) {
        return rc;
    }
    return mmc_poll_for_busy(m, MMC_CMD6_TIMEOUT_MS);
}

/* ---------------------------------------------------------------- 块读写 */

static int mmc_read_blocks(br_mmc_t *m, void *dst, br_u64 start, br_u32 cnt)
{
    br_mmc_cmd_t  c;
    br_mmc_data_t d;

    c.cmdidx = (cnt > 1u) ? MMC_CMD_READ_MULTIPLE_BLOCK : MMC_CMD_READ_SINGLE_BLOCK;
    c.resp_type = MMC_RSP_R1;
    c.cmdarg = (m->high_capacity == BR_TRUE) ? (br_u32)start
                                             : (br_u32)(start * m->read_bl_len);

    d.buf = dst;
    d.flags = MMC_DATA_READ;
    d.blocks = cnt;
    d.blocksize = m->read_bl_len;

    const int rc = mmc_send_cmd(m, &c, &d);
    if (rc != 0) {
        return rc;
    }
    if (cnt > 1u) {
        br_mmc_cmd_t s;
        s.cmdidx = MMC_CMD_STOP_TRANSMISSION;
        s.resp_type = MMC_RSP_R1b;
        s.cmdarg = 0u;
        (void)mmc_send_cmd(m, &s, BR_NULL);   /* 收尾; 失败也不掩盖已读到的数据 */
    }
    return (int)cnt;
}

static int mmc_write_blocks(br_mmc_t *m, const void *src, br_u64 start, br_u32 cnt)
{
    br_mmc_cmd_t  c;
    br_mmc_data_t d;

    c.cmdidx = (cnt > 1u) ? MMC_CMD_WRITE_MULTIPLE_BLOCK : MMC_CMD_WRITE_SINGLE_BLOCK;
    c.resp_type = MMC_RSP_R1;
    c.cmdarg = (m->high_capacity == BR_TRUE) ? (br_u32)start
                                             : (br_u32)(start * m->read_bl_len);

    d.buf = (void *)src;
    d.flags = MMC_DATA_WRITE;
    d.blocks = cnt;
    d.blocksize = m->read_bl_len;

    const int rc = mmc_send_cmd(m, &c, &d);
    if (rc != 0) {
        return rc;
    }
    if (cnt > 1u) {
        br_mmc_cmd_t s;
        s.cmdidx = MMC_CMD_STOP_TRANSMISSION;
        s.resp_type = MMC_RSP_R1b;
        s.cmdarg = 0u;
        (void)mmc_send_cmd(m, &s, BR_NULL);
    }
    /* 写路径要等卡把数据真正写进去(FLUSH 语义的基础; ADR-0021 D-5) */
    return mmc_poll_for_busy(m, MMC_CMD6_TIMEOUT_MS);
}

/* ---------------------------------------------------------------- 对外 */

int br_mmc_init(br_mmc_t *m, const br_mmc_host_ops_t *ops, void *host)
{
    m->ops = ops;
    m->host = host;
    m->clock = 400000u;
    m->bus_width = 1u;
    m->mode = BR_MMC_MODE_LEGACY;
    m->rca = 1u;
    m->read_bl_len = MMC_MAX_BLOCK_LEN;
    m->high_capacity = BR_TRUE;          /* eMMC 恒为高容量(CSD v2); CMD1 的 HCS 会再确认 */
    m->capacity_sectors = 0u;
    m->card_type = 0u;

    int rc = ops->set_ios(host, m->clock, m->bus_width, m->mode);
    if (rc != 0) {
        return rc;
    }

    if ((rc = mmc_go_idle(m)) != 0) { return rc; }
    if ((rc = mmc_send_op_cond(m)) != 0) { return rc; }
    if ((rc = mmc_all_send_cid(m)) != 0) { return rc; }
    if ((rc = mmc_set_relative_addr(m)) != 0) { return rc; }
    if ((rc = mmc_send_csd(m)) != 0) { return rc; }
    if ((rc = mmc_select_card(m)) != 0) { return rc; }
    if ((rc = mmc_send_ext_csd(m, m->ext_csd)) != 0) { return rc; }

    m->capacity_sectors = (br_u64)mmc_le32(&m->ext_csd[EXT_CSD_SEC_CNT]);
    m->card_type = m->ext_csd[EXT_CSD_CARD_TYPE];

    /* 总线宽度: 先切 4-bit(命令/响应走 CMD 线, 不受 DAT 宽度影响), 再让 host 跟上 */
    rc = mmc_switch(m, EXT_CSD_BUS_WIDTH, EXT_CSD_BUS_WIDTH_4);
    if (rc != 0) {
        br_log_warn("uboot-mmc: 切 4-bit 总线失败 rc=%d(继续 1-bit)", rc);
    } else {
        m->bus_width = 4u;
        rc = ops->set_ios(host, m->clock, m->bus_width, m->mode);
        if (rc != 0) {
            return rc;
        }
    }

    /* 高速时序(仅当卡声明支持 52 MHz) */
    if ((m->card_type & MMC_CARD_TYPE_52) != 0u) {
        rc = mmc_switch(m, EXT_CSD_HS_TIMING, EXT_CSD_TIMING_HS);
        if (rc != 0) {
            br_log_warn("uboot-mmc: 切 HS 时序失败 rc=%d(继续 legacy)", rc);
        } else {
            m->mode = BR_MMC_MODE_HS;
            m->clock = 52000000u;
            rc = ops->set_ios(host, m->clock, m->bus_width, m->mode);
            if (rc != 0) {
                return rc;
            }
        }
    }

    (void)mmc_set_blocklen(m, MMC_MAX_BLOCK_LEN);   /* eMMC 恒 512; 不接受也不致命 */

    if (m->capacity_sectors == 0u) {
        return BR_ERR(BR_EIO);           /* 容量读不出来 = 识别没完成, 不往下走 */
    }

    br_log_info("uboot-mmc: eMMC ready (rca=%u, %lu sectors = %lu MiB, %u-bit, %s, %u Hz)",
                (unsigned)m->rca, (unsigned long)m->capacity_sectors,
                (unsigned long)(m->capacity_sectors / 2048u),
                (unsigned)m->bus_width,
                (m->mode == BR_MMC_MODE_HS) ? "HS" : "legacy",
                (unsigned)m->clock);
    return 0;
}

int br_mmc_read(br_mmc_t *m, br_u64 lba, void *buf, br_size_t n)
{
    if (buf == BR_NULL || n == 0u) {
        return BR_ERR(BR_EINVAL);
    }
    if ((lba + (br_u64)n) > m->capacity_sectors) {
        return BR_ERR(BR_EIO);           /* 越界: 不发给卡, 免得触发未定义行为 */
    }

    br_u8 *p = (br_u8 *)buf;
    br_u64 cur = lba;
    br_size_t todo = n;

    while (todo > 0u) {
        br_u32 chunk = (todo > (br_size_t)BR_EMMC_MAX_RW_SECTORS)
                     ? BR_EMMC_MAX_RW_SECTORS : (br_u32)todo;
        const int rc = mmc_read_blocks(m, p, cur, chunk);
        if (rc < 0) {
            return rc;
        }
        p += (br_size_t)chunk * m->read_bl_len;
        cur += chunk;
        todo -= chunk;
    }
    return (int)n;
}

int br_mmc_write(br_mmc_t *m, br_u64 lba, const void *buf, br_size_t n)
{
    if (buf == BR_NULL || n == 0u) {
        return BR_ERR(BR_EINVAL);
    }
    if ((lba + (br_u64)n) > m->capacity_sectors) {
        return BR_ERR(BR_EIO);
    }

    const br_u8 *p = (const br_u8 *)buf;
    br_u64 cur = lba;
    br_size_t todo = n;

    while (todo > 0u) {
        br_u32 chunk = (todo > (br_size_t)BR_EMMC_MAX_RW_SECTORS)
                     ? BR_EMMC_MAX_RW_SECTORS : (br_u32)todo;
        const int rc = mmc_write_blocks(m, p, cur, chunk);
        if (rc < 0) {
            return rc;
        }
        p += (br_size_t)chunk * m->read_bl_len;
        cur += chunk;
        todo -= chunk;
    }
    return (int)n;
}

int br_mmc_flush(br_mmc_t *m)
{
    /*
     * WORKAROUND(br-wa-emmc-001): v1 的 flush 只做"等卡不忙"。
     * 写命令返回前已经轮询过状态, 所以语义上"写到卡内部的块已落介质"; 但**没有**发
     * EXT_CSD CACHE_FLUSH(CMD6 SET_BIT[32]), 也没有判定 CACHE_CTRL 是否使能。
     * 设计与还债动作见 `WORKAROUNDS.md` 的 `br-wa-emmc-001`(ADR-0021 D-5)。
     */
    return mmc_poll_for_busy(m, MMC_CMD6_TIMEOUT_MS);
}
