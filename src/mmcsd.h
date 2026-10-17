/*
 * MMCSDNT - SD/SDHC/SDXC and MMC/eMMC SCSI miniport driver for standard
 * SD host controllers (SDHCI), on PCI or described by ACPI
 *
 *   MMC_W9X  Windows 95/98/Me (implies MMC_NT4)
 *   MMC_NT4  Windows NT 3.1/3.50/3.51/4.0
 *   (neither) Windows 2000/XP/2003, x86 and x64
 */

#ifndef _MMCSD_H_
#define _MMCSD_H_

#include <miniport.h>
#include <scsi.h>

/* ------------------------------------------------------------------ */
/* Target configuration                                                */
/* ------------------------------------------------------------------ */

#if defined(MMC_W9X) && !defined(MMC_NT4)
#define MMC_NT4
#endif

#ifdef MMC_NT4
#define MMC_AUTOSENSE           0
#define MMC_HAS_DMA64           0   /* no Dma64BitAddresses, no memory above 4 GB */
#define MMC_HAS_CQ              0
#else
#define MMC_AUTOSENSE           1
#define MMC_HAS_DMA64           1
#define MMC_HAS_CQ              1   /* eMMC command queuing (CQ=1), 2000/XP/x64 only */
#endif

/* The NT build also runs on NT 3.1 (40h-byte HW_INITIALIZATION_DATA, 58h-byte
   ConfigInfo, no HAL PCI access, no ScsiPortSetBusDataByOffset) */
#if defined(MMC_NT4) && !defined(MMC_W9X)
#define MMC_NT31_COMPAT
#endif

#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field)       ((LONG)&(((type *)0)->field))
#endif

#ifdef MMC_NT31_COMPAT
#define MMC_HWINIT_SIZE_NT31            ((ULONG)FIELD_OFFSET(HW_INITIALIZATION_DATA, VendorId))
#define MMC_HWINIT_SIZE_NT35            ((ULONG)FIELD_OFFSET(HW_INITIALIZATION_DATA, DeviceId) + sizeof(PVOID))
#endif
#define MMC_CONFIG_HAS(ci, field)       ((ci)->Length >= (ULONG)FIELD_OFFSET(PORT_CONFIGURATION_INFORMATION, field) + sizeof((ci)->field))

#if !defined(MMC_W9X) || defined(DEBUG)
#define MMC_DBG_ENABLED
#endif

#ifdef MMC_W9X
#define MMC_DRIVER_NAME         "mmcsd9x"
#else
#define MMC_DRIVER_NAME         "MMCSDNT"
#endif

#define MMC_DRIVER_VERSION      0x01000000  /* 1.0.0.0 */

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

/* MMC_DBG((MMC_PFX "Slot %lu\n", s)); -- double parentheses (no variadic
   macros in VC++ 2.0 / MSVC 4.0). Use %lu %lX %s %c only. */
#define MMC_PFX                 "[" MMC_DRIVER_NAME "] "

#ifdef MMC_DBG_ENABLED
#ifdef MMC_W9X
VOID __cdecl MmcDbgPrint(PCHAR Format, ...);
#define MMC_DBG(args)           MmcDbgPrint args
#else
ULONG __cdecl DbgPrint(PCH Format, ...);
#define MMC_DBG(args)           DbgPrint args
#endif
#else
#define MMC_DBG(args)
#endif

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */

#if defined(_AMD64_) && defined(__GNUC__)
#undef ScsiPortReadPortUlong
#undef ScsiPortWritePortUlong
#undef ScsiPortReadRegisterUlong
#undef ScsiPortWriteRegisterUlong
ULONG  ScsiPortReadPortUlong(PULONG Port);
VOID   ScsiPortWritePortUlong(PULONG Port, ULONG Value);
ULONG  ScsiPortReadRegisterUlong(PULONG Register);
VOID   ScsiPortWriteRegisterUlong(PULONG Register, ULONG Value);
#endif

#ifdef _WIN64
#define MMC_PTR_LOW(p)          ((ULONG)(unsigned __int64)(p))
#else
#define MMC_PTR_LOW(p)          ((ULONG)(p))
#endif

/* Every SDHCI register is reached with aligned 32-bit accesses: some
   controllers reject narrower ones, and the 16-bit command register is
   written together with the transfer mode below it, as the specification
   allows. */
#define MMC_RD(s, off)          ScsiPortReadRegisterUlong((PULONG)((s)->Regs + (off)))
#define MMC_WR(s, off, v)       ScsiPortWriteRegisterUlong((PULONG)((s)->Regs + (off)), (ULONG)(v))
#define MMC_CQ_RD(s, off)       ScsiPortReadRegisterUlong((PULONG)((s)->CqRegs + (off)))
#define MMC_CQ_WR(s, off, v)    ScsiPortWriteRegisterUlong((PULONG)((s)->CqRegs + (off)), (ULONG)(v))

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define MMC_PAGE_SIZE                   4096
#define MMC_MAX_SLOTS                   6           /* per PCI function (SDHCI) */
#define MMC_MAX_CONTROLLERS             8
#define MMC_MAX_TRANSFER                0x20000     /* 128 KB */
#define MMC_MAX_PHYS_BREAKS             32
#define MMC_BLOCK_SIZE                  512
#define MMC_ADMA_ENTRIES                64
#define MMC_ADMA_TABLE_SIZE             1024        /* 64 descriptors of 8 or 12 bytes */
#define MMC_ADMA_TABLES_PER_PAGE        (MMC_PAGE_SIZE / MMC_ADMA_TABLE_SIZE)
#define MMC_SCRATCH_SIZE                4096        /* EXT_CSD, SCR, switch status, VERIFY */
#define MMC_SDMA_BOUNCE_SIZE            0x10000     /* SDMA-only controllers */
#define MMC_SENSE_MAX                   32

/* ------------------------------------------------------------------ */
/* PCI                                                                 */
/* ------------------------------------------------------------------ */

#define PCI_CLASS_BASE_PERIPHERAL       0x08
#define PCI_SUBCLASS_SD_HOST            0x05
#define MMC_PROGIF_STANDARD             0x00        /* SDHCI, no DMA */
#define MMC_PROGIF_STANDARD_DMA         0x01        /* SDHCI with DMA */
#define MMC_PCI_SLOT_INFO               0x40        /* [6:4] slots - 1, [2:0] first BAR */

