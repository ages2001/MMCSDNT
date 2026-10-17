#include "mmcsd.h"

/* mmcmain.c - DriverEntry, controller discovery (PCI class 08:05, ACPI),
   initialization, ScsiPort entry points, interrupt watchdog, power */

BOOLEAN MmcHwInitialize(IN PVOID DeviceExtension);
BOOLEAN MmcStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN MmcInterrupt(IN PVOID DeviceExtension);
BOOLEAN MmcResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId);
ULONG   MmcFindAdapter(IN PVOID DeviceExtension, IN PVOID Context, IN PVOID BusInformation,
                       IN PCHAR ArgumentString, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                       OUT PBOOLEAN Again);
VOID    MmcTimer(IN PVOID DeviceExtension);
#ifdef MMC_W9X
BOOLEAN MmcAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState);
#endif
#if !defined(MMC_NT4)
SCSI_ADAPTER_CONTROL_STATUS MmcAdapterControl(IN PVOID DeviceExtension,
                                              IN SCSI_ADAPTER_CONTROL_TYPE ControlType,
                                              IN PVOID Parameters);
#endif

/* Crash dump / hibernation copy of the driver (no driver object):
   polled, no registry access */
static BOOLEAN g_MmcDumpMode;

/* ------------------------------------------------------------------ */
/* Windows 9x: VMM services                                            */
/* ------------------------------------------------------------------ */

#ifdef MMC_W9X

#define VMM_DEVICE_ID                   0x0001
#define Debug_Printf_Service_Ordinal    0x012D

/* _MapPhysToLinear(PhysAddr, nBytes, flags), VMM service 006Ch; the 9x
   ScsiPortGetDeviceBase does not map memory reliably (KB Q169584) */
#ifdef __GNUC__
static PVOID
MmcMapPhysToLinear(ULONG PhysAddr, ULONG Bytes)
{
    PVOID linear;

    __asm__ __volatile__(
        "pushl $0\n\t"
        "pushl %2\n\t"
        "pushl %1\n\t"
        "int $0x20\n\t"
        ".word 0x006C\n\t"
        ".word 0x0001\n\t"
        "addl $12, %%esp"
        : "=a" (linear)
        : "r" (PhysAddr), "r" (Bytes)
        : "ecx", "edx", "memory", "cc");
    return linear;
}
#else
static PVOID
MmcMapPhysToLinear(ULONG PhysAddr, ULONG Bytes)
{
    PVOID linear;

    __asm push 0
    __asm push Bytes
    __asm push PhysAddr
    __asm int  0x20
    __asm _emit 0x6C
    __asm _emit 0x00
    __asm _emit ((VMM_DEVICE_ID >> 0) & 0xFF)
    __asm _emit ((VMM_DEVICE_ID >> 8) & 0xFF)
    __asm add  esp, 3*4
    __asm mov  linear, eax
    return linear;
}
#endif

#ifdef MMC_DBG_ENABLED
/* _Debug_Printf_Service(format, argptr): WDEB386, SoftICE or DebugView */
VOID __cdecl
MmcDbgPrint(PCHAR Format, ...)
{
    __asm lea  eax, (Format + 4)
    __asm push eax
    __asm push Format
    __asm int  0x20
    __asm _emit ((Debug_Printf_Service_Ordinal >> 0) & 0xFF)
    __asm _emit ((Debug_Printf_Service_Ordinal >> 8) & 0xFF)
    __asm _emit ((VMM_DEVICE_ID >> 0) & 0xFF)
    __asm _emit ((VMM_DEVICE_ID >> 8) & 0xFF)
    __asm add  esp, 2*4
}
#endif

/* Required by SCSIPORT.PDR. After a "restore" the BIOS may have used the
   controller (APM resume): the cards are set up again. */
BOOLEAN
MmcAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState)
{
    (VOID)Context;
    MMC_DBG((MMC_PFX "AdapterState: %s\n", SaveState ? "save" : "restore"));
    if (!SaveState) ((PHW_DEVICE_EXTENSION)DeviceExtension)->CheckState = TRUE;
    return TRUE;
}

#endif /* MMC_W9X */

/* Physical memory for the ACPI tables (mmcacpi.c) */
#if defined(MMC_NT4)
PUCHAR
MmcMapPhys(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Physical, IN ULONG Length)
{
#ifdef MMC_W9X
    PVOID p;
    (VOID)HwInit;
    p = MmcMapPhysToLinear(Physical, Length);
    return (p == (PVOID)0xFFFFFFFF) ? NULL : (PUCHAR)p;
#else
    SCSI_PHYSICAL_ADDRESS pa;
    pa.HighPart = 0;
    pa.LowPart = Physical;
    return (PUCHAR)ScsiPortGetDeviceBase(HwInit, Isa, 0, pa, Length, FALSE);
#endif
}

VOID
MmcUnmapPhys(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR Virtual)
{
#ifdef MMC_W9X
    (VOID)HwInit;
    (VOID)Virtual;
#else
    if (Virtual != NULL) ScsiPortFreeDeviceBase(HwInit, Virtual);
#endif
}
#endif

/* ------------------------------------------------------------------ */
/* Claim table: controllers by the physical address of their first slot */
/* ------------------------------------------------------------------ */

#define MMC_MAX_CLAIMED                 (MMC_MAX_CONTROLLERS * 2)

static ULONG g_MmcClaimed[MMC_MAX_CLAIMED];
static ULONG g_MmcClaimedCount;

static BOOLEAN
MmcIsClaimed(IN ULONG Base)
{
    ULONG i;
    for (i = 0; i < g_MmcClaimedCount; i++) if (g_MmcClaimed[i] == Base) return TRUE;
    return FALSE;
}

static VOID
MmcClaim(IN ULONG Base)
{
    if (!MmcIsClaimed(Base) && g_MmcClaimedCount < MMC_MAX_CLAIMED) g_MmcClaimed[g_MmcClaimedCount++] = Base;
}

/* ------------------------------------------------------------------ */
/* PCI configuration access                                            */
/* ------------------------------------------------------------------ */

#define MMC_PCI_HEADER_LENGTH           68          /* through the slot information register */
#define MMC_PCI_MULTIFUNCTION           0x80

#ifdef MMC_NT31_COMPAT
/* Configuration mechanism #1 through the I/O ports, for a HAL without PCI
   support (NT 3.1) */
#define MMC_PCI_CONFIG_ADDRESS          0xCF8
#define MMC_PCI_CONFIG_DATA             0xCFC
#define MMC_PCI_CONFIG_ENABLE           0x80000000UL

static BOOLEAN g_MmcPciProbed;
static BOOLEAN g_MmcPciViaPorts;

static VOID
MmcPciSelect(IN ULONG Bus, IN ULONG Slot, IN ULONG Offset)
{
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = Slot;
    ScsiPortWritePortUlong((PULONG)MMC_PCI_CONFIG_ADDRESS,
                           MMC_PCI_CONFIG_ENABLE | ((Bus & 0xFF) << 16) |
                           ((ULONG)slot.u.bits.DeviceNumber << 11) |
                           ((ULONG)slot.u.bits.FunctionNumber << 8) | (Offset & 0xFC));
}

static ULONG
MmcPciReadPortDword(IN ULONG Bus, IN ULONG Slot, IN ULONG Offset)
{
    MmcPciSelect(Bus, Slot, Offset);
    return ScsiPortReadPortUlong((PULONG)MMC_PCI_CONFIG_DATA);
}

static VOID
MmcPciProbeAccess(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG id, save;

    if (g_MmcPciProbed) return;
    g_MmcPciProbed = TRUE;
    if (ScsiPortGetBusData(HwInit, PCIConfiguration, 0, 0, &id, sizeof(ULONG)) != 0) return;
    save = ScsiPortReadPortUlong((PULONG)MMC_PCI_CONFIG_ADDRESS);
    ScsiPortWritePortUlong((PULONG)MMC_PCI_CONFIG_ADDRESS, MMC_PCI_CONFIG_ENABLE);
    if (ScsiPortReadPortUlong((PULONG)MMC_PCI_CONFIG_ADDRESS) == MMC_PCI_CONFIG_ENABLE) g_MmcPciViaPorts = TRUE;
    ScsiPortWritePortUlong((PULONG)MMC_PCI_CONFIG_ADDRESS, save);
    MMC_DBG((MMC_PFX "PCI: no HAL config access, mechanism #1 %s\n", g_MmcPciViaPorts ? "used" : "not found"));
}
#endif

