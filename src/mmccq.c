#include "mmcsd.h"

/* mmccq.c - eMMC 5.1 command queuing through the command queuing engine
   (CQHCI) of the host: up to MMC_CQ_DEPTH reads and writes are in flight
   per card. On request only (CQ=1, or CQ=<register offset>), and only
   with interrupts. Anything else than a queued read or write halts the
   engine and turns command queue mode of the card off first; the next
   read or write turns both on again. An error fails the tasks in flight
   with a bus reset (the class driver retries them), turns command
   queuing off for good and sets the card up again. */

#if MMC_HAS_CQ

#define MMC_CQ_SDHCI_INTS       (SDHCI_INT_CQE | SDHCI_INT_CMD_ERRORS | SDHCI_INT_DATA_ERRORS)
#define MMC_CQ_HALT_TIMEOUT_US  100000

static ULONG
MmcCqTaskLength(IN PMMC_SLOT Slot)
{
    return Slot->CqWide ? 16 : 8;
}

/* Task descriptor list entry: the task descriptor, then the link to the
   transfer descriptors of the task */
static PULONG
MmcCqTaskDescriptor(IN PMMC_SLOT Slot, IN ULONG Tag)
{
    return (PULONG)(Slot->CqTdl + Tag * 2 * MmcCqTaskLength(Slot));
}

static BOOLEAN
MmcCqWaitCtl(IN PMMC_SLOT Slot, IN ULONG Mask, IN ULONG Want)
{
    ULONG elapsed;

    for (elapsed = 0; elapsed < MMC_CQ_HALT_TIMEOUT_US; elapsed += 10) {
        if ((MMC_CQ_RD(Slot, CQHCI_CTL) & Mask) == Want) return TRUE;
        ScsiPortStallExecution(10);
    }
    return FALSE;
}

/* The engine halted and off, the slot back to ordinary SDHCI interrupts */
static VOID
MmcCqEngineOff(IN PMMC_SLOT Slot)
{
    if (!(MMC_CQ_RD(Slot, CQHCI_CTL) & CQHCI_CTL_HALT)) {
        MMC_CQ_WR(Slot, CQHCI_CTL, CQHCI_CTL_HALT);
        if (!MmcCqWaitCtl(Slot, CQHCI_CTL_HALT, CQHCI_CTL_HALT)) {
            MMC_DBG((MMC_PFX "Slot %lu: command queuing engine did not halt\n", (ULONG)Slot->Index));
        }
    }
    MMC_CQ_WR(Slot, CQHCI_ISTE, 0);
    MMC_CQ_WR(Slot, CQHCI_ISGE, 0);
    MMC_CQ_WR(Slot, CQHCI_CFG, MMC_CQ_RD(Slot, CQHCI_CFG) & ~CQHCI_CFG_ENABLE);
    MMC_CQ_WR(Slot, CQHCI_IS, MMC_CQ_RD(Slot, CQHCI_IS));
    MMC_CQ_WR(Slot, CQHCI_TCN, MMC_CQ_RD(Slot, CQHCI_TCN));
    MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);
    MMC_WR(Slot, SDHCI_INT_ENABLE, SDHCI_INT_ENABLE_MASK);
    MMC_WR(Slot, SDHCI_SIGNAL_ENABLE, 0);
    Slot->CqHalted = TRUE;
}

/* Registers of the engine at the offset given with CQ=; CQHCI 5.x only */
VOID
MmcCqProbe(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG ver;

    Slot->CqRegs = NULL;
    if (!HwInit->CqAllowed) return;
    Slot->CqRegs = Slot->Regs + HwInit->CqOffset;
    ver = MMC_CQ_RD(Slot, CQHCI_VER);
    if (ver == 0xFFFFFFFF || ((ver >> 8) & 0x0F) != 5) {
        MMC_DBG((MMC_PFX "Slot %lu: no command queuing engine at +%03lX (version %08lX)\n",
                 (ULONG)Slot->Index, HwInit->CqOffset, ver));
        Slot->CqRegs = NULL;
        return;
    }
    MMC_DBG((MMC_PFX "Slot %lu: command queuing engine %lu.%lu at +%03lX, capabilities %08lX\n",
             (ULONG)Slot->Index, (ver >> 8) & 0x0F, (ver >> 4) & 0x0F, HwInit->CqOffset,
             MMC_CQ_RD(Slot, CQHCI_CAP)));
    Slot->CqHalted = FALSE;
    MmcCqEngineOff(Slot);
}