/* ------------------------------------------------------------------ */
/* SDHCI registers (SD Host Controller Simplified Specification)      */
/* ------------------------------------------------------------------ */

#define SDHCI_REG_SIZE                  0x100

/* eMMC command queuing host controller interface (CQHCI, JEDEC JESD84-B51),
   at a vendor offset from the SDHCI registers (Intel: 200h) */
#define MMC_CQ_DEFAULT_OFFSET           0x200
#define MMC_CQ_DEPTH                    8           /* tasks the driver keeps in flight */
#define MMC_CQ_SLOTS                    32          /* task descriptor list entries */
#define MMC_CQ_TRD_ENTRIES              64          /* transfer descriptors per task */
#define CQHCI_VER                       0x00
#define CQHCI_CAP                       0x04
#define CQHCI_CFG                       0x08
#define CQHCI_CTL                       0x0C
#define CQHCI_IS                        0x10
#define CQHCI_ISTE                      0x14
#define CQHCI_ISGE                      0x18
#define CQHCI_IC                        0x1C
#define CQHCI_TDLBA                     0x20
#define CQHCI_TDLBAU                    0x24
#define CQHCI_TDBR                      0x28
#define CQHCI_TCN                       0x2C
#define CQHCI_TCLR                      0x38
#define CQHCI_SSC1                      0x40
#define CQHCI_SSC2                      0x44
#define CQHCI_CFG_ENABLE                0x00000001
#define CQHCI_CFG_TASK_DESC_128         0x00000100
#define CQHCI_CTL_HALT                  0x00000001
#define CQHCI_CTL_CLEAR_ALL             0x00000100
#define CQHCI_IS_HAC                    0x00000001
#define CQHCI_IS_TCC                    0x00000002
#define CQHCI_IS_RED                    0x00000004
#define CQHCI_IS_TCL                    0x00000008
#define CQHCI_IS_ERRORS                 0x00000034  /* RED, GCE, ICCE */
#define CQHCI_IS_MASK                   0x0000003F
#define CQHCI_DESC_VALID                0x00000001
#define CQHCI_DESC_END                  0x00000002
#define CQHCI_DESC_INT                  0x00000004
#define CQHCI_ACT_TRAN                  (0x4 << 3)
#define CQHCI_ACT_TASK                  (0x5 << 3)
#define CQHCI_ACT_LINK                  (0x6 << 3)
#define CQHCI_TASK_FORCED_PROG          0x00000040
#define CQHCI_TASK_READ                 0x00001000
#define EXT_CSD_CMDQ_MODE_EN            15
#define EXT_CSD_CMDQ_DEPTH              307
#define EXT_CSD_CMDQ_SUPPORT            308
#define EXT_CSD_TIMING_HS400            3
#define EXT_CSD_CARD_TYPE_HS400_18      0x40
#define SDHCI_CTRL2_UHS_HS400           0x0005      /* not in the SDHCI specification; the usual vendor value */

#define SDHCI_SDMA_ADDRESS              0x00        /* also the CMD23 argument (v4) */
#define SDHCI_BLOCK_SIZE                0x04        /* [11:0] size, [14:12] SDMA boundary; [31:16] count */
#define SDHCI_ARGUMENT                  0x08
#define SDHCI_TRANSFER_MODE             0x0C        /* [15:0] transfer mode, [31:16] command */
#define SDHCI_RESPONSE                  0x10        /* 0x10..0x1F */
#define SDHCI_BUFFER                    0x20
#define SDHCI_PRESENT_STATE             0x24
#define SDHCI_HOST_CONTROL              0x28        /* [7:0] host control, [15:8] power, [23:16] block gap, [31:24] wakeup */
#define SDHCI_CLOCK_CONTROL             0x2C        /* [15:0] clock, [23:16] timeout, [31:24] software reset */
#define SDHCI_INT_STATUS                0x30        /* [15:0] normal, [31:16] error */
#define SDHCI_INT_ENABLE                0x34
#define SDHCI_SIGNAL_ENABLE             0x38
#define SDHCI_HOST_CONTROL2             0x3C        /* [15:0] auto CMD error, [31:16] host control 2 */
#define SDHCI_CAPABILITIES              0x40
#define SDHCI_CAPABILITIES_1            0x44
#define SDHCI_ADMA_ERROR                0x54
#define SDHCI_ADMA_ADDRESS              0x58
#define SDHCI_ADMA_ADDRESS_HI           0x5C
#define SDHCI_SLOT_INT_VERSION          0xFC        /* [15:0] slot interrupt status, [31:16] version */

/* Transfer mode (low half of 0x0C) */
#define SDHCI_TRNS_DMA                  0x0001
#define SDHCI_TRNS_BLK_CNT_EN           0x0002
#define SDHCI_TRNS_AUTO_CMD12           0x0004
#define SDHCI_TRNS_READ                 0x0010
#define SDHCI_TRNS_MULTI                0x0020

/* Command register (high half of 0x0C) */
#define SDHCI_CMD_RESP_NONE             0x0000
#define SDHCI_CMD_RESP_136              0x0001
#define SDHCI_CMD_RESP_48               0x0002
#define SDHCI_CMD_RESP_48_BUSY          0x0003
#define SDHCI_CMD_CRC                   0x0008
#define SDHCI_CMD_INDEX                 0x0010
#define SDHCI_CMD_DATA                  0x0020
#define SDHCI_CMD_ABORT                 0x00C0
#define SDHCI_MAKE_CMD(idx, flags)      ((ULONG)(((idx) & 0x3F) << 8) | (flags))

/* Present state */
#define SDHCI_CMD_INHIBIT               0x00000001
#define SDHCI_DATA_INHIBIT              0x00000002
#define SDHCI_DAT_ACTIVE                0x00000004
#define SDHCI_WRITE_ACTIVE              0x00000100
#define SDHCI_READ_ACTIVE               0x00000200
#define SDHCI_SPACE_AVAILABLE           0x00000400
#define SDHCI_DATA_AVAILABLE            0x00000800
#define SDHCI_CARD_PRESENT              0x00010000
#define SDHCI_CARD_STABLE               0x00020000
#define SDHCI_CARD_DETECT_PIN           0x00040000
#define SDHCI_WRITE_ENABLED             0x00080000  /* write protect switch: 1 = writable */