ULONG
MmcPciReadConfig(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Offset, IN ULONG Width)
{
    UCHAR buffer[256];
    ULONG value = 0;

    if (!HwInit->HasPci || Offset + Width > sizeof(buffer)) return 0;
#ifdef MMC_NT31_COMPAT
    if (g_MmcPciViaPorts) {
        ULONG dw = MmcPciReadPortDword(HwInit->PciBus, HwInit->PciSlot, Offset & ~3UL);
        dw >>= 8 * (Offset & 3);
        return (Width == 1) ? (dw & 0xFF) : (Width == 2) ? (dw & 0xFFFF) : dw;
    }
#endif
    if (ScsiPortGetBusData(HwInit, PCIConfiguration, HwInit->PciBus, HwInit->PciSlot,
                           buffer, Offset + Width) < Offset + Width) {
        return 0;
    }
    CopyMemoryBytes(&value, &buffer[Offset], Width);
    return value;
}

VOID
MmcPciWriteConfig(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Offset, IN ULONG Width, IN ULONG Value)
{
    if (!HwInit->HasPci) return;
#ifdef MMC_NT31_COMPAT
    MmcPciSelect(HwInit->PciBus, HwInit->PciSlot, Offset & ~3UL);
    if (Width == 1) {
        ScsiPortWritePortUchar((PUCHAR)(MMC_PCI_CONFIG_DATA + (Offset & 3)), (UCHAR)Value);
    } else if (Width == 2) {
        ScsiPortWritePortUshort((PUSHORT)(MMC_PCI_CONFIG_DATA + (Offset & 2)), (USHORT)Value);
    } else {
        ScsiPortWritePortUlong((PULONG)MMC_PCI_CONFIG_DATA, Value);
    }
#else
    ScsiPortSetBusDataByOffset(HwInit, PCIConfiguration, HwInit->PciBus, HwInit->PciSlot, &Value, Offset, Width);
#endif
}

static BOOLEAN
MmcReadPciHeader(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Bus, IN ULONG Slot,
                 OUT PPCI_COMMON_CONFIG PciConfig, OUT PBOOLEAN BusMissing)
{
    ULONG bytesRead;

#ifdef MMC_NT31_COMPAT
    if (g_MmcPciViaPorts) {
        PULONG dw = (PULONG)PciConfig;
        ULONG i;
        if (BusMissing != NULL) *BusMissing = FALSE;
        dw[0] = MmcPciReadPortDword(Bus, Slot, 0);
        if ((dw[0] & 0xFFFF) == 0xFFFF || (dw[0] & 0xFFFF) == 0) return FALSE;
        for (i = 1; i < MMC_PCI_HEADER_LENGTH / sizeof(ULONG); i++) dw[i] = MmcPciReadPortDword(Bus, Slot, i * 4);
        return TRUE;
    }
#endif
    bytesRead = ScsiPortGetBusData(HwInit, PCIConfiguration, Bus, Slot, PciConfig, MMC_PCI_HEADER_LENGTH);
    if (BusMissing != NULL) *BusMissing = (BOOLEAN)(bytesRead == 0);
    if (bytesRead < MMC_PCI_HEADER_LENGTH) return FALSE;
    if (PciConfig->VendorID == 0xFFFF || PciConfig->VendorID == 0x0000) return FALSE;
    return TRUE;
}

/* An SD host controller following the SDHCI register layout */
static BOOLEAN
MmcPciIsSdhci(IN PPCI_COMMON_CONFIG PciConfig)
{
    return (BOOLEAN)(PciConfig->BaseClass == PCI_CLASS_BASE_PERIPHERAL &&
                     PciConfig->SubClass == PCI_SUBCLASS_SD_HOST &&
                     (PciConfig->ProgIf == MMC_PROGIF_STANDARD || PciConfig->ProgIf == MMC_PROGIF_STANDARD_DMA) &&
                     (PciConfig->HeaderType & 0x7F) == 0);
}

/* Slot register blocks of a PCI SDHCI function: physical addresses */
static ULONG
MmcPciSlots(IN PPCI_COMMON_CONFIG PciConfig, OUT PULONG Bases)
{
    UCHAR info = PciConfig->DeviceSpecific[MMC_PCI_SLOT_INFO - 64];
    ULONG first = info & 0x07, count = ((info >> 4) & 0x07) + 1, n = 0, i, bar;

    if (first + count > PCI_TYPE0_ADDRESSES) count = PCI_TYPE0_ADDRESSES - first;
    for (i = 0; i < count && n < MMC_MAX_SLOTS; i++) {
        bar = PciConfig->u.type0.BaseAddresses[first + i];
        if ((bar & 1) || (bar & 0xFFFFFFF0) == 0) continue;     /* memory BARs only */
        if ((bar & 0x06) == 0x04) {                             /* 64-bit BAR: two entries */
            i++;
            /* placed above 4 GB by the firmware: not reachable here */
            if (first + i < PCI_TYPE0_ADDRESSES && PciConfig->u.type0.BaseAddresses[first + i] != 0) continue;
        }
        Bases[n++] = bar & 0xFFFFFFF0;
    }
    return n;
}

#if defined(MMC_NT4)
static BOOLEAN
MmcScanPci(IN PHW_DEVICE_EXTENSION HwInit, OUT PULONG FoundBus, OUT PULONG FoundSlot, OUT PPCI_COMMON_CONFIG PciConfig)
{
    PCI_SLOT_NUMBER slot;
    ULONG bus, device, function, bases[MMC_MAX_SLOTS];
    BOOLEAN busMissing, multiFunction;

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            multiFunction = FALSE;
            for (function = 0; function < 8; function++) {
                if (function > 0 && !multiFunction) break;
                slot.u.AsULONG = 0;
                slot.u.bits.DeviceNumber = device;
                slot.u.bits.FunctionNumber = function;
                if (!MmcReadPciHeader(HwInit, bus, slot.u.AsULONG, PciConfig, &busMissing)) {
                    if (busMissing && device == 0 && function == 0) return FALSE;
                    if (function == 0) break;
                    continue;
                }
                if (function == 0) multiFunction = (BOOLEAN)((PciConfig->HeaderType & MMC_PCI_MULTIFUNCTION) != 0);
                if (!MmcPciIsSdhci(PciConfig)) continue;
                if (MmcPciSlots(PciConfig, bases) == 0 || MmcIsClaimed(bases[0])) continue;
                *FoundBus = bus;
                *FoundSlot = slot.u.AsULONG;
                return TRUE;
            }
        }
    }
    return FALSE;
}
#endif

/* ------------------------------------------------------------------ */
/* DMA area: per slot an ADMA2 table and a scratch buffer              */
/* ------------------------------------------------------------------ */

/* Uncached extension: one page per slot for the scratch buffer, the
   ADMA2 tables (four slots per page), and for an SDMA-only controller the
   bounce buffer. The area is not physically contiguous on every system
   (SCSIPORT.PDR): every page gets its own physical address. */
#define MMC_ADMA_PAGES(slots)           (((slots) + MMC_ADMA_TABLES_PER_PAGE - 1) / MMC_ADMA_TABLES_PER_PAGE)
#define MMC_DMA_PAGES(slots, sdma)      (1 + (slots) + MMC_ADMA_PAGES(slots) + \
                                         ((sdma) ? MMC_SDMA_BOUNCE_SIZE / MMC_PAGE_SIZE : 0))
/* Command queuing: the task descriptor list page and two pages of
   transfer descriptors (1 KB per task) per slot */
