#include "mmcsd.h"

/* mmcsatl.c - SCSI command set on SD/MMC cards, miniport IOCTLs */

/* ------------------------------------------------------------------ */
/* 64-bit block number helpers                                         */
/* ------------------------------------------------------------------ */

static MMC_LBA
MmcLbaFromBigEndian(IN PUCHAR Bytes, IN ULONG Count)
{
    MMC_LBA lba;
    ULONG i;

    lba.Low = 0;
    lba.High = 0;
    for (i = 0; i < Count; i++) {
        lba.High = (lba.High << 8) | (lba.Low >> 24);
        lba.Low = (lba.Low << 8) | Bytes[i];
    }
    return lba;
}

static ULONG
MmcBigEndian32(IN PUCHAR Bytes)
{
    return ((ULONG)Bytes[0] << 24) | ((ULONG)Bytes[1] << 16) | ((ULONG)Bytes[2] << 8) | (ULONG)Bytes[3];
}

static VOID
MmcPutBigEndian32(OUT PUCHAR Bytes, IN ULONG Value)
{
    Bytes[0] = (UCHAR)(Value >> 24);
    Bytes[1] = (UCHAR)(Value >> 16);
    Bytes[2] = (UCHAR)(Value >> 8);
    Bytes[3] = (UCHAR)Value;
}

static MMC_LBA
MmcLastLba(IN PMMC_SLOT Slot)
{
    MMC_LBA last = Slot->Sectors;

    if (last.Low == 0 && last.High == 0) return last;
    if (last.Low == 0) last.High--;
    last.Low--;
    return last;
}

static BOOLEAN
MmcLbaRangeValid(IN PMMC_SLOT Slot, IN MMC_LBA Lba, IN ULONG Count)
{
    ULONG endLow = Lba.Low + Count, endHigh = Lba.High;

    if (endLow < Lba.Low) {
        if (endHigh == 0xFFFFFFFF) return FALSE;
        endHigh++;
    }
    if (endHigh != Slot->Sectors.High) return (BOOLEAN)(endHigh < Slot->Sectors.High);
    return (BOOLEAN)(endLow <= Slot->Sectors.Low);
}

/* ------------------------------------------------------------------ */
/* Sense data                                                          */
/* ------------------------------------------------------------------ */

static VOID
MmcBuildSense(OUT PSENSE_DATA Sense, IN UCHAR Key, IN UCHAR Asc, IN UCHAR Ascq)
{
    ZeroMemoryBytes(Sense, sizeof(SENSE_DATA));
    Sense->ErrorCode = 0x70;
    Sense->Valid = (Key != SCSI_SENSE_NO_SENSE) ? 1 : 0;
    Sense->SenseKey = Key;
    Sense->AdditionalSenseLength = sizeof(SENSE_DATA) - 8;
    Sense->AdditionalSenseCode = Asc;
    Sense->AdditionalSenseCodeQualifier = Ascq;
}

static VOID
MmcCopyToSrb(IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Data, IN ULONG Length)
{
    ULONG n = Length;

    if (Srb->DataBuffer == NULL) return;
    if (n > Srb->DataTransferLength) n = Srb->DataTransferLength;
    ZeroMemoryBytes(Srb->DataBuffer, Srb->DataTransferLength);
    CopyMemoryBytes(Srb->DataBuffer, Data, n);
}

static VOID
MmcGood(IN PSCSI_REQUEST_BLOCK Srb)
{
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

static VOID
MmcFailWithSense(IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR Key, IN UCHAR Asc, IN UCHAR Ascq)
{
    SENSE_DATA sense;

    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    MmcBuildSense(&sense, Key, Asc, Ascq);

#if MMC_AUTOSENSE
    if (Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength != 0 &&
        !(Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)) {
        ULONG n = sizeof(sense);
        if (n > Srb->SenseInfoBufferLength) n = Srb->SenseInfoBufferLength;
        ZeroMemoryBytes(Srb->SenseInfoBuffer, Srb->SenseInfoBufferLength);
        CopyMemoryBytes(Srb->SenseInfoBuffer, &sense, n);
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
        return;
    }
#endif
    CopyMemoryBytes(Slot->Sense, &sense, sizeof(sense));
    Slot->SenseLength = (UCHAR)sizeof(sense);
    Slot->SensePending = TRUE;
}

static VOID
MmcInvalidField(IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_INVALID_CDB_FIELD, 0);
}

static VOID
MmcNoMedium(IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    MmcFailWithSense(Slot, Srb, SCSI_SENSE_NOT_READY, MMC_ASC_NO_MEDIA, 0);
}

/* MmcExecute / MmcReadWrite result -> SCSI status and sense */
static VOID
MmcCompleteResult(IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb, IN ULONG Result, IN BOOLEAN IsWrite)
{
    switch (Result) {
    case MMC_OK:
        MmcGood(Srb);
        break;
    case MMC_ERR_NO_CARD:
        MmcNoMedium(Slot, Srb);
        break;
    case MMC_ERR_BAD_BUFFER:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    case MMC_ERR_CARD:
        /* OUT_OF_RANGE, ADDRESS_ERROR */
        if (Slot->LastCardStatus & 0xC0000000) {
            MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_LBA_OUT_OF_RANGE, 0);
        } else if (Slot->LastCardStatus & 0x04000000) {           /* WP_VIOLATION */
            MmcFailWithSense(Slot, Srb, SCSI_SENSE_DATA_PROTECT, MMC_ASC_WRITE_PROTECTED, 0);
        } else {
            MmcFailWithSense(Slot, Srb, SCSI_SENSE_MEDIUM_ERROR,
                             (UCHAR)(IsWrite ? MMC_ASC_WRITE_ERROR : MMC_ASC_UNRECOVERED_READ), 0);
        }
        break;
    case MMC_ERR_DATA:
    case MMC_ERR_CRC:
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_MEDIUM_ERROR,
                         (UCHAR)(IsWrite ? MMC_ASC_WRITE_ERROR : MMC_ASC_UNRECOVERED_READ), 0);
        break;
    default:
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_HARDWARE_ERROR, MMC_ASC_INTERNAL_FAILURE, 0);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* INQUIRY                                                             */
/* ------------------------------------------------------------------ */