/* Host control */
#define SDHCI_CTRL_LED                  0x01
#define SDHCI_CTRL_4BIT                 0x02
#define SDHCI_CTRL_HISPD                0x04
#define SDHCI_CTRL_DMA_MASK             0x18
#define SDHCI_CTRL_SDMA                 0x00
#define SDHCI_CTRL_ADMA32               0x10
#define SDHCI_CTRL_ADMA64               0x18
#define SDHCI_CTRL_8BIT                 0x20
#define SDHCI_CTRL_CD_TEST_INS          0x40
#define SDHCI_CTRL_CD_TEST              0x80

/* Power control (bits 15:8 of 0x28) */
#define SDHCI_POWER_ON                  0x01
#define SDHCI_POWER_180                 0x0A
#define SDHCI_POWER_300                 0x0C
#define SDHCI_POWER_330                 0x0E

/* Clock control (low half of 0x2C) */
#define SDHCI_CLOCK_INT_EN              0x0001
#define SDHCI_CLOCK_INT_STABLE          0x0002
#define SDHCI_CLOCK_CARD_EN             0x0004
#define SDHCI_CLOCK_PROG_MODE           0x0020
#define SDHCI_DIVIDER_SHIFT             8
#define SDHCI_DIVIDER_HI_SHIFT          6

/* Software reset (bits 31:24 of 0x2C) */
#define SDHCI_RESET_ALL                 0x01
#define SDHCI_RESET_CMD                 0x02
#define SDHCI_RESET_DATA                0x04

/* Interrupts: normal in [15:0], error in [31:16] */
#define SDHCI_INT_RESPONSE              0x00000001
#define SDHCI_INT_DATA_END              0x00000002
#define SDHCI_INT_BLK_GAP               0x00000004
#define SDHCI_INT_DMA_END               0x00000008
#define SDHCI_INT_SPACE_AVAIL           0x00000010
#define SDHCI_INT_DATA_AVAIL            0x00000020
#define SDHCI_INT_CARD_INSERT           0x00000040
#define SDHCI_INT_CARD_REMOVE           0x00000080
#define SDHCI_INT_CARD_INT              0x00000100
#define SDHCI_INT_ERROR                 0x00008000
#define SDHCI_INT_TIMEOUT               0x00010000
#define SDHCI_INT_CRC                   0x00020000
#define SDHCI_INT_END_BIT               0x00040000
#define SDHCI_INT_INDEX                 0x00080000
#define SDHCI_INT_DATA_TIMEOUT          0x00100000
#define SDHCI_INT_DATA_CRC              0x00200000
#define SDHCI_INT_DATA_END_BIT          0x00400000
#define SDHCI_INT_BUS_POWER             0x00800000
#define SDHCI_INT_AUTO_CMD_ERR          0x01000000
#define SDHCI_INT_ADMA_ERROR            0x02000000
#define SDHCI_INT_CQE                   0x00004000  /* command queuing engine event */

#define SDHCI_INT_CMD_ERRORS            (SDHCI_INT_TIMEOUT | SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)
#define SDHCI_INT_DATA_ERRORS           (SDHCI_INT_DATA_TIMEOUT | SDHCI_INT_DATA_CRC | SDHCI_INT_DATA_END_BIT | \
                                         SDHCI_INT_ADMA_ERROR | SDHCI_INT_AUTO_CMD_ERR | SDHCI_INT_BUS_POWER)
#define SDHCI_INT_ALL_ERRORS            0xFFFF0000
#define SDHCI_INT_ENABLE_MASK           (SDHCI_INT_RESPONSE | SDHCI_INT_DATA_END | SDHCI_INT_DMA_END | \
                                         SDHCI_INT_SPACE_AVAIL | SDHCI_INT_DATA_AVAIL | SDHCI_INT_CARD_INSERT | \
                                         SDHCI_INT_CARD_REMOVE | SDHCI_INT_ERROR | SDHCI_INT_CMD_ERRORS | \
                                         SDHCI_INT_DATA_ERRORS)
/* Raised to the interrupt line while an interrupt-completed transfer runs */
#define SDHCI_INT_SIGNAL_MASK           (SDHCI_INT_DATA_END | SDHCI_INT_DMA_END | SDHCI_INT_ERROR | \
                                         SDHCI_INT_CMD_ERRORS | SDHCI_INT_DATA_ERRORS)

/* Host control 2 (bits 31:16 of 0x3C) */
#define SDHCI_CTRL2_UHS_MASK            0x0007
#define SDHCI_CTRL2_UHS_SDR12           0x0000
#define SDHCI_CTRL2_UHS_SDR25           0x0001
#define SDHCI_CTRL2_UHS_SDR50           0x0002
#define SDHCI_CTRL2_UHS_SDR104          0x0003
#define SDHCI_CTRL2_UHS_DDR50           0x0004
#define SDHCI_CTRL2_VDD_180             0x0008
#define SDHCI_CTRL2_EXEC_TUNING         0x0040
#define SDHCI_CTRL2_TUNED_CLK           0x0080
#define SDHCI_CTRL2_PRESET_VALUE        0x8000

/* Capabilities 1 (0x44) */
#define SDHCI_CAN1_SDR50                0x00000001
#define SDHCI_CAN1_SDR104               0x00000002
#define SDHCI_CAN1_DDR50                0x00000004
#define SDHCI_CAN1_TUNING_SDR50         0x00002000
#define SDHCI_CAN1_UHS                  (SDHCI_CAN1_SDR50 | SDHCI_CAN1_SDR104 | SDHCI_CAN1_DDR50)

/* Maximum current (0x48), 1.8 V in bits 23:16, 4 mA units */
#define SDHCI_MAX_CURRENT               0x48

/* Present state: DAT[3:0] line levels */
#define SDHCI_DATA_LVL_MASK             0x00F00000

/* Capabilities */
#define SDHCI_CAN_TIMEOUT_UNIT_MHZ      0x00000080
#define SDHCI_BASE_CLOCK_SHIFT          8
#define SDHCI_BASE_CLOCK_MASK_V2        0x00003F00
#define SDHCI_BASE_CLOCK_MASK_V3        0x0000FF00
#define SDHCI_MAX_BLOCK_SHIFT           16
#define SDHCI_CAN_8BIT                  0x00040000
#define SDHCI_CAN_ADMA2                 0x00080000
#define SDHCI_CAN_ADMA1                 0x00100000
#define SDHCI_CAN_HISPD                 0x00200000
#define SDHCI_CAN_SDMA                  0x00400000
#define SDHCI_CAN_VDD_330               0x01000000
#define SDHCI_CAN_VDD_300               0x02000000
#define SDHCI_CAN_VDD_180               0x04000000
#define SDHCI_CAN_64BIT                 0x10000000
#define SDHCI_CAN_ASYNC_INT             0x20000000
#define SDHCI_SLOT_TYPE_MASK            0xC0000000
#define SDHCI_SLOT_TYPE_EMBEDDED        0x40000000