#define MMC_CQ_PAGES                    3

/* Physical address of a page of the area; 0 when it is not usable: not
   page aligned, or above 4 GB without 64-bit ADMA2 */
static ULONG
MmcPagePhysical(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR Virtual, OUT PULONG High)
{
    SCSI_PHYSICAL_ADDRESS pa;
    ULONG length = MMC_PAGE_SIZE;

    pa = ScsiPortGetPhysicalAddress(HwInit, NULL, Virtual, &length);
    *High = 0;
    if ((pa.LowPart == 0 && pa.HighPart == 0) || (pa.LowPart & 0xFFF) != 0) return 0;
    if (pa.HighPart != 0) {
        if (!HwInit->Dma64) return 0;
        *High = (ULONG)pa.HighPart;
    }
    return pa.LowPart;
}

static BOOLEAN
MmcAllocateDma(IN PHW_DEVICE_EXTENSION HwInit, IN PPORT_CONFIGURATION_INFORMATION ConfigInfo, IN BOOLEAN Sdma)
{
    ULONG length, misalign, s, i, phys, high, page;
    PUCHAR virt, tables;

    if (HwInit->DmaArea != NULL) return TRUE;
    HwInit->DmaLength = MMC_DMA_PAGES(HwInit->SlotCount, Sdma) * MMC_PAGE_SIZE;
#if MMC_HAS_CQ
    if (HwInit->CqAllowed) HwInit->DmaLength += HwInit->SlotCount * MMC_CQ_PAGES * MMC_PAGE_SIZE;
#endif
    HwInit->DmaArea = (PUCHAR)ScsiPortGetUncachedExtension(HwInit, ConfigInfo, HwInit->DmaLength);
    if (HwInit->DmaArea == NULL) {
        MMC_DBG((MMC_PFX "AllocateDma: ScsiPortGetUncachedExtension(%lu) failed\n", HwInit->DmaLength));
        return FALSE;
    }
    length = HwInit->DmaLength;
    HwInit->DmaAreaPhysical = ScsiPortGetPhysicalAddress(HwInit, NULL, HwInit->DmaArea, &length);
    ZeroMemoryBytes(HwInit->DmaArea, HwInit->DmaLength);

    /* Page aligned in the virtual space; the first page is the slack */
    virt = HwInit->DmaArea;
    misalign = MMC_PTR_LOW(virt) & 0xFFF;
    if (misalign != 0) virt += MMC_PAGE_SIZE - misalign;

    /* A page without a physical address below 4 GB (a PAE or x64 system
       whose ScsiPort did not keep the area low): the slots run with PIO,
       which uses the same pages through their virtual addresses */
    for (s = 0; s < HwInit->SlotCount; s++) {
        PMMC_SLOT slot = &HwInit->Slots[s];
        phys = MmcPagePhysical(HwInit, virt, &high);
        if (phys == 0) HwInit->DmaAllowed = FALSE;
        slot->Scratch = virt;
        slot->ScratchPhysical = phys;
        slot->ScratchPhysicalHigh = high;
        virt += MMC_PAGE_SIZE;
    }
    tables = virt;
    phys = high = 0;
    for (s = 0; s < HwInit->SlotCount; s++) {
        PMMC_SLOT slot = &HwInit->Slots[s];
        page = s / MMC_ADMA_TABLES_PER_PAGE;
        if (s % MMC_ADMA_TABLES_PER_PAGE == 0) {
            phys = MmcPagePhysical(HwInit, tables + page * MMC_PAGE_SIZE, &high);
            if (phys == 0) HwInit->DmaAllowed = FALSE;
        }
        slot->Adma = (PULONG)(tables + page * MMC_PAGE_SIZE + (s % MMC_ADMA_TABLES_PER_PAGE) * MMC_ADMA_TABLE_SIZE);
        slot->AdmaPhysical = phys + (s % MMC_ADMA_TABLES_PER_PAGE) * MMC_ADMA_TABLE_SIZE;
        slot->AdmaPhysicalHigh = high;
    }
    MMC_DBG((MMC_PFX "AllocateDma: %lu bytes at %08lX:%08lX, scratch %08lX:%08lX, ADMA2 tables %08lX:%08lX%s\n",
             HwInit->DmaLength, (ULONG)HwInit->DmaAreaPhysical.HighPart, HwInit->DmaAreaPhysical.LowPart,
             HwInit->Slots[0].ScratchPhysicalHigh, HwInit->Slots[0].ScratchPhysical, HwInit->Slots[0].AdmaPhysicalHigh,
             HwInit->Slots[0].AdmaPhysical, HwInit->DmaAllowed ? "" : ", no DMA (above 4 GB)"));
    virt = tables + MMC_ADMA_PAGES(HwInit->SlotCount) * MMC_PAGE_SIZE;
#if MMC_HAS_CQ
    if (HwInit->CqAllowed) {
        for (s = 0; s < HwInit->SlotCount; s++) {
            PMMC_SLOT slot = &HwInit->Slots[s];
            ULONG t, p;
            BOOLEAN ok = TRUE;

            slot->CqTdl = virt;
            slot->CqTdlPhysical = MmcPagePhysical(HwInit, virt, &slot->CqTdlPhysicalHigh);
            if (slot->CqTdlPhysical == 0) ok = FALSE;
            for (t = 0; t < MMC_CQ_DEPTH; t++) {
                p = t / (MMC_PAGE_SIZE / 1024);
                slot->CqTrd[t] = virt + (1 + p) * MMC_PAGE_SIZE + (t % (MMC_PAGE_SIZE / 1024)) * 1024;
                if (t % (MMC_PAGE_SIZE / 1024) == 0) {
                    phys = MmcPagePhysical(HwInit, virt + (1 + p) * MMC_PAGE_SIZE, &high);
                    if (phys == 0) ok = FALSE;
                }
                slot->CqTrdPhysical[t] = phys + (t % (MMC_PAGE_SIZE / 1024)) * 1024;
                slot->CqTrdPhysicalHigh[t] = high;
            }
            if (!ok || !HwInit->DmaAllowed) slot->CqTdl = NULL;
            virt += MMC_CQ_PAGES * MMC_PAGE_SIZE;
        }
    }
#endif
    if (Sdma && HwInit->DmaAllowed) {
        /* An SDMA transfer goes to one physical range: as many bounce pages
           as follow each other physically, without crossing a 512 KB
           address boundary (the SDMA buffer boundary used) */
        HwInit->SdmaBouncePhysical = MmcPagePhysical(HwInit, virt, &high);
        if (HwInit->SdmaBouncePhysical != 0) {
            HwInit->SdmaBounce = virt;
            HwInit->SdmaRun = MMC_PAGE_SIZE;
            for (i = 1; i < MMC_SDMA_BOUNCE_SIZE / MMC_PAGE_SIZE; i++) {
                phys = MmcPagePhysical(HwInit, virt + i * MMC_PAGE_SIZE, &high);
                if (phys != HwInit->SdmaBouncePhysical + i * MMC_PAGE_SIZE ||
                    (phys >> 19) != (HwInit->SdmaBouncePhysical >> 19)) {
                    break;
                }
                HwInit->SdmaRun += MMC_PAGE_SIZE;
            }
            MMC_DBG((MMC_PFX "AllocateDma: SDMA bounce %08lX, %lu bytes per transfer\n",
                     HwInit->SdmaBouncePhysical, HwInit->SdmaRun));
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

static VOID
MmcInitializeAll(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG s;

    for (s = 0; s < HwInit->SlotCount; s++) {
        PMMC_SLOT slot = &HwInit->Slots[s];
        if (!slot->Present) continue;
        slot->Outstanding = NULL;
        slot->Pending = NULL;
        if (!MmcCardInit(HwInit, slot)) {
            /* No card, or it did not answer: the slot stays empty */
            slot->NeedsInit = FALSE;
        }
        slot->Async = (BOOLEAN)(HwInit->AsyncMode && slot->Xfer == MMC_XFER_ADMA2);
    }
    HwInit->Stopped = FALSE;
    HwInit->CheckState = FALSE;
}

/* After a power transition: everything set up again. The cache setting
   wanted through MODE SELECT is kept per slot. */
BOOLEAN
MmcReinitialize(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG s;

    MMC_DBG((MMC_PFX "Reinitialize\n"));
    for (s = 0; s < HwInit->SlotCount; s++) {
        PMMC_SLOT slot = &HwInit->Slots[s];
        if (slot->Outstanding != NULL || slot->Pending != NULL) MmcFailOutstanding(HwInit, slot, SRB_STATUS_BUS_RESET);
#if MMC_HAS_CQ
        if (slot->CqBusy != 0) MmcCqFail(HwInit, slot, SRB_STATUS_BUS_RESET);
#endif
    }
    if (HwInit->HasPci) {
        ULONG command = MmcPciReadConfig(HwInit, 0x04, 2);
        if (command != 0xFFFF && (command & 0x0006) != 0x0006) MmcPciWriteConfig(HwInit, 0x04, 2, command | 0x0006);
    }
    MmcInitializeAll(HwInit);
    return TRUE;
}

/* A BIOS may have used the controller behind the driver's back (APM
   resume on 9x): the cards are set up again before the next request */
VOID
MmcCheckState(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG s;

    if (HwInit->Stopped) {
        MmcReinitialize(HwInit);
        return;
    }
    if (!HwInit->CheckState) return;
    HwInit->CheckState = FALSE;
    for (s = 0; s < HwInit->SlotCount; s++) {
        PMMC_SLOT slot = &HwInit->Slots[s];
        if (!slot->Present) continue;
        MmcDrainSlot(HwInit, slot);
        if (slot->CardType != MMC_CARD_NONE) slot->NeedsInit = TRUE;
    }
}

/* Flush, put the cards in stand-by, stop the clocks. The next request or
   POWER_UP brings everything back. */
BOOLEAN
MmcPowerDown(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG s;

    MMC_DBG((MMC_PFX "PowerDown\n"));
    for (s = 0; s < HwInit->SlotCount; s++) {
        PMMC_SLOT slot = &HwInit->Slots[s];
        if (!slot->Present) continue;
        MmcDrainSlot(HwInit, slot);
        MmcSleep(HwInit, slot, TRUE);
        MmcHostStop(HwInit, slot);
    }
    HwInit->Stopped = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Interrupt watchdog                                                  */
/* ------------------------------------------------------------------ */

VOID
MmcArmTimer(IN PHW_DEVICE_EXTENSION HwInit)
{
    if (HwInit->TimerArmed || !HwInit->AsyncMode) return;
    HwInit->TimerArmed = TRUE;
    ScsiPortNotification(RequestTimerCall, HwInit, MmcTimer, MMC_TIMER_USEC);
}

static VOID
MmcDisableAsync(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG s;

    MMC_DBG((MMC_PFX "Interrupts not delivered, back to Fast Polling\n"));
    for (s = 0; s < HwInit->SlotCount; s++) {
        if (HwInit->Slots[s].Present) MmcDrainSlot(HwInit, &HwInit->Slots[s]);
        HwInit->Slots[s].Async = FALSE;
    }
    HwInit->AsyncMode = FALSE;
}

VOID
MmcTimer(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    ULONG s, missed = 0;
    BOOLEAN pending = FALSE;

    hwInit->TimerArmed = FALSE;
    if (!hwInit->AsyncMode || hwInit->Stopped) return;
    for (s = 0; s < hwInit->SlotCount; s++) {
#if MMC_HAS_CQ
        if (hwInit->Slots[s].CqBusy != 0) {
            ULONG before = hwInit->Slots[s].CqBusy;
            MmcCqService(hwInit, &hwInit->Slots[s]);
            if (hwInit->Slots[s].CqBusy != before) missed++;
            if (hwInit->Slots[s].CqBusy != 0) pending = TRUE;
            continue;
        }
#endif
        if (hwInit->Slots[s].Outstanding == NULL) continue;
        missed += MmcServiceSlot(hwInit, &hwInit->Slots[s], FALSE);
        if (hwInit->Slots[s].Outstanding != NULL) pending = TRUE;
    }
    if (missed != 0 && ++hwInit->MissedInterrupts >= MMC_MISSED_IRQ_LIMIT) {
        MmcDisableAsync(hwInit);
        return;
    }
    if (pending) MmcArmTimer(hwInit);
}

/* ------------------------------------------------------------------ */
/* Registry (NT family)                                                */
/* ------------------------------------------------------------------ */

#if !defined(MMC_NT4)
typedef struct _MMC_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    USHORT *Buffer;
} MMC_UNICODE_STRING, *PMMC_UNICODE_STRING;

typedef struct _MMC_OBJECT_ATTRIBUTES {
    ULONG Length;
    PVOID RootDirectory;
    PMMC_UNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} MMC_OBJECT_ATTRIBUTES;

LONG __stdcall ZwOpenKey(PVOID *KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes);
LONG __stdcall ZwCreateKey(PVOID *KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes,
                           ULONG TitleIndex, PVOID Class, ULONG CreateOptions, PULONG Disposition);
LONG __stdcall ZwSetValueKey(PVOID KeyHandle, PVOID ValueName, ULONG TitleIndex, ULONG Type,
                             PVOID Data, ULONG DataSize);
LONG __stdcall ZwClose(PVOID Handle);

#define MMC_OBJ_CASE_INSENSITIVE        0x00000040
#define MMC_OBJ_KERNEL_HANDLE           0x00000200
#define MMC_KEY_READ_WRITE              0x0002001F
#define MMC_REG_CREATED_NEW_KEY         1
#define MMC_REG_DWORD                   4

static VOID
MmcInitString(OUT PMMC_UNICODE_STRING String, IN USHORT *Text, IN ULONG Bytes)
{
    String->Buffer = Text;
    String->Length = (USHORT)(Bytes - sizeof(USHORT));
    String->MaximumLength = (USHORT)Bytes;
}

static VOID
MmcInitAttributes(OUT MMC_OBJECT_ATTRIBUTES *Oa, IN PMMC_UNICODE_STRING Name, IN PVOID Root)
{
    Oa->Length = sizeof(MMC_OBJECT_ATTRIBUTES);
    Oa->RootDirectory = Root;
    Oa->ObjectName = Name;
    Oa->Attributes = MMC_OBJ_CASE_INSENSITIVE | MMC_OBJ_KERNEL_HANDLE;
    Oa->SecurityDescriptor = NULL;
    Oa->SecurityQualityOfService = NULL;
}

/* Text-mode setup boots with a bare service key: without
   Parameters\PnpInterface ScsiPort does not start the PnP adapter (STOP
   0x7B). The entries the INF makes: PCI (5), and for ACPI device nodes
   Isa (1, what ScsiPort assumes for a node without a bus type), Internal
   (0) and PNPBus (15). */
static USHORT g_MmcParametersName[] = L"Parameters";
static USHORT g_MmcPnpInterfaceName[] = L"PnpInterface";
static USHORT g_MmcBus0[] = L"0";
static USHORT g_MmcBus1[] = L"1";
static USHORT g_MmcBus5[] = L"5";
static USHORT g_MmcBus15[] = L"15";

/* Shorter names first: ScsiPort 5.0 reads each name into the buffer of
   the one before without clearing it, "1" after "15" would read as 15 */
static const struct { USHORT *Text; ULONG Bytes; } g_MmcBusNames[] = {
    { g_MmcBus0, sizeof(g_MmcBus0) }, { g_MmcBus1, sizeof(g_MmcBus1) },
    { g_MmcBus5, sizeof(g_MmcBus5) }, { g_MmcBus15, sizeof(g_MmcBus15) }
};

static VOID
MmcEnsurePnpInterface(IN PVOID RegistryPath)
{
    MMC_OBJECT_ATTRIBUTES oa;
    MMC_UNICODE_STRING name;
    PVOID service, params, pnp;
    ULONG disposition = 0, one = 1, i;

    if (RegistryPath == NULL) return;
    MmcInitAttributes(&oa, (PMMC_UNICODE_STRING)RegistryPath, NULL);
    if (ZwOpenKey(&service, MMC_KEY_READ_WRITE, &oa) < 0) return;

    MmcInitString(&name, g_MmcParametersName, sizeof(g_MmcParametersName));
    MmcInitAttributes(&oa, &name, service);
    if (ZwCreateKey(&params, MMC_KEY_READ_WRITE, &oa, 0, NULL, 0, &disposition) >= 0) {
        MmcInitString(&name, g_MmcPnpInterfaceName, sizeof(g_MmcPnpInterfaceName));
        MmcInitAttributes(&oa, &name, params);
        if (ZwCreateKey(&pnp, MMC_KEY_READ_WRITE, &oa, 0, NULL, 0, &disposition) >= 0) {
            if (disposition == MMC_REG_CREATED_NEW_KEY) {
                for (i = 0; i < sizeof(g_MmcBusNames) / sizeof(g_MmcBusNames[0]); i++) {
                    MmcInitString(&name, g_MmcBusNames[i].Text, g_MmcBusNames[i].Bytes);
                    ZwSetValueKey(pnp, &name, 0, MMC_REG_DWORD, &one, sizeof(one));
                }
                MMC_DBG((MMC_PFX "DriverEntry: Parameters\\PnpInterface added\n"));
            }
            ZwClose(pnp);
        }
        ZwClose(params);
    }
    ZwClose(service);
}
#endif

/* ------------------------------------------------------------------ */
/* DriverEntry                                                         */
/* ------------------------------------------------------------------ */

#ifdef MMC_NT31_COMPAT
static BOOLEAN g_MmcFindAdapterCalled;
#endif

ULONG
DriverEntry(IN PVOID DriverObject, IN PVOID Argument2)
{
    HW_INITIALIZATION_DATA initData;
    ULONG status, status2;

    g_MmcDumpMode = (BOOLEAN)(DriverObject == NULL);
    MMC_DBG((MMC_PFX "DriverEntry%s\n", g_MmcDumpMode ? " (dump)" : ""));

    ZeroMemoryBytes(&initData, sizeof(HW_INITIALIZATION_DATA));
#ifdef MMC_NT31_COMPAT
    initData.HwInitializationDataSize = MMC_HWINIT_SIZE_NT35;
#else
    initData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
#endif
    initData.HwInitialize = MmcHwInitialize;
    initData.HwStartIo = MmcStartIo;
    initData.HwInterrupt = MmcInterrupt;
    initData.HwResetBus = MmcResetBus;
    initData.HwFindAdapter = MmcFindAdapter;
#ifdef MMC_W9X
    initData.HwAdapterState = MmcAdapterState;
#endif
#if !defined(MMC_NT4)
    initData.HwAdapterControl = MmcAdapterControl;
#endif
    initData.DeviceExtensionSize = sizeof(HW_DEVICE_EXTENSION);
    initData.AdapterInterfaceType = PCIBus;
    initData.NumberOfAccessRanges = MMC_MAX_SLOTS;
    initData.MapBuffers = TRUE;
    initData.NeedPhysicalAddresses = TRUE;
    initData.AutoRequestSense = FALSE;
#if MMC_HAS_CQ
    /* Several queued tasks per card with command queuing (CQ=1); without
       it the INQUIRY data has no CmdQue and nothing changes */
    initData.TaggedQueuing = (BOOLEAN)!g_MmcDumpMode;
    initData.MultipleRequestPerLu = (BOOLEAN)!g_MmcDumpMode;
#else
    initData.TaggedQueuing = FALSE;
    initData.MultipleRequestPerLu = FALSE;
#endif

#if !defined(MMC_NT4)
    if (!g_MmcDumpMode) MmcEnsurePnpInterface(Argument2);
#endif

    status = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);

#ifdef MMC_NT31_COMPAT
    /* NT 3.1: its own 40h-byte structure, on the ISA bus; PCI through the
       ports, ACPI controllers from the tables */
    if (status != 0 && !g_MmcFindAdapterCalled) {
        MMC_DBG((MMC_PFX "DriverEntry: %08lX, retrying with the NT 3.1 layout\n", status));
        initData.HwInitializationDataSize = MMC_HWINIT_SIZE_NT31;
        initData.AdapterInterfaceType = Isa;
        status = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);
        MMC_DBG((MMC_PFX "DriverEntry: ScsiPortInitialize returned %08lX\n", status));
        return status;
    }
#endif

#if defined(MMC_NT4)
    /* The ISA pass: controllers only ACPI describes */
    if (!g_MmcDumpMode) {
        initData.AdapterInterfaceType = Isa;
        status2 = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);
        MMC_DBG((MMC_PFX "DriverEntry: ScsiPortInitialize returned %08lX (PCI), %08lX (ISA)\n", status, status2));
        if (status != 0 && status2 == 0) status = status2;
    } else {
        MMC_DBG((MMC_PFX "DriverEntry: ScsiPortInitialize returned %08lX\n", status));
    }
#else
    /* 2000/XP: PCI functions, and ACPI device nodes, which ScsiPort
       matches by their bus type (Isa when the node reports none; Internal
       or PNPBus), all listed in Parameters\PnpInterface */
    if (!g_MmcDumpMode || status != 0) {
        static const INTERFACE_TYPE acpiTypes[] = { Internal, Isa, PNPBus };
        ULONG t;
        for (t = 0; t < sizeof(acpiTypes) / sizeof(acpiTypes[0]); t++) {
            initData.AdapterInterfaceType = acpiTypes[t];
            status2 = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);
            MMC_DBG((MMC_PFX "DriverEntry: bus type %lu: %08lX\n", (ULONG)acpiTypes[t], status2));
            if (status != 0 && status2 == 0) status = status2;
        }
    }
    MMC_DBG((MMC_PFX "DriverEntry: ScsiPortInitialize returned %08lX\n", status));