/* Card set up from the start: the engine off, nothing queued */
VOID
MmcCqReset(IN PMMC_SLOT Slot)
{
    Slot->CqActive = FALSE;
    Slot->CqCardOn = FALSE;
    if (Slot->CqRegs != NULL && !Slot->CqHalted) MmcCqEngineOff(Slot);
    Slot->CqHalted = TRUE;
}

/* End of the eMMC setup: command queuing used from the next read or
   write on, when card, host and settings allow it */
VOID
MmcCqStart(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN ULONG CardDepth)
{
    Slot->CqActive = FALSE;
    if (Slot->CqRegs == NULL || Slot->NoCq || Slot->CqTdl == NULL) return;
    if (CardDepth == 0) {
        MMC_DBG((MMC_PFX "Slot %lu: the card has no command queue\n", (ULONG)Slot->Index));
        return;
    }
    if (!HwInit->AsyncMode || HwInit->DumpMode || Slot->Xfer != MMC_XFER_ADMA2 || !Slot->BlockAddressing) {
        MMC_DBG((MMC_PFX "Slot %lu: command queuing needs interrupts, ADMA2 and sector addressing\n", (ULONG)Slot->Index));
        return;
    }
    Slot->CqDepth = (UCHAR)((CardDepth < MMC_CQ_DEPTH) ? CardDepth : MMC_CQ_DEPTH);
    Slot->CqWide = Slot->Adma64;
    Slot->CqBusy = 0;
    Slot->CqActive = TRUE;
    MMC_DBG((MMC_PFX "Slot %lu: command queuing, %lu of %lu tasks, %s descriptors\n", (ULONG)Slot->Index,
             (ULONG)Slot->CqDepth, CardDepth, Slot->CqWide ? "128-bit" : "64-bit"));
}

/* Command queuing given up on the card */
static VOID
MmcCqGiveUp(IN PMMC_SLOT Slot, IN PCHAR Why)
{
    MMC_DBG((MMC_PFX "Slot %lu: command queuing off: %s\n", (ULONG)Slot->Index, Why));
    (VOID)Why;
    Slot->NoCq = TRUE;
    Slot->CqActive = FALSE;
}