/* Version (bits 23:16 of 0xFC): 0 = 1.00, 1 = 2.00, 2 = 3.00, 3+ = 4.x */
#define SDHCI_SPEC_100                  0
#define SDHCI_SPEC_200                  1
#define SDHCI_SPEC_300                  2

/* ADMA2 descriptor attributes */
#define SDHCI_ADMA_VALID                0x0001
#define SDHCI_ADMA_END                  0x0002
#define SDHCI_ADMA_INT                  0x0004
#define SDHCI_ADMA_TRAN                 0x0020
#define SDHCI_ADMA_MAX_LENGTH           0x10000     /* length field 0 */

/* ------------------------------------------------------------------ */
/* SD / MMC commands                                                   */
/* ------------------------------------------------------------------ */

#define MMC_GO_IDLE_STATE               0
#define MMC_SEND_OP_COND                1           /* MMC */
#define MMC_ALL_SEND_CID                2
#define MMC_SET_RELATIVE_ADDR           3           /* SD: SEND_RELATIVE_ADDR */
#define MMC_SWITCH                      6           /* MMC: SWITCH; SD: SWITCH_FUNC */
#define MMC_SELECT_CARD                 7
#define MMC_SEND_EXT_CSD                8           /* MMC */
#define SD_SEND_IF_COND                 8           /* SD */
#define MMC_SEND_CSD                    9
#define MMC_STOP_TRANSMISSION           12
#define MMC_SEND_STATUS                 13
#define MMC_SLEEP_AWAKE                 5           /* MMC */
#define SD_VOLTAGE_SWITCH               11          /* SD */
#define MMC_SET_BLOCKLEN                16
#define SD_SEND_TUNING_BLOCK            19          /* SD, 64 bytes */
#define MMC_SEND_TUNING_BLOCK_HS200     21          /* MMC, 64 or 128 bytes */
#define SD_ERASE_WR_BLK_START           32
#define SD_ERASE_WR_BLK_END             33
#define MMC_ERASE_GROUP_START           35
#define MMC_ERASE_GROUP_END             36
#define MMC_ERASE                       38
#define MMC_READ_SINGLE_BLOCK           17
#define MMC_READ_MULTIPLE_BLOCK         18
#define MMC_SET_BLOCK_COUNT             23
#define MMC_WRITE_BLOCK                 24
#define MMC_WRITE_MULTIPLE_BLOCK        25
#define MMC_APP_CMD                     55
#define SD_APP_SET_BUS_WIDTH            6           /* ACMD6 */
#define SD_APP_SEND_OP_COND             41          /* ACMD41 */
#define SD_APP_SEND_SCR                 51          /* ACMD51 */

/* OCR */
#define MMC_OCR_VDD_27_36               0x00FF8000
#define MMC_OCR_HCS                     0x40000000  /* SD: SDHC/SDXC; MMC: sector mode */
#define MMC_OCR_BUSY                    0x80000000  /* power-up done */
#define MMC_OCR_SECTOR_MODE             0x40000000
#define SD_OCR_S18                      0x01000000  /* ACMD41: 1.8 V request (S18R) and accepted (S18A) */
#define SD_OCR_XPC                      0x10000000  /* ACMD41: more than 150 mA allowed */
#define SD_APP_SD_STATUS                13

/* R1 card status */
#define MMC_R1_ERRORS                   0xFDF98008  /* the error bits of the R1 status */
#define MMC_R1_STATE(r)                 (((r) >> 9) & 0x0F)
#define MMC_R1_READY_FOR_DATA           0x00000100
#define MMC_R1_APP_CMD                  0x00000020
#define MMC_STATE_TRAN                  4
#define MMC_STATE_DATA                  5
#define MMC_STATE_RCV                   6
#define MMC_STATE_PRG                   7

/* EXT_CSD (MMC) */
#define EXT_CSD_FLUSH_CACHE             32
#define EXT_CSD_CACHE_CTRL              33
#define EXT_CSD_POWER_OFF_NOTIFICATION  34
#define EXT_CSD_ERASE_GROUP_DEF         175
#define EXT_CSD_BUS_WIDTH               183
#define EXT_CSD_HS_TIMING               185
#define EXT_CSD_REV                     192
#define EXT_CSD_CARD_TYPE               196
#define EXT_CSD_SEC_COUNT               212
#define EXT_CSD_S_A_TIMEOUT             217
#define EXT_CSD_ERASE_TIMEOUT_MULT      223
#define EXT_CSD_HC_ERASE_GRP_SIZE       224
#define EXT_CSD_SEC_FEATURE_SUPPORT     231
#define EXT_CSD_TRIM_MULT               232
#define EXT_CSD_POWER_OFF_LONG_TIME     247
#define EXT_CSD_GENERIC_CMD6_TIME       248
#define EXT_CSD_CACHE_SIZE              249
#define EXT_CSD_PRE_EOL_INFO            267
#define EXT_CSD_LIFE_TIME_EST_A         268
#define EXT_CSD_LIFE_TIME_EST_B         269
#define EXT_CSD_ACCESS_WRITE_BYTE       3
#define EXT_CSD_CARD_TYPE_26            0x01
#define EXT_CSD_CARD_TYPE_52            0x02
#define EXT_CSD_CARD_TYPE_DDR52_18_30   0x04
#define EXT_CSD_CARD_TYPE_HS200_18      0x10
#define EXT_CSD_BUS_WIDTH_4             1
#define EXT_CSD_BUS_WIDTH_8             2
#define EXT_CSD_BUS_WIDTH_4_DDR         5
#define EXT_CSD_BUS_WIDTH_8_DDR         6
#define EXT_CSD_TIMING_HS               1
#define EXT_CSD_TIMING_HS200            2
#define EXT_CSD_POWER_ON                1
#define EXT_CSD_POWER_OFF_SHORT         2
#define EXT_CSD_POWER_OFF_LONG          3
#define EXT_CSD_SEC_GB_CL_EN            0x10        /* TRIM supported */
#define MMC_ERASE_ARG_ERASE             0x00000000
#define MMC_ERASE_ARG_TRIM              0x00000001
#define MMC_ERASE_ARG_DISCARD           0x00000003
#define SD_ERASE_ARG_DISCARD            0x00000001