#endif
    return status;
}

/* ------------------------------------------------------------------ */
/* HwFindAdapter                                                       */
/* ------------------------------------------------------------------ */

/* "KEY" in the argument string, at the start of a word (HS=0 is not found
   in UHS=0); Value: the hexadecimal number after it */
static BOOLEAN
MmcArg(IN PCHAR Args, IN PCHAR Key, OUT PULONG Value)
{
    ULONG i, j, v;
    CHAR a, c;

    if (Args == NULL) return FALSE;
    for (i = 0; Args[i] != 0; i++) {
        if (i > 0) {
            c = Args[i - 1];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
        }
        for (j = 0; Key[j] != 0; j++) {
            a = Args[i + j];
            if (a >= 'a' && a <= 'z') a = (CHAR)(a - 'a' + 'A');
            if (a != Key[j]) break;
        }
        if (Key[j] != 0) continue;
        if (Value != NULL) {
            v = 0;
            for (j = i + j; Args[j] != 0; j++) {
                c = Args[j];
                if (c >= '0' && c <= '9') v = v * 16 + (ULONG)(c - '0');
                else if (c >= 'a' && c <= 'f') v = v * 16 + (ULONG)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v = v * 16 + (ULONG)(c - 'A' + 10);
                else break;
            }
            *Value = v;
        }
        return TRUE;
    }
    return FALSE;
}

