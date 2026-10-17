#include "mmcsd.h"

/* mmccmd.c - SD host controller (SDHCI) and SD/MMC card protocol: host
   bring-up, clock, card initialization, commands, PIO/SDMA/ADMA2 data
   transfers, interrupt-completed transfers, error recovery */

/* ------------------------------------------------------------------ */
/* Low level                                                           */
/* ------------------------------------------------------------------ */

static VOID
MmcDelayMs(IN ULONG Ms)
{
    while (Ms--) ScsiPortStallExecution(1000);
}

static VOID
MmcSetHostControl(IN PMMC_SLOT Slot, IN ULONG Clear, IN ULONG Set)
{
    ULONG v = MMC_RD(Slot, SDHCI_HOST_CONTROL);
    v = (v & ~Clear) | Set;
    MMC_WR(Slot, SDHCI_HOST_CONTROL, v);
}

/* Host control 2 lives in the upper half of 0x3C; the lower half is the
   read-only auto CMD error status */
static ULONG
MmcHostControl2(IN PMMC_SLOT Slot)
{
    return (MMC_RD(Slot, SDHCI_HOST_CONTROL2) >> 16) & 0xFFFF;
}

static VOID
MmcSetHostControl2(IN PMMC_SLOT Slot, IN ULONG Clear, IN ULONG Set)
{
    ULONG v = (MmcHostControl2(Slot) & ~Clear) | Set;
    MMC_WR(Slot, SDHCI_HOST_CONTROL2, v << 16);
}

/* Software reset of the given lines (SDHCI_RESET_xxx) */
static BOOLEAN
MmcResetLines(IN PMMC_SLOT Slot, IN ULONG Lines)
{
    ULONG v, i;

    v = MMC_RD(Slot, SDHCI_CLOCK_CONTROL) & 0x00FFFFFF;
    MMC_WR(Slot, SDHCI_CLOCK_CONTROL, v | (Lines << 24));
    for (i = 0; i < 100000; i += 10) {
        if (((MMC_RD(Slot, SDHCI_CLOCK_CONTROL) >> 24) & Lines) == 0) return TRUE;
        ScsiPortStallExecution(10);
    }
    MMC_DBG((MMC_PFX "Slot %lu: reset %02lX did not finish\n", (ULONG)Slot->Index, Lines));
    return FALSE;
}

/* Interrupt status until one of Mask (or an error) is set */
static ULONG
MmcWaitInt(IN PMMC_SLOT Slot, IN ULONG Mask, IN ULONG TimeoutMs, OUT PBOOLEAN TimedOut)
{
    ULONG st, elapsed = 0, limit = TimeoutMs * 1000;

    *TimedOut = FALSE;
    for (;;) {
        st = MMC_RD(Slot, SDHCI_INT_STATUS);
        if (st & (Mask | SDHCI_INT_ERROR)) return st;
        if (st == 0xFFFFFFFF) {                 /* controller gone (decoding off, removed) */
            *TimedOut = TRUE;
            return st;
        }
        if (elapsed >= limit) {
            *TimedOut = TRUE;
            return st;
        }
        ScsiPortStallExecution(MMC_POLL_INTERVAL_USEC);
        elapsed += MMC_POLL_INTERVAL_USEC;
    }
}

BOOLEAN
MmcCardPresent(IN PMMC_SLOT Slot)
{
    ULONG ps = MMC_RD(Slot, SDHCI_PRESENT_STATE);
    if (ps == 0xFFFFFFFF) return FALSE;
    /* Embedded slots (eMMC) often report no card; the card answers anyway */
    if ((Slot->Caps & SDHCI_SLOT_TYPE_MASK) == SDHCI_SLOT_TYPE_EMBEDDED) return TRUE;
    return (BOOLEAN)((ps & SDHCI_CARD_PRESENT) != 0);
}

/* Bits [Start + Size - 1 : Start] of a 128-bit register (R[0] = 127:96) */
static ULONG
MmcBits(IN PULONG R, IN ULONG Start, IN ULONG Size)
{
    ULONG word = 3 - (Start / 32);
    ULONG shift = Start % 32;
    ULONG v = R[word] >> shift;

    if (shift + Size > 32 && word > 0) v |= R[word - 1] << (32 - shift);
    if (Size < 32) v &= (1UL << Size) - 1;
    return v;
}

/* ------------------------------------------------------------------ */
/* Clock and host                                                      */
/* ------------------------------------------------------------------ */

static VOID
MmcSetClock(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG Khz)
{
    ULONG base = Slot->BaseClockKhz, div = 0, actual, v, i;

    if (HwInit->MaxClockKhz != 0 && Khz > HwInit->MaxClockKhz) Khz = HwInit->MaxClockKhz;

    v = MMC_RD(Slot, SDHCI_CLOCK_CONTROL) & 0xFFFF0000;
    MMC_WR(Slot, SDHCI_CLOCK_CONTROL, v);       /* clock off */
    if (Khz == 0) {
        Slot->ClockKhz = 0;
        return;
    }

    if (base <= Khz) {
        div = 0;
        actual = base;
    } else if (Slot->HostVersion >= SDHCI_SPEC_300) {
        /* 10-bit divided clock: base / (2 * N) */
        div = (base + 2 * Khz - 1) / (2 * Khz);
        if (div > 1023) div = 1023;
        actual = base / (2 * div);
    } else {
        /* 8-bit divider, a power of two */
        for (div = 1; div < 128 && base / (2 * div) > Khz; div <<= 1) ;
        actual = base / (2 * div);
    }

    v |= ((div & 0xFF) << SDHCI_DIVIDER_SHIFT) | (((div >> 8) & 0x03) << SDHCI_DIVIDER_HI_SHIFT) |
         SDHCI_CLOCK_INT_EN;
    MMC_WR(Slot, SDHCI_CLOCK_CONTROL, v);
    for (i = 0; i < 20000; i += 10) {
        if (MMC_RD(Slot, SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) break;
        ScsiPortStallExecution(10);
    }
    MMC_WR(Slot, SDHCI_CLOCK_CONTROL, v | SDHCI_CLOCK_CARD_EN);
    Slot->ClockKhz = actual;
}

/* Reset the controller slot, power, 400 kHz, interrupts (status only) */
BOOLEAN
MmcHostInit(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG v, power, clockMhz;

    v = MMC_RD(Slot, SDHCI_SLOT_INT_VERSION);
    if (v == 0xFFFFFFFF) {
        MMC_DBG((MMC_PFX "Slot %lu: registers read FFFFFFFF\n", (ULONG)Slot->Index));
        return FALSE;
    }
    Slot->HostVersion = (UCHAR)((v >> 16) & 0xFF);
    Slot->Caps = MMC_RD(Slot, SDHCI_CAPABILITIES);
    Slot->Caps1 = MMC_RD(Slot, SDHCI_CAPABILITIES_1);

    if (!MmcResetLines(Slot, SDHCI_RESET_ALL)) return FALSE;
    if (Slot->Signal18) {
        /* The reset turned the power off: keep it off long enough for a
           card that signals at 1.8 V to start again at 3.3 V */
        v = MMC_RD(Slot, SDHCI_HOST_CONTROL) & 0xFFFF00FF;
        MMC_WR(Slot, SDHCI_HOST_CONTROL, v);
        MmcDelayMs(20);
        Slot->Signal18 = FALSE;
    }
    Slot->Timing = MMC_TIMING_LEGACY;

    if (Slot->HostVersion >= SDHCI_SPEC_300) {
        clockMhz = (Slot->Caps & SDHCI_BASE_CLOCK_MASK_V3) >> SDHCI_BASE_CLOCK_SHIFT;
    } else {
        clockMhz = (Slot->Caps & SDHCI_BASE_CLOCK_MASK_V2) >> SDHCI_BASE_CLOCK_SHIFT;
    }
    if (clockMhz == 0) clockMhz = 50;           /* not reported: the usual value */
    Slot->BaseClockKhz = clockMhz * 1000;

    /* Highest voltage the slot supports; 3.3 V in practice */
    if (Slot->Caps & SDHCI_CAN_VDD_330) {
        power = SDHCI_POWER_330;
    } else if (Slot->Caps & SDHCI_CAN_VDD_300) {
        power = SDHCI_POWER_300;
    } else {
        power = SDHCI_POWER_180;
    }
    v = MMC_RD(Slot, SDHCI_HOST_CONTROL) & 0xFFFF0000;
    MMC_WR(Slot, SDHCI_HOST_CONTROL, v | (power << 8));
    MMC_WR(Slot, SDHCI_HOST_CONTROL, v | ((power | SDHCI_POWER_ON) << 8));
    MmcDelayMs(2);

    /* Data timeout: the longest */
    v = MMC_RD(Slot, SDHCI_CLOCK_CONTROL) & 0x0000FFFF;
    MMC_WR(Slot, SDHCI_CLOCK_CONTROL, v | (0x0E << 16));

    MMC_WR(Slot, SDHCI_INT_ENABLE, SDHCI_INT_ENABLE_MASK);
    MMC_WR(Slot, SDHCI_SIGNAL_ENABLE, 0);
    MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);

    MmcSetClock(HwInit, Slot, MMC_CLOCK_INIT_KHZ);

    /* Best data transfer method of the controller */
    Slot->Adma64 = FALSE;
    if (!HwInit->DmaAllowed) {
        Slot->XferBest = MMC_XFER_PIO;
    } else if ((Slot->Caps & SDHCI_CAN_ADMA2) && Slot->HostVersion >= SDHCI_SPEC_200) {
        Slot->XferBest = MMC_XFER_ADMA2;
        Slot->Adma64 = HwInit->Dma64;
    } else if ((Slot->Caps & SDHCI_CAN_SDMA) && HwInit->SdmaBounce != NULL) {
        Slot->XferBest = MMC_XFER_SDMA;
    } else {
        Slot->XferBest = MMC_XFER_PIO;
    }

    MMC_DBG((MMC_PFX "Slot %lu: SDHCI %lu.00, caps %08lX:%08lX, base clock %lu MHz, %s\n",
             (ULONG)Slot->Index, (ULONG)Slot->HostVersion + 1, Slot->Caps1, Slot->Caps, clockMhz,
             Slot->XferBest == MMC_XFER_ADMA2 ? (Slot->Adma64 ? "ADMA2 64-bit" : "ADMA2") :
             Slot->XferBest == MMC_XFER_SDMA ? "SDMA" : "PIO"));
    return TRUE;
}

/* Clock and power off (power transitions) */
VOID
MmcHostStop(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG v;

    MMC_WR(Slot, SDHCI_SIGNAL_ENABLE, 0);
    MmcSetClock(HwInit, Slot, 0);
    v = MMC_RD(Slot, SDHCI_HOST_CONTROL) & 0xFFFF00FF;
    MMC_WR(Slot, SDHCI_HOST_CONTROL, v);
    Slot->CardReady = FALSE;
    Slot->NeedsInit = TRUE;
}

/* ------------------------------------------------------------------ */
/* DMA descriptors                                                     */
/* ------------------------------------------------------------------ */