static BOOLEAN
MmcIsSd(IN PMMC_SLOT Slot)
{
    return (BOOLEAN)(Slot->CardType == MMC_CARD_SDSC || Slot->CardType == MMC_CARD_SDHC);
}

/* CID field as bytes (big endian order of the register) */
static UCHAR
MmcCidByte(IN PMMC_SLOT Slot, IN ULONG Index)
{
    return (UCHAR)(Slot->Cid[Index / 4] >> (24 - 8 * (Index % 4)));
}

typedef struct _MMC_VENDOR {
    UCHAR Mid;
    UCHAR Sd;
    PCHAR Name;
} MMC_VENDOR;

static const MMC_VENDOR g_MmcVendors[] = {
    { 0x01, 1, "Panasonc" }, { 0x02, 1, "Toshiba " }, { 0x03, 1, "SanDisk " },
    { 0x1B, 1, "Samsung " }, { 0x1D, 1, "ADATA   " }, { 0x27, 1, "Phison  " },
    { 0x28, 1, "Lexar   " }, { 0x31, 1, "Silicon " }, { 0x41, 1, "Kingston" },
    { 0x74, 1, "Transcnd" }, { 0x76, 1, "Patriot " }, { 0x82, 1, "Sony    " },
    { 0x9F, 1, "Kingston" },
    { 0x11, 0, "Toshiba " }, { 0x13, 0, "Micron  " }, { 0x15, 0, "Samsung " },
    { 0x45, 0, "SanDisk " }, { 0x70, 0, "Kingston" }, { 0x88, 0, "Foresee " },
    { 0x90, 0, "Hynix   " }, { 0xD6, 0, "Foresee " }, { 0xFE, 0, "Micron  " },
    { 0x00, 0, NULL }
};

static VOID
MmcPutText(OUT PUCHAR Dst, IN ULONG Size, IN PCHAR Src)
{
    ULONG i;
    for (i = 0; i < Size; i++) Dst[i] = (UCHAR)((Src != NULL && *Src) ? *Src++ : ' ');
}

static VOID
MmcHandleInquiryVpd(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR page[64];
    ULONG len, psn, i, allocLen = ((ULONG)Srb->Cdb[3] << 8) | Srb->Cdb[4];
    static const CHAR hex[] = "0123456789ABCDEF";

    ZeroMemoryBytes(page, sizeof(page));
    page[0] = DIRECT_ACCESS_DEVICE;
    page[1] = Srb->Cdb[2];

    switch (Srb->Cdb[2]) {
    case MMC_VPD_SUPPORTED_PAGES:
        page[3] = 5;
        page[4] = MMC_VPD_SUPPORTED_PAGES;
        page[5] = MMC_VPD_SERIAL_NUMBER;
        page[6] = MMC_VPD_BLOCK_LIMITS;
        page[7] = MMC_VPD_BLOCK_CHARACTERISTICS;
        page[8] = MMC_VPD_PROVISIONING;
        len = 4 + 5;
        break;
    case MMC_VPD_BLOCK_LIMITS:
        /* Transfer limits; UNMAP limits where UNMAP is accepted */
        page[3] = 0x3C;
        MmcPutBigEndian32(&page[8], MMC_MAX_TRANSFER / MMC_BLOCK_SIZE);
        MmcPutBigEndian32(&page[12], MMC_MAX_TRANSFER / MMC_BLOCK_SIZE);
        if (HwInit->UnmapAllowed) {
            MmcPutBigEndian32(&page[20], MMC_UNMAP_MAX_BLOCKS);
            MmcPutBigEndian32(&page[24], MMC_UNMAP_MAX_DESCRIPTORS);
            MmcPutBigEndian32(&page[28], MmcUnmapGranularity(Slot));
        }
        len = 4 + 0x3C;
        break;
    case MMC_VPD_BLOCK_CHARACTERISTICS:
        /* Non-rotating medium */
        page[3] = 0x3C;
        page[5] = 0x01;
        len = 4 + 0x3C;
        break;
    case MMC_VPD_PROVISIONING:
        page[3] = 4;
        page[5] = (UCHAR)(HwInit->UnmapAllowed ? 0x80 : 0x00);      /* LBPU */
        len = 4 + 4;
        break;
    case MMC_VPD_SERIAL_NUMBER:
        /* Product serial number from the CID */
        if (MmcIsSd(Slot)) {
            psn = ((ULONG)MmcCidByte(Slot, 9) << 24) | ((ULONG)MmcCidByte(Slot, 10) << 16) |
                  ((ULONG)MmcCidByte(Slot, 11) << 8) | MmcCidByte(Slot, 12);
        } else {
            psn = ((ULONG)MmcCidByte(Slot, 10) << 24) | ((ULONG)MmcCidByte(Slot, 11) << 16) |
                  ((ULONG)MmcCidByte(Slot, 12) << 8) | MmcCidByte(Slot, 13);
        }
        page[3] = 8;
        for (i = 0; i < 8; i++) page[4 + i] = (UCHAR)hex[(psn >> (28 - 4 * i)) & 0x0F];
        len = 4 + 8;
        break;
    default:
        MmcInvalidField(Slot, Srb);
        return;
    }
    if (len > allocLen) len = allocLen;
    MmcCopyToSrb(Srb, page, len);
    MmcGood(Srb);
}