/* Bus timing in use (MMC_IOCTL_SLOT.Timing) */
#define MMC_TIMING_LEGACY               0
#define MMC_TIMING_HS                   1           /* SD high speed, MMC HS26/HS52 */
#define MMC_TIMING_SDR50                2
#define MMC_TIMING_SDR104               3
#define MMC_TIMING_DDR50                4
#define MMC_TIMING_HS200                5
#define MMC_TIMING_DDR52                6
#define MMC_TIMING_SDR25                7           /* UHS-I at 1.8 V, no tuning */
#define MMC_TIMING_HS400                8

/* How UNMAP reaches the card (MMC_IOCTL_SLOT.EraseMode) */
#define MMC_UNMAP_ERASE                 0
#define MMC_UNMAP_TRIM                  1
#define MMC_UNMAP_DISCARD               2

/* Clocks, kHz */
#define MMC_CLOCK_INIT_KHZ              400
#define MMC_CLOCK_SD_DEFAULT_KHZ        25000
#define MMC_CLOCK_SD_HS_KHZ             50000
#define MMC_CLOCK_MMC_DEFAULT_KHZ       20000
#define MMC_CLOCK_MMC_HS26_KHZ          26000
#define MMC_CLOCK_MMC_HS52_KHZ          52000
#define MMC_CLOCK_SDR50_KHZ             100000
#define MMC_CLOCK_SDR104_KHZ            208000
#define MMC_CLOCK_HS200_KHZ             200000
#define MMC_TUNING_LOOPS                40
#define MMC_UNMAP_MAX_DESCRIPTORS       64
#define MMC_UNMAP_MAX_BLOCKS            0x00400000  /* per descriptor reported in VPD B0 (2 GB) */
#define MMC_ERASE_MAX_MS                30000       /* longest single erase command the driver waits for */

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

#define MMC_POLL_INTERVAL_USEC          10
#define MMC_CMD_TIMEOUT_MS              250         /* command line */
#define MMC_INIT_TIMEOUT_MS             1500        /* ACMD41 / CMD1 power-up */
#define MMC_BUSY_TIMEOUT_MS             3000        /* R1b, programming */
#define MMC_POLL_TIMEOUT_DEFAULT_SEC    10
#define MMC_POLL_TIMEOUT_MAX_SEC        60
#define MMC_TIMER_USEC                  100000
#define MMC_MISSED_IRQ_LIMIT            5
#define MMC_DMA_ERROR_LIMIT             3           /* then PIO for the slot */
#define MMC_RETRY_LIMIT                 2           /* a failed transfer, card reinitialized */

/* ------------------------------------------------------------------ */
/* SCSI                                                                */
/* ------------------------------------------------------------------ */

#ifndef SCSIOP_SYNCHRONIZE_CACHE
#define SCSIOP_SYNCHRONIZE_CACHE        0x35
#endif
#ifndef SCSIOP_MODE_SENSE10
#define SCSIOP_MODE_SENSE10             0x5A
#endif

#define MMC_SCSIOP_START_STOP_UNIT      0x1B
#define MMC_SCSIOP_MODE_SELECT6         0x15
#define MMC_SCSIOP_MODE_SELECT10        0x55
#define MMC_SCSIOP_REPORT_LUNS          0xA0
#define MMC_SCSIOP_READ12               0xA8
#define MMC_SCSIOP_WRITE12              0xAA
#define MMC_SCSIOP_VERIFY12             0xAF
#define MMC_SCSIOP_READ16               0x88
#define MMC_SCSIOP_WRITE16              0x8A
#define MMC_SCSIOP_VERIFY16             0x8F
#define MMC_SCSIOP_SYNC_CACHE16         0x91
#define MMC_SCSIOP_SERVICE_ACTION_IN16  0x9E
#define MMC_SCSIOP_UNMAP                0x42
#define MMC_SCSIOP_LOG_SENSE            0x4D
#define MMC_SA_READ_CAPACITY16          0x10
#define MMC_READ_CAPACITY16_DATA_LEN    32

#define MMC_INQUIRY_VERSION_SPC3        0x05
#define MMC_VPD_SUPPORTED_PAGES         0x00
#define MMC_VPD_SERIAL_NUMBER           0x80
#define MMC_VPD_BLOCK_LIMITS            0xB0
#define MMC_VPD_BLOCK_CHARACTERISTICS   0xB1
#define MMC_VPD_PROVISIONING            0xB2

#define MMC_LOG_PAGE_SUPPORTED          0x00
#define MMC_LOG_PAGE_SOLID_STATE        0x11
#define MMC_LOG_PAGE_INFO_EXCEPTIONS    0x2F
#define MMC_ASC_FAILURE_PREDICTION      0x5D

#define MMC_MODE_PAGE_GEOMETRY          0x04
#define MMC_MODE_PAGE_CACHING           0x08
#define MMC_MODE_PAGE_CONTROL           0x0A
#define MMC_MODE_PAGE_ALL               0x3F

#define MMC_ASC_UNRECOVERED_READ        0x11
#define MMC_ASC_PARAM_LIST_LENGTH       0x1A
#define MMC_ASC_INVALID_OPCODE          0x20
#define MMC_ASC_LBA_OUT_OF_RANGE        0x21
#define MMC_ASC_INVALID_CDB_FIELD       0x24
#define MMC_ASC_INVALID_PARAM_FIELD     0x26
#define MMC_ASC_WRITE_PROTECTED         0x27
#define MMC_ASC_NO_MEDIA                0x3A
#define MMC_ASC_WRITE_ERROR             0x0C
#define MMC_ASC_INTERNAL_FAILURE        0x44

/* ------------------------------------------------------------------ */
/* 64-bit block numbers without compiler 64-bit types                  */
/* ------------------------------------------------------------------ */

typedef struct _MMC_LBA {
    ULONG Low;
    ULONG High;
} MMC_LBA, *PMMC_LBA;

typedef struct _MODE_PARAMETER_HEADER_10 {
    UCHAR ModeDataLengthMsb;
    UCHAR ModeDataLengthLsb;
    UCHAR MediumType;
    UCHAR DeviceSpecificParameter;
    UCHAR Reserved[2];
    UCHAR BlockDescriptorLengthMsb;
    UCHAR BlockDescriptorLengthLsb;
} MODE_PARAMETER_HEADER_10, *PMODE_PARAMETER_HEADER_10;

