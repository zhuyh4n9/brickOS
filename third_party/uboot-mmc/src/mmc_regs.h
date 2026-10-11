/*
 * third_party/uboot-mmc — MMC/SD 协议常量与 SDHCI 寄存器(私有)
 *
 * 来源(逐条对照, GPL-2.0+): u-boot NXP 分支
 *   `nxp-imx/uboot-imx` branch `lf_v2026.04` commit `bb41975aeb4cbd153b49c9fd62da1d64cd65968d`
 *   - `include/mmc.h`(命令码 / 响应位 / OCR / EXT_CSD 偏移 / 状态位)
 *   - `include/sdhci.h`(SDHCI 寄存器偏移 / 中断位 / 传输模式位 / ADMA2 描述符)
 * 只摘本驱动用到的子集; 命名去掉 u-boot 前缀, 但数值逐字对齐(见 NOTICE 的来源清单)。
 */
#ifndef BR_THIRD_PARTY_UBOOT_MMC_REGS_H
#define BR_THIRD_PARTY_UBOOT_MMC_REGS_H

#include <br/core/br_types.h>

/* ==================================================================== MMC 命令 */
#define MMC_CMD_GO_IDLE_STATE        0u
#define MMC_CMD_SEND_OP_COND         1u
#define MMC_CMD_ALL_SEND_CID         2u
#define MMC_CMD_SET_RELATIVE_ADDR    3u
#define MMC_CMD_SWITCH               6u
#define MMC_CMD_SELECT_CARD          7u
#define MMC_CMD_SEND_EXT_CSD         8u
#define MMC_CMD_SEND_CSD             9u
#define MMC_CMD_SEND_CID            10u
#define MMC_CMD_STOP_TRANSMISSION   12u
#define MMC_CMD_SEND_STATUS         13u
#define MMC_CMD_SET_BLOCKLEN        16u
#define MMC_CMD_READ_SINGLE_BLOCK   17u
#define MMC_CMD_READ_MULTIPLE_BLOCK 18u
#define MMC_CMD_WRITE_SINGLE_BLOCK  24u
#define MMC_CMD_WRITE_MULTIPLE_BLOCK 25u

/* 响应类型(mmC_RSP_*): 位含义与 u-boot 一致 */
#define MMC_RSP_PRESENT  (1u << 0)
#define MMC_RSP_136      (1u << 1)
#define MMC_RSP_CRC      (1u << 2)
#define MMC_RSP_BUSY     (1u << 3)
#define MMC_RSP_OPCODE   (1u << 4)

#define MMC_RSP_NONE  0u
#define MMC_RSP_R1    (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)
#define MMC_RSP_R1b   (MMC_RSP_R1 | MMC_RSP_BUSY)
#define MMC_RSP_R2    (MMC_RSP_PRESENT | MMC_RSP_136 | MMC_RSP_CRC)
#define MMC_RSP_R3    (MMC_RSP_PRESENT)
#define MMC_RSP_R6    (MMC_RSP_PRESENT | MMC_RSP_CRC | MMC_RSP_OPCODE)

/* 数据传输方向 */
#define MMC_DATA_READ   1u
#define MMC_DATA_WRITE  2u

/* ==================================================================== OCR */
#define OCR_BUSY         0x80000000u
#define OCR_HCS          0x40000000u
#define OCR_VOLTAGE_MASK 0x007FFF80u
#define OCR_ACCESS_MODE  0x60000000u

/* ==================================================================== 状态位(CMD13) */
#define MMC_STATUS_SWITCH_ERROR (1u << 7)
#define MMC_STATUS_RDY_FOR_DATA (1u << 8)
#define MMC_STATUS_CURR_STATE   (0xfu << 9)
#define MMC_STATE_PRG           (7u << 9)
#define MMC_STATE_TRANS         (4u << 9)

/* ==================================================================== EXT_CSD */
#define EXT_CSD_PART_CONF     179u
#define EXT_CSD_BUS_WIDTH     183u
#define EXT_CSD_HS_TIMING     185u
#define EXT_CSD_REV           192u
#define EXT_CSD_CARD_TYPE     196u
#define EXT_CSD_SEC_CNT       212u   /* 4 字节小端: 512 B 扇区数 */

#define EXT_CSD_CMD_SET_NORMAL 0u
#define EXT_CSD_TIMING_LEGACY  0u
#define EXT_CSD_TIMING_HS      1u
#define EXT_CSD_BUS_WIDTH_1    0u    /* 1-bit */
#define EXT_CSD_BUS_WIDTH_4    1u    /* 4-bit */
#define EXT_CSD_BUS_WIDTH_8    2u    /* 8-bit */

#define MMC_SWITCH_MODE_WRITE_BYTE 3u

#define MMC_MAX_BLOCK_LEN 512u

#define MMC_CARD_TYPE_26  (1u << 0)
#define MMC_CARD_TYPE_52  (1u << 1)

/* ==================================================================== SDHCI 寄存器 */
#define SDHCI_DMA_ADDRESS     0x00u
#define SDHCI_BLOCK_SIZE      0x04u
#define SDHCI_BLOCK_COUNT     0x06u
#define SDHCI_ARGUMENT        0x08u
#define SDHCI_TRANSFER_MODE   0x0Cu
#define SDHCI_COMMAND         0x0Eu
#define SDHCI_RESPONSE        0x10u
#define SDHCI_BUFFER          0x20u
#define SDHCI_PRESENT_STATE   0x24u
#define SDHCI_HOST_CONTROL    0x28u
#define SDHCI_POWER_CONTROL   0x29u
#define SDHCI_CLOCK_CONTROL   0x2Cu
#define SDHCI_TIMEOUT_CONTROL 0x2Eu
#define SDHCI_SOFTWARE_RESET  0x2Fu
#define SDHCI_INT_STATUS      0x30u
#define SDHCI_INT_ENABLE      0x34u
#define SDHCI_SIGNAL_ENABLE   0x38u
#define SDHCI_HOST_CONTROL2   0x3Eu
#define SDHCI_CAPABILITIES    0x40u
#define SDHCI_CAPABILITIES_1  0x44u
#define SDHCI_ADMA_ADDRESS    0x58u
#define SDHCI_ADMA_ADDRESS_HI 0x5Cu
#define SDHCI_HOST_VERSION    0xFEu