static VOID
MmcHandleInquiry(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    INQUIRYDATA inq;
    UCHAR mid = MmcCidByte(Slot, 0), prv;
    BOOLEAN sd = MmcIsSd(Slot);
    PCHAR vendor = NULL;
    ULONG i, n;

    if (Srb->Cdb[1] & 0x01) {
        MmcHandleInquiryVpd(HwInit, Slot, Srb);
        return;
    }
    if (Srb->Cdb[2] != 0) {
        MmcInvalidField(Slot, Srb);
        return;
    }

    ZeroMemoryBytes(&inq, sizeof(inq));
    inq.DeviceType = DIRECT_ACCESS_DEVICE;
    inq.RemovableMedia = 0;                     /* boot and system disk: a fixed disk */
    inq.Versions = MMC_INQUIRY_VERSION_SPC3;
    inq.ResponseDataFormat = 2;
    inq.AdditionalLength = 31;
#if MMC_HAS_CQ
    inq.CommandQueue = (UCHAR)(Slot->CqActive ? 1 : 0);
#endif

    for (i = 0; g_MmcVendors[i].Name != NULL; i++) {
        if (g_MmcVendors[i].Mid == mid && (g_MmcVendors[i].Sd != 0) == (sd != 0)) {
            vendor = g_MmcVendors[i].Name;
            break;
        }
    }
    MmcPutText(inq.VendorId, 8, vendor != NULL ? vendor : (sd ? "SD" : "MMC"));

    /* Product name: SD 5 characters (CID bytes 3-7), MMC 6 (bytes 3-8) */
    n = sd ? 5 : 6;
    MmcPutText(inq.ProductId, 16, NULL);
    for (i = 0; i < n; i++) {
        UCHAR c = MmcCidByte(Slot, 3 + i);
        inq.ProductId[i] = (UCHAR)((c >= 0x20 && c < 0x7F) ? c : ' ');
    }
    inq.ProductId[n + 1] = (UCHAR)(sd ? 'S' : 'M');
    inq.ProductId[n + 2] = (UCHAR)(sd ? 'D' : 'M');
    inq.ProductId[n + 3] = (UCHAR)(sd ? ' ' : 'C');

    /* Revision: n.m from the BCD product revision */
    prv = MmcCidByte(Slot, sd ? 8 : 9);
    inq.ProductRevisionLevel[0] = (UCHAR)('0' + ((prv >> 4) % 10));
    inq.ProductRevisionLevel[1] = '.';
    inq.ProductRevisionLevel[2] = (UCHAR)('0' + ((prv & 0x0F) % 10));
    inq.ProductRevisionLevel[3] = ' ';

    MmcCopyToSrb(Srb, &inq, sizeof(inq));
    MmcGood(Srb);
}

/* ------------------------------------------------------------------ */
/* Other commands answered by the driver                               */
/* ------------------------------------------------------------------ */

static VOID
MmcHandleReportLuns(IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[16];
    ULONG allocLen = MmcBigEndian32(&Srb->Cdb[6]);
    ULONG len = 16;

    ZeroMemoryBytes(data, sizeof(data));
    MmcPutBigEndian32(&data[0], 8);
    if (len > allocLen) len = allocLen;
    MmcCopyToSrb(Srb, data, len);
    MmcGood(Srb);
}

/* READ CAPACITY(10) for 2 TB and more (last LBA FFFFFFFFh or beyond):
   FFFFFFFFh sends a class driver that knows READ CAPACITY(16) there.
   One that does not (NT 3.x/4.0, 9x, 2000, XP x86) adds one to it in 32
   bits and sees a card of 0 bytes; it gets FFFFFFFEh, the most it can
   address with READ(10), and sees the first 2 TB. */
static VOID
MmcHandleReadCapacity10(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[8];
    MMC_LBA last = MmcLastLba(Slot);

    if (last.High != 0 || last.Low == 0xFFFFFFFF) {
        last.Low = HwInit->Capacity16 ? 0xFFFFFFFF : 0xFFFFFFFE;
    }
    MmcPutBigEndian32(&data[0], last.Low);
    MmcPutBigEndian32(&data[4], MMC_BLOCK_SIZE);
    MmcCopyToSrb(Srb, data, sizeof(data));
    MmcGood(Srb);
}

static VOID
MmcHandleReadCapacity16(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[MMC_READ_CAPACITY16_DATA_LEN];
    MMC_LBA last = MmcLastLba(Slot);
    ULONG allocLen = MmcBigEndian32(&Srb->Cdb[10]);
    ULONG n = sizeof(data);

    ZeroMemoryBytes(data, sizeof(data));
    MmcPutBigEndian32(&data[0], last.High);
    MmcPutBigEndian32(&data[4], last.Low);
    MmcPutBigEndian32(&data[8], MMC_BLOCK_SIZE);
    if (HwInit->UnmapAllowed) data[14] = 0x80;          /* LBPME: UNMAP accepted */
    if (n > allocLen) n = allocLen;
    MmcCopyToSrb(Srb, data, n);
    MmcGood(Srb);
}