/* ------------------------------------------------------------------ */
/* Miniport IOCTLs                                                     */
/* ------------------------------------------------------------------ */

typedef struct _MMC_SRB_IO_CONTROL {
    ULONG HeaderLength;
    UCHAR Signature[8];
    ULONG Timeout;
    ULONG ControlCode;
    ULONG ReturnCode;
    ULONG Length;
} MMC_SRB_IO_CONTROL, *PMMC_SRB_IO_CONTROL;

/* "MMCSDNT ": information, power control for an ACPI driver on NT 3.1-4.0.
   Through IOCTL_SCSI_MINIPORT to \\.\ScsiN:. */
#define MMC_IOCTL_SIGNATURE             "MMCSDNT "
#define MMC_IOCTL_GET_INFO              0x00000001
#define MMC_IOCTL_POWER_DOWN            0x00000010
#define MMC_IOCTL_POWER_UP              0x00000011
#define MMC_IOCTL_TRIM_MODE_ON          0x00000020  /* 4 KB pattern after the header */
#define MMC_IOCTL_TRIM_MODE_OFF         0x00000021
#define MMC_TRIM_PATTERN_SIZE           4096

#define MMC_INFO_ASYNC                  0x00000001  /* transfers completed by interrupt */
#define MMC_INFO_POWERED_DOWN           0x00000002
#define MMC_INFO_ACPI                   0x00000004  /* controller found through ACPI */
#define MMC_INFO_TRIM_MODE              0x00000008  /* writes of the TRIM pattern become discards */
#define MMC_INFO_DMA64                  0x00000010  /* buffers above 4 GB go to the controller directly */

/* Card type in MMC_IOCTL_SLOT.CardType */
#define MMC_CARD_NONE                   0
#define MMC_CARD_SDSC                   1           /* SD 1.x / 2.0 standard capacity */
#define MMC_CARD_SDHC                   2           /* SDHC / SDXC (block addressed) */
#define MMC_CARD_MMC                    3           /* MMC, byte addressed */
#define MMC_CARD_MMC_HC                 4           /* MMC / eMMC, sector addressed */

/* Transfer method in MMC_IOCTL_SLOT.Dma */
#define MMC_XFER_PIO                    0
#define MMC_XFER_SDMA                   1
#define MMC_XFER_ADMA2                  2
#define MMC_XFER_ADMA2_64               3           /* 64-bit addressing */

typedef struct _MMC_IOCTL_SLOT {
    UCHAR  CardType;
    UCHAR  BusWidth;                    /* 1, 4, 8 */
    UCHAR  Dma;                         /* MMC_XFER_xxx */
    UCHAR  HighSpeed;
    UCHAR  WriteProtected;
    UCHAR  CacheOn;                     /* eMMC cache */
    UCHAR  HostVersion;                 /* SDHCI_SPEC_xxx */
    UCHAR  DmaErrors;
    ULONG  ClockKhz;
    ULONG  SectorsLow;
    ULONG  SectorsHigh;
    ULONG  Cid[4];
    ULONG  Csd[4];
    ULONG  Ocr;
    ULONG  Capabilities;
    ULONG  Retries;
    UCHAR  ExtCsdRev;
    UCHAR  PreEol;                      /* eMMC 5.0: 1 normal, 2 warning, 3 urgent; 0 not reported */
    UCHAR  LifeTimeA;                   /* eMMC 5.0: 1 = 0-10 % used ... 0Bh = exceeded */
    UCHAR  LifeTimeB;
    UCHAR  Timing;                      /* MMC_TIMING_xxx */
    UCHAR  EraseMode;                   /* MMC_UNMAP_xxx */
    UCHAR  Signal18;                    /* 1.8 V signalling */
    UCHAR  PowerOffNotify;              /* eMMC power-off notification in use */
    UCHAR  CqState;                     /* MMC_CQ_STATE_xxx */
    UCHAR  CqDepth;                     /* tasks in flight at most */
    UCHAR  CqPeak;                      /* tasks in flight at once, highest seen */
    UCHAR  Reserved;
} MMC_IOCTL_SLOT, *PMMC_IOCTL_SLOT;

#define MMC_CQ_STATE_ENGINE             0x01        /* command queuing engine found (CQ=1) */
#define MMC_CQ_STATE_ACTIVE             0x02        /* reads and writes are queued */
#define MMC_CQ_STATE_OFF                0x04        /* given up after an error */

typedef struct _MMC_IOCTL_INFO {
    ULONG Version;                      /* MMC_DRIVER_VERSION */
    ULONG Flags;                        /* MMC_INFO_xxx */
    ULONG SlotCount;
    ULONG PciVendorDevice;              /* 0 for an ACPI controller */
    ULONG RegisterBase;                 /* physical address of slot 0 */
    ULONG Irq;
    ULONG InterruptCount;
    MMC_IOCTL_SLOT Slot[MMC_MAX_SLOTS];
} MMC_IOCTL_INFO, *PMMC_IOCTL_INFO;

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