static VOID
MmcSetAccessRange(IN PPORT_CONFIGURATION_INFORMATION ConfigInfo, IN ULONG Index, IN ULONG Base, IN ULONG Length)
{
    PACCESS_RANGE ranges = *ConfigInfo->AccessRanges;

    if (ranges == NULL || Index >= ConfigInfo->NumberOfAccessRanges) return;
    ranges[Index].RangeStart.HighPart = 0;
    ranges[Index].RangeStart.LowPart = Base;
    ranges[Index].RangeLength = Length;
    ranges[Index].RangeInMemory = TRUE;
}

ULONG
MmcFindAdapter(
    IN PVOID DeviceExtension,
    IN PVOID Context,
    IN PVOID BusInformation,
    IN PCHAR ArgumentString,
    IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    OUT PBOOLEAN Again
)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    PCI_COMMON_CONFIG pciConfig;
    ULONG bases[MMC_MAX_SLOTS], count = 0, s, irq = 0, value;
    ULONG targetBus = 0, targetSlot = 0;
    BOOLEAN hasSlot, pci = FALSE, ownEnumeration, levelIrq = TRUE, sdma = FALSE, dma64 = TRUE, interrupts;
#if MMC_HAS_CQ
    BOOLEAN cqMapped = FALSE;
#endif
    INTERFACE_TYPE busType;
    ULONG busNumber;

    (VOID)Context;
    (VOID)BusInformation;
    *Again = FALSE;

