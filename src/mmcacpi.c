#include "mmcsd.h"

/* mmcacpi.c - SD host controllers that only the ACPI tables describe
   (no PCI function): RSDP, RSDT/XSDT, DSDT and SSDTs are read, devices
   with a known SD host _HID (or _CID PNP0D40) are looked for in the AML,
   and their register block and interrupt taken from a static _CRS
   (a resource template under _CRS, or a named one a _CRS method
   returns). No AML is executed. NT 3.1-4.0 and 9x; 2000/XP get the
   resources from their own ACPI driver. */

#if defined(MMC_NT4)

PUCHAR MmcMapPhys(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Physical, IN ULONG Length);
VOID   MmcUnmapPhys(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR Virtual);

#define ACPI_HEADER_LENGTH      36
#define ACPI_MAX_TABLES         32
#define ACPI_MAX_TABLE_LENGTH   0x00100000

static const PCHAR g_MmcAcpiHids[] = {
    "PNP0D40",                  /* SD host controller (standard) */
    "80860F14",                 /* Intel Bay Trail / Cherry Trail eMMC, SD */
    "80860F16",
    "INT33BB",
    "INT3436",
    "80865ACA",                 /* Intel Broxton / Apollo Lake */
    "80865ACC",
    "80865AD0",
    "AMDI0040",                 /* AMD eMMC */
    NULL
};

static ULONG
MmcRead32(IN PUCHAR P)
{
    return (ULONG)P[0] | ((ULONG)P[1] << 8) | ((ULONG)P[2] << 16) | ((ULONG)P[3] << 24);
}

static BOOLEAN
MmcChecksumOk(IN PUCHAR P, IN ULONG Length)
{
    UCHAR sum = 0;
    while (Length--) sum = (UCHAR)(sum + *P++);
    return (BOOLEAN)(sum == 0);
}