#define SDHCI_MAKE_BLKSZ(dma, blksz) ((((dma) & 0x7u) << 12) | ((blksz) & 0xFFFu))
#define SDHCI_MAKE_CMD(c, f)         ((((c) & 0xFFu) << 8) | ((f) & 0xFFu))

#define SDHCI_TRNS_DMA       (1u << 0)
#define SDHCI_TRNS_BLK_CNT_EN (1u << 1)
#define SDHCI_TRNS_READ      (1u << 4)
#define SDHCI_TRNS_MULTI     (1u << 5)

#define SDHCI_CMD_CRC        0x08u
#define SDHCI_CMD_INDEX      0x10u
#define SDHCI_CMD_DATA       0x20u
#define SDHCI_CMD_RESP_NONE  0x00u
#define SDHCI_CMD_RESP_LONG  0x01u
#define SDHCI_CMD_RESP_SHORT 0x02u
#define SDHCI_CMD_RESP_SHORT_BUSY 0x03u

#define SDHCI_CMD_INHIBIT  (1u << 0)
#define SDHCI_DATA_INHIBIT (1u << 1)
#define SDHCI_DAT_ACTIVE   (1u << 2)
#define SDHCI_SPACE_AVAILABLE (1u << 10)
#define SDHCI_DATA_AVAILABLE  (1u << 11)

#define SDHCI_CTRL_4BITBUS   (1u << 1)
#define SDHCI_CTRL_HISPD     (1u << 2)
#define SDHCI_CTRL_DMA_MASK  0x18u
#define SDHCI_CTRL_ADMA32    0x10u
#define SDHCI_CTRL_8BITBUS   (1u << 5)

#define SDHCI_POWER_ON   0x01u
#define SDHCI_POWER_330  0x0Eu

#define SDHCI_DIVIDER_SHIFT    8u
#define SDHCI_DIVIDER_HI_SHIFT 6u
#define SDHCI_DIV_MASK         0xFFu
#define SDHCI_DIV_MASK_LEN     8u
#define SDHCI_DIV_HI_MASK      0x300u
#define SDHCI_CLOCK_CARD_EN    (1u << 2)
#define SDHCI_CLOCK_INT_STABLE (1u << 1)
#define SDHCI_CLOCK_INT_EN     (1u << 0)

#define SDHCI_RESET_ALL  0x01u
#define SDHCI_RESET_CMD  0x02u
#define SDHCI_RESET_DATA 0x04u

#define SDHCI_INT_RESPONSE    (1u << 0)
#define SDHCI_INT_DATA_END    (1u << 1)
#define SDHCI_INT_DMA_END     (1u << 3)
#define SDHCI_INT_SPACE_AVAIL (1u << 4)
#define SDHCI_INT_DATA_AVAIL  (1u << 5)
#define SDHCI_INT_ERROR       (1u << 15)
#define SDHCI_INT_TIMEOUT     (1u << 16)
#define SDHCI_INT_CRC         (1u << 17)
#define SDHCI_INT_END_BIT     (1u << 18)
#define SDHCI_INT_INDEX       (1u << 19)
#define SDHCI_INT_DATA_TIMEOUT (1u << 20)
#define SDHCI_INT_DATA_CRC    (1u << 21)
#define SDHCI_INT_DATA_END_BIT (1u << 22)
#define SDHCI_INT_ADMA_ERROR  (1u << 25)

#define SDHCI_INT_ERROR_MASK 0xFFFF8000u

/* 本驱动关心且会打开信号的中断集合(ISR 只清这些) */
#define SDHCI_INT_OURS (SDHCI_INT_RESPONSE | SDHCI_INT_DATA_END | SDHCI_INT_DMA_END | \
                        SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_AVAIL | \
                        SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | \
                        SDHCI_INT_INDEX | SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC | \
                        SDHCI_INT_DATA_END_BIT | SDHCI_INT_ADMA_ERROR)

#define SDHCI_MAX_DIV_SPEC_300 2046u
#define SDHCI_SPEC_300         2u

/* SDHCI 能力位(0x40) */
#define SDHCI_CAN_DO_8BIT    (1u << 18)
#define SDHCI_CAN_DO_ADMA2   (1u << 19)
#define SDHCI_CAN_DO_HISPD   (1u << 21)
#define SDHCI_CLOCK_V3_BASE_MASK 0x0000FF00u
#define SDHCI_CLOCK_BASE_SHIFT   8u

/* ADMA2 描述符(32 位; 8 字节, packed) */
#define ADMA_DESC_ATTR_VALID  (1u << 0)
#define ADMA_DESC_ATTR_END    (1u << 1)
#define ADMA_DESC_ATTR_ACT2   (1u << 5)   /* TRANSFER_DATA */
#define ADMA_MAX_LEN          65532u

typedef struct br_adma2_desc {
    br_u8  attr;
    br_u8  reserved;
    br_u16 len;
    br_u32 addr_lo;
} __attribute__((packed)) br_adma2_desc_t;

#endif /* BR_THIRD_PARTY_UBOOT_MMC_REGS_H */