#ifdef MMC_NT31_COMPAT
    g_MmcFindAdapterCalled = TRUE;
    MmcPciProbeAccess(hwInit);
    hasSlot = (BOOLEAN)MMC_CONFIG_HAS(ConfigInfo, SlotNumber);
#else
    hasSlot = TRUE;
#endif
    if (hwInit->SlotCount != 0) return SP_RETURN_FOUND;

    hwInit->DumpMode = (BOOLEAN)(g_MmcDumpMode || MmcArg(ArgumentString, "DUMP=", NULL));
    hwInit->DmaAllowed = (BOOLEAN)!MmcArg(ArgumentString, "DMA=0", NULL);
    hwInit->HighSpeedAllowed = (BOOLEAN)!MmcArg(ArgumentString, "HS=0", NULL);
    hwInit->UhsAllowed = (BOOLEAN)!MmcArg(ArgumentString, "UHS=0", NULL);
    hwInit->Hs200Allowed = (BOOLEAN)!MmcArg(ArgumentString, "HS200=0", NULL);
    hwInit->DdrAllowed = (BOOLEAN)!MmcArg(ArgumentString, "DDR=0", NULL);
    hwInit->UnmapAllowed = (BOOLEAN)!MmcArg(ArgumentString, "TRIM=0", NULL);
    hwInit->Hs400Allowed = (BOOLEAN)(MmcArg(ArgumentString, "HS400=1", NULL) && hwInit->Hs200Allowed);
#if MMC_HAS_CQ
    /* CQ=1: the engine at the Intel offset 200h; CQ=<hex>: at that offset */
    if (!hwInit->DumpMode && hwInit->DmaAllowed && MmcArg(ArgumentString, "CQ=", &value) && value != 0) {
        hwInit->CqOffset = (value == 1) ? MMC_CQ_DEFAULT_OFFSET : (value & 0xFF00);
        hwInit->CqAllowed = (BOOLEAN)(hwInit->CqOffset >= SDHCI_REG_SIZE && hwInit->CqOffset <= 0xF00);
    }
#endif
    if (MmcArg(ArgumentString, "CLOCK=", &value) && value != 0) {
        /* MHz, given in decimal digits: read as hex, converted back */
        hwInit->MaxClockKhz = (((value >> 12) & 0xF) * 1000 + ((value >> 8) & 0xF) * 100 +
                               ((value >> 4) & 0xF) * 10 + (value & 0xF)) * 1000;
    }
#if defined(MMC_NT4)
    ownEnumeration = (BOOLEAN)!hwInit->DumpMode;
#else
    ownEnumeration = FALSE;
#endif

    MMC_DBG((MMC_PFX "FindAdapter: bus type %lu, bus %lu slot %08lX, %lu range(s), IRQ %lu%s\n",
             (ULONG)ConfigInfo->AdapterInterfaceType, ConfigInfo->SystemIoBusNumber,
             hasSlot ? ConfigInfo->SlotNumber : 0xFFFFFFFF, ConfigInfo->NumberOfAccessRanges,
             ConfigInfo->BusInterruptLevel, hwInit->DumpMode ? ", dump" : ""));

    /* 1: the PCI function ScsiPort, PnP or SCSIPORT.PDR called us for */
    if (hasSlot && ConfigInfo->AdapterInterfaceType == PCIBus) {
        targetBus = ConfigInfo->SystemIoBusNumber;
        targetSlot = ConfigInfo->SlotNumber;
        if (MmcReadPciHeader(hwInit, targetBus, targetSlot, &pciConfig, NULL) && MmcPciIsSdhci(&pciConfig)) {
            count = MmcPciSlots(&pciConfig, bases);
            pci = (BOOLEAN)(count != 0 && (!ownEnumeration || !MmcIsClaimed(bases[0])));
        }
    }

#if defined(MMC_NT4)
    /* 2: the next PCI SDHCI function (NT 4.0 and NT 3.1 enumerate themselves) */
    if (!pci && ownEnumeration && (ConfigInfo->AdapterInterfaceType != Isa || !hasSlot)) {
        if (MmcScanPci(hwInit, &targetBus, &targetSlot, &pciConfig)) {
            count = MmcPciSlots(&pciConfig, bases);
            pci = TRUE;
        }
    }