static VOID
MmcHandleRequestSense(IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    SENSE_DATA sense;

    if (Slot->SensePending) {
        MmcCopyToSrb(Srb, Slot->Sense, Slot->SenseLength);
        Slot->SensePending = FALSE;
    } else {
        MmcBuildSense(&sense, SCSI_SENSE_NO_SENSE, 0, 0);
        MmcCopyToSrb(Srb, &sense, sizeof(sense));
    }
    MmcGood(Srb);
}

static VOID
MmcHandleModeSense(IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    BOOLEAN ten = (BOOLEAN)(Srb->Cdb[0] == SCSIOP_MODE_SENSE10);
    BOOLEAN dbd = (BOOLEAN)((Srb->Cdb[1] & 0x08) != 0);
    BOOLEAN llba = (BOOLEAN)(ten && (Srb->Cdb[1] & 0x10) != 0);
    UCHAR pageCode = (UCHAR)(Srb->Cdb[2] & 0x3F);
    UCHAR pc = (UCHAR)(Srb->Cdb[2] >> 6);
    ULONG allocLen = ten ? (((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8]) : Srb->Cdb[4];
    UCHAR buf[96];
    ULONG hdrLen = ten ? 8 : 4, len, descLen = 0, blocks, cyls, total32;
    PUCHAR p;
    BOOLEAN any = FALSE;

    ZeroMemoryBytes(buf, sizeof(buf));
    len = hdrLen;

    if (!dbd && llba) {
        p = &buf[len];
        MmcPutBigEndian32(&p[0], Slot->Sectors.High);
        MmcPutBigEndian32(&p[4], Slot->Sectors.Low);
        MmcPutBigEndian32(&p[12], MMC_BLOCK_SIZE);
        descLen = 16;
        len += 16;
    } else if (!dbd) {
        blocks = (Slot->Sectors.High != 0 || Slot->Sectors.Low > 0xFFFFFF) ? 0xFFFFFF : Slot->Sectors.Low;
        p = &buf[len];
        p[1] = (UCHAR)(blocks >> 16);
        p[2] = (UCHAR)(blocks >> 8);
        p[3] = (UCHAR)blocks;
        p[6] = (UCHAR)(MMC_BLOCK_SIZE >> 8);
        descLen = 8;
        len += 8;
    }

    if (pageCode == MMC_MODE_PAGE_GEOMETRY || pageCode == MMC_MODE_PAGE_ALL) {
        total32 = (Slot->Sectors.High != 0) ? 0xFFFFFFFF : Slot->Sectors.Low;
        cyls = total32 / (255 * 63);
        if (cyls == 0) cyls = 1;
        p = &buf[len];
        p[0] = MMC_MODE_PAGE_GEOMETRY;
        p[1] = 0x16;
        if (pc != 1) {
            p[2] = (UCHAR)(cyls >> 16);
            p[3] = (UCHAR)(cyls >> 8);
            p[4] = (UCHAR)cyls;
            p[5] = 255;
            p[21] = 63;
        }
        len += 24;
        any = TRUE;
    }

    if (pageCode == MMC_MODE_PAGE_CACHING || pageCode == MMC_MODE_PAGE_ALL) {
        p = &buf[len];
        p[0] = MMC_MODE_PAGE_CACHING;
        p[1] = 0x12;
        if (pc == 1) {
            if (Slot->EmmcCache) p[2] |= 0x04;
        } else if (Slot->CacheOn) {
            p[2] |= 0x04;
        }
        len += 20;
        any = TRUE;
    }

    if (pageCode == MMC_MODE_PAGE_CONTROL || pageCode == MMC_MODE_PAGE_ALL) {
        p = &buf[len];
        p[0] = MMC_MODE_PAGE_CONTROL;
        p[1] = 0x0A;
        len += 12;
        any = TRUE;
    }

    if (!any && pageCode != 0) {
        MmcInvalidField(Slot, Srb);
        return;
    }

    /* Device-specific parameter: WP, DPOFUA */
    if (ten) {
        buf[0] = (UCHAR)((len - 2) >> 8);
        buf[1] = (UCHAR)(len - 2);
        buf[3] = (UCHAR)(0x10 | (Slot->WriteProtected ? 0x80 : 0));
        buf[4] = (UCHAR)((descLen == 16) ? 0x01 : 0x00);
        buf[7] = (UCHAR)descLen;
    } else {
        buf[0] = (UCHAR)(len - 1);
        buf[2] = (UCHAR)(0x10 | (Slot->WriteProtected ? 0x80 : 0));
        buf[3] = (UCHAR)descLen;
    }
    if (len > allocLen) len = allocLen;
    MmcCopyToSrb(Srb, buf, len);
    MmcGood(Srb);
}

/* MODE SELECT: the caching page turns the eMMC cache on and off */
static VOID
MmcHandleModeSelect(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    BOOLEAN ten = (BOOLEAN)(Srb->Cdb[0] == MMC_SCSIOP_MODE_SELECT10);
    PUCHAR data = (PUCHAR)Srb->DataBuffer;
    ULONG listLen, pos, hdrLen, pageLen;
    BOOLEAN wce;

    listLen = ten ? (((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8]) : Srb->Cdb[4];
    if (listLen > Srb->DataTransferLength) listLen = Srb->DataTransferLength;
    if (listLen == 0 || data == NULL) {
        MmcGood(Srb);
        return;
    }
    hdrLen = ten ? 8 : 4;
    if (listLen < hdrLen) {
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_PARAM_LIST_LENGTH, 0);
        return;
    }
    pos = hdrLen + (ten ? (((ULONG)data[6] << 8) | data[7]) : data[3]);

    while (pos + 2 <= listLen) {
        pageLen = (ULONG)data[pos + 1] + 2;
        if (pos + pageLen > listLen) break;
        if ((data[pos] & 0x3F) == MMC_MODE_PAGE_CACHING && pageLen >= 3) {
            wce = (BOOLEAN)((data[pos + 2] & 0x04) != 0);
            if (wce && !Slot->EmmcCache) {
                MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_INVALID_PARAM_FIELD, 0);
                return;
            }
            if (Slot->EmmcCache && wce != Slot->CacheOn) {
                Slot->WantCache = (UCHAR)(wce ? 1 : 2);
                if (MmcSetCache(HwInit, Slot, wce) != MMC_OK) {
                    MmcFailWithSense(Slot, Srb, SCSI_SENSE_HARDWARE_ERROR, MMC_ASC_INTERNAL_FAILURE, 0);
                    return;
                }
            }
        }
        pos += pageLen;
    }
    MmcGood(Srb);
}