typedef struct _MMC_SLOT {
    BOOLEAN                 Present;        /* registers mapped */
    BOOLEAN                 CardReady;      /* card initialized */
    BOOLEAN                 NeedsInit;      /* initialize again before the next request */
    BOOLEAN                 Async;          /* transfers complete in the interrupt */
    UCHAR                   Index;
    UCHAR                   HostVersion;
    UCHAR                   Xfer;           /* MMC_XFER_xxx in use */
    UCHAR                   XferBest;       /* what the controller offers */
    BOOLEAN                 Adma64;         /* ADMA2 with 96-bit descriptors */
    PUCHAR                  Regs;
    ULONG                   RegsPhysical;
    ULONG                   Caps;
    ULONG                   Caps1;
    ULONG                   BaseClockKhz;
    ULONG                   ClockKhz;
    UCHAR                   BusWidth;
    BOOLEAN                 HighSpeed;
    BOOLEAN                 Cmd23;          /* SET_BLOCK_COUNT usable */
    UCHAR                   Timing;         /* MMC_TIMING_xxx */
    BOOLEAN                 Signal18;       /* 1.8 V signalling in use */
    BOOLEAN                 NoUhs;          /* UHS-I / 1.8 V modes failed: not tried again */
    BOOLEAN                 NoHs200;
    BOOLEAN                 NoDdr;
    BOOLEAN                 NoHs400;

    /* Command queuing (CQ=1) */
    PUCHAR                  CqRegs;         /* CQHCI registers, NULL without */
    BOOLEAN                 CqWide;         /* 64-bit DMA: 128-bit descriptors */
    BOOLEAN                 CqActive;       /* reads and writes go through the engine */
    BOOLEAN                 CqCardOn;       /* the card is in command queue mode */
    BOOLEAN                 CqHalted;
    BOOLEAN                 NoCq;           /* failed once: not used again */
    UCHAR                   CqDepth;
    ULONG                   CqBusy;         /* tasks in flight */
    UCHAR                   CqPeak;
    PSCSI_REQUEST_BLOCK     CqSrb[MMC_CQ_DEPTH];
    PUCHAR                  CqTdl;
    ULONG                   CqTdlPhysical;
    ULONG                   CqTdlPhysicalHigh;
    PUCHAR                  CqTrd[MMC_CQ_DEPTH];
    ULONG                   CqTrdPhysical[MMC_CQ_DEPTH];
    ULONG                   CqTrdPhysicalHigh[MMC_CQ_DEPTH];
    UCHAR                   DmaErrors;
    ULONG                   Retries;

    /* Card */
    UCHAR                   CardType;       /* MMC_CARD_xxx */
    BOOLEAN                 BlockAddressing;
    BOOLEAN                 WriteProtected;
    BOOLEAN                 EmmcCache;      /* the card has a cache */
    BOOLEAN                 CacheOn;
    UCHAR                   WantCache;      /* MODE SELECT: 0 as is, 1 on, 2 off */
    UCHAR                   ExtCsdRev;
    BOOLEAN                 PowerOffNotify; /* eMMC 4.5: POWER_OFF_NOTIFICATION switched on */
    UCHAR                   PreEol;
    UCHAR                   LifeTimeA;
    UCHAR                   LifeTimeB;
    UCHAR                   EraseMode;      /* MMC_UNMAP_xxx */
    UCHAR                   SdDiscard;      /* SD 5.1 discard supported */
    ULONG                   EraseGroup;     /* blocks per erase unit (ERASE only) */
    ULONG                   EraseUnitMs;    /* time per erase unit (or per TRIM/DISCARD command) */
    ULONG                   PowerOffMs;     /* POWER_OFF_LONG_TIME */
    ULONG                   SleepMs;        /* S_A_TIMEOUT */
    USHORT                  Rca;
    ULONG                   Ocr;
    ULONG                   Cid[4];         /* [0] = bits 127:96 */
    ULONG                   Csd[4];
    UCHAR                   Scr[8];
    MMC_LBA                 Sectors;

    /* Sense for the class driver's REQUEST SENSE (NT4/9x) */
    BOOLEAN                 SensePending;
    UCHAR                   SenseLength;
    UCHAR                   Sense[MMC_SENSE_MAX];

    /* Last command result */
    ULONG                   LastIntStatus;
    ULONG                   LastCardStatus;

    /* DMA area */
    PULONG                  Adma;
    ULONG                   AdmaPhysical;
    ULONG                   AdmaPhysicalHigh;
    PUCHAR                  Scratch;
    ULONG                   ScratchPhysical;
    ULONG                   ScratchPhysicalHigh;

    /* Interrupt-completed transfer: one in flight, one waiting */
    PSCSI_REQUEST_BLOCK     Outstanding;
    PSCSI_REQUEST_BLOCK     Pending;
    BOOLEAN                 ActiveWrite;
    BOOLEAN                 ActiveMulti;
    BOOLEAN                 ActiveFua;
} MMC_SLOT, *PMMC_SLOT;

typedef struct _HW_DEVICE_EXTENSION {
    ULONG                   PciBus;
    ULONG                   PciSlot;
    USHORT                  VendorId;
    USHORT                  DeviceId;
    BOOLEAN                 HasPci;
    BOOLEAN                 Acpi;           /* found through the ACPI tables or an ACPI device node */
    UCHAR                   SlotCount;
    UCHAR                   Irq;

    BOOLEAN                 AsyncMode;
    BOOLEAN                 DmaAllowed;     /* DMA=0 turns it off */
    BOOLEAN                 Dma64;          /* every slot does 64-bit ADMA2; DMA64=0 turns it off */
    BOOLEAN                 HighSpeedAllowed;   /* HS=0 turns it off */
    BOOLEAN                 UhsAllowed;     /* UHS=0: no 1.8 V modes (UHS-I, HS200, DDR52) */
    BOOLEAN                 Hs200Allowed;   /* HS200=0 */
    BOOLEAN                 DdrAllowed;     /* DDR=0 */
    BOOLEAN                 UnmapAllowed;   /* TRIM=0: UNMAP refused */
    BOOLEAN                 Hs400Allowed;   /* HS400=1 */
    BOOLEAN                 CqAllowed;      /* CQ=1 or CQ=<offset> */
    ULONG                   CqOffset;
    BOOLEAN                 TrimMode;       /* TRIM_MODE_ON: pattern writes become discards */
    BOOLEAN                 Stopped;
    BOOLEAN                 TimerArmed;
    BOOLEAN                 DumpMode;
    BOOLEAN                 CheckState;
    ULONG                   MissedInterrupts;
    ULONG                   InterruptCount;
    ULONG                   MaxClockKhz;    /* CLOCK=n (MHz) limit, 0 = none */

    MMC_SLOT                Slots[MMC_MAX_SLOTS];

    PUCHAR                  DmaArea;
    ULONG                   DmaLength;
    SCSI_PHYSICAL_ADDRESS   DmaAreaPhysical;
    PUCHAR                  SdmaBounce;     /* SDMA-only controllers */
    ULONG                   SdmaBouncePhysical;
    ULONG                   SdmaRun;        /* bytes physically contiguous from the start, within one 512 KB window */
    UCHAR                   TrimPattern[MMC_TRIM_PATTERN_SIZE];
} HW_DEVICE_EXTENSION, *PHW_DEVICE_EXTENSION;

/* One SD/MMC command, with or without data */
#define MMC_RSP_NONE            0
#define MMC_RSP_R1              1           /* 48 bits, CRC, index */
#define MMC_RSP_R1B             2           /* R1 and busy on DAT0 */
#define MMC_RSP_R2              3           /* 136 bits */
#define MMC_RSP_R3              4           /* 48 bits, no CRC, no index (OCR) */