#endif

    if (pci) {
        hwInit->HasPci = TRUE;
        hwInit->PciBus = targetBus;
        hwInit->PciSlot = targetSlot;
        hwInit->VendorId = pciConfig.VendorID;
        hwInit->DeviceId = pciConfig.DeviceID;
        irq = (pciConfig.u.type0.InterruptLine == 0xFF) ? 0 : pciConfig.u.type0.InterruptLine;
        levelIrq = TRUE;
        /* Memory space and bus master on */
        if ((pciConfig.Command & 0x0006) != 0x0006) MmcPciWriteConfig(hwInit, 0x04, 2, pciConfig.Command | 0x0006);
        MMC_DBG((MMC_PFX "FindAdapter: %04lX:%04lX at bus %lu slot %08lX, progif %02lX, %lu slot(s) at %08lX\n",
                 (ULONG)pciConfig.VendorID, (ULONG)pciConfig.DeviceID, targetBus, targetSlot,
                 (ULONG)pciConfig.ProgIf, count, bases[0]));
#if defined(MMC_NT4)
    } else if (ownEnumeration && (ConfigInfo->AdapterInterfaceType == Isa || !hasSlot)) {
        /* 3: a controller only ACPI describes, or one given with BASE= */
        MMC_ACPI_DEVICE acpi[MMC_MAX_CONTROLLERS];
        ULONG n, i;

        if (MmcArg(ArgumentString, "BASE=", &value) && value != 0 && !MmcIsClaimed(value)) {
            ZeroMemoryBytes(&acpi[0], sizeof(acpi[0]));
            acpi[0].MmioBase = value;
            acpi[0].MmioLength = SDHCI_REG_SIZE;
            if (MmcArg(ArgumentString, "IRQ=", &value) && value <= 15) acpi[0].Irq = value;
            n = 1;
            MMC_DBG((MMC_PFX "FindAdapter: BASE=%08lX from the parameters\n", acpi[0].MmioBase));
        } else {
            n = MmcAcpiFindControllers(hwInit, acpi, MMC_MAX_CONTROLLERS);
        }
        for (i = 0; i < n && MmcIsClaimed(acpi[i].MmioBase); i++) ;
        if (i >= n) return SP_RETURN_NOT_FOUND;
        bases[0] = acpi[i].MmioBase;
        count = 1;
        irq = acpi[i].Irq;
        levelIrq = acpi[i].LevelIrq;
        hwInit->Acpi = TRUE;
        if (irq > 15) {
            MMC_DBG((MMC_PFX "FindAdapter: interrupt %lu is beyond the PIC, Fast Polling\n", irq));
            irq = 0;
        }
#endif
    } else {
#if defined(MMC_NT4)
        return SP_RETURN_NOT_FOUND;
#else
        /* An ACPI device node (2000/XP) or the dump stack: the register
           block(s) from the resources */
        PACCESS_RANGE ranges = *ConfigInfo->AccessRanges;
        for (s = 0; ranges != NULL && s < ConfigInfo->NumberOfAccessRanges && count < MMC_MAX_SLOTS; s++) {
            if (ranges[s].RangeInMemory && ranges[s].RangeLength >= SDHCI_REG_SIZE && ranges[s].RangeStart.LowPart != 0 &&
                ranges[s].RangeStart.HighPart == 0) {
                bases[count++] = ranges[s].RangeStart.LowPart;
            }
        }
        if (count == 0) return SP_RETURN_NOT_FOUND;
        hwInit->Acpi = (BOOLEAN)(ConfigInfo->AdapterInterfaceType != PCIBus);
        irq = ConfigInfo->BusInterruptLevel;
        MMC_DBG((MMC_PFX "FindAdapter: %lu register block(s) from the resources, first at %08lX\n", count, bases[0]));
#endif
    }

    /* Bus type for mapping, interrupt and DMA */
    busType = ConfigInfo->AdapterInterfaceType;
    busNumber = ConfigInfo->SystemIoBusNumber;
    if (ownEnumeration) {
        if (pci) {
            busType = PCIBus;
            busNumber = targetBus;
            if (hasSlot) {
                ConfigInfo->SystemIoBusNumber = targetBus;
                ConfigInfo->SlotNumber = targetSlot;
            }
            ConfigInfo->InterruptMode = LevelSensitive;
#ifdef MMC_NT31_COMPAT
            if (!hasSlot) {
                /* NT 3.1: no PCI bus handler; the EISA bus, INTx only on request */
                busType = Eisa;
                busNumber = 0;
                ConfigInfo->AdapterInterfaceType = Eisa;
                if (!MmcArg(ArgumentString, "IRQ=1", NULL)) irq = 0;
            }
#endif
        } else {
            /* ACPI: the EISA bus gives a 32-bit bus master DMA adapter and
               maps memory and ISA interrupts like the ISA bus */
            busType = Eisa;
            busNumber = 0;
            ConfigInfo->AdapterInterfaceType = Eisa;
            ConfigInfo->SystemIoBusNumber = 0;
            ConfigInfo->InterruptMode = levelIrq ? LevelSensitive : Latched;
        }
        ConfigInfo->BusInterruptLevel = irq;
        ConfigInfo->BusInterruptVector = irq;
        for (s = 0; s < ConfigInfo->NumberOfAccessRanges; s++) MmcSetAccessRange(ConfigInfo, s, 0, 0);
        for (s = 0; s < count; s++) MmcSetAccessRange(ConfigInfo, s, bases[s], SDHCI_REG_SIZE);
        *Again = TRUE;
    } else if (pci) {
        busType = PCIBus;
        busNumber = targetBus;
    }
    if (MmcArg(ArgumentString, "IRQ=0", NULL)) {
        ConfigInfo->BusInterruptLevel = 0;
        ConfigInfo->BusInterruptVector = 0;
    }

    /* Map the slots */
    for (s = 0; s < count; s++) {
        PMMC_SLOT slot = &hwInit->Slots[s];
        SCSI_PHYSICAL_ADDRESS pa;

        ZeroMemoryBytes(slot, sizeof(MMC_SLOT));
        slot->Index = (UCHAR)s;
        slot->RegsPhysical = bases[s];
        pa.HighPart = 0;
        pa.LowPart = bases[s];
#ifdef MMC_W9X
        (VOID)pa;
        (VOID)busType;
        (VOID)busNumber;
        slot->Regs = (PUCHAR)MmcMapPhysToLinear(bases[s], SDHCI_REG_SIZE);
        if (slot->Regs == (PUCHAR)0xFFFFFFFF) slot->Regs = NULL;
#else
        slot->Regs = NULL;
#if MMC_HAS_CQ
        /* With the command queuing registers; without them where that
           much is not mapped */
        if (hwInit->CqAllowed) {
            slot->Regs = (PUCHAR)ScsiPortGetDeviceBase(hwInit, busType, busNumber, pa,
                                                       hwInit->CqOffset + SDHCI_REG_SIZE, FALSE);
            cqMapped = (BOOLEAN)(slot->Regs != NULL);
        }
#endif
        if (slot->Regs == NULL) {
            slot->Regs = (PUCHAR)ScsiPortGetDeviceBase(hwInit, busType, busNumber, pa, SDHCI_REG_SIZE, FALSE);
        }
#endif
        if (slot->Regs == NULL) {
            MMC_DBG((MMC_PFX "FindAdapter: slot %lu at %08lX not mapped\n", s, bases[s]));
            return SP_RETURN_ERROR;
        }
        if (MMC_RD(slot, SDHCI_SLOT_INT_VERSION) == 0xFFFFFFFF) {
            MMC_DBG((MMC_PFX "FindAdapter: slot %lu at %08lX does not answer\n", s, bases[s]));
            return SP_RETURN_NOT_FOUND;
        }
        slot->Present = TRUE;
        slot->Caps = MMC_RD(slot, SDHCI_CAPABILITIES);
#if MMC_HAS_CQ
        if (cqMapped) MmcCqProbe(hwInit, slot);
        cqMapped = FALSE;
#endif
        if (!(slot->Caps & SDHCI_CAN_ADMA2) && (slot->Caps & SDHCI_CAN_SDMA)) sdma = TRUE;
        /* 64-bit ADMA2: SDHCI 3.00 or later with the 64-bit system bus */
        if (((MMC_RD(slot, SDHCI_SLOT_INT_VERSION) >> 16) & 0xFF) < SDHCI_SPEC_300 ||
            (slot->Caps & (SDHCI_CAN_ADMA2 | SDHCI_CAN_64BIT)) != (SDHCI_CAN_ADMA2 | SDHCI_CAN_64BIT)) {
            dma64 = FALSE;
        }
    }
    hwInit->SlotCount = (UCHAR)count;
    hwInit->Irq = (UCHAR)ConfigInfo->BusInterruptLevel;
    MmcClaim(bases[0]);

    if (MMC_CONFIG_HAS(ConfigInfo, MaximumNumberOfTargets)) ConfigInfo->MaximumNumberOfTargets = (UCHAR)count;
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->InitiatorBusId[0] = 7;
    ConfigInfo->MaximumTransferLength = MMC_MAX_TRANSFER;
    ConfigInfo->NumberOfPhysicalBreaks = MMC_MAX_PHYS_BREAKS;
    ConfigInfo->AlignmentMask = 0x1;
    ConfigInfo->CachesData = TRUE;             /* ScsiPort passes SRB_FUNCTION_SHUTDOWN and FLUSH only then */
    ConfigInfo->Master = TRUE;
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->Dma32BitAddresses = TRUE;
#if MMC_HAS_DMA64
    /* Buffers above 4 GB go to the controller as they are when every slot
       has 64-bit ADMA2; otherwise ScsiPort copies them below 4 GB */
    if (!hwInit->DmaAllowed || MmcArg(ArgumentString, "DMA64=0", NULL)) dma64 = FALSE;
    MMC_DBG((MMC_PFX "FindAdapter: 64-bit ADMA2 %s, system %s\n", dma64 ? "on" : "off",
             (ConfigInfo->Dma64BitAddresses & SCSI_DMA64_SYSTEM_SUPPORTED) ? "above 4 GB" : "below 4 GB"));
    hwInit->Dma64 = dma64;
    ConfigInfo->Dma64BitAddresses = dma64 ? SCSI_DMA64_MINIPORT_SUPPORTED : 0;
#else
    (VOID)dma64;
#endif

    /* Interrupt-completed transfers where an interrupt is connected; off
       in the dump stack, with IRQ=0, on 9x unless IRQ=1 */
    interrupts = (BOOLEAN)(ConfigInfo->BusInterruptLevel != 0 || ConfigInfo->BusInterruptVector != 0);
    if (hwInit->DumpMode) interrupts = FALSE;
#ifdef MMC_W9X
    if (!MmcArg(ArgumentString, "IRQ=1", NULL)) interrupts = FALSE;