/* One ADMA2 descriptor: 8 bytes (32-bit) or 12 bytes (64-bit) */
static VOID
MmcAdmaEntry(IN PMMC_SLOT Slot, IN ULONG Index, IN ULONG Length, IN ULONG Low, IN ULONG High)
{
    ULONG attr = SDHCI_ADMA_VALID | SDHCI_ADMA_TRAN | ((Length & 0xFFFF) << 16);

    if (Slot->Adma64) {
        Slot->Adma[Index * 3] = attr;
        Slot->Adma[Index * 3 + 1] = Low;
        Slot->Adma[Index * 3 + 2] = High;
    } else {
        Slot->Adma[Index * 2] = attr;
        Slot->Adma[Index * 2 + 1] = Low;
    }
}

/* ADMA2 table for the request's buffer. FALSE when a piece of it cannot
   be described (above 4 GB without 64-bit ADMA2, not dword aligned): the
   caller retries with PIO. */
static BOOLEAN
MmcBuildAdma(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PMMC_REQUEST Req)
{
    ULONG total = Req->BlockSize * Req->BlockCount;
    ULONG left = total, index = 0, segLen, toPage, chunk;
    PUCHAR virt = (PUCHAR)Req->Buffer;
    SCSI_PHYSICAL_ADDRESS phys;

    if (Req->Buffer == NULL) {
        /* The scratch buffer; VERIFY reads every piece into the same place */
        while (left > 0 && index < MMC_ADMA_ENTRIES) {
            chunk = (left > MMC_SCRATCH_SIZE) ? MMC_SCRATCH_SIZE : left;
            MmcAdmaEntry(Slot, index, chunk, Slot->ScratchPhysical, Slot->ScratchPhysicalHigh);
            left -= chunk;
            index++;
            if (!Req->ScratchRepeat && left > 0) return FALSE;
        }
    } else {
        if ((total & 3) != 0) return FALSE;
        while (left > 0 && index < MMC_ADMA_ENTRIES) {
            phys = ScsiPortGetPhysicalAddress(HwInit, Req->Srb, virt, &segLen);
            if ((phys.LowPart == 0 && phys.HighPart == 0) || (phys.LowPart & 3) ||
                (phys.HighPart != 0 && !Slot->Adma64)) {
                return FALSE;
            }
            if (segLen > left) segLen = left;
            toPage = 4096 - (phys.LowPart & 0xFFF);
            if (segLen > toPage) segLen = toPage;
            segLen &= ~3UL;
            if (segLen == 0) return FALSE;
            MmcAdmaEntry(Slot, index, segLen, phys.LowPart, (ULONG)phys.HighPart);
            virt += segLen;
            left -= segLen;
            index++;
        }
    }
    if (left > 0 || index == 0) return FALSE;
    Slot->Adma[(index - 1) * (Slot->Adma64 ? 3 : 2)] |= SDHCI_ADMA_END;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static ULONG
MmcCommandFlags(IN PMMC_REQUEST Req)
{
    ULONG f;

    switch (Req->Response) {
    case MMC_RSP_R1:  f = SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX; break;
    case MMC_RSP_R1B: f = SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX; break;
    case MMC_RSP_R2:  f = SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC; break;
    case MMC_RSP_R3:  f = SDHCI_CMD_RESP_48; break;
    default:          f = SDHCI_CMD_RESP_NONE; break;
    }
    if (Req->BlockSize != 0) f |= SDHCI_CMD_DATA;
    if (Req->Abort) f |= SDHCI_CMD_ABORT;
    return f;
}

static ULONG
MmcTimeoutMs(IN PMMC_REQUEST Req)
{
    ULONG sec;

    if (Req->TimeoutMs != 0) return Req->TimeoutMs;
    if (Req->Srb != NULL && Req->BlockSize != 0) {
        sec = Req->Srb->TimeOutValue ? Req->Srb->TimeOutValue : MMC_POLL_TIMEOUT_DEFAULT_SEC;
        if (sec > 1) sec--;
        if (sec > MMC_POLL_TIMEOUT_MAX_SEC) sec = MMC_POLL_TIMEOUT_MAX_SEC;
        return sec * 1000;
    }
    if (Req->BlockSize != 0) return MMC_POLL_TIMEOUT_DEFAULT_SEC * 1000;
    if (Req->Response == MMC_RSP_R1B) return MMC_BUSY_TIMEOUT_MS;
    return MMC_CMD_TIMEOUT_MS;
}

/* Back to the transfer state after a failed data command */
VOID
MmcRecover(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN BOOLEAN Data)
{
    MMC_REQUEST stop, status;
    ULONG i;

    MmcResetLines(Slot, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
    MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);
    if (!Slot->CardReady) return;

    for (i = 0; i < 2; i++) {
        ZeroMemoryBytes(&status, sizeof(status));
        status.Command = MMC_SEND_STATUS;
        status.Argument = (ULONG)Slot->Rca << 16;
        status.Response = MMC_RSP_R1;
        MmcExecute(HwInit, Slot, &status);
        if (MMC_R1_STATE(status.Resp[0]) == MMC_STATE_TRAN || !Data) return;
        /* Sending or receiving data still: stop it */
        ZeroMemoryBytes(&stop, sizeof(stop));
        stop.Command = MMC_STOP_TRANSMISSION;
        stop.Response = MMC_RSP_R1B;
        stop.Abort = TRUE;
        MmcExecute(HwInit, Slot, &stop);
    }
}

/* PIO: one block through the buffer data port */
static VOID
MmcPioBlock(IN PMMC_SLOT Slot, IN PUCHAR Buffer, IN ULONG Bytes, IN BOOLEAN IsWrite)
{
    ULONG i, v;

    if ((MMC_PTR_LOW(Buffer) & 3) == 0) {
        PULONG p = (PULONG)Buffer;
        for (i = 0; i < Bytes / 4; i++) {
            if (IsWrite) MMC_WR(Slot, SDHCI_BUFFER, p[i]); else p[i] = MMC_RD(Slot, SDHCI_BUFFER);
        }
        return;
    }
    for (i = 0; i < Bytes; i += 4) {
        if (IsWrite) {
            v = (ULONG)Buffer[i] | ((ULONG)Buffer[i + 1] << 8) | ((ULONG)Buffer[i + 2] << 16) |
                ((ULONG)Buffer[i + 3] << 24);
            MMC_WR(Slot, SDHCI_BUFFER, v);
        } else {
            v = MMC_RD(Slot, SDHCI_BUFFER);
            Buffer[i] = (UCHAR)v;
            Buffer[i + 1] = (UCHAR)(v >> 8);
            Buffer[i + 2] = (UCHAR)(v >> 16);
            Buffer[i + 3] = (UCHAR)(v >> 24);
        }
    }
}

static ULONG
MmcErrorFromStatus(IN ULONG St)
{
    if (St & (SDHCI_INT_CRC | SDHCI_INT_END_BIT | SDHCI_INT_INDEX)) return MMC_ERR_CRC;
    if (St & SDHCI_INT_TIMEOUT) return MMC_ERR_TIMEOUT;
    return MMC_ERR_DATA;
}

/* Program the data registers and the DMA engine for Req. Returns the
   transfer mode bits; *Dma tells whether DMA is used. */
static ULONG
MmcSetupData(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PMMC_REQUEST Req,
             OUT PUCHAR Dma, OUT PBOOLEAN BadBuffer)
{
    ULONG mode = SDHCI_TRNS_BLK_CNT_EN;
    UCHAR xfer = Req->NoDma ? MMC_XFER_PIO : Slot->Xfer;
    ULONG total = Req->BlockSize * Req->BlockCount;

    *BadBuffer = FALSE;
    if (xfer == MMC_XFER_ADMA2) {
        if (!MmcBuildAdma(HwInit, Slot, Req)) {
            *BadBuffer = TRUE;
            xfer = MMC_XFER_PIO;
        }
    } else if (xfer == MMC_XFER_SDMA) {
        if (Req->Buffer == NULL) {
            if (total > MMC_SCRATCH_SIZE && !Req->ScratchRepeat) xfer = MMC_XFER_PIO;
            if (Req->ScratchRepeat && total > MMC_SCRATCH_SIZE) xfer = MMC_XFER_PIO;
        } else if (total > HwInit->SdmaRun || HwInit->SdmaBounce == NULL) {
            xfer = MMC_XFER_PIO;
        }
    }
    *Dma = xfer;

    if (xfer == MMC_XFER_ADMA2) {
        MmcSetHostControl(Slot, SDHCI_CTRL_DMA_MASK, Slot->Adma64 ? SDHCI_CTRL_ADMA64 : SDHCI_CTRL_ADMA32);
        MMC_WR(Slot, SDHCI_ADMA_ADDRESS, Slot->AdmaPhysical);
        if (Slot->Caps & SDHCI_CAN_64BIT) MMC_WR(Slot, SDHCI_ADMA_ADDRESS_HI, Slot->AdmaPhysicalHigh);
        mode |= SDHCI_TRNS_DMA;
    } else if (xfer == MMC_XFER_SDMA) {
        MmcSetHostControl(Slot, SDHCI_CTRL_DMA_MASK, SDHCI_CTRL_SDMA);
        if (Req->Buffer == NULL) {
            MMC_WR(Slot, SDHCI_SDMA_ADDRESS, Slot->ScratchPhysical);
        } else {
            if (Req->IsWrite) CopyMemoryBytes(HwInit->SdmaBounce, Req->Buffer, total);
            MMC_WR(Slot, SDHCI_SDMA_ADDRESS, HwInit->SdmaBouncePhysical);
        }
        mode |= SDHCI_TRNS_DMA;
    }

    /* Block size with the 512 KB SDMA boundary, block count */
    MMC_WR(Slot, SDHCI_BLOCK_SIZE, (Req->BlockSize & 0xFFF) | (7UL << 12) | (Req->BlockCount << 16));
    if (Req->BlockCount > 1) {
        mode |= SDHCI_TRNS_MULTI;
        if (Req->AutoStop) mode |= SDHCI_TRNS_AUTO_CMD12;
    }
    if (!Req->IsWrite) mode |= SDHCI_TRNS_READ;
    return mode;
}

/* The data phase after the command was accepted (polled) */
static ULONG
MmcDataPhase(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PMMC_REQUEST Req, IN UCHAR Dma)
{
    ULONG timeout = MmcTimeoutMs(Req);
    ULONG st, block, total = Req->BlockSize * Req->BlockCount;
    PUCHAR p = (PUCHAR)(Req->Buffer != NULL ? Req->Buffer : Slot->Scratch);
    ULONG offset = 0;
    BOOLEAN timedOut;

    if (Dma == MMC_XFER_PIO) {
        for (block = 0; block < Req->BlockCount; block++) {
            st = MmcWaitInt(Slot, Req->IsWrite ? SDHCI_INT_SPACE_AVAIL : SDHCI_INT_DATA_AVAIL, timeout, &timedOut);
            if (timedOut || (st & SDHCI_INT_ERROR)) {
                Slot->LastIntStatus = st;
                return timedOut ? MMC_ERR_TIMEOUT : MMC_ERR_DATA;
            }
            MMC_WR(Slot, SDHCI_INT_STATUS, Req->IsWrite ? SDHCI_INT_SPACE_AVAIL : SDHCI_INT_DATA_AVAIL);
            MmcPioBlock(Slot, p + offset, Req->BlockSize, Req->IsWrite);
            offset += Req->BlockSize;
            if (Req->Buffer == NULL && offset >= MMC_SCRATCH_SIZE) offset = 0;
        }
    }

    for (;;) {
        st = MmcWaitInt(Slot, SDHCI_INT_DATA_END | SDHCI_INT_DMA_END, timeout, &timedOut);
        if (timedOut || (st & SDHCI_INT_ERROR)) {
            Slot->LastIntStatus = st;
            return timedOut ? MMC_ERR_TIMEOUT : MMC_ERR_DATA;
        }
        if (st & SDHCI_INT_DATA_END) break;
        /* SDMA buffer boundary: go on from where the engine stopped (no
           transfer is set up to cross one) */
        MMC_WR(Slot, SDHCI_INT_STATUS, SDHCI_INT_DMA_END);
        MMC_WR(Slot, SDHCI_SDMA_ADDRESS, MMC_RD(Slot, SDHCI_SDMA_ADDRESS));
    }
    MMC_WR(Slot, SDHCI_INT_STATUS, SDHCI_INT_DATA_END | SDHCI_INT_DMA_END);

    if (Dma == MMC_XFER_SDMA && !Req->IsWrite && Req->Buffer != NULL) {
        CopyMemoryBytes(Req->Buffer, HwInit->SdmaBounce, total);
    }
    return MMC_OK;
}

/* Wait until the command and data lines are free */
BOOLEAN
MmcWaitIdle(IN PMMC_SLOT Slot, IN ULONG Mask, IN ULONG TimeoutMs)
{
    ULONG elapsed = 0, limit = TimeoutMs * 1000, ps;

    for (;;) {
        ps = MMC_RD(Slot, SDHCI_PRESENT_STATE);
        if (ps == 0xFFFFFFFF) return FALSE;
        if ((ps & Mask) == 0) return TRUE;
        if (elapsed >= limit) return FALSE;
        ScsiPortStallExecution(MMC_POLL_INTERVAL_USEC);
        elapsed += MMC_POLL_INTERVAL_USEC;
    }
}

/* Send the command of Req (data registers already set up when it has
   data) and wait for its response */
static ULONG
MmcSendCommand(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PMMC_REQUEST Req, IN ULONG Mode)
{
    ULONG st, r0, r1, r2, r3;
    BOOLEAN timedOut;

    (VOID)HwInit;
    MMC_WR(Slot, SDHCI_ARGUMENT, Req->Argument);
    MMC_WR(Slot, SDHCI_TRANSFER_MODE, (Mode & 0xFFFF) |
           (SDHCI_MAKE_CMD(Req->Command, MmcCommandFlags(Req)) << 16));

    st = MmcWaitInt(Slot, SDHCI_INT_RESPONSE, MMC_CMD_TIMEOUT_MS, &timedOut);
    if (timedOut || (st & SDHCI_INT_ERROR)) {
        Slot->LastIntStatus = st;
        return timedOut ? MMC_ERR_TIMEOUT : MmcErrorFromStatus(st);
    }
    MMC_WR(Slot, SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);

    if (Req->Response == MMC_RSP_R2) {
        /* The register holds bits 127:8 of the response, without the CRC */
        r0 = MMC_RD(Slot, SDHCI_RESPONSE);
        r1 = MMC_RD(Slot, SDHCI_RESPONSE + 4);
        r2 = MMC_RD(Slot, SDHCI_RESPONSE + 8);
        r3 = MMC_RD(Slot, SDHCI_RESPONSE + 12);
        Req->Resp[0] = (r3 << 8) | (r2 >> 24);
        Req->Resp[1] = (r2 << 8) | (r1 >> 24);
        Req->Resp[2] = (r1 << 8) | (r0 >> 24);
        Req->Resp[3] = r0 << 8;
    } else if (Req->Response != MMC_RSP_NONE) {
        Req->Resp[0] = MMC_RD(Slot, SDHCI_RESPONSE);
    }
    return MMC_OK;
}

/* One command with optional data, polled. Data errors leave the card in
   the transfer state again (MmcRecover). */
ULONG
MmcExecute(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN OUT PMMC_REQUEST Req)
{
    ULONG mode = 0, result, st;
    UCHAR dma = MMC_XFER_PIO;
    BOOLEAN badBuffer = FALSE, timedOut;
    ULONG inhibit;

    if (Req->App) {
        MMC_REQUEST app;
        ZeroMemoryBytes(&app, sizeof(app));
        app.Command = MMC_APP_CMD;
        app.Argument = (ULONG)Slot->Rca << 16;
        app.Response = MMC_RSP_R1;
        result = MmcExecute(HwInit, Slot, &app);
        if (result != MMC_OK) return result;
        if (!(app.Resp[0] & MMC_R1_APP_CMD) && Slot->Rca != 0) return MMC_ERR_CARD;
    }

    inhibit = SDHCI_CMD_INHIBIT;
    if (Req->BlockSize != 0 || Req->Response == MMC_RSP_R1B) {
        if (!Req->Abort) inhibit |= SDHCI_DATA_INHIBIT;
    }
    if (!MmcWaitIdle(Slot, inhibit, MMC_BUSY_TIMEOUT_MS)) {
        MMC_DBG((MMC_PFX "Slot %lu: lines busy before CMD%lu (state %08lX)\n", (ULONG)Slot->Index,
                 (ULONG)Req->Command, MMC_RD(Slot, SDHCI_PRESENT_STATE)));
        MmcResetLines(Slot, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
        if (!MmcWaitIdle(Slot, inhibit, 10)) return MMC_ERR_TIMEOUT;
    }
    MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);

    if (Req->BlockSize != 0) {
        mode = MmcSetupData(HwInit, Slot, Req, &dma, &badBuffer);
        if (badBuffer && !Req->NoDma && Req->Buffer != NULL) return MMC_ERR_BAD_BUFFER;
    }

    result = MmcSendCommand(HwInit, Slot, Req, mode);
    if (result != MMC_OK) {
        MmcResetLines(Slot, SDHCI_RESET_CMD | ((Req->BlockSize != 0 || Req->Response == MMC_RSP_R1B) ? SDHCI_RESET_DATA : 0));
        MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);
        return result;
    }

    if (Req->BlockSize != 0) {
        result = MmcDataPhase(HwInit, Slot, Req, dma);
        if (result != MMC_OK) {
            MMC_DBG((MMC_PFX "Slot %lu: CMD%lu data %s failed, status %08lX\n", (ULONG)Slot->Index,
                     (ULONG)Req->Command, Req->IsWrite ? "write" : "read", Slot->LastIntStatus));
            if (dma != MMC_XFER_PIO && ++Slot->DmaErrors >= MMC_DMA_ERROR_LIMIT && Slot->Xfer != MMC_XFER_PIO) {
                MMC_DBG((MMC_PFX "Slot %lu: DMA off after %lu errors\n", (ULONG)Slot->Index, (ULONG)Slot->DmaErrors));
                Slot->Xfer = MMC_XFER_PIO;
            }
            MmcRecover(HwInit, Slot, TRUE);
            return result;
        }
        if (dma != MMC_XFER_PIO) Slot->DmaErrors = 0;
    } else if (Req->Response == MMC_RSP_R1B) {
        /* Busy on DAT0: the controller reports its end as transfer complete */
        st = MmcWaitInt(Slot, SDHCI_INT_DATA_END, MmcTimeoutMs(Req), &timedOut);
        if (st & SDHCI_INT_ERROR) {
            Slot->LastIntStatus = st;
            MmcResetLines(Slot, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);
            return MmcErrorFromStatus(st);
        }
        if (timedOut && !MmcWaitIdle(Slot, SDHCI_DATA_INHIBIT, 10)) {
            MmcResetLines(Slot, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            return MMC_ERR_TIMEOUT;
        }
        MMC_WR(Slot, SDHCI_INT_STATUS, SDHCI_INT_DATA_END);
    }

    /* R1 status (not for SD CMD3's R6, CMD8's R7, CMD12 of an abort) */
    if ((Req->Response == MMC_RSP_R1 || Req->Response == MMC_RSP_R1B) &&
        Req->Command != MMC_SET_RELATIVE_ADDR && !(Req->Command == SD_SEND_IF_COND && Req->BlockSize == 0) &&
        Req->Command != MMC_APP_CMD && !Req->Abort) {
        Slot->LastCardStatus = Req->Resp[0];
        if (Req->Resp[0] & MMC_R1_ERRORS) {
            MMC_DBG((MMC_PFX "Slot %lu: CMD%lu card status %08lX\n", (ULONG)Slot->Index,
                     (ULONG)Req->Command, Req->Resp[0]));
            return MMC_ERR_CARD;
        }
    }
    return MMC_OK;
}

static ULONG
MmcSimple(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN UCHAR Command, IN ULONG Argument,
          IN UCHAR Response, OUT PULONG Resp)
{
    MMC_REQUEST req;
    ULONG result;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Command = Command;
    req.Argument = Argument;
    req.Response = Response;
    result = MmcExecute(HwInit, Slot, &req);
    if (Resp != NULL) *Resp = req.Resp[0];
    return result;
}

/* A small read into the scratch buffer (SCR, EXT_CSD, switch status) */
static ULONG
MmcReadScratch(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN UCHAR Command, IN ULONG Argument,
               IN BOOLEAN App, IN ULONG Bytes)
{
    MMC_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    ZeroMemoryBytes(Slot->Scratch, Bytes);
    req.Command = Command;
    req.Argument = Argument;
    req.Response = MMC_RSP_R1;
    req.App = App;
    req.BlockSize = Bytes;
    req.BlockCount = 1;
    req.TimeoutMs = 1000;
    return MmcExecute(HwInit, Slot, &req);
}

/* MMC SWITCH: write one EXT_CSD byte */
ULONG
MmcSwitch(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG Index, IN ULONG Value, IN ULONG TimeoutMs)
{
    MMC_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Command = MMC_SWITCH;
    req.Argument = (EXT_CSD_ACCESS_WRITE_BYTE << 24) | (Index << 16) | (Value << 8);
    req.Response = MMC_RSP_R1B;
    req.TimeoutMs = TimeoutMs;
    return MmcExecute(HwInit, Slot, &req);
}

/* ------------------------------------------------------------------ */
/* Bus speed modes at 1.8 V (UHS-I, HS200, DDR52)                      */
/* ------------------------------------------------------------------ */

/* Clock off, the UHS mode of the host, clock on at Khz */
static VOID
MmcSetTiming(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG UhsMode, IN ULONG Khz, IN UCHAR Timing)
{
    MmcSetClock(HwInit, Slot, 0);
    MmcSetHostControl2(Slot, SDHCI_CTRL2_UHS_MASK, UhsMode);
    MmcSetHostControl(Slot, SDHCI_CTRL_HISPD, (Khz > MMC_CLOCK_SD_DEFAULT_KHZ) ? SDHCI_CTRL_HISPD : 0);
    MmcSetClock(HwInit, Slot, Khz);
    Slot->Timing = Timing;
    Slot->HighSpeed = TRUE;
}

/* Tuning (SDR104, SDR50 where the host asks for it, HS200): the host
   reads the tuning block until it has found its sampling point */
static BOOLEAN
MmcExecuteTuning(IN PMMC_SLOT Slot, IN UCHAR Command)
{
    ULONG i, st, ctrl2, blockSize;
    BOOLEAN timedOut;

    blockSize = (Command == MMC_SEND_TUNING_BLOCK_HS200 && Slot->BusWidth == 8) ? 128 : 64;
    MmcSetHostControl2(Slot, SDHCI_CTRL2_TUNED_CLK, SDHCI_CTRL2_EXEC_TUNING);
    for (i = 0; i < MMC_TUNING_LOOPS; i++) {
        if (!MmcWaitIdle(Slot, SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT, 50)) {
            MmcResetLines(Slot, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
        }
        MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);
        MMC_WR(Slot, SDHCI_BLOCK_SIZE, blockSize | (1UL << 16));
        MMC_WR(Slot, SDHCI_ARGUMENT, 0);
        MMC_WR(Slot, SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ |
               (SDHCI_MAKE_CMD(Command, SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA) << 16));
        st = MmcWaitInt(Slot, SDHCI_INT_DATA_AVAIL, 150, &timedOut);
        MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);
        if (timedOut || (st & SDHCI_INT_ERROR)) MmcResetLines(Slot, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
        ctrl2 = MmcHostControl2(Slot);
        if (!(ctrl2 & SDHCI_CTRL2_EXEC_TUNING)) break;
    }
    MmcWaitIdle(Slot, SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT, 50);
    MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);
    ctrl2 = MmcHostControl2(Slot);
    if (!(ctrl2 & SDHCI_CTRL2_EXEC_TUNING) && (ctrl2 & SDHCI_CTRL2_TUNED_CLK)) {
        MMC_DBG((MMC_PFX "Slot %lu: tuned after %lu block(s)\n", (ULONG)Slot->Index, i + 1));
        return TRUE;
    }
    MMC_DBG((MMC_PFX "Slot %lu: tuning failed (host control 2 %04lX)\n", (ULONG)Slot->Index, ctrl2));
    MmcSetHostControl2(Slot, SDHCI_CTRL2_EXEC_TUNING | SDHCI_CTRL2_TUNED_CLK, 0);
    MmcResetLines(Slot, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
    return FALSE;
}

/* A card still sending a tuning block the host did not take is stopped */
static VOID
MmcAfterTuning(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    MMC_REQUEST req;
    ULONG resp = 0;

    MmcSimple(HwInit, Slot, MMC_SEND_STATUS, (ULONG)Slot->Rca << 16, MMC_RSP_R1, &resp);
    if (MMC_R1_STATE(resp) == MMC_STATE_DATA) {
        ZeroMemoryBytes(&req, sizeof(req));
        req.Command = MMC_STOP_TRANSMISSION;
        req.Response = MMC_RSP_R1B;
        req.Abort = TRUE;
        MmcExecute(HwInit, Slot, &req);
    }
}

/* 1.8 V signalling on the host side; FALSE when the host keeps 3.3 V */
static BOOLEAN
MmcHostSignal18(IN PMMC_SLOT Slot)
{
    MmcSetHostControl2(Slot, 0, SDHCI_CTRL2_VDD_180);
    MmcDelayMs(5);
    if (!(MmcHostControl2(Slot) & SDHCI_CTRL2_VDD_180)) return FALSE;
    Slot->Signal18 = TRUE;
    return TRUE;
}

/* SD: CMD11 and the signal voltage switch sequence. FALSE: the card has to
   be powered off before it is set up again. */
static BOOLEAN
MmcSdVoltageSwitch(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG v, khz = Slot->ClockKhz;

    if (MmcSimple(HwInit, Slot, SD_VOLTAGE_SWITCH, 0, MMC_RSP_R1, NULL) != MMC_OK) return FALSE;
    v = MMC_RD(Slot, SDHCI_CLOCK_CONTROL);
    MMC_WR(Slot, SDHCI_CLOCK_CONTROL, v & ~(ULONG)SDHCI_CLOCK_CARD_EN);
    MmcDelayMs(1);
    if (MMC_RD(Slot, SDHCI_PRESENT_STATE) & SDHCI_DATA_LVL_MASK) return FALSE;      /* card holds DAT low */
    Slot->Signal18 = TRUE;
    if (!MmcHostSignal18(Slot)) return FALSE;
    MmcSetClock(HwInit, Slot, khz);
    MmcDelayMs(1);
    if ((MMC_RD(Slot, SDHCI_PRESENT_STATE) & SDHCI_DATA_LVL_MASK) != SDHCI_DATA_LVL_MASK) return FALSE;
    MMC_DBG((MMC_PFX "Slot %lu: 1.8 V signalling\n", (ULONG)Slot->Index));
    return TRUE;
}

/* SD SWITCH_FUNC, set mode, one group: the function the card switched to */
static ULONG
MmcSdSetFunction(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG Group, IN ULONG Function)
{
    ULONG arg = (0x80FFFFFFUL & ~(0x0FUL << (Group * 4))) | (Function << (Group * 4));
    ULONG byte, shift;

    if (MmcReadScratch(HwInit, Slot, MMC_SWITCH, arg, FALSE, 64) != MMC_OK) return 0x0F;
    /* Result of group g: bits 379+4g:376+4g of the status */
    byte = 16 - Group / 2;
    shift = (Group & 1) ? 4 : 0;
    return (Slot->Scratch[byte] >> shift) & 0x0F;
}

/* SD UHS-I after the 4-bit bus is set: SDR104, DDR50 or SDR50, tuned;
   SDR25 at 1.8 V when tuning fails */
static VOID
MmcSdSelectUhs(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG support, mode = 0, uhs = 0, khz = 0, current, ma;
    UCHAR timing = MMC_TIMING_SDR25;
    BOOLEAN tune = FALSE;

    if (MmcReadScratch(HwInit, Slot, MMC_SWITCH, 0x00FFFFFF, FALSE, 64) != MMC_OK) return;
    support = Slot->Scratch[13];
    if ((support & 0x08) && (Slot->Caps1 & SDHCI_CAN1_SDR104)) {
        mode = 3; uhs = SDHCI_CTRL2_UHS_SDR104; khz = MMC_CLOCK_SDR104_KHZ; timing = MMC_TIMING_SDR104; tune = TRUE;
    } else if ((support & 0x10) && (Slot->Caps1 & SDHCI_CAN1_DDR50) && HwInit->DdrAllowed && !Slot->NoDdr) {
        mode = 4; uhs = SDHCI_CTRL2_UHS_DDR50; khz = MMC_CLOCK_SD_HS_KHZ; timing = MMC_TIMING_DDR50;
    } else if ((support & 0x04) && (Slot->Caps1 & SDHCI_CAN1_SDR50)) {
        mode = 2; uhs = SDHCI_CTRL2_UHS_SDR50; khz = MMC_CLOCK_SDR50_KHZ; timing = MMC_TIMING_SDR50;
        tune = (BOOLEAN)((Slot->Caps1 & SDHCI_CAN1_TUNING_SDR50) != 0);
    }

    if (mode != 0) {
        /* Current limit from what the host can deliver at 1.8 V */
        ma = ((MMC_RD(Slot, SDHCI_MAX_CURRENT) >> 16) & 0xFF) * 4;
        current = (ma >= 800) ? 3 : (ma >= 600) ? 2 : (ma >= 400) ? 1 : 0;
        if (current != 0) MmcSdSetFunction(HwInit, Slot, 3, current);
        if (MmcSdSetFunction(HwInit, Slot, 0, mode) == mode) {
            MmcSetTiming(HwInit, Slot, uhs, khz, timing);
            if (!tune) return;
            if (MmcExecuteTuning(Slot, SD_SEND_TUNING_BLOCK)) {
                MmcAfterTuning(HwInit, Slot);
                return;
            }
        }
    }
    /* SDR25: no tuning */
    if (MmcSdSetFunction(HwInit, Slot, 0, 1) == 1) {
        MmcSetTiming(HwInit, Slot, SDHCI_CTRL2_UHS_SDR25, MMC_CLOCK_SD_HS_KHZ, MMC_TIMING_SDR25);
    } else {
        MmcSetTiming(HwInit, Slot, SDHCI_CTRL2_UHS_SDR12, MMC_CLOCK_SD_DEFAULT_KHZ, MMC_TIMING_SDR25);
        Slot->HighSpeed = FALSE;
    }
}

/* SD status (ACMD13): allocation unit, erase timing, discard */
static VOID
MmcSdReadStatus(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    static const ULONG auKb[16] = { 0, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
                                    8192, 12288, 16384, 24576, 32768, 65536 };
    ULONG au, eraseSize, eraseTimeout, eraseOffset;
    PUCHAR st = Slot->Scratch;

    Slot->EraseGroup = 8192;                    /* 4 MB when the card does not say */
    Slot->EraseUnitMs = 250;
    Slot->SdDiscard = FALSE;
    if (MmcReadScratch(HwInit, Slot, SD_APP_SD_STATUS, 0, TRUE, 64) != MMC_OK) return;
    au = auKb[(st[10] >> 4) & 0x0F] * 2;
    eraseSize = ((ULONG)st[11] << 8) | st[12];
    eraseTimeout = st[13] >> 2;
    eraseOffset = st[13] & 0x03;
    if (au != 0) Slot->EraseGroup = au;
    if (eraseSize != 0 && eraseTimeout != 0) {
        Slot->EraseUnitMs = (eraseTimeout * 1000) / eraseSize + eraseOffset * 1000;
        if (Slot->EraseUnitMs == 0) Slot->EraseUnitMs = 250;
    }
    Slot->SdDiscard = (UCHAR)((st[24] & 0x02) != 0);
}

/* eMMC HS200: 1.8 V, HS_TIMING 2, 200 MHz, tuning with CMD21 */
static BOOLEAN
MmcEmmcHs200(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG Cmd6Ms)
{
    if (!MmcHostSignal18(Slot)) return FALSE;
    if (MmcSwitch(HwInit, Slot, EXT_CSD_HS_TIMING, EXT_CSD_TIMING_HS200, Cmd6Ms) != MMC_OK) return FALSE;
    MmcSetTiming(HwInit, Slot, SDHCI_CTRL2_UHS_SDR104, MMC_CLOCK_HS200_KHZ, MMC_TIMING_HS200);
    if (!MmcExecuteTuning(Slot, MMC_SEND_TUNING_BLOCK_HS200)) return FALSE;
    MmcAfterTuning(HwInit, Slot);
    /* The card answers at the new timing */
    return (BOOLEAN)(MmcSimple(HwInit, Slot, MMC_SEND_STATUS, (ULONG)Slot->Rca << 16, MMC_RSP_R1, NULL) == MMC_OK);
}

/* eMMC DDR52 after HS52: double data rate bus width, DDR50 host mode; the
   EXT_CSD is read back to check that data moves */
static BOOLEAN
MmcEmmcDdr52(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG Cmd6Ms)
{
    ULONG width = (Slot->BusWidth == 8) ? EXT_CSD_BUS_WIDTH_8_DDR : EXT_CSD_BUS_WIDTH_4_DDR;

    if (!(Slot->Signal18 || MmcHostSignal18(Slot))) return FALSE;
    if (MmcSwitch(HwInit, Slot, EXT_CSD_BUS_WIDTH, width, Cmd6Ms) != MMC_OK) return FALSE;
    MmcSetTiming(HwInit, Slot, SDHCI_CTRL2_UHS_DDR50, MMC_CLOCK_MMC_HS52_KHZ, MMC_TIMING_DDR52);
    return (BOOLEAN)(MmcReadScratch(HwInit, Slot, MMC_SEND_EXT_CSD, 0, FALSE, 512) == MMC_OK);
}

/* eMMC HS400 after HS200 (8-bit bus, the HS200 tuning kept): back to high
   speed timing at 52 MHz, 8-bit DDR bus, HS_TIMING 3, host HS400 mode at
   200 MHz; the EXT_CSD is read back to check that data moves */
static BOOLEAN
MmcEmmcHs400(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG Cmd6Ms)
{
    MmcSetClock(HwInit, Slot, MMC_CLOCK_MMC_HS52_KHZ);
    if (MmcSwitch(HwInit, Slot, EXT_CSD_HS_TIMING, EXT_CSD_TIMING_HS, Cmd6Ms) != MMC_OK) return FALSE;
    MmcSetTiming(HwInit, Slot, 0, MMC_CLOCK_MMC_HS52_KHZ, MMC_TIMING_HS);
    if (MmcSimple(HwInit, Slot, MMC_SEND_STATUS, (ULONG)Slot->Rca << 16, MMC_RSP_R1, NULL) != MMC_OK) return FALSE;
    if (MmcSwitch(HwInit, Slot, EXT_CSD_BUS_WIDTH, EXT_CSD_BUS_WIDTH_8_DDR, Cmd6Ms) != MMC_OK) return FALSE;
    if (MmcSwitch(HwInit, Slot, EXT_CSD_HS_TIMING, EXT_CSD_TIMING_HS400, Cmd6Ms) != MMC_OK) return FALSE;
    MmcSetTiming(HwInit, Slot, SDHCI_CTRL2_UHS_HS400, MMC_CLOCK_HS200_KHZ, MMC_TIMING_HS400);
    if (MmcSimple(HwInit, Slot, MMC_SEND_STATUS, (ULONG)Slot->Rca << 16, MMC_RSP_R1, NULL) != MMC_OK) return FALSE;
    return (BOOLEAN)(MmcReadScratch(HwInit, Slot, MMC_SEND_EXT_CSD, 0, FALSE, 512) == MMC_OK);
}

/* ------------------------------------------------------------------ */
/* Card initialization                                                 */
/* ------------------------------------------------------------------ */

static VOID
MmcSetBusWidth(IN PMMC_SLOT Slot, IN UCHAR Width)
{
    ULONG set = (Width == 8) ? SDHCI_CTRL_8BIT : (Width == 4) ? SDHCI_CTRL_4BIT : 0;
    MmcSetHostControl(Slot, SDHCI_CTRL_4BIT | SDHCI_CTRL_8BIT, set);
    Slot->BusWidth = Width;
}

/* Capacity from the CSD (SD all versions, MMC byte addressed) */
static VOID
MmcCapacityFromCsd(IN PMMC_SLOT Slot, IN BOOLEAN Sd)
{
    ULONG structure = MmcBits(Slot->Csd, 126, 2);
    ULONG cSize, mult, blLen, blocks;

    Slot->Sectors.Low = 0;
    Slot->Sectors.High = 0;
    if (Sd && structure == 1) {
        cSize = MmcBits(Slot->Csd, 48, 22);                 /* (C_SIZE + 1) * 512 KB */
        Slot->Sectors.Low = (cSize + 1) << 10;
        Slot->Sectors.High = (cSize + 1) >> 22;
    } else if (Sd && structure == 2) {
        cSize = MmcBits(Slot->Csd, 48, 28);                 /* SDUC */
        Slot->Sectors.Low = (cSize + 1) << 10;
        Slot->Sectors.High = (cSize + 1) >> 22;
    } else {
        cSize = MmcBits(Slot->Csd, 62, 12);
        mult = MmcBits(Slot->Csd, 47, 3);
        blLen = MmcBits(Slot->Csd, 80, 4);
        blocks = (cSize + 1) << (mult + 2);
        if (blLen >= 9) {
            Slot->Sectors.Low = blocks << (blLen - 9);
        } else {
            Slot->Sectors.Low = blocks >> (9 - blLen);
        }
    }
}

static BOOLEAN
MmcInitSd(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG resp, result;
    UCHAR spec;

    /* SCR: bus widths, spec version, CMD23 */
    result = MmcReadScratch(HwInit, Slot, SD_APP_SEND_SCR, 0, TRUE, 8);
    if (result == MMC_OK) {
        CopyMemoryBytes(Slot->Scr, Slot->Scratch, 8);
    } else {
        MMC_DBG((MMC_PFX "Slot %lu: SCR not read (%lu)\n", (ULONG)Slot->Index, result));
        ZeroMemoryBytes(Slot->Scr, 8);
        Slot->Scr[1] = 0x05;                    /* 1 and 4 bits, as every SD card has */
    }
    spec = (UCHAR)(Slot->Scr[0] & 0x0F);
    Slot->Cmd23 = (BOOLEAN)((Slot->Scr[3] & 0x02) != 0);

    if (Slot->CardType == MMC_CARD_SDSC) {
        MmcSimple(HwInit, Slot, MMC_SET_BLOCKLEN, MMC_BLOCK_SIZE, MMC_RSP_R1, NULL);
    }

    /* 4-bit bus */
    if (Slot->Scr[1] & 0x04) {
        MMC_REQUEST req;
        ZeroMemoryBytes(&req, sizeof(req));
        req.Command = SD_APP_SET_BUS_WIDTH;
        req.Argument = 2;
        req.Response = MMC_RSP_R1;
        req.App = TRUE;
        if (MmcExecute(HwInit, Slot, &req) == MMC_OK) MmcSetBusWidth(Slot, 4);
    }

    MmcSdReadStatus(HwInit, Slot);

    /* UHS-I: the card switched to 1.8 V during identification */
    if (Slot->Signal18 && Slot->BusWidth == 4) {
        MmcSdSelectUhs(HwInit, Slot);
    } else if (spec >= 1 && (Slot->Caps & SDHCI_CAN_HISPD) && HwInit->HighSpeedAllowed) {
        /* High speed (SD 1.10 and later): CMD6 check, then switch */
        result = MmcReadScratch(HwInit, Slot, MMC_SWITCH, 0x00FFFFF1, FALSE, 64);
        if (result == MMC_OK && (Slot->Scratch[13] & 0x02)) {
            result = MmcReadScratch(HwInit, Slot, MMC_SWITCH, 0x80FFFFF1, FALSE, 64);
            if (result == MMC_OK && (Slot->Scratch[16] & 0x0F) == 1) {
                MmcSetHostControl(Slot, 0, SDHCI_CTRL_HISPD);
                Slot->HighSpeed = TRUE;
                Slot->Timing = MMC_TIMING_HS;
            }
        }
    }
    if (!Slot->Signal18) MmcSetClock(HwInit, Slot, Slot->HighSpeed ? MMC_CLOCK_SD_HS_KHZ : MMC_CLOCK_SD_DEFAULT_KHZ);
    Slot->EraseMode = (UCHAR)(Slot->SdDiscard ? MMC_UNMAP_DISCARD : MMC_UNMAP_ERASE);

    /* Write protect switch (not on an embedded slot) */
    if ((Slot->Caps & SDHCI_SLOT_TYPE_MASK) != SDHCI_SLOT_TYPE_EMBEDDED) {
        Slot->WriteProtected = (BOOLEAN)((MMC_RD(Slot, SDHCI_PRESENT_STATE) & SDHCI_WRITE_ENABLED) == 0);
    }
    (VOID)resp;
    return TRUE;
}

/* Erase group from the CSD (no EXT_CSD high-capacity erase unit) */
static VOID
MmcCsdEraseGroup(IN PMMC_SLOT Slot)
{
    ULONG size = MmcBits(Slot->Csd, 42, 5), mult = MmcBits(Slot->Csd, 37, 5);
    ULONG blLen = MmcBits(Slot->Csd, 22, 4);

    Slot->EraseGroup = (size + 1) * (mult + 1);
    if (blLen > 9) Slot->EraseGroup <<= (blLen - 9);
    Slot->EraseUnitMs = 1000;
}

/* 0: done; 1: set the card up again (a fast mode failed and is now off) */
static ULONG
MmcInitMmc(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG spec = MmcBits(Slot->Csd, 122, 4);
    ULONG cardType = 0, cache = 0, secCount, cmd6Ms = 1000, v, cqDepth = 0;
    UCHAR hcErase = 0, eraseMult = 0, trimMult = 0, secFeature = 0;
    PUCHAR ext = Slot->Scratch;
    UCHAR width;

    MmcSetClock(HwInit, Slot, MMC_CLOCK_MMC_DEFAULT_KHZ);
    MmcCsdEraseGroup(Slot);
    Slot->EraseMode = MMC_UNMAP_ERASE;
    Slot->PowerOffNotify = FALSE;
    Slot->PreEol = Slot->LifeTimeA = Slot->LifeTimeB = 0;

    if (spec >= 4 && MmcReadScratch(HwInit, Slot, MMC_SEND_EXT_CSD, 0, FALSE, 512) == MMC_OK) {
        Slot->ExtCsdRev = ext[EXT_CSD_REV];
        cardType = ext[EXT_CSD_CARD_TYPE];
        if (Slot->ExtCsdRev >= 6 && ext[EXT_CSD_GENERIC_CMD6_TIME] != 0) {
            cmd6Ms = (ULONG)ext[EXT_CSD_GENERIC_CMD6_TIME] * 10;
            if (cmd6Ms < 100) cmd6Ms = 100;
        }
        hcErase = ext[EXT_CSD_HC_ERASE_GRP_SIZE];
        eraseMult = ext[EXT_CSD_ERASE_TIMEOUT_MULT];
        trimMult = ext[EXT_CSD_TRIM_MULT];
        secFeature = ext[EXT_CSD_SEC_FEATURE_SUPPORT];
        Slot->PowerOffMs = (ULONG)ext[EXT_CSD_POWER_OFF_LONG_TIME] * 10;
        if (Slot->PowerOffMs < 1000) Slot->PowerOffMs = 1000;
        if (Slot->PowerOffMs > MMC_ERASE_MAX_MS) Slot->PowerOffMs = MMC_ERASE_MAX_MS;
        v = ext[EXT_CSD_S_A_TIMEOUT];
        Slot->SleepMs = (v != 0 && v <= 0x17) ? ((1UL << v) / 10000) + 1 : 1000;
        if (Slot->SleepMs < 10) Slot->SleepMs = 10;
        if (Slot->ExtCsdRev >= 7) {
            Slot->PreEol = ext[EXT_CSD_PRE_EOL_INFO];
            Slot->LifeTimeA = ext[EXT_CSD_LIFE_TIME_EST_A];
            Slot->LifeTimeB = ext[EXT_CSD_LIFE_TIME_EST_B];
        }
        if (Slot->ExtCsdRev >= 8 && (ext[EXT_CSD_CMDQ_SUPPORT] & 1)) cqDepth = (ext[EXT_CSD_CMDQ_DEPTH] & 0x1F) + 1;
        cache = (ULONG)ext[EXT_CSD_CACHE_SIZE] | ((ULONG)ext[EXT_CSD_CACHE_SIZE + 1] << 8) |
                ((ULONG)ext[EXT_CSD_CACHE_SIZE + 2] << 16) | ((ULONG)ext[EXT_CSD_CACHE_SIZE + 3] << 24);
        secCount = (ULONG)ext[EXT_CSD_SEC_COUNT] | ((ULONG)ext[EXT_CSD_SEC_COUNT + 1] << 8) |
                   ((ULONG)ext[EXT_CSD_SEC_COUNT + 2] << 16) | ((ULONG)ext[EXT_CSD_SEC_COUNT + 3] << 24);
        if (Slot->BlockAddressing && secCount != 0) {
            Slot->Sectors.Low = secCount;
            Slot->Sectors.High = 0;
        }

        /* Bus width: 8 bits where the slot has them, else 4 */
        width = (UCHAR)((Slot->Caps & SDHCI_CAN_8BIT) ? 8 : 4);
        if (MmcSwitch(HwInit, Slot, EXT_CSD_BUS_WIDTH,
                      (width == 8) ? EXT_CSD_BUS_WIDTH_8 : EXT_CSD_BUS_WIDTH_4, 1000) == MMC_OK) {
            MmcSetBusWidth(Slot, width);
        } else if (width == 8 &&
                   MmcSwitch(HwInit, Slot, EXT_CSD_BUS_WIDTH, EXT_CSD_BUS_WIDTH_4, 1000) == MMC_OK) {
            MmcSetBusWidth(Slot, 4);
        }

        /* Erase unit: the high-capacity erase group where the card has one */
        if (Slot->BlockAddressing && hcErase != 0 &&
            MmcSwitch(HwInit, Slot, EXT_CSD_ERASE_GROUP_DEF, 1, cmd6Ms) == MMC_OK) {
            Slot->EraseGroup = (ULONG)hcErase * 1024;
            Slot->EraseUnitMs = (eraseMult != 0) ? (ULONG)eraseMult * 300 : 1000;
        }
        if (secFeature & EXT_CSD_SEC_GB_CL_EN) {
            Slot->EraseMode = (UCHAR)((Slot->ExtCsdRev >= 6) ? MMC_UNMAP_DISCARD : MMC_UNMAP_TRIM);
            Slot->EraseUnitMs = (trimMult != 0) ? (ULONG)trimMult * 300 : 1000;
        }

        /* Power-off notification (eMMC 4.5): the card is told before power goes */
        if (Slot->ExtCsdRev >= 6 &&
            MmcSwitch(HwInit, Slot, EXT_CSD_POWER_OFF_NOTIFICATION, EXT_CSD_POWER_ON, cmd6Ms) == MMC_OK) {
            Slot->PowerOffNotify = TRUE;
        }

        /* HS200 (1.8 V, tuned), else high speed timing, then DDR52 */
        if ((cardType & EXT_CSD_CARD_TYPE_HS200_18) && (Slot->Caps1 & SDHCI_CAN1_SDR104) &&
            HwInit->UhsAllowed && HwInit->Hs200Allowed && HwInit->HighSpeedAllowed && !Slot->NoHs200 &&
            Slot->BusWidth >= 4) {
            if (!MmcEmmcHs200(HwInit, Slot, cmd6Ms)) {
                MMC_DBG((MMC_PFX "Slot %lu: HS200 failed, set up again without it\n", (ULONG)Slot->Index));
                Slot->NoHs200 = TRUE;
                return 1;
            }
            /* HS400 only on request (HS400=1): the strobe and delay settings
               are vendor specific */
            if ((cardType & EXT_CSD_CARD_TYPE_HS400_18) && HwInit->Hs400Allowed && !Slot->NoHs400 &&
                Slot->BusWidth == 8 && !MmcEmmcHs400(HwInit, Slot, cmd6Ms)) {
                MMC_DBG((MMC_PFX "Slot %lu: HS400 failed, set up again with HS200\n", (ULONG)Slot->Index));
                Slot->NoHs400 = TRUE;
                return 1;
            }
        } else if ((cardType & (EXT_CSD_CARD_TYPE_26 | EXT_CSD_CARD_TYPE_52)) &&
                   (Slot->Caps & SDHCI_CAN_HISPD) && HwInit->HighSpeedAllowed &&
                   MmcSwitch(HwInit, Slot, EXT_CSD_HS_TIMING, EXT_CSD_TIMING_HS, cmd6Ms) == MMC_OK) {
            MmcSetHostControl(Slot, 0, SDHCI_CTRL_HISPD);
            Slot->HighSpeed = TRUE;
            Slot->Timing = MMC_TIMING_HS;
            MmcSetClock(HwInit, Slot, (cardType & EXT_CSD_CARD_TYPE_52) ? MMC_CLOCK_MMC_HS52_KHZ : MMC_CLOCK_MMC_HS26_KHZ);
            if ((cardType & EXT_CSD_CARD_TYPE_52) && (cardType & EXT_CSD_CARD_TYPE_DDR52_18_30) &&
                (Slot->Caps1 & SDHCI_CAN1_DDR50) && HwInit->UhsAllowed && HwInit->DdrAllowed && !Slot->NoDdr &&
                Slot->BusWidth >= 4 && !MmcEmmcDdr52(HwInit, Slot, cmd6Ms)) {
                MMC_DBG((MMC_PFX "Slot %lu: DDR52 failed, set up again without it\n", (ULONG)Slot->Index));
                Slot->NoDdr = TRUE;
                return 1;
            }
        }

        /* Volatile cache (eMMC 4.5) */
        Slot->EmmcCache = (BOOLEAN)(Slot->ExtCsdRev >= 6 && cache != 0);
        if (Slot->EmmcCache && Slot->WantCache != 2) {
            if (MmcSwitch(HwInit, Slot, EXT_CSD_CACHE_CTRL, 1, cmd6Ms) == MMC_OK) Slot->CacheOn = TRUE;
        }
    }
#if MMC_HAS_CQ
    MmcCqStart(HwInit, Slot, cqDepth);
#else
    (VOID)cqDepth;
#endif

    if (!Slot->BlockAddressing) {
        MmcSimple(HwInit, Slot, MMC_SET_BLOCKLEN, MMC_BLOCK_SIZE, MMC_RSP_R1, NULL);
    }
    Slot->Cmd23 = TRUE;
    return 0;
}

/* Full card initialization (identification, selection, bus width, speed).
   0: ready; 1: failed; 2: a fast mode failed, set the card up again. */
static ULONG
MmcCardInitOnce(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    MMC_REQUEST req;
    ULONG resp = 0, elapsed, result, arg;
    BOOLEAN v2 = FALSE, sd = TRUE, uhs;
    UCHAR wantCache = Slot->WantCache;

    Slot->CardReady = FALSE;
    Slot->NeedsInit = FALSE;
    Slot->CardType = MMC_CARD_NONE;
    Slot->Rca = 0;
    Slot->HighSpeed = FALSE;
    Slot->WriteProtected = FALSE;
    Slot->EmmcCache = FALSE;
    Slot->CacheOn = FALSE;
    Slot->WantCache = wantCache;
    Slot->Sectors.Low = 0;
    Slot->Sectors.High = 0;
    Slot->ExtCsdRev = 0;
    Slot->PowerOffNotify = FALSE;
    Slot->EraseMode = MMC_UNMAP_ERASE;
#if MMC_HAS_CQ
    MmcCqReset(Slot);
#endif

    if (!MmcHostInit(HwInit, Slot)) return 1;
    Slot->Xfer = (Slot->DmaErrors >= MMC_DMA_ERROR_LIMIT) ? MMC_XFER_PIO : Slot->XferBest;
    MmcSetBusWidth(Slot, 1);
    MmcSetHostControl(Slot, SDHCI_CTRL_HISPD, 0);
    if (!MmcCardPresent(Slot)) {
        MMC_DBG((MMC_PFX "Slot %lu: no card\n", (ULONG)Slot->Index));
        return 1;
    }
    MmcDelayMs(2);                              /* 74 clocks before the first command */

    MmcSimple(HwInit, Slot, MMC_GO_IDLE_STATE, 0, MMC_RSP_NONE, NULL);
    MmcDelayMs(2);

    /* SD 2.0: SEND_IF_COND, 2.7-3.6 V, check pattern AAh */
    if (MmcSimple(HwInit, Slot, SD_SEND_IF_COND, 0x1AA, MMC_RSP_R1, &resp) == MMC_OK && (resp & 0xFFF) == 0x1AA) {
        v2 = TRUE;
    }

    /* SD: ACMD41 until the card is powered up; 1.8 V requested where the
       host has UHS-I modes */
    uhs = (BOOLEAN)(v2 && (Slot->Caps1 & SDHCI_CAN1_UHS) && Slot->HostVersion >= SDHCI_SPEC_300 &&
                    HwInit->UhsAllowed && HwInit->HighSpeedAllowed && !Slot->NoUhs);
    arg = MMC_OCR_VDD_27_36 | (v2 ? MMC_OCR_HCS : 0) | (uhs ? (SD_OCR_S18 | SD_OCR_XPC) : 0);
    for (elapsed = 0; ; elapsed += 10) {
        ZeroMemoryBytes(&req, sizeof(req));
        req.Command = SD_APP_SEND_OP_COND;
        req.Argument = arg;
        req.Response = MMC_RSP_R3;
        req.App = TRUE;
        result = MmcExecute(HwInit, Slot, &req);
        if (result != MMC_OK) {
            sd = FALSE;
            break;
        }
        if (req.Resp[0] & MMC_OCR_BUSY) {
            Slot->Ocr = req.Resp[0];
            break;
        }
        if (elapsed >= MMC_INIT_TIMEOUT_MS) {
            MMC_DBG((MMC_PFX "Slot %lu: SD card did not power up (OCR %08lX)\n", (ULONG)Slot->Index, req.Resp[0]));
            return 1;
        }
        MmcDelayMs(10);
    }

    if (sd) {
        Slot->BlockAddressing = (BOOLEAN)(v2 && (Slot->Ocr & MMC_OCR_HCS));
        Slot->CardType = (UCHAR)(Slot->BlockAddressing ? MMC_CARD_SDHC : MMC_CARD_SDSC);
        if (uhs && (Slot->Ocr & SD_OCR_S18) && !MmcSdVoltageSwitch(HwInit, Slot)) {
            MMC_DBG((MMC_PFX "Slot %lu: 1.8 V switch failed, card powered off, set up again at 3.3 V\n",
                     (ULONG)Slot->Index));
            Slot->NoUhs = TRUE;
            Slot->Signal18 = TRUE;              /* the next host reset keeps the power off long enough */
            return 2;
        }
    } else {
        /* MMC / eMMC: CMD1 with sector mode requested */
        MmcSimple(HwInit, Slot, MMC_GO_IDLE_STATE, 0, MMC_RSP_NONE, NULL);
        MmcDelayMs(2);
        for (elapsed = 0; ; elapsed += 10) {
            result = MmcSimple(HwInit, Slot, MMC_SEND_OP_COND, MMC_OCR_VDD_27_36 | MMC_OCR_SECTOR_MODE | 0x80,
                               MMC_RSP_R3, &resp);
            if (result != MMC_OK) {
                MMC_DBG((MMC_PFX "Slot %lu: no SD or MMC card answers (%lu)\n", (ULONG)Slot->Index, result));
                return 1;
            }
            if (resp & MMC_OCR_BUSY) break;
            if (elapsed >= MMC_INIT_TIMEOUT_MS) {
                MMC_DBG((MMC_PFX "Slot %lu: MMC did not power up (OCR %08lX)\n", (ULONG)Slot->Index, resp));
                return 1;
            }
            MmcDelayMs(10);
        }
        Slot->Ocr = resp;
        Slot->BlockAddressing = (BOOLEAN)((resp & 0x60000000) == MMC_OCR_SECTOR_MODE);
        Slot->CardType = (UCHAR)(Slot->BlockAddressing ? MMC_CARD_MMC_HC : MMC_CARD_MMC);
    }

    /* CID */
    ZeroMemoryBytes(&req, sizeof(req));
    req.Command = MMC_ALL_SEND_CID;
    req.Response = MMC_RSP_R2;
    if (MmcExecute(HwInit, Slot, &req) != MMC_OK) {
        MMC_DBG((MMC_PFX "Slot %lu: CID not read\n", (ULONG)Slot->Index));
        return 1;
    }
    CopyMemoryBytes(Slot->Cid, req.Resp, sizeof(Slot->Cid));

    /* Relative address: the card picks it (SD), the host assigns it (MMC) */
    if (sd) {
        if (MmcSimple(HwInit, Slot, MMC_SET_RELATIVE_ADDR, 0, MMC_RSP_R1, &resp) != MMC_OK) return 1;
        Slot->Rca = (USHORT)(resp >> 16);
    } else {
        Slot->Rca = (USHORT)(Slot->Index + 1);
        if (MmcSimple(HwInit, Slot, MMC_SET_RELATIVE_ADDR, (ULONG)Slot->Rca << 16, MMC_RSP_R1, NULL) != MMC_OK) return 1;
    }

    /* CSD */
    ZeroMemoryBytes(&req, sizeof(req));
    req.Command = MMC_SEND_CSD;
    req.Argument = (ULONG)Slot->Rca << 16;
    req.Response = MMC_RSP_R2;
    if (MmcExecute(HwInit, Slot, &req) != MMC_OK) {
        MMC_DBG((MMC_PFX "Slot %lu: CSD not read\n", (ULONG)Slot->Index));
        return 1;
    }
    CopyMemoryBytes(Slot->Csd, req.Resp, sizeof(Slot->Csd));
    MmcCapacityFromCsd(Slot, sd);

    /* Select: transfer state */
    if (MmcSimple(HwInit, Slot, MMC_SELECT_CARD, (ULONG)Slot->Rca << 16, MMC_RSP_R1B, NULL) != MMC_OK) {
        MMC_DBG((MMC_PFX "Slot %lu: card not selected\n", (ULONG)Slot->Index));
        return 1;
    }

    if (sd) {
        MmcInitSd(HwInit, Slot);
    } else if (MmcInitMmc(HwInit, Slot) != 0) {
        return 2;
    }

    if (Slot->Sectors.Low == 0 && Slot->Sectors.High == 0) {
        MMC_DBG((MMC_PFX "Slot %lu: capacity 0\n", (ULONG)Slot->Index));
        return 1;
    }

    Slot->CardReady = TRUE;
    MMC_DBG((MMC_PFX "Slot %lu: %s, %lu-bit, %lu kHz%s, %08lX:%08lX sectors, %s%s%s\n",
             (ULONG)Slot->Index,
             Slot->CardType == MMC_CARD_SDSC ? "SDSC" : Slot->CardType == MMC_CARD_SDHC ? "SDHC/SDXC" :
             Slot->CardType == MMC_CARD_MMC ? "MMC" : "MMC/eMMC (sector)",
             (ULONG)Slot->BusWidth, Slot->ClockKhz, Slot->HighSpeed ? " high speed" : "",
             Slot->Sectors.High, Slot->Sectors.Low,
             Slot->Xfer == MMC_XFER_ADMA2 ? (Slot->Adma64 ? "ADMA2 64-bit" : "ADMA2") :
             Slot->Xfer == MMC_XFER_SDMA ? "SDMA" : "PIO",
             Slot->CacheOn ? ", cache on" : "", Slot->WriteProtected ? ", write protected" : ""));
    MMC_DBG((MMC_PFX "Slot %lu: timing %lu%s, unmap %lu (unit %lu blocks, %lu ms)%s, life %02lX/%02lX/%02lX\n",
             (ULONG)Slot->Index, (ULONG)Slot->Timing, Slot->Signal18 ? " at 1.8 V" : "", (ULONG)Slot->EraseMode,
             Slot->EraseGroup, Slot->EraseUnitMs, Slot->PowerOffNotify ? ", power-off notification" : "",
             (ULONG)Slot->PreEol, (ULONG)Slot->LifeTimeA, (ULONG)Slot->LifeTimeB));
    return 0;
}

BOOLEAN
MmcCardInit(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG i, result = 1;

    for (i = 0; i < 4; i++) {
        result = MmcCardInitOnce(HwInit, Slot);
        if (result != 2) break;
    }
    return (BOOLEAN)(result == 0);
}

/* ------------------------------------------------------------------ */
/* Reads, writes, verify, cache                                        */
/* ------------------------------------------------------------------ */

static ULONG
MmcMaxBlocks(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    if (Slot->Xfer == MMC_XFER_SDMA) return HwInit->SdmaRun / MMC_BLOCK_SIZE;
    return MMC_MAX_TRANSFER / MMC_BLOCK_SIZE;
}

static VOID
MmcFillRw(IN PMMC_SLOT Slot, OUT PMMC_REQUEST Req, IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Buffer,
          IN MMC_LBA Lba, IN ULONG Count, IN BOOLEAN IsWrite)
{
    ZeroMemoryBytes(Req, sizeof(*Req));
    if (IsWrite) {
        Req->Command = (UCHAR)((Count > 1) ? MMC_WRITE_MULTIPLE_BLOCK : MMC_WRITE_BLOCK);
    } else {
        Req->Command = (UCHAR)((Count > 1) ? MMC_READ_MULTIPLE_BLOCK : MMC_READ_SINGLE_BLOCK);
    }
    Req->Argument = Slot->BlockAddressing ? Lba.Low : Lba.Low * MMC_BLOCK_SIZE;
    Req->Response = MMC_RSP_R1;
    Req->Buffer = Buffer;
    Req->BlockSize = MMC_BLOCK_SIZE;
    Req->BlockCount = Count;
    Req->IsWrite = IsWrite;
    Req->AutoStop = (BOOLEAN)(Count > 1);
    Req->Srb = Srb;
}

/* Reads or writes Count blocks, in pieces the transfer method allows; DMA
   that cannot describe the buffer goes with PIO; a failed piece is tried
   again after the card was set up anew */
ULONG
MmcReadWrite(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb,
             IN PVOID Buffer, IN MMC_LBA Lba, IN ULONG Count, IN BOOLEAN IsWrite)
{
    MMC_REQUEST req;
    PUCHAR p = (PUCHAR)Buffer;
    ULONG chunk, result = MMC_OK, tries;

    if (Lba.High != 0) return MMC_ERR_CARD;
    if (!Slot->BlockAddressing && (Lba.Low >= 0x00800000 || Lba.Low + Count > 0x00800000)) return MMC_ERR_CARD;

    while (Count > 0) {
        chunk = MmcMaxBlocks(HwInit, Slot);
        if (chunk > Count) chunk = Count;
        for (tries = 0; ; tries++) {
            MmcFillRw(Slot, &req, Srb, p, Lba, chunk, IsWrite);
            result = MmcExecute(HwInit, Slot, &req);
            if (result == MMC_ERR_BAD_BUFFER) {
                MmcFillRw(Slot, &req, Srb, p, Lba, chunk, IsWrite);
                req.NoDma = TRUE;
                result = MmcExecute(HwInit, Slot, &req);
            }
            if (result == MMC_OK || tries >= MMC_RETRY_LIMIT || HwInit->DumpMode) break;
            Slot->Retries++;
            MMC_DBG((MMC_PFX "Slot %lu: %s at %08lX failed (%lu), card set up again\n", (ULONG)Slot->Index,
                     IsWrite ? "write" : "read", Lba.Low, result));
            if (!MmcCardInit(HwInit, Slot)) {
                Slot->NeedsInit = TRUE;
                return MMC_ERR_NO_CARD;
            }
        }
        if (result != MMC_OK) return result;
        p += chunk * MMC_BLOCK_SIZE;
        Lba.Low += chunk;
        Count -= chunk;
    }
    return MMC_OK;
}

/* VERIFY: read the blocks into the scratch buffer (SD has no verify) */
ULONG
MmcVerify(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN MMC_LBA Lba, IN ULONG Count)
{
    MMC_REQUEST req;
    ULONG chunk, result, max;

    max = (Slot->Xfer == MMC_XFER_ADMA2) ? MMC_MAX_TRANSFER / MMC_BLOCK_SIZE : MMC_SCRATCH_SIZE / MMC_BLOCK_SIZE;
    while (Count > 0) {
        chunk = (Count > max) ? max : Count;
        MmcFillRw(Slot, &req, NULL, NULL, Lba, chunk, FALSE);
        req.ScratchRepeat = TRUE;
        result = MmcExecute(HwInit, Slot, &req);
        if (result != MMC_OK) return result;
        Lba.Low += chunk;
        Count -= chunk;
    }
    return MMC_OK;
}

/* eMMC cache: flush it (SD cards: nothing to do) */
ULONG
MmcFlush(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    if (!Slot->CardReady || !Slot->CacheOn) return MMC_OK;
    return MmcSwitch(HwInit, Slot, EXT_CSD_FLUSH_CACHE, 1, 30000);
}

ULONG
MmcSetCache(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN BOOLEAN On)
{
    ULONG result;

    if (!Slot->EmmcCache) return On ? MMC_ERR_CARD : MMC_OK;
    if (!On) MmcFlush(HwInit, Slot);
    result = MmcSwitch(HwInit, Slot, EXT_CSD_CACHE_CTRL, On ? 1 : 0, 1000);
    if (result == MMC_OK) Slot->CacheOn = On;
    return result;
}

static BOOLEAN
MmcIsSdCard(IN PMMC_SLOT Slot)
{
    return (BOOLEAN)(Slot->CardType == MMC_CARD_SDSC || Slot->CardType == MMC_CARD_SDHC);
}

/* Power down the card: flush, deselect (stand-by), MMC/eMMC also to the
   sleep state (CMD5), clock off. The next request sets the card up again. */
ULONG
MmcSleep(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN BOOLEAN Sleep)
{
    MMC_REQUEST req;
    ULONG result = MMC_OK;

    if (Sleep) {
        if (!Slot->CardReady) return MMC_OK;
        result = MmcFlush(HwInit, Slot);
        MmcSimple(HwInit, Slot, MMC_SELECT_CARD, 0, MMC_RSP_NONE, NULL);
        if (!MmcIsSdCard(Slot) && Slot->ExtCsdRev >= 3) {
            ZeroMemoryBytes(&req, sizeof(req));
            req.Command = MMC_SLEEP_AWAKE;
            req.Argument = ((ULONG)Slot->Rca << 16) | 0x8000;
            req.Response = MMC_RSP_R1B;
            req.TimeoutMs = Slot->SleepMs;
            if (MmcExecute(HwInit, Slot, &req) == MMC_OK) {
                MMC_DBG((MMC_PFX "Slot %lu: sleep\n", (ULONG)Slot->Index));
            }
        }
        MmcSetClock(HwInit, Slot, 0);
        Slot->CardReady = FALSE;
        Slot->NeedsInit = TRUE;
        return result;
    }
    return MmcCardInit(HwInit, Slot) ? MMC_OK : MMC_ERR_NO_CARD;
}

/* System shutdown: flush; eMMC 4.5 gets the long power-off notification
   and its power is turned off, an older MMC goes to sleep. The next
   request, if one still comes, sets the card up again. */
ULONG
MmcPowerOffNotify(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG result;

    if (!Slot->CardReady) return MMC_OK;
    result = MmcFlush(HwInit, Slot);
    if (MmcIsSdCard(Slot)) return result;
    if (!Slot->PowerOffNotify) {
        MmcSleep(HwInit, Slot, TRUE);
        return result;
    }
    if (MmcSwitch(HwInit, Slot, EXT_CSD_POWER_OFF_NOTIFICATION, EXT_CSD_POWER_OFF_LONG, Slot->PowerOffMs) == MMC_OK) {
        MMC_DBG((MMC_PFX "Slot %lu: power-off notification sent\n", (ULONG)Slot->Index));
    }
    MmcHostStop(HwInit, Slot);
    return result;
}

/* ------------------------------------------------------------------ */
/* UNMAP: discard, trim or erase                                       */
/* ------------------------------------------------------------------ */

/* Blocks the card unmaps in one unit: 1 for TRIM/DISCARD, else the erase
   group (VPD B0 optimal unmap granularity) */
ULONG
MmcUnmapGranularity(IN PMMC_SLOT Slot)
{
    if (Slot->EraseMode != MMC_UNMAP_ERASE) return 1;
    return Slot->EraseGroup ? Slot->EraseGroup : 1;
}

/* One erase command sequence for First..First+Count-1 */
static ULONG
MmcEraseRange(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG First, IN ULONG Count, IN ULONG Arg,
              IN ULONG TimeoutMs)
{
    BOOLEAN sd = MmcIsSdCard(Slot);
    ULONG start = Slot->BlockAddressing ? First : First * MMC_BLOCK_SIZE;
    ULONG end = Slot->BlockAddressing ? First + Count - 1 : (First + Count - 1) * MMC_BLOCK_SIZE;
    ULONG result, resp, elapsed;
    MMC_REQUEST req;

    result = MmcSimple(HwInit, Slot, (UCHAR)(sd ? SD_ERASE_WR_BLK_START : MMC_ERASE_GROUP_START), start, MMC_RSP_R1, NULL);
    if (result != MMC_OK) return result;
    result = MmcSimple(HwInit, Slot, (UCHAR)(sd ? SD_ERASE_WR_BLK_END : MMC_ERASE_GROUP_END), end, MMC_RSP_R1, NULL);
    if (result != MMC_OK) return result;
    ZeroMemoryBytes(&req, sizeof(req));
    req.Command = MMC_ERASE;
    req.Argument = Arg;
    req.Response = MMC_RSP_R1B;
    req.TimeoutMs = TimeoutMs;
    result = MmcExecute(HwInit, Slot, &req);
    if (result != MMC_OK) return result;
    /* Some cards end the busy signal before programming is over */
    for (elapsed = 0; elapsed < TimeoutMs; elapsed += 10) {
        if (MmcSimple(HwInit, Slot, MMC_SEND_STATUS, (ULONG)Slot->Rca << 16, MMC_RSP_R1, &resp) != MMC_OK) break;
        if (MMC_R1_STATE(resp) != MMC_STATE_PRG) break;
        MmcDelayMs(10);
    }
    return MMC_OK;
}

/* Unmap Count blocks from Lba: the ranges go out in pieces whose erase
   time stays below MMC_ERASE_MAX_MS; with ERASE only whole erase groups
   are erased (UNMAP is advisory: the rest keeps its data) */
ULONG
MmcUnmap(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN MMC_LBA Lba, IN ULONG Count)
{
    ULONG unit = Slot->EraseGroup ? Slot->EraseGroup : 1, unitMs = Slot->EraseUnitMs ? Slot->EraseUnitMs : 1000;
    ULONG first = Lba.Low, end, arg, maxUnits, piece, units, result;
    BOOLEAN sd = MmcIsSdCard(Slot);

    if (Lba.High != 0 || Count == 0) return MMC_OK;
    if (!Slot->BlockAddressing && (Lba.Low >= 0x00800000 || Lba.Low + Count > 0x00800000)) return MMC_ERR_CARD;
    end = first + Count;

    switch (Slot->EraseMode) {
    case MMC_UNMAP_DISCARD:
        arg = sd ? SD_ERASE_ARG_DISCARD : MMC_ERASE_ARG_DISCARD;
        break;
    case MMC_UNMAP_TRIM:
        arg = MMC_ERASE_ARG_TRIM;
        break;
    default:
        arg = MMC_ERASE_ARG_ERASE;
        if (!sd) {
            /* MMC erases whole groups only */
            first = ((first + unit - 1) / unit) * unit;
            end = (end / unit) * unit;
        }
        break;
    }
    maxUnits = MMC_ERASE_MAX_MS / unitMs;
    if (maxUnits == 0) maxUnits = 1;

    while (first < end) {
        /* As many blocks as touch at most maxUnits erase units */
        piece = (maxUnits * unit) - (first % unit);
        if (piece > end - first) piece = end - first;
        units = (first % unit + piece + unit - 1) / unit;
        result = MmcEraseRange(HwInit, Slot, first, piece, arg, units * unitMs + 1000);
        if (result != MMC_OK) return result;
        first += piece;
    }
    return MMC_OK;
}

/* ------------------------------------------------------------------ */
/* Interrupt-completed transfers (ADMA2)                               */
/* ------------------------------------------------------------------ */

/* Start a read or write and return; HwInterrupt (or the watchdog)
   completes it. SRB_STATUS_PENDING, or the status to complete with;
   SRB_STATUS_INVALID_REQUEST: do it polled instead. */
UCHAR
MmcIssueAsync(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb,
              IN MMC_LBA Lba, IN ULONG Count, IN BOOLEAN IsWrite)
{
    MMC_REQUEST req;
    ULONG mode, result;
    UCHAR dma;
    BOOLEAN badBuffer;

    if (Slot->Outstanding != NULL) return SRB_STATUS_BUSY;
    if (Slot->Xfer != MMC_XFER_ADMA2 || Count > MMC_MAX_TRANSFER / MMC_BLOCK_SIZE) return SRB_STATUS_INVALID_REQUEST;
    if (!Slot->BlockAddressing && (Lba.Low >= 0x00800000 || Lba.Low + Count > 0x00800000)) return SRB_STATUS_INVALID_REQUEST;

    MmcFillRw(Slot, &req, Srb, Srb->DataBuffer, Lba, Count, IsWrite);
    if (!MmcWaitIdle(Slot, SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT, MMC_BUSY_TIMEOUT_MS)) return SRB_STATUS_INVALID_REQUEST;
    MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);
    mode = MmcSetupData(HwInit, Slot, &req, &dma, &badBuffer);
    if (badBuffer || dma != MMC_XFER_ADMA2) return SRB_STATUS_INVALID_REQUEST;

    MMC_WR(Slot, SDHCI_SIGNAL_ENABLE, SDHCI_INT_SIGNAL_MASK);
    result = MmcSendCommand(HwInit, Slot, &req, mode);
    if (result != MMC_OK || (req.Resp[0] & MMC_R1_ERRORS)) {
        MMC_WR(Slot, SDHCI_SIGNAL_ENABLE, 0);
        MmcRecover(HwInit, Slot, TRUE);
        return SRB_STATUS_INVALID_REQUEST;      /* polled, with its retries */
    }
    Slot->Outstanding = Srb;
    Slot->ActiveWrite = IsWrite;
    Slot->ActiveMulti = (BOOLEAN)(Count > 1);
    return SRB_STATUS_PENDING;
}