/* A request the engine takes */
BOOLEAN
MmcCqWanted(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    if (!Slot->CqActive || Srb->Function != SRB_FUNCTION_EXECUTE_SCSI) return FALSE;
    if (!HwInit->AsyncMode || HwInit->DumpMode || HwInit->Stopped || HwInit->CheckState) return FALSE;
    if (!Slot->CardReady || Slot->NeedsInit || Srb->Lun != 0) return FALSE;
    switch (Srb->Cdb[0]) {
    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
    case SCSIOP_READ:
    case SCSIOP_WRITE:
    case MMC_SCSIOP_READ12:
    case MMC_SCSIOP_WRITE12:
    case MMC_SCSIOP_READ16:
    case MMC_SCSIOP_WRITE16:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Card in command queue mode, engine on and running */
static BOOLEAN
MmcCqResume(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG cfg, v;

    if (!Slot->CqCardOn) {
        if (!MmcWaitIdle(Slot, SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT, MMC_BUSY_TIMEOUT_MS)) return FALSE;
        if (MmcSwitch(HwInit, Slot, EXT_CSD_CMDQ_MODE_EN, 1, 1000) != MMC_OK) {
            MmcCqGiveUp(Slot, "the card did not take CMDQ_MODE_EN");
            return FALSE;
        }
        Slot->CqCardOn = TRUE;
    }
    if (!Slot->CqHalted) return TRUE;
    if (!MmcWaitIdle(Slot, SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT, MMC_BUSY_TIMEOUT_MS)) return FALSE;

    /* SDHCI side: ADMA2, 512-byte blocks, only the engine's interrupts */
    v = MMC_RD(Slot, SDHCI_HOST_CONTROL) & ~(ULONG)SDHCI_CTRL_DMA_MASK;
    MMC_WR(Slot, SDHCI_HOST_CONTROL, v | (Slot->CqWide ? SDHCI_CTRL_ADMA64 : SDHCI_CTRL_ADMA32));
    MMC_WR(Slot, SDHCI_BLOCK_SIZE, MMC_BLOCK_SIZE | (7UL << 12));
    MMC_WR(Slot, SDHCI_INT_STATUS, 0xFFFFFFFF);
    MMC_WR(Slot, SDHCI_INT_ENABLE, MMC_CQ_SDHCI_INTS);
    MMC_WR(Slot, SDHCI_SIGNAL_ENABLE, MMC_CQ_SDHCI_INTS);

    /* Engine: configured while off, then on and out of halt */
    cfg = MMC_CQ_RD(Slot, CQHCI_CFG);
    if (cfg & CQHCI_CFG_ENABLE) {
        cfg &= ~CQHCI_CFG_ENABLE;
        MMC_CQ_WR(Slot, CQHCI_CFG, cfg);
    }
    cfg &= ~(CQHCI_CFG_TASK_DESC_128 | 0x00001000);
    if (Slot->CqWide) cfg |= CQHCI_CFG_TASK_DESC_128;
    MMC_CQ_WR(Slot, CQHCI_CFG, cfg);
    MMC_CQ_WR(Slot, CQHCI_TDLBA, Slot->CqTdlPhysical);
    MMC_CQ_WR(Slot, CQHCI_TDLBAU, Slot->CqTdlPhysicalHigh);
    MMC_CQ_WR(Slot, CQHCI_SSC2, Slot->Rca);
    MMC_CQ_WR(Slot, CQHCI_IS, MMC_CQ_RD(Slot, CQHCI_IS));
    MMC_CQ_WR(Slot, CQHCI_TCN, MMC_CQ_RD(Slot, CQHCI_TCN));
    MMC_CQ_WR(Slot, CQHCI_ISTE, CQHCI_IS_MASK);
    MMC_CQ_WR(Slot, CQHCI_ISGE, CQHCI_IS_MASK);
    MMC_CQ_WR(Slot, CQHCI_CFG, cfg | CQHCI_CFG_ENABLE);
    Slot->CqHalted = FALSE;
    if (MMC_CQ_RD(Slot, CQHCI_CTL) & CQHCI_CTL_HALT) MMC_CQ_WR(Slot, CQHCI_CTL, 0);
    if (!MmcCqWaitCtl(Slot, CQHCI_CTL_HALT, 0)) {
        MmcCqEngineOff(Slot);
        MmcCqGiveUp(Slot, "the engine stays halted");
        return FALSE;
    }
    return TRUE;
}

/* Wait for the tasks in flight, halt the engine, card out of command
   queue mode: ordinary commands can go to the card */
VOID
MmcCqHalt(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG elapsed = 0;

    if (Slot->CqRegs == NULL || (Slot->CqHalted && !Slot->CqCardOn && Slot->CqBusy == 0)) return;
    while (Slot->CqBusy != 0) {
        MmcCqService(HwInit, Slot);
        if (Slot->CqBusy == 0) break;
        if (elapsed >= MMC_POLL_TIMEOUT_MAX_SEC * 1000000UL) {
            MMC_DBG((MMC_PFX "Slot %lu: queued tasks did not finish\n", (ULONG)Slot->Index));
            MmcCqFail(HwInit, Slot, SRB_STATUS_BUS_RESET);
            return;
        }
        ScsiPortStallExecution(MMC_POLL_INTERVAL_USEC);
        elapsed += MMC_POLL_INTERVAL_USEC;
    }
    if (!Slot->CqHalted) MmcCqEngineOff(Slot);
    if (Slot->CqCardOn) {
        Slot->CqCardOn = FALSE;
        if (MmcSwitch(HwInit, Slot, EXT_CSD_CMDQ_MODE_EN, 0, 1000) != MMC_OK) {
            MmcCqGiveUp(Slot, "CMDQ_MODE_EN not cleared");
            if (Slot->CardType != MMC_CARD_NONE) Slot->NeedsInit = TRUE;
        }
    }
}

/* Transfer descriptors of a task for the request's buffer: 8 bytes, or
   12 with 64-bit addresses (the ADMA2 layout of the slot) */
static BOOLEAN
MmcCqBuildTransfer(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb,
                   IN ULONG Tag, IN ULONG Bytes)
{
    PULONG d = (PULONG)Slot->CqTrd[Tag];
    ULONG left = Bytes, index = 0, segLen, toPage, step = Slot->CqWide ? 3 : 2;
    PUCHAR virt = (PUCHAR)Srb->DataBuffer;
    SCSI_PHYSICAL_ADDRESS phys;

    while (left > 0 && index < MMC_CQ_TRD_ENTRIES) {
        phys = ScsiPortGetPhysicalAddress(HwInit, Srb, virt, &segLen);
        if ((phys.LowPart == 0 && phys.HighPart == 0) || (phys.LowPart & 3) ||
            (phys.HighPart != 0 && !Slot->CqWide)) {
            return FALSE;
        }
        if (segLen > left) segLen = left;
        toPage = 4096 - (phys.LowPart & 0xFFF);
        if (segLen > toPage) segLen = toPage;
        segLen &= ~3UL;
        if (segLen == 0) return FALSE;
        d[index * step] = CQHCI_DESC_VALID | CQHCI_ACT_TRAN | ((segLen & 0xFFFF) << 16);
        d[index * step + 1] = phys.LowPart;
        if (Slot->CqWide) d[index * step + 2] = (ULONG)phys.HighPart;
        virt += segLen;
        left -= segLen;
        index++;
    }
    if (left > 0 || index == 0) return FALSE;
    d[(index - 1) * step] |= CQHCI_DESC_END;
    return TRUE;
}

static ULONG
MmcCqCount(IN ULONG Bits)
{
    ULONG n = 0;
    while (Bits != 0) {
        n += Bits & 1;
        Bits >>= 1;
    }
    return n;
}

/* A read or write as a task. SRB_STATUS_PENDING; SRB_STATUS_BUSY when
   every task is in flight; SRB_STATUS_INVALID_REQUEST: the caller halts
   the engine and does it the ordinary way. */
UCHAR
MmcCqIssue(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb,
           IN MMC_LBA Lba, IN ULONG Count, IN BOOLEAN IsWrite, IN BOOLEAN Fua)
{
    PULONG td;
    ULONG tag, bit, attr;

    if (!MmcCqWanted(HwInit, Slot, Srb) || Slot->Outstanding != NULL || Slot->Xfer != MMC_XFER_ADMA2) {
        return SRB_STATUS_INVALID_REQUEST;
    }
    if (Lba.High != 0 || Count == 0 || Count > MMC_MAX_TRANSFER / MMC_BLOCK_SIZE || Srb->DataBuffer == NULL) {
        return SRB_STATUS_INVALID_REQUEST;
    }
    for (tag = 0; tag < Slot->CqDepth; tag++) {
        if (!(Slot->CqBusy & (1UL << tag))) break;
    }
    if (tag >= Slot->CqDepth) return SRB_STATUS_BUSY;
    bit = 1UL << tag;
    if (!MmcCqBuildTransfer(HwInit, Slot, Srb, tag, Count * MMC_BLOCK_SIZE)) return SRB_STATUS_INVALID_REQUEST;
    if (!MmcCqResume(HwInit, Slot)) return SRB_STATUS_INVALID_REQUEST;

    /* Task: block count and address; forced programming for FUA */
    attr = CQHCI_DESC_VALID | CQHCI_DESC_END | CQHCI_DESC_INT | CQHCI_ACT_TASK | (Count << 16);
    if (!IsWrite) attr |= CQHCI_TASK_READ;
    if (IsWrite && Fua) attr |= CQHCI_TASK_FORCED_PROG;
    td = MmcCqTaskDescriptor(Slot, tag);
    td[0] = attr;
    td[1] = Lba.Low;
    if (Slot->CqWide) {
        td[2] = 0;
        td[3] = 0;
    }
    /* Link to the transfer descriptors */
    td += MmcCqTaskLength(Slot) / 4;
    td[0] = CQHCI_DESC_VALID | CQHCI_ACT_LINK;
    td[1] = Slot->CqTrdPhysical[tag];
    if (Slot->CqWide) {
        td[2] = Slot->CqTrdPhysicalHigh[tag];
        td[3] = 0;
    }

    Slot->CqSrb[tag] = Srb;
    Slot->CqBusy |= bit;
    MMC_CQ_WR(Slot, CQHCI_TDBR, bit);
    tag = MmcCqCount(Slot->CqBusy);
    if (tag > Slot->CqPeak) Slot->CqPeak = (UCHAR)tag;
    if (tag < Slot->CqDepth) {
        ScsiPortNotification(NextLuRequest, HwInit, Srb->PathId, Srb->TargetId, Srb->Lun);
    }
    return SRB_STATUS_PENDING;
}

/* Completed tasks, errors. 0: nothing for the engine. */
ULONG
MmcCqService(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot)
{
    ULONG st, is, done, tag, completed = 0;
    PSCSI_REQUEST_BLOCK srb;

    if (Slot->CqRegs == NULL || Slot->CqHalted) return 0;
    st = MMC_RD(Slot, SDHCI_INT_STATUS);
    is = MMC_CQ_RD(Slot, CQHCI_IS);
    if (st == 0xFFFFFFFF || is == 0xFFFFFFFF) {
        MmcCqFail(HwInit, Slot, SRB_STATUS_BUS_RESET);
        return 1;
    }
    if (st & (SDHCI_INT_CQE | SDHCI_INT_ALL_ERRORS)) MMC_WR(Slot, SDHCI_INT_STATUS, st & (SDHCI_INT_CQE | SDHCI_INT_ALL_ERRORS));
    if (is != 0) MMC_CQ_WR(Slot, CQHCI_IS, is);
    if ((st & SDHCI_INT_ALL_ERRORS) || (is & CQHCI_IS_ERRORS)) {
        MMC_DBG((MMC_PFX "Slot %lu: queued task error, SDHCI %08lX, CQHCI %08lX\n", (ULONG)Slot->Index, st, is));
        Slot->LastIntStatus = st;
        MmcCqFail(HwInit, Slot, SRB_STATUS_BUS_RESET);
        return 1;
    }
    done = MMC_CQ_RD(Slot, CQHCI_TCN);
    if (done != 0) MMC_CQ_WR(Slot, CQHCI_TCN, done);
    done &= Slot->CqBusy;
    for (tag = 0; done != 0 && tag < MMC_CQ_DEPTH; tag++) {
        if (!(done & (1UL << tag))) continue;
        done &= ~(1UL << tag);
        srb = Slot->CqSrb[tag];
        Slot->CqSrb[tag] = NULL;
        Slot->CqBusy &= ~(1UL << tag);
        if (srb != NULL) {
            srb->SrbStatus = SRB_STATUS_SUCCESS;
            srb->ScsiStatus = SCSISTAT_GOOD;
            ScsiPortNotification(RequestComplete, HwInit, srb);
        }
        completed++;
    }
    if (completed != 0) ScsiPortNotification(NextLuRequest, HwInit, 0, Slot->Index, 0);
    return (completed != 0 || is != 0 || (st & SDHCI_INT_CQE)) ? 1 : 0;
}

/* Error or reset: every task fails with SrbStatus, the engine is cleared
   and off, command queuing is not used again, the card is set up again */
VOID
MmcCqFail(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN UCHAR SrbStatus)
{
    PSCSI_REQUEST_BLOCK srb;
    ULONG tag;

    if (Slot->CqRegs == NULL) return;
    if (!Slot->CqHalted) {
        MMC_CQ_WR(Slot, CQHCI_CTL, CQHCI_CTL_HALT);
        MmcCqWaitCtl(Slot, CQHCI_CTL_HALT, CQHCI_CTL_HALT);
        MMC_CQ_WR(Slot, CQHCI_CTL, CQHCI_CTL_HALT | CQHCI_CTL_CLEAR_ALL);
        MmcCqWaitCtl(Slot, CQHCI_CTL_CLEAR_ALL, 0);
        MmcCqEngineOff(Slot);
        MmcRecover(HwInit, Slot, FALSE);
    }
    for (tag = 0; tag < MMC_CQ_DEPTH; tag++) {
        srb = Slot->CqSrb[tag];
        Slot->CqSrb[tag] = NULL;
        if (srb != NULL) {
            srb->SrbStatus = SrbStatus;
            ScsiPortNotification(RequestComplete, HwInit, srb);
        }
    }
    MmcCqGiveUp(Slot, "error or reset");
    Slot->CqBusy = 0;
    Slot->CqCardOn = FALSE;
    Slot->CqHalted = TRUE;
    if (Slot->CardType != MMC_CARD_NONE) Slot->NeedsInit = TRUE;
}

#endif /* MMC_HAS_CQ */