#endif
    hwInit->AsyncMode = interrupts;

    MMC_DBG((MMC_PFX "FindAdapter: %lu slot(s), IRQ %lu, %s%s\n", count, ConfigInfo->BusInterruptLevel,
             interrupts ? "interrupt" : "Fast Polling", hwInit->DmaAllowed ? "" : ", DMA off"));

    if (!MmcAllocateDma(hwInit, ConfigInfo, sdma)) return SP_RETURN_ERROR;
    return SP_RETURN_FOUND;
}

/* ------------------------------------------------------------------ */
/* HwInitialize, HwStartIo                                             */
/* ------------------------------------------------------------------ */

BOOLEAN
MmcHwInitialize(IN PVOID DeviceExtension)
{
    MmcInitializeAll((PHW_DEVICE_EXTENSION)DeviceExtension);
    MMC_DBG((MMC_PFX "HwInitialize: done\n"));
    return TRUE;
}

/* A read/write for a slot that is busy with one already: it waits and is
   started from the completion of the first */
static BOOLEAN
MmcCanQueue(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    if (Srb->Function != SRB_FUNCTION_EXECUTE_SCSI || HwInit->DumpMode) return FALSE;
    if (!Slot->Async || !Slot->CardReady || Slot->NeedsInit || HwInit->CheckState) return FALSE;
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

static VOID
MmcFlushAll(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG s;
    for (s = 0; s < HwInit->SlotCount; s++) {
        if (!HwInit->Slots[s].Present) continue;
        MmcDrainSlot(HwInit, &HwInit->Slots[s]);
        MmcFlush(HwInit, &HwInit->Slots[s]);
    }
}

static VOID
MmcShutdownAll(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG s;
    for (s = 0; s < HwInit->SlotCount; s++) {
        PMMC_SLOT slot = &HwInit->Slots[s];
        if (!slot->Present) continue;
        MmcDrainSlot(HwInit, slot);
        MmcPowerOffNotify(HwInit, slot);
    }
}

BOOLEAN
MmcStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    PMMC_SLOT slot = NULL;

    Srb->SrbStatus = SRB_STATUS_PENDING;
    if (Srb->TargetId < hwInit->SlotCount) slot = &hwInit->Slots[Srb->TargetId];

#if MMC_HAS_CQ
    /* Queued tasks run on: only a queued read or write goes to the engine;
       everything else waits for the tasks and the halt (a reset fails them) */
    if (Srb->Function != SRB_FUNCTION_RESET_BUS && Srb->Function != SRB_FUNCTION_RESET_DEVICE) {
        ULONG s;
        for (s = 0; s < hwInit->SlotCount; s++) {
            PMMC_SLOT other = &hwInit->Slots[s];
            if (other->CqRegs == NULL) continue;
            if (other == slot && Srb->Function == SRB_FUNCTION_EXECUTE_SCSI) {
                if (!MmcCqWanted(hwInit, slot, Srb)) MmcCqHalt(hwInit, slot);
            } else if (Srb->Function != SRB_FUNCTION_EXECUTE_SCSI) {
                MmcCqHalt(hwInit, other);
            }
        }
    }
#endif

    if (slot != NULL && slot->Outstanding != NULL) {
        if (slot->Pending == NULL && MmcCanQueue(hwInit, slot, Srb)) {
            slot->Pending = Srb;
            ScsiPortNotification(NextRequest, hwInit, NULL);
            return TRUE;
        }
        MmcDrainSlot(hwInit, slot);
    }

    switch (Srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        MmcCheckState(hwInit);
        MmcSatlProcessSrb(hwInit, Srb);
        break;
    case SRB_FUNCTION_IO_CONTROL:
        MmcProcessIoControl(hwInit, Srb);
        break;
    case SRB_FUNCTION_RESET_BUS:
    case SRB_FUNCTION_RESET_DEVICE:
        MmcResetBus(hwInit, Srb->PathId);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    case SRB_FUNCTION_ABORT_COMMAND:
        Srb->SrbStatus = SRB_STATUS_ABORT_FAILED;
        break;
    case SRB_FUNCTION_SHUTDOWN:
        MmcCheckState(hwInit);
        MmcShutdownAll(hwInit);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    case SRB_FUNCTION_FLUSH:
        MmcCheckState(hwInit);
        MmcFlushAll(hwInit);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;
    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    if (Srb->SrbStatus != SRB_STATUS_PENDING) {
        ScsiPortNotification(RequestComplete, hwInit, Srb);
    } else {
        MmcArmTimer(hwInit);
    }
    ScsiPortNotification(NextRequest, hwInit, NULL);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwInterrupt, HwResetBus                                             */
/* ------------------------------------------------------------------ */

BOOLEAN
MmcInterrupt(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    BOOLEAN claimed = FALSE;
    ULONG s;

    for (s = 0; s < hwInit->SlotCount; s++) {
        PMMC_SLOT slot = &hwInit->Slots[s];
        if (!slot->Present) continue;
#if MMC_HAS_CQ
        if (slot->CqRegs != NULL && !slot->CqHalted) {
            if (MmcCqService(hwInit, slot) != 0) {
                hwInit->MissedInterrupts = 0;
                claimed = TRUE;
            }
            continue;
        }
#endif
        if (slot->Outstanding != NULL) {
            if (MmcServiceSlot(hwInit, slot, TRUE) != 0) {
                hwInit->MissedInterrupts = 0;
                claimed = TRUE;
            }
        } else if (MmcAckStray(slot)) {
            claimed = TRUE;
        }
    }
    if (claimed) hwInit->InterruptCount++;
    return claimed;
}

/* The cards are set up again before the next request */
BOOLEAN
MmcResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)HwDeviceExtension;
    ULONG s;

    (VOID)PathId;
    MMC_DBG((MMC_PFX "ResetBus\n"));
    for (s = 0; s < hwInit->SlotCount; s++) {
        PMMC_SLOT slot = &hwInit->Slots[s];
        if (!slot->Present) continue;
        if (slot->Outstanding != NULL && ++slot->DmaErrors >= MMC_DMA_ERROR_LIMIT) slot->Xfer = MMC_XFER_PIO;
#if MMC_HAS_CQ
        if (slot->CqBusy != 0) MmcCqFail(hwInit, slot, SRB_STATUS_BUS_RESET);
#endif
        MmcFailOutstanding(hwInit, slot, SRB_STATUS_BUS_RESET);
        MMC_WR(slot, SDHCI_SIGNAL_ENABLE, 0);
        if (slot->CardType != MMC_CARD_NONE) slot->NeedsInit = TRUE;
        slot->SensePending = FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwAdapterControl (2000/XP/x64): stop and restart for power changes  */
/* ------------------------------------------------------------------ */

#if !defined(MMC_NT4)
SCSI_ADAPTER_CONTROL_STATUS
MmcAdapterControl(IN PVOID DeviceExtension, IN SCSI_ADAPTER_CONTROL_TYPE ControlType, IN PVOID Parameters)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST list;
    ULONG i, s;

    switch (ControlType) {
    case ScsiQuerySupportedControlTypes:
        list = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        for (i = 0; i < list->MaxControlType; i++) {
            list->SupportedTypeList[i] = (BOOLEAN)(i == ScsiQuerySupportedControlTypes ||
                                                  i == ScsiStopAdapter || i == ScsiRestartAdapter);
        }
        return ScsiAdapterControlSuccess;
    case ScsiStopAdapter:
        MMC_DBG((MMC_PFX "AdapterControl: stop\n"));
        for (s = 0; s < hwInit->SlotCount; s++) {
            PMMC_SLOT slot = &hwInit->Slots[s];
            if (!slot->Present) continue;
            MmcDrainSlot(hwInit, slot);
            MmcSleep(hwInit, slot, TRUE);
            MmcHostStop(hwInit, slot);
        }
        hwInit->Stopped = TRUE;
        return ScsiAdapterControlSuccess;
    case ScsiRestartAdapter:
        MMC_DBG((MMC_PFX "AdapterControl: restart\n"));
        MmcReinitialize(hwInit);
        return ScsiAdapterControlSuccess;
    default:
        return ScsiAdapterControlUnsuccessful;
    }
}
#endif