/* Complete the transfer in flight if the controller is done with it.
   Returns 1 when something was completed. */
ULONG
MmcServiceSlot(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN BOOLEAN FromInterrupt)
{
    PSCSI_REQUEST_BLOCK srb = Slot->Outstanding;
    ULONG st, result = MMC_OK;

    (VOID)FromInterrupt;
    if (srb == NULL) return 0;

    st = MMC_RD(Slot, SDHCI_INT_STATUS);
    if (st == 0xFFFFFFFF) {
        result = MMC_ERR_DATA;
    } else if (!(st & (SDHCI_INT_DATA_END | SDHCI_INT_ERROR))) {
        return 0;
    } else if (st & SDHCI_INT_ERROR) {
        result = MMC_ERR_DATA;
    }
    MMC_WR(Slot, SDHCI_SIGNAL_ENABLE, 0);

    if (result != MMC_OK) {
        Slot->LastIntStatus = st;
        MMC_DBG((MMC_PFX "Slot %lu: %s failed, status %08lX\n", (ULONG)Slot->Index,
                 Slot->ActiveWrite ? "write" : "read", st));
        if (++Slot->DmaErrors >= MMC_DMA_ERROR_LIMIT) Slot->Xfer = MMC_XFER_PIO;
        MmcRecover(HwInit, Slot, TRUE);
    } else {
        MMC_WR(Slot, SDHCI_INT_STATUS, st & (SDHCI_INT_DATA_END | SDHCI_INT_DMA_END | SDHCI_INT_RESPONSE));
        Slot->DmaErrors = 0;
    }

    Slot->Outstanding = NULL;
    MmcSatlCompleteAsync(HwInit, Slot, srb, result);

    if (Slot->Pending != NULL) {
        PSCSI_REQUEST_BLOCK next = Slot->Pending;
        Slot->Pending = NULL;
        MmcSatlStartPending(HwInit, Slot, next);
    }
    return 1;
}