/* ------------------------------------------------------------------ */
/* Reads, writes, verify                                               */
/* ------------------------------------------------------------------ */

/* TRIM mode: a write whose data is the pattern, repeated from the first
   byte, over its whole length */
static BOOLEAN
MmcIsTrimPattern(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR Data, IN ULONG Length)
{
    ULONG i, j;

    if (Length < MMC_TRIM_PATTERN_SIZE) return FALSE;
    for (i = 0; i < Length; i += MMC_TRIM_PATTERN_SIZE) {
        for (j = 0; j < MMC_TRIM_PATTERN_SIZE && i + j < Length; j += 4) {
            if (*(ULONG UNALIGNED *)(Data + i + j) != *(ULONG UNALIGNED *)(&HwInit->TrimPattern[j])) return FALSE;
        }
    }
    return TRUE;
}

static VOID
MmcScsiReadWrite(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb,
                 IN MMC_LBA Lba, IN ULONG Count, IN BOOLEAN IsWrite, IN BOOLEAN Fua)
{
    ULONG result;
    UCHAR status;

    if (Count == 0) {
        MmcGood(Srb);
        return;
    }
    if (!MmcLbaRangeValid(Slot, Lba, Count)) {
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_LBA_OUT_OF_RANGE, 0);
        return;
    }
    if (Count > Srb->DataTransferLength / MMC_BLOCK_SIZE || Srb->DataBuffer == NULL) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }
    if (IsWrite && Slot->WriteProtected) {
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_DATA_PROTECT, MMC_ASC_WRITE_PROTECTED, 0);
        return;
    }
    if (IsWrite && HwInit->TrimMode && HwInit->UnmapAllowed &&
        MmcIsTrimPattern(HwInit, (PUCHAR)Srb->DataBuffer, Count * MMC_BLOCK_SIZE)) {
#if MMC_HAS_CQ
        MmcCqHalt(HwInit, Slot);
#endif
        MmcCompleteResult(Slot, Srb, MmcUnmap(HwInit, Slot, Lba, Count), TRUE);
        return;
    }

#if MMC_HAS_CQ
    /* Command queuing: a task; anything it cannot take goes on below with
       the engine halted */
    if (Slot->CqActive) {
        status = MmcCqIssue(HwInit, Slot, Srb, Lba, Count, IsWrite, Fua);
        if (status == SRB_STATUS_PENDING) return;
        if (status != SRB_STATUS_INVALID_REQUEST) {
            Srb->SrbStatus = status;
            return;
        }
        MmcCqHalt(HwInit, Slot);
    }
#endif

    /* Interrupt-completed: a plain read or write (FUA with the cache on
       needs the flush afterwards and stays polled) */
    if (Slot->Async && !HwInit->DumpMode && !(IsWrite && Fua && Slot->CacheOn)) {
        status = MmcIssueAsync(HwInit, Slot, Srb, Lba, Count, IsWrite);
        if (status == SRB_STATUS_PENDING) return;
        if (status != SRB_STATUS_INVALID_REQUEST) {
            Srb->SrbStatus = status;
            return;
        }
    }

    result = MmcReadWrite(HwInit, Slot, Srb, Srb->DataBuffer, Lba, Count, IsWrite);
    if (result == MMC_OK && IsWrite && Fua) result = MmcFlush(HwInit, Slot);
    MmcCompleteResult(Slot, Srb, result, IsWrite);
}

static VOID
MmcScsiVerify(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb,
              IN MMC_LBA Lba, IN ULONG Count)
{
    if (Srb->Cdb[1] & 0x02) {                   /* BYTCHK: compare with data from the host */
        MmcInvalidField(Slot, Srb);
        return;
    }
    if (Count == 0) {
        MmcGood(Srb);
        return;
    }
    if (!MmcLbaRangeValid(Slot, Lba, Count)) {
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_LBA_OUT_OF_RANGE, 0);
        return;
    }
    MmcCompleteResult(Slot, Srb, MmcVerify(HwInit, Slot, Lba, Count), FALSE);
}