typedef struct _MMC_REQUEST {
    UCHAR     Command;
    UCHAR     Response;         /* MMC_RSP_xxx */
    BOOLEAN   App;              /* CMD55 first */
    BOOLEAN   Abort;            /* command type abort (CMD12) */
    ULONG     Argument;
    /* Data phase */
    PVOID     Buffer;           /* NULL: the slot's scratch buffer */
    ULONG     BlockSize;        /* 0: no data */
    ULONG     BlockCount;
    BOOLEAN   IsWrite;
    BOOLEAN   AutoStop;         /* multi-block: auto CMD12 */
    BOOLEAN   NoDma;            /* PIO for this one */
    BOOLEAN   ScratchRepeat;    /* VERIFY: every block into the scratch buffer */
    ULONG     TimeoutMs;        /* 0: the default for the kind of command */
    PSCSI_REQUEST_BLOCK Srb;    /* for physical addresses */
    ULONG     Resp[4];          /* R2: Resp[0] = bits 127:96 */
} MMC_REQUEST, *PMMC_REQUEST;

/* MmcExecute results */
#define MMC_OK                  0
#define MMC_ERR_TIMEOUT         1           /* no response / busy too long */
#define MMC_ERR_CRC             2
#define MMC_ERR_DATA            3           /* data phase: CRC, end bit, timeout, ADMA */
#define MMC_ERR_CARD            4           /* R1 status reports an error */
#define MMC_ERR_BAD_BUFFER      5           /* buffer cannot be described for DMA */
#define MMC_ERR_NO_CARD         6

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#if !defined(MMC_NT4)
#ifdef _MSC_VER
#pragma function(memset)
#endif
static __inline void * __cdecl memset(void *dst, int val, size_t count) {
    char *p = (char *)dst;
    while (count--) *p++ = (char)val;
    return dst;
}
#define ZeroMemoryBytes(dst, len)       memset((dst), 0, (len))
#else
static __inline void ZeroMemoryBytes(PVOID dst, ULONG len) {
    PUCHAR d = (PUCHAR)dst;
    while (len--) *d++ = 0;
}
#endif

static __inline void CopyMemoryBytes(PVOID dst, PVOID src, ULONG len) {
    PUCHAR d = (PUCHAR)dst;
    PUCHAR s = (PUCHAR)src;
    while (len--) *d++ = *s++;
}

/* ------------------------------------------------------------------ */
/* Cross-file prototypes                                               */
/* ------------------------------------------------------------------ */

/* mmcmain.c */
BOOLEAN MmcReinitialize(IN PHW_DEVICE_EXTENSION HwInit);
VOID    MmcCheckState(IN PHW_DEVICE_EXTENSION HwInit);
VOID    MmcArmTimer(IN PHW_DEVICE_EXTENSION HwInit);
BOOLEAN MmcPowerDown(IN PHW_DEVICE_EXTENSION HwInit);
ULONG   MmcPciReadConfig(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Offset, IN ULONG Width);
VOID    MmcPciWriteConfig(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Offset, IN ULONG Width, IN ULONG Value);

/* mmccmd.c */
BOOLEAN MmcHostInit(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot);
VOID    MmcHostStop(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot);
BOOLEAN MmcCardInit(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot);
ULONG   MmcExecute(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN OUT PMMC_REQUEST Req);
ULONG   MmcReadWrite(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb,
                     IN PVOID Buffer, IN MMC_LBA Lba, IN ULONG Count, IN BOOLEAN IsWrite);
ULONG   MmcVerify(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN MMC_LBA Lba, IN ULONG Count);
ULONG   MmcFlush(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot);
ULONG   MmcSetCache(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN BOOLEAN On);
ULONG   MmcSleep(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN BOOLEAN Sleep);
ULONG   MmcPowerOffNotify(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot);
ULONG   MmcUnmap(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN MMC_LBA Lba, IN ULONG Count);
ULONG   MmcUnmapGranularity(IN PMMC_SLOT Slot);
ULONG   MmcSwitch(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG Index, IN ULONG Value, IN ULONG TimeoutMs);
VOID    MmcRecover(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN BOOLEAN Data);
BOOLEAN MmcWaitIdle(IN PMMC_SLOT Slot, IN ULONG Mask, IN ULONG TimeoutMs);
BOOLEAN MmcCardPresent(IN PMMC_SLOT Slot);
UCHAR   MmcIssueAsync(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb,
                      IN MMC_LBA Lba, IN ULONG Count, IN BOOLEAN IsWrite);
ULONG   MmcServiceSlot(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN BOOLEAN FromInterrupt);
VOID    MmcDrainSlot(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot);
VOID    MmcFailOutstanding(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN UCHAR SrbStatus);
BOOLEAN MmcAckStray(IN PMMC_SLOT Slot);

#if MMC_HAS_CQ
/* mmccq.c */
VOID    MmcCqProbe(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot);
VOID    MmcCqStart(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG CardDepth);
VOID    MmcCqReset(IN PMMC_SLOT Slot);
BOOLEAN MmcCqWanted(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb);
VOID    MmcCqHalt(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot);
UCHAR   MmcCqIssue(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb,
                   IN MMC_LBA Lba, IN ULONG Count, IN BOOLEAN IsWrite, IN BOOLEAN Fua);
ULONG   MmcCqService(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot);
VOID    MmcCqFail(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN UCHAR SrbStatus);
#endif

/* mmcsatl.c */
VOID    MmcSatlProcessSrb(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb);
VOID    MmcProcessIoControl(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb);
VOID    MmcSatlCompleteAsync(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb, IN ULONG Result);
BOOLEAN MmcSatlStartPending(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb);

/* mmcacpi.c */
typedef struct _MMC_ACPI_DEVICE {
    ULONG   MmioBase;
    ULONG   MmioLength;
    ULONG   Irq;            /* GSI, 0 = none */
    BOOLEAN LevelIrq;
    CHAR    Hid[9];
} MMC_ACPI_DEVICE, *PMMC_ACPI_DEVICE;

ULONG   MmcAcpiFindControllers(IN PHW_DEVICE_EXTENSION HwInit, OUT PMMC_ACPI_DEVICE Devices, IN ULONG Max);

#endif /* _MMCSD_H_ */