/* Poll until the transfer in flight is finished */
VOID
MmcDrainSlot(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG elapsed = 0;

#if MMC_HAS_CQ
    MmcCqHalt(HwInit, Slot);
#endif
    while (Slot->Outstanding != NULL) {
        MmcServiceSlot(HwInit, Slot, FALSE);
        if (Slot->Outstanding == NULL) break;
        if (elapsed >= MMC_POLL_TIMEOUT_MAX_SEC * 1000000UL) {
            MMC_DBG((MMC_PFX "Slot %lu: transfer did not finish\n", (ULONG)Slot->Index));
            MmcFailOutstanding(HwInit, Slot, SRB_STATUS_BUS_RESET);
            MmcRecover(HwInit, Slot, TRUE);
            break;
        }
        ScsiPortStallExecution(MMC_POLL_INTERVAL_USEC);
        elapsed += MMC_POLL_INTERVAL_USEC;
    }
}

VOID
MmcFailOutstanding(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN UCHAR SrbStatus)
{
    PSCSI_REQUEST_BLOCK srb;

    if (Slot->Outstanding != NULL) MMC_WR(Slot, SDHCI_SIGNAL_ENABLE, 0);
    srb = Slot->Outstanding;
    Slot->Outstanding = NULL;
    if (srb != NULL) {
        srb->SrbStatus = SrbStatus;
        ScsiPortNotification(RequestComplete, HwInit, srb);
    }
    srb = Slot->Pending;
    Slot->Pending = NULL;
    if (srb != NULL) {
        srb->SrbStatus = SrbStatus;
        ScsiPortNotification(RequestComplete, HwInit, srb);
    }
}

/* An interrupt with nothing in flight: acknowledge what the slot signals */
BOOLEAN
MmcAckStray(IN PMMC_SLOT Slot)
{
    ULONG sig = MMC_RD(Slot, SDHCI_SIGNAL_ENABLE);
    ULONG st = MMC_RD(Slot, SDHCI_INT_STATUS);

    if (st == 0xFFFFFFFF || (st & sig) == 0) return FALSE;
    MMC_WR(Slot, SDHCI_INT_STATUS, st & sig);
    MMC_WR(Slot, SDHCI_SIGNAL_ENABLE, 0);
    return TRUE;
}