/* UNMAP: every block descriptor is discarded, trimmed or erased */
static VOID
MmcScsiUnmap(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR param = (PUCHAR)Srb->DataBuffer;
    ULONG listLen = ((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8], descLen, n, i, count, result;
    MMC_LBA lba;

    if (!HwInit->UnmapAllowed) {
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_INVALID_OPCODE, 0);
        return;
    }
    if (Slot->WriteProtected) {
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_DATA_PROTECT, MMC_ASC_WRITE_PROTECTED, 0);
        return;
    }
    if (listLen > Srb->DataTransferLength) listLen = Srb->DataTransferLength;
    if (listLen == 0 || param == NULL) {
        MmcGood(Srb);
        return;
    }
    if (listLen < 8) {
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_PARAM_LIST_LENGTH, 0);
        return;
    }
    descLen = ((ULONG)param[2] << 8) | param[3];
    if (descLen > listLen - 8) descLen = listLen - 8;
    n = descLen / 16;
    if (n > MMC_UNMAP_MAX_DESCRIPTORS) {
        MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_INVALID_PARAM_FIELD, 0);
        return;
    }
    for (i = 0; i < n; i++) {
        PUCHAR d = param + 8 + i * 16;
        lba = MmcLbaFromBigEndian(d, 8);
        count = MmcBigEndian32(d + 8);
        if (count == 0) continue;
        if (count > MMC_UNMAP_MAX_BLOCKS || !MmcLbaRangeValid(Slot, lba, count)) {
            MmcFailWithSense(Slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_LBA_OUT_OF_RANGE, 0);
            return;
        }
    }
    for (i = 0; i < n; i++) {
        PUCHAR d = param + 8 + i * 16;
        lba = MmcLbaFromBigEndian(d, 8);
        count = MmcBigEndian32(d + 8);
        result = MmcUnmap(HwInit, Slot, lba, count);
        if (result != MMC_OK) {
            MmcCompleteResult(Slot, Srb, result, TRUE);
            return;
        }
    }
    MmcGood(Srb);
}

/* LOG SENSE: supported pages, solid state media (eMMC life time),
   informational exceptions (eMMC pre-EOL) */