static BOOLEAN
MmcSame(IN PUCHAR A, IN PCHAR B, IN ULONG N)
{
    ULONG i;
    for (i = 0; i < N; i++) if (A[i] != (UCHAR)B[i]) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* AML                                                                 */
/* ------------------------------------------------------------------ */

/* PkgLength at P: the length it encodes (counted from P), and its size */
static ULONG
MmcPkgLength(IN PUCHAR P, IN PUCHAR End, OUT PULONG Bytes)
{
    ULONG lead, count, len, i;

    if (P >= End) return 0;
    lead = P[0];
    count = lead >> 6;
    *Bytes = count + 1;
    if (P + count >= End) return 0;
    if (count == 0) return lead & 0x3F;
    len = lead & 0x0F;
    for (i = 0; i < count; i++) len |= (ULONG)P[1 + i] << (4 + 8 * i);
    return len;
}

/* Size of the NameString at P */
static ULONG
MmcNameStringSize(IN PUCHAR P, IN PUCHAR End)
{
    PUCHAR q = P;

    while (q < End && (*q == '\\' || *q == '^')) q++;
    if (q >= End) return 0;
    if (*q == 0x00) return (ULONG)(q + 1 - P);
    if (*q == 0x2E) return (ULONG)(q + 1 + 8 - P);
    if (*q == 0x2F) return (q + 1 < End) ? (ULONG)(q + 2 + 4 * q[1] - P) : 0;
    return (ULONG)(q + 4 - P);
}

/* Last NameSeg of the NameString at P */
static PUCHAR
MmcLastSeg(IN PUCHAR P, IN ULONG Size)
{
    return (Size >= 4) ? P + Size - 4 : NULL;
}

/* _HID/_CID value: a string, or a compressed EISA id (DWordConst) */
static BOOLEAN
MmcAmlId(IN PUCHAR P, IN PUCHAR End, OUT PCHAR Id)
{
    static const CHAR hex[] = "0123456789ABCDEF";
    ULONG i, v;

    if (P >= End) return FALSE;
    if (P[0] == 0x0D) {                         /* String */
        for (i = 0; i < 8 && P + 1 + i < End && P[1 + i] != 0; i++) Id[i] = (CHAR)P[1 + i];
        Id[i] = 0;
        return (BOOLEAN)(i > 0);
    }
    if (P[0] == 0x0C && P + 4 < End) {          /* EISA id */
        v = MmcRead32(P + 1);
        Id[0] = (CHAR)(((v >> 2) & 0x1F) + 0x40);
        Id[1] = (CHAR)((((v & 0x03) << 3) | ((v >> 13) & 0x07)) + 0x40);
        Id[2] = (CHAR)(((v >> 8) & 0x1F) + 0x40);
        Id[3] = hex[(v >> 20) & 0x0F];
        Id[4] = hex[(v >> 16) & 0x0F];
        Id[5] = hex[(v >> 28) & 0x0F];
        Id[6] = hex[(v >> 24) & 0x0F];
        Id[7] = 0;
        return TRUE;
    }
    return FALSE;
}

static BOOLEAN
MmcKnownHid(IN PCHAR Id)
{
    ULONG i, j;

    for (i = 0; g_MmcAcpiHids[i] != NULL; i++) {
        for (j = 0; g_MmcAcpiHids[i][j] != 0 && Id[j] == g_MmcAcpiHids[i][j]; j++) ;
        if (g_MmcAcpiHids[i][j] == 0 && Id[j] == 0) return TRUE;
    }
    return FALSE;
}

/* A resource template: memory range and interrupt */
static BOOLEAN
MmcParseResources(IN PUCHAR P, IN ULONG Length, OUT PMMC_ACPI_DEVICE Dev)
{
    PUCHAR end = P + Length;
    ULONG len, t, mask, i;

    while (P < end) {
        if (P[0] & 0x80) {
            if (P + 3 > end) break;
            t = P[0];
            len = (ULONG)P[1] | ((ULONG)P[2] << 8);
            if (P + 3 + len > end) break;
            if (t == 0x86 && len >= 9 && Dev->MmioBase == 0) {            /* Memory32Fixed */
                Dev->MmioBase = MmcRead32(P + 4);
                Dev->MmioLength = MmcRead32(P + 8);
            } else if (t == 0x85 && len >= 17 && Dev->MmioBase == 0) {    /* Memory32 */
                Dev->MmioBase = MmcRead32(P + 4);
                Dev->MmioLength = MmcRead32(P + 16);
            } else if (t == 0x87 && len >= 23 && P[3] == 0 && Dev->MmioBase == 0) {   /* DWord memory */
                Dev->MmioBase = MmcRead32(P + 10);
                Dev->MmioLength = MmcRead32(P + 22);
            } else if (t == 0x8A && len >= 43 && P[3] == 0 && Dev->MmioBase == 0 &&
                       MmcRead32(P + 18) == 0) {                          /* QWord memory below 4 GB */
                Dev->MmioBase = MmcRead32(P + 14);
                Dev->MmioLength = MmcRead32(P + 38);
            } else if (t == 0x89 && len >= 6 && Dev->Irq == 0 && P[4] >= 1) {      /* Extended interrupt */
                Dev->Irq = MmcRead32(P + 5);
                Dev->LevelIrq = (BOOLEAN)((P[3] & 0x02) == 0);
            }
            P += 3 + len;
        } else {
            t = (P[0] >> 3) & 0x0F;
            len = P[0] & 0x07;
            if (P + 1 + len > end) break;
            if (t == 0x0F) break;                                         /* End tag */
            if (t == 0x04 && len >= 2 && Dev->Irq == 0) {                 /* IRQ */
                mask = (ULONG)P[1] | ((ULONG)P[2] << 8);
                for (i = 0; i < 16; i++) {
                    if (mask & (1UL << i)) {
                        Dev->Irq = i;
                        Dev->LevelIrq = (BOOLEAN)(len >= 3 && (P[3] & 0x01) == 0);
                        break;
                    }
                }
            }
            P += 1 + len;
        }
    }
    return (BOOLEAN)(Dev->MmioBase != 0);
}

/* BufferOp at P: its byte list */
static BOOLEAN
MmcAmlBuffer(IN PUCHAR P, IN PUCHAR End, OUT PUCHAR *Data, OUT PULONG Length)
{
    ULONG pkg, pkgBytes, skip;
    PUCHAR body, bodyEnd;

    if (P >= End || P[0] != 0x11) return FALSE;
    pkg = MmcPkgLength(P + 1, End, &pkgBytes);
    if (pkg == 0) return FALSE;
    body = P + 1 + pkgBytes;
    bodyEnd = P + 1 + pkg;
    if (bodyEnd > End || body >= bodyEnd) return FALSE;
    /* BufferSize: an integer constant */
    switch (body[0]) {
    case 0x00: case 0x01: case 0xFF: skip = 1; break;
    case 0x0A: skip = 2; break;
    case 0x0B: skip = 3; break;
    case 0x0C: skip = 5; break;
    default: return FALSE;
    }
    if (body + skip > bodyEnd) return FALSE;
    *Data = body + skip;
    *Length = (ULONG)(bodyEnd - *Data);
    return TRUE;
}

/* NameOp (shallow, nested devices skipped) for Seg in [P, End) */
static PUCHAR
MmcFindName(IN PUCHAR P, IN PUCHAR End, IN PUCHAR Seg)
{
    ULONG size, pkg, pkgBytes;

    while (P + 5 < End) {
        if (P[0] == 0x5B && P[1] == 0x82) {                 /* nested device: skip */
            pkg = MmcPkgLength(P + 2, End, &pkgBytes);
            if (pkg == 0) {
                P++;
                continue;
            }
            P += 2 + pkg;
            continue;
        }
        if (P[0] == 0x08) {
            size = MmcNameStringSize(P + 1, End);
            if (size >= 4 && P + 1 + size < End && MmcSame(MmcLastSeg(P + 1, size), (PCHAR)Seg, 4)) {
                return P + 1 + size;                        /* the value */
            }
        }
        P++;
    }
    return NULL;
}

/* MethodOp _CRS: the buffer it returns, directly or by name */
static BOOLEAN
MmcCrsFromMethod(IN PUCHAR Body, IN PUCHAR BodyEnd, IN PUCHAR DevBody, IN PUCHAR DevEnd,
                 OUT PMMC_ACPI_DEVICE Dev)
{
    PUCHAR p, value, data;
    ULONG size, length;

    for (p = Body; p + 5 < BodyEnd; p++) {
        if (p[0] != 0xA4) continue;                         /* ReturnOp */
        if (MmcAmlBuffer(p + 1, BodyEnd, &data, &length)) return MmcParseResources(data, length, Dev);
        size = MmcNameStringSize(p + 1, BodyEnd);
        if (size >= 4) {
            value = MmcFindName(DevBody, DevEnd, MmcLastSeg(p + 1, size));
            if (value != NULL && MmcAmlBuffer(value, DevEnd, &data, &length)) {
                return MmcParseResources(data, length, Dev);
            }
        }
    }
    return FALSE;
}

/* One Device(): known _HID/_CID and a usable _CRS */
static BOOLEAN
MmcExamineDevice(IN PUCHAR Body, IN PUCHAR End, OUT PMMC_ACPI_DEVICE Dev)
{
    PUCHAR value, data, p;
    ULONG length, size, pkg, pkgBytes;
    CHAR id[9];
    BOOLEAN match = FALSE;

    ZeroMemoryBytes(Dev, sizeof(*Dev));
    value = MmcFindName(Body, End, (PUCHAR)"_HID");
    if (value == NULL || !MmcAmlId(value, End, id)) return FALSE;
    if (MmcKnownHid(id)) match = TRUE;
    CopyMemoryBytes(Dev->Hid, id, sizeof(id));
    if (!match) {
        value = MmcFindName(Body, End, (PUCHAR)"_CID");
        if (value != NULL && MmcAmlId(value, End, id) && MmcKnownHid(id)) match = TRUE;
    }
    if (!match) return FALSE;

    /* Name(_STA, 0): disabled */
    value = MmcFindName(Body, End, (PUCHAR)"_STA");
    if (value != NULL && value[0] == 0x00) return FALSE;

    value = MmcFindName(Body, End, (PUCHAR)"_CRS");
    if (value != NULL && MmcAmlBuffer(value, End, &data, &length)) {
        return MmcParseResources(data, length, Dev);
    }

    /* Method(_CRS) */
    for (p = Body; p + 6 < End; p++) {
        if (p[0] == 0x5B && p[1] == 0x82) {
            pkg = MmcPkgLength(p + 2, End, &pkgBytes);
            if (pkg != 0) p += 1 + pkg;
            continue;
        }
        if (p[0] != 0x14) continue;
        pkg = MmcPkgLength(p + 1, End, &pkgBytes);
        if (pkg == 0 || p + 1 + pkg > End) continue;
        size = MmcNameStringSize(p + 1 + pkgBytes, End);
        if (size < 4 || !MmcSame(MmcLastSeg(p + 1 + pkgBytes, size), "_CRS", 4)) continue;
        /* name, then the flags byte, then the method body */
        if (MmcCrsFromMethod(p + 1 + pkgBytes + size + 1, p + 1 + pkg, Body, End, Dev)) return TRUE;
        break;
    }
    MMC_DBG((MMC_PFX "ACPI: %s without a register block in a static _CRS\n", Dev->Hid));
    return FALSE;
}

static ULONG
MmcScanAml(IN PUCHAR Aml, IN ULONG Length, OUT PMMC_ACPI_DEVICE Devices, IN ULONG Count, IN ULONG Max)
{
    PUCHAR p = Aml, end = Aml + Length, body, devEnd;
    ULONG pkg, pkgBytes, size, i;
    MMC_ACPI_DEVICE dev;

    for (; p + 6 < end && Count < Max; p++) {
        if (p[0] != 0x5B || p[1] != 0x82) continue;
        pkg = MmcPkgLength(p + 2, end, &pkgBytes);
        if (pkg < pkgBytes + 4 || p + 2 + pkg > end) continue;
        size = MmcNameStringSize(p + 2 + pkgBytes, end);
        if (size < 4) continue;
        body = p + 2 + pkgBytes + size;
        devEnd = p + 2 + pkg;
        if (body >= devEnd) continue;
        if (!MmcExamineDevice(body, devEnd, &dev)) continue;
        for (i = 0; i < Count; i++) if (Devices[i].MmioBase == dev.MmioBase) break;
        if (i < Count) continue;
        if (dev.MmioLength < SDHCI_REG_SIZE) dev.MmioLength = SDHCI_REG_SIZE;
        Devices[Count++] = dev;
        MMC_DBG((MMC_PFX "ACPI: %s at %08lX, interrupt %lu (%s)\n", dev.Hid, dev.MmioBase, dev.Irq,
                 dev.LevelIrq ? "level" : "edge"));
    }
    return Count;
}

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

static ULONG
MmcFindRsdp(IN PHW_DEVICE_EXTENSION HwInit, OUT PULONG Rsdt, OUT PULONG Xsdt)
{
    PUCHAR bda, area;
    ULONG ebda, base, i, len;

    *Rsdt = 0;
    *Xsdt = 0;
    for (i = 0; i < 2; i++) {
        if (i == 0) {
            bda = MmcMapPhys(HwInit, 0x400, 0x100);
            if (bda == NULL) continue;
            ebda = ((ULONG)bda[0x0E] | ((ULONG)bda[0x0F] << 8)) << 4;
            MmcUnmapPhys(HwInit, bda);
            if (ebda < 0x80000 || ebda >= 0xA0000) continue;
            base = ebda;
            len = 0x400;
        } else {
            base = 0xE0000;
            len = 0x20000;
        }
        area = MmcMapPhys(HwInit, base, len);
        if (area == NULL) continue;
        for (len -= 20; ; ) {
            ULONG off;
            for (off = 0; off <= len; off += 16) {
                PUCHAR r = area + off;
                if (!MmcSame(r, "RSD PTR ", 8) || !MmcChecksumOk(r, 20)) continue;
                *Rsdt = MmcRead32(r + 16);
                if (r[15] >= 2 && off + 36 <= len + 20 && MmcChecksumOk(r, 36) && MmcRead32(r + 28) == 0) {
                    *Xsdt = MmcRead32(r + 24);
                }
                MmcUnmapPhys(HwInit, area);
                return base + off;
            }
            break;
        }
        MmcUnmapPhys(HwInit, area);
    }
    return 0;
}

/* Map a whole table; NULL when it is not there or broken */
static PUCHAR
MmcMapTable(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Physical, OUT PULONG Length)
{
    PUCHAR hdr, table;
    ULONG len;

    if (Physical == 0) return NULL;
    hdr = MmcMapPhys(HwInit, Physical, ACPI_HEADER_LENGTH);
    if (hdr == NULL) return NULL;
    len = MmcRead32(hdr + 4);
    MmcUnmapPhys(HwInit, hdr);
    if (len < ACPI_HEADER_LENGTH || len > ACPI_MAX_TABLE_LENGTH) return NULL;
    table = MmcMapPhys(HwInit, Physical, len);
    if (table == NULL) return NULL;
    if (!MmcChecksumOk(table, len)) {
        MMC_DBG((MMC_PFX "ACPI: table %c%c%c%c at %08lX has a bad checksum, used anyway\n",
                 table[0], table[1], table[2], table[3], Physical));
    }
    *Length = len;
    return table;
}

ULONG
MmcAcpiFindControllers(IN PHW_DEVICE_EXTENSION HwInit, OUT PMMC_ACPI_DEVICE Devices, IN ULONG Max)
{
    ULONG rsdtPhys, xsdtPhys, rsdp, len, n, i, entry, step, count = 0, dsdt = 0, tlen;
    ULONG tables[ACPI_MAX_TABLES];
    ULONG tableCount = 0;
    PUCHAR root, t;

    rsdp = MmcFindRsdp(HwInit, &rsdtPhys, &xsdtPhys);
    if (rsdp == 0) {
        MMC_DBG((MMC_PFX "ACPI: no RSDP\n"));
        return 0;
    }
    root = MmcMapTable(HwInit, xsdtPhys ? xsdtPhys : rsdtPhys, &len);
    if (root == NULL) return 0;
    step = xsdtPhys ? 8 : 4;
    n = (len - ACPI_HEADER_LENGTH) / step;
    for (i = 0; i < n && tableCount < ACPI_MAX_TABLES; i++) {
        entry = MmcRead32(root + ACPI_HEADER_LENGTH + i * step);
        if (step == 8 && MmcRead32(root + ACPI_HEADER_LENGTH + i * step + 4) != 0) continue;
        tables[tableCount++] = entry;
    }
    MmcUnmapPhys(HwInit, root);

    /* FADT: the DSDT */
    for (i = 0; i < tableCount; i++) {
        t = MmcMapTable(HwInit, tables[i], &tlen);
        if (t == NULL) continue;
        if (MmcSame(t, "FACP", 4) && tlen >= 44) {
            dsdt = MmcRead32(t + 40);
            if (tlen >= 148 && MmcRead32(t + 144) == 0 && MmcRead32(t + 140) != 0) dsdt = MmcRead32(t + 140);
        }
        MmcUnmapPhys(HwInit, t);
    }
    t = MmcMapTable(HwInit, dsdt, &tlen);
    if (t != NULL) {
        count = MmcScanAml(t + ACPI_HEADER_LENGTH, tlen - ACPI_HEADER_LENGTH, Devices, count, Max);
        MmcUnmapPhys(HwInit, t);
    }
    for (i = 0; i < tableCount && count < Max; i++) {
        t = MmcMapTable(HwInit, tables[i], &tlen);
        if (t == NULL) continue;
        if (MmcSame(t, "SSDT", 4)) {
            count = MmcScanAml(t + ACPI_HEADER_LENGTH, tlen - ACPI_HEADER_LENGTH, Devices, count, Max);
        }
        MmcUnmapPhys(HwInit, t);
    }
    MMC_DBG((MMC_PFX "ACPI: RSDP at %08lX, %lu table(s), %lu SD host controller(s)\n", rsdp, tableCount, count));
    return count;
}

#else

ULONG
MmcAcpiFindControllers(IN PHW_DEVICE_EXTENSION HwInit, OUT PMMC_ACPI_DEVICE Devices, IN ULONG Max)
{
    (VOID)HwInit;
    (VOID)Devices;
    (VOID)Max;
    return 0;
}

#endif