static VOID
MmcHandleLogSense(IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR pageCode = (UCHAR)(Srb->Cdb[2] & 0x3F), buf[16], life;
    ULONG allocLen = ((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8], len, used;
    BOOLEAN health = (BOOLEAN)(Slot->PreEol != 0 || Slot->LifeTimeA != 0 || Slot->LifeTimeB != 0);

    ZeroMemoryBytes(buf, sizeof(buf));
    buf[0] = pageCode;
    switch (pageCode) {
    case MMC_LOG_PAGE_SUPPORTED:
        buf[4] = MMC_LOG_PAGE_SUPPORTED;
        len = 5;
        if (health) buf[len++] = MMC_LOG_PAGE_SOLID_STATE;
        buf[len++] = MMC_LOG_PAGE_INFO_EXCEPTIONS;
        break;
    case MMC_LOG_PAGE_SOLID_STATE:
        if (!health) {
            MmcInvalidField(Slot, Srb);
            return;
        }
        /* Percentage used endurance indicator: the larger estimate, in
           steps of 10 % (0Bh: beyond the rated life) */
        life = (Slot->LifeTimeA > Slot->LifeTimeB) ? Slot->LifeTimeA : Slot->LifeTimeB;
        used = (life >= 1) ? (ULONG)life * 10 : 0;
        if (used > 255) used = 255;
        buf[5] = 0x01;
        buf[6] = 0x03;
        buf[7] = 4;
        buf[11] = (UCHAR)used;
        len = 12;
        break;
    case MMC_LOG_PAGE_INFO_EXCEPTIONS:
        buf[6] = 0x03;
        buf[7] = 3;
        if (Slot->PreEol >= 3 || Slot->LifeTimeA >= 0x0B || Slot->LifeTimeB >= 0x0B) {
            buf[8] = MMC_ASC_FAILURE_PREDICTION;
        }
        buf[10] = 0xFF;                         /* temperature not reported */
        len = 11;
        break;
    default:
        MmcInvalidField(Slot, Srb);
        return;
    }
    buf[2] = (UCHAR)((len - 4) >> 8);
    buf[3] = (UCHAR)(len - 4);
    if (len > allocLen) len = allocLen;
    MmcCopyToSrb(Srb, buf, len);
    MmcGood(Srb);
}

static VOID
MmcStartStop(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR pc = (UCHAR)(Srb->Cdb[4] >> 4);
    BOOLEAN start = (BOOLEAN)((Srb->Cdb[4] & 0x01) != 0);
    ULONG result = MMC_OK;

    if ((pc == 0 && !start) || pc == 0x3 || pc == 0xB) {
        result = MmcSleep(HwInit, Slot, TRUE);
    } else if ((pc == 0 && start) || pc == 0x1) {
        if (!Slot->CardReady) result = MmcSleep(HwInit, Slot, FALSE);
    }
    MmcCompleteResult(Slot, Srb, result, FALSE);
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

VOID
MmcSatlProcessSrb(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR cdb = Srb->Cdb;
    PMMC_SLOT slot;
    MMC_LBA lba;
    ULONG count;
    BOOLEAN fua = (BOOLEAN)((cdb[1] & 0x08) != 0);

    if (Srb->PathId != 0 || Srb->TargetId >= HwInit->SlotCount || !HwInit->Slots[Srb->TargetId].Present) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return;
    }
    if (Srb->Lun != 0) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        return;
    }
    slot = &HwInit->Slots[Srb->TargetId];

    /* A card set up again after a power transition, an error or a stop */
    if (slot->NeedsInit && slot->Outstanding == NULL) {
        MMC_DBG((MMC_PFX "Slot %lu: setting the card up again\n", (ULONG)slot->Index));
        MmcCardInit(HwInit, slot);
    }
    if (!slot->CardReady) {
        /* No card at all since start: the target does not exist */
        if (slot->CardType == MMC_CARD_NONE && cdb[0] == SCSIOP_INQUIRY) {
            Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
            return;
        }
        if (cdb[0] != SCSIOP_REQUEST_SENSE && cdb[0] != SCSIOP_INQUIRY && cdb[0] != MMC_SCSIOP_REPORT_LUNS) {
            if (cdb[0] == MMC_SCSIOP_START_STOP_UNIT) {
                MmcStartStop(HwInit, slot, Srb);
            } else {
                MmcNoMedium(slot, Srb);
            }
            return;
        }
    }
    if (cdb[0] != SCSIOP_REQUEST_SENSE) slot->SensePending = FALSE;

    switch (cdb[0]) {
    case SCSIOP_INQUIRY:
        MmcHandleInquiry(HwInit, slot, Srb);
        return;
    case SCSIOP_REQUEST_SENSE:
        MmcHandleRequestSense(slot, Srb);
        return;
    case MMC_SCSIOP_REPORT_LUNS:
        MmcHandleReportLuns(Srb);
        return;
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_MEDIUM_REMOVAL:
        MmcGood(Srb);
        return;
    case SCSIOP_SYNCHRONIZE_CACHE:
    case MMC_SCSIOP_SYNC_CACHE16:
        MmcCompleteResult(slot, Srb, MmcFlush(HwInit, slot), TRUE);
        return;
    case MMC_SCSIOP_START_STOP_UNIT:
        MmcStartStop(HwInit, slot, Srb);
        return;
    case SCSIOP_READ_CAPACITY:
        MmcHandleReadCapacity10(HwInit, slot, Srb);
        return;
    case MMC_SCSIOP_SERVICE_ACTION_IN16:
        if ((cdb[1] & 0x1F) == MMC_SA_READ_CAPACITY16) {
            MmcHandleReadCapacity16(HwInit, slot, Srb);
            return;
        }
        break;
    case SCSIOP_MODE_SENSE:
    case SCSIOP_MODE_SENSE10:
        MmcHandleModeSense(slot, Srb);
        return;
    case MMC_SCSIOP_MODE_SELECT6:
    case MMC_SCSIOP_MODE_SELECT10:
        MmcHandleModeSelect(HwInit, slot, Srb);
        return;
    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
        lba.High = 0;
        lba.Low = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
        count = cdb[4] ? cdb[4] : 256;
        MmcScsiReadWrite(HwInit, slot, Srb, lba, count, (BOOLEAN)(cdb[0] == SCSIOP_WRITE6), FALSE);
        return;
    case SCSIOP_READ:
    case SCSIOP_WRITE:
        lba = MmcLbaFromBigEndian(&cdb[2], 4);
        count = ((ULONG)cdb[7] << 8) | cdb[8];
        MmcScsiReadWrite(HwInit, slot, Srb, lba, count, (BOOLEAN)(cdb[0] == SCSIOP_WRITE), fua);
        return;
    case MMC_SCSIOP_READ12:
    case MMC_SCSIOP_WRITE12:
        lba = MmcLbaFromBigEndian(&cdb[2], 4);
        count = MmcBigEndian32(&cdb[6]);
        MmcScsiReadWrite(HwInit, slot, Srb, lba, count, (BOOLEAN)(cdb[0] == MMC_SCSIOP_WRITE12), fua);
        return;
    case MMC_SCSIOP_READ16:
    case MMC_SCSIOP_WRITE16:
        lba = MmcLbaFromBigEndian(&cdb[2], 8);
        count = MmcBigEndian32(&cdb[10]);
        MmcScsiReadWrite(HwInit, slot, Srb, lba, count, (BOOLEAN)(cdb[0] == MMC_SCSIOP_WRITE16), fua);
        return;
    case SCSIOP_VERIFY:
        MmcScsiVerify(HwInit, slot, Srb, MmcLbaFromBigEndian(&cdb[2], 4), ((ULONG)cdb[7] << 8) | cdb[8]);
        return;
    case MMC_SCSIOP_VERIFY12:
        MmcScsiVerify(HwInit, slot, Srb, MmcLbaFromBigEndian(&cdb[2], 4), MmcBigEndian32(&cdb[6]));
        return;
    case MMC_SCSIOP_VERIFY16:
        MmcScsiVerify(HwInit, slot, Srb, MmcLbaFromBigEndian(&cdb[2], 8), MmcBigEndian32(&cdb[10]));
        return;
    case MMC_SCSIOP_UNMAP:
        MmcScsiUnmap(HwInit, slot, Srb);
        return;
    case MMC_SCSIOP_LOG_SENSE:
        MmcHandleLogSense(slot, Srb);
        return;
    }
    MmcFailWithSense(slot, Srb, SCSI_SENSE_ILLEGAL_REQUEST, MMC_ASC_INVALID_OPCODE, 0);
}

/* A transfer the interrupt (or the watchdog) finished */
VOID
MmcSatlCompleteAsync(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb, IN ULONG Result)
{
    MmcCompleteResult(Slot, Srb, Result, Slot->ActiveWrite);
    ScsiPortNotification(RequestComplete, HwInit, Srb);
}

/* The request that waited for the slot */
BOOLEAN
MmcSatlStartPending(IN PHW_DEVICE_EXTENSION HwInit, IN PMMC_SLOT Slot, IN PSCSI_REQUEST_BLOCK Srb)
{
    (VOID)Slot;
    Srb->SrbStatus = SRB_STATUS_PENDING;
    MmcSatlProcessSrb(HwInit, Srb);
    if (Srb->SrbStatus != SRB_STATUS_PENDING) {
        ScsiPortNotification(RequestComplete, HwInit, Srb);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Miniport IOCTLs ("MMCSDNT ")                                        */
/* ------------------------------------------------------------------ */

static BOOLEAN
MmcSignatureIs(IN PMMC_SRB_IO_CONTROL Ctl, IN PCHAR Signature)
{
    ULONG i;
    for (i = 0; i < 8; i++) {
        if (Ctl->Signature[i] != (UCHAR)Signature[i]) return FALSE;
    }
    return TRUE;
}

static VOID
MmcFillInfo(IN PHW_DEVICE_EXTENSION HwInit, OUT PMMC_IOCTL_INFO Info)
{
    ULONG s;

    ZeroMemoryBytes(Info, sizeof(MMC_IOCTL_INFO));
    Info->Version = MMC_DRIVER_VERSION;
    Info->Flags = (HwInit->AsyncMode ? MMC_INFO_ASYNC : 0) | (HwInit->Stopped ? MMC_INFO_POWERED_DOWN : 0) |
                  (HwInit->Acpi ? MMC_INFO_ACPI : 0) | (HwInit->TrimMode ? MMC_INFO_TRIM_MODE : 0) |
                  (HwInit->Dma64 ? MMC_INFO_DMA64 : 0);
    Info->SlotCount = HwInit->SlotCount;
    Info->PciVendorDevice = HwInit->HasPci ? (((ULONG)HwInit->DeviceId << 16) | HwInit->VendorId) : 0;
    Info->RegisterBase = HwInit->Slots[0].RegsPhysical;
    Info->Irq = HwInit->Irq;
    Info->InterruptCount = HwInit->InterruptCount;
    for (s = 0; s < HwInit->SlotCount; s++) {
        PMMC_SLOT slot = &HwInit->Slots[s];
        PMMC_IOCTL_SLOT o = &Info->Slot[s];
        o->CardType = slot->CardReady ? slot->CardType : MMC_CARD_NONE;
        o->BusWidth = slot->BusWidth;
        o->Dma = (slot->Xfer == MMC_XFER_ADMA2 && slot->Adma64) ? MMC_XFER_ADMA2_64 : slot->Xfer;
        o->HighSpeed = slot->HighSpeed;
        o->WriteProtected = slot->WriteProtected;
        o->CacheOn = slot->CacheOn;
        o->HostVersion = slot->HostVersion;
        o->DmaErrors = slot->DmaErrors;
        o->ClockKhz = slot->ClockKhz;
        o->SectorsLow = slot->Sectors.Low;
        o->SectorsHigh = slot->Sectors.High;
        CopyMemoryBytes(o->Cid, slot->Cid, sizeof(o->Cid));
        CopyMemoryBytes(o->Csd, slot->Csd, sizeof(o->Csd));
        o->Ocr = slot->Ocr;
        o->Capabilities = slot->Caps;
        o->Retries = slot->Retries;
        o->ExtCsdRev = slot->ExtCsdRev;
        o->PreEol = slot->PreEol;
        o->LifeTimeA = slot->LifeTimeA;
        o->LifeTimeB = slot->LifeTimeB;
        o->Timing = slot->Timing;
        o->EraseMode = slot->EraseMode;
        o->Signal18 = slot->Signal18;
        o->PowerOffNotify = slot->PowerOffNotify;
        o->CqState = (UCHAR)((slot->CqRegs != NULL ? MMC_CQ_STATE_ENGINE : 0) | (slot->CqActive ? MMC_CQ_STATE_ACTIVE : 0) |
                             (slot->NoCq ? MMC_CQ_STATE_OFF : 0));
        o->CqDepth = slot->CqActive ? slot->CqDepth : 0;
        o->CqPeak = slot->CqPeak;
    }
}

VOID
MmcProcessIoControl(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb)
{
    PMMC_SRB_IO_CONTROL ctl = (PMMC_SRB_IO_CONTROL)Srb->DataBuffer;
    ULONG available;

    if (ctl == NULL || Srb->DataTransferLength < sizeof(MMC_SRB_IO_CONTROL) ||
        ctl->HeaderLength < sizeof(MMC_SRB_IO_CONTROL) || ctl->HeaderLength > Srb->DataTransferLength) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }
    available = Srb->DataTransferLength - ctl->HeaderLength;
    if (available > ctl->Length) available = ctl->Length;

    if (!MmcSignatureIs(ctl, MMC_IOCTL_SIGNATURE)) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }
    /* The controller is not woken up for these: an ACPI driver may query
       it while it is powered down */
    ctl->ReturnCode = 0;
    switch (ctl->ControlCode) {
    case MMC_IOCTL_GET_INFO:
        if (available >= sizeof(MMC_IOCTL_INFO)) {
            MmcFillInfo(HwInit, (PMMC_IOCTL_INFO)((PUCHAR)ctl + ctl->HeaderLength));
        } else {
            ctl->ReturnCode = 1;
        }
        break;
    case MMC_IOCTL_POWER_DOWN:
        MmcPowerDown(HwInit);
        break;
    case MMC_IOCTL_POWER_UP:
        if (HwInit->Stopped) MmcReinitialize(HwInit);
        break;
    case MMC_IOCTL_TRIM_MODE_ON:
        if (available < MMC_TRIM_PATTERN_SIZE || !HwInit->UnmapAllowed) {
            ctl->ReturnCode = 1;
            break;
        }
        CopyMemoryBytes(HwInit->TrimPattern, (PUCHAR)ctl + ctl->HeaderLength, MMC_TRIM_PATTERN_SIZE);
        HwInit->TrimMode = TRUE;
        break;
    case MMC_IOCTL_TRIM_MODE_OFF:
        HwInit->TrimMode = FALSE;
        break;
    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
}
