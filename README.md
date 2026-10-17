# MMCSDNT — SD/SDHC/SDXC and MMC/eMMC Driver for Windows 95/98/Me and Windows NT 3.1 to XP x64

**MMCSDNT** is a SCSI miniport driver for SD cards and eMMC on standard SD host controllers (SDHCI). It presents every card as a **fixed, bootable disk**, so Windows 95, 98 and Me, Windows NT 3.1, 3.50, 3.51 and 4.0, and Windows 2000, XP and Server 2003 (x86 and x64) can be installed on, boot from and use the eMMC or SD card of tablets, thin clients, netbooks and embedded boards — storage none of these systems knows.

| | |
|---|---|
| **Version** | 1.0.0.0 |
| **Operating systems** | Windows 95 / 98 / Me · Windows NT 3.1 / 3.50 / 3.51 / 4.0 · Windows 2000 / XP / Server 2003 (x86) · Windows XP x64 / Server 2003 x64 |
| **Hardware** | SD host controllers (SDHCI 1.00–3.00) on PCI (class code `08-05`) or described only by ACPI (Intel Bay Trail, Cherry Trail, Braswell, Apollo Lake, AMD and others); up to 6 slots per controller, 8 controllers |
| **Cards** | SD, SDHC and SDXC (up to UHS-I SDR104), MMC and eMMC (up to HS200, HS400 on request) |
| **Binaries** | `mmcsd9x.mpd` (Windows 9x/Me) · `mmcsdnt.sys` (NT 3.1–4.0) · `mmcsdnt.sys` (2000/XP/2003, x86 and x64) |

## Highlights

- **Fixed, bootable disks**: every card is partitioned, formatted, paged to and booted from like a hard disk; F6 / OEM driver disks for text-mode setup from NT 3.1 to XP x64
- **Fast bus modes with automatic fallback**: SD UHS-I (SDR104, DDR50, SDR50) with the 1.8 V switch and tuning, eMMC HS200, DDR52 and HS400; a mode that fails is dropped and the card set up again with the next slower one
- **DMA everywhere it exists**: ADMA2 with scatter/gather (64-bit addressing on 2000/XP/x64), SDMA through a bounce buffer, PIO otherwise; interrupt-completed transfers with a Fast Polling watchdog
- **Hosts only ACPI describes** (SoC tablets and laptops): found through their ACPI device nodes on 2000/XP, and by reading the ACPI tables on NT 3.1–4.0 and 9x
- **eMMC care**: the cache, power-off notification and sleep at shutdown, device life time estimates in LOG SENSE, discard for UNMAP; eMMC 5.1 command queuing on request
- **TRIM** as eMMC discard/trim, SD discard or erase, plus `tools\trimnt.exe` to discard the free space of a volume on systems that never send TRIM
- **One source tree** for all targets; the differences are a handful of `#ifdef`s

## Quick start

| System | Files | How |
|---|---|---|
| Windows 95 / 98 / Me | `bin\9X` | [Installing on Windows 95 / 98 / Me](#windows-95--98--me) |
| Windows NT 3.1 – 4.0 | `bin\NT\floppy` | [Installing on Windows NT](#windows-nt-31--350--351--nt-40) |
| Windows 2000 / XP / 2003, installed | `bin\2KXP` (Have Disk) | [GUI-mode setup](#gui-mode-setup-windows-2000xpxp-x64server-2003) |
| Windows 2000 / XP / 2003, new installation | `bin\2KXP\floppy_i386` or `floppy_amd64` (F6) | [Text-mode (F6) setup](#text-mode-f6-setup--windows-2000xpxp-x64server-2003) |

## Contents

- [Overview](#overview) · [Features](#features) · [Architecture](#architecture) · [Feature matrix by target](#feature-matrix-by-target) · [Known Limitations](#known-limitations)
- [Building from source](#building-from-source) · [Installation](#installation) · [Configuration](#configuration) · [Debugging](#debugging) · [Technical Notes](#technical-notes)
- [Version history](#version-history) · [Related drivers](#related-drivers) · [License](#license)

## Overview

MMCSDNT is a SCSI miniport driver built on the classic `ScsiPort` framework (`SCSIPORT.PDR` on Windows 9x/Me, `scsiport.sys` on the NT family). It drives the SD Host Controller Standard register set (SDHCI 1.00–3.00) of a PCI function (class code `08-05`) or of a host controller that only the ACPI tables describe, brings up SD (SDSC, SDHC, SDXC) and MMC/eMMC cards, and translates the SCSI requests of the Windows storage stack into SD/MMC commands. Transfers use ADMA2 with scatter/gather where the host has it, SDMA through a bounce buffer on SDMA-only hosts, and PIO otherwise.

### Why?

None of these systems knows SD host controllers at all. Windows XP got an SD bus driver only as a late update, and that one presents cards as removable media, which cannot hold a system. Many small machines (tablets, thin clients, netbooks, embedded boards) have nothing but an eMMC or an SD slot as their storage. MMCSDNT lets those machines install and boot these systems from that storage, with the same SCSI translation layer, IOCTL interface and Fast Polling fallback as AHCINT, IDENT and NVMENT.

## Features

- **Host controller support**
  - Any **PCI SD host controller** with class code `08-05` and programming interface `00` (SDHCI, no DMA) or `01` (SDHCI with DMA): Intel, Ricoh, O2Micro, Realtek, JMicron, TI, AMD and others in standard mode
  - **Several slots per PCI function**: one SDHCI register block per BAR, as the slot information register (config space 40h) gives it, up to 6 slots; every slot is one SCSI target
  - **Host controllers only ACPI describes** (SoC hosts such as Intel Bay Trail/Cherry Trail/Braswell, Apollo Lake and AMD): on 2000/XP through their ACPI device nodes (`ACPI\PNP0D40`, `ACPI\80860F14`, `ACPI\80860F16`, `ACPI\INT33BB`, `ACPI\INT3436`, `ACPI\80865ACA`, `ACPI\80865ACC`, `ACPI\80865AD0`, `ACPI\AMDI0040`); on NT 3.1–4.0 and 9x the driver finds them in the ACPI tables itself (see [In-driver ACPI](#in-driver-acpi))
  - Up to 8 host controllers per system (build-time, see [Build-time limits](#build-time-limits)), PCI and ACPI mixed
  - SDHCI specification versions 1.00, 2.00 and 3.00; the clock divider of each (power-of-two 8-bit divider, 10-bit divided clock mode); 3.3 V signalling, and 1.8 V signalling for the UHS-I and HS200/HS400 modes on 3.00 hosts that offer them
  - **64-bit ADMA2** (2000/XP/x64) on SDHCI 3.00 hosts with the 64-bit system bus capability: buffers above 4 GB go to the host directly (96-bit descriptors)
  - **eMMC command queuing engine** (CQHCI, eMMC 5.1 hosts; opt-in with `CQ=1`, 2000/XP/x64), see [Command queuing](#command-queuing)

- **Card support**
  - **SD**: SDSC (byte-addressed, version 1.x and 2.00), SDHC and SDXC (block-addressed); 4-bit bus from the SCR; **High Speed** (50 MHz) through CMD6 when the card and host support it; **UHS-I** (SDR104, DDR50, SDR50, SDR25) with the 1.8 V switch (CMD11), tuning (CMD19) and the card's current limit; the write-protect switch of a standard slot
  - **MMC and eMMC**: byte-addressed MMC up to 2 GB and sector-addressed eMMC; EXT_CSD read; **8-bit or 4-bit bus** (tried in that order, as the host allows); **High Speed 26/52 MHz**, **DDR52**, **HS200** (1.8 V, tuned with CMD21) and **HS400** (opt-in with `HS400=1`); the **eMMC cache** (eMMC 4.5+) switched on at bring-up, flushed on SYNCHRONIZE CACHE, shutdown and power transitions, and turned on and off through MODE SELECT
  - eMMC **power-off notification** (eMMC 4.5+) at shutdown, **sleep** (CMD5) otherwise; the high-capacity erase group; the device life time estimates (eMMC 5.0) in LOG SENSE
  - Every faster mode falls back by itself: a failed 1.8 V switch powers the card off and sets it up again at 3.3 V; a failed HS200/HS400/DDR52 sets the card up again with the next slower mode (see [Card initialization](#card-initialization))
  - Every card is a **fixed disk** (INQUIRY RMB = 0): Windows partitions, formats, pages and boots from it like from a hard disk
  - Cards larger than 2 TB (SDUC) are not addressed; eMMC boot and RPMB partitions are not exposed (only the user area)

- **Transfer methods**
  - **ADMA2** (32-bit descriptors, or 64-bit on hosts that have them; 64 entries per slot, one per page): scatter/gather straight to and from the request buffer, 128 KB per request, 32 physical breaks
  - **SDMA** on hosts without ADMA2: through a 64 KB bounce buffer
  - **PIO** on hosts without DMA, with `DMA=0`, for a buffer that is not dword-aligned, and after three DMA errors on a slot
  - Multi-block reads and writes with Auto CMD12

- **SCSI translation** — the same layer as AHCINT/IDENT
  - INQUIRY (vendor, product and revision from the card's CID; VPD pages `00h`, `80h` (the serial number from the CID), `B0h` (block and unmap limits), `B1h` (non-rotating) and `B2h` (logical block provisioning)), REQUEST SENSE, TEST UNIT READY, MEDIUM REMOVAL, REPORT LUNS
  - READ CAPACITY(10)/(16) (LBPME), READ/WRITE(6), (10), (12) and (16), VERIFY(10)/(12)/(16) (the blocks are read into a scratch buffer), SYNCHRONIZE CACHE(10)/(16)
  - **UNMAP** as eMMC DISCARD (eMMC 4.5+) or TRIM (4.4), SD DISCARD (SD 5.1), or ERASE of whole erase groups otherwise; pieces sized so each erase command stays within the card's timeout
  - **LOG SENSE** pages `00h`, `11h` (solid state media: percentage used from the eMMC life time estimates) and `2Fh` (informational exceptions: ASC 5Dh when the eMMC reports urgent pre-EOL or a life time beyond its rating)
  - **TRIM mode** for Windows versions that send no TRIM: with `tools\trimnt.exe` the free space of a volume is discarded (see [TRIM mode](#trim-mode))
  - MODE SENSE(6)/(10) with the rigid disk geometry, caching (`08h`, WCE = eMMC cache) and control pages; MODE SELECT on the caching page turns the eMMC cache on and off (refused with ILLEGAL REQUEST on a card without one)
  - START STOP UNIT: stop = flush, deselect, clock off; the next request sets the card up again
  - INQUIRY VERSION `05h` (SPC-3), so the 16-byte CDBs are used where the OS knows them

- **Multi-target support from one source tree**
  - Windows 95 / 98 / Me (x86) — `SCSIPORT.PDR` miniport (`mmcsd9x.mpd`)
  - Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86) — `mmcsdnt.sys` (one binary for all four)
  - Windows 2000 / XP / Server 2003 (x86) and Windows XP x64 Edition / Server 2003 x64 (amd64) — `mmcsdnt.sys`

- **Interrupt-completed ADMA2 with a watchdog** — reads and writes complete from `HwInterrupt` where an interrupt is connected; a 100 ms timer checks the slots while a transfer is outstanding, and after five completions that no interrupt reported the driver turns the interrupt path off and continues with Fast Polling for good; see [I/O model](#io-model)

- **Power management** — flush, eMMC power-off notification (or sleep) and card stand-by on shutdown and on power transitions, `HwAdapterControl` (stop/restart) on 2000/XP, hibernation and crash dump support, re-initialization of a card whose host lost its state, and a power-down/up IOCTL for an ACPI driver on NT 3.1–4.0 and 9x; see [Power management](#power-management)

## Architecture

```
          Windows 9x/Me                         Windows NT/2000/XP

        Application Layer                       Application Layer
               ↓                                       ↓
     IOS (I/O Supervisor) layers                SCSI Class Driver
               ↓                                       ↓
          SCSIPORT.PDR                            ScsiPort.sys
               ↓                                       ↓
          mmcsd9x.mpd                             mmcsdnt.sys
      (SCSI miniport driver)                 (SCSI miniport driver)
               ↓                                       ↓
     SDHCI registers + ADMA2/SDMA          SDHCI registers + ADMA2/SDMA
     (PCI function or ACPI host)           (PCI function or ACPI host)
               ↓                                       ↓
         SD card / eMMC                          SD card / eMMC
```

### Key Components

| File | Purpose |
|---|---|
| `src\mmcsd.h` | SDHCI register layout, SD/MMC command set, EXT_CSD indices, data structures (`HW_DEVICE_EXTENSION`, `MMC_SLOT`, `MMC_REQUEST`), IOCTL structures, limits |
| `src\mmcmain.c` | `DriverEntry`, `HwFindAdapter` (PCI scan and PnP resources, ACPI hosts), DMA area, `HwInitialize`, `HwStartIo`, `HwInterrupt`, the watchdog timer, `HwResetBus`, `HwAdapterControl`, NT 3.1 compatibility, the 9x VMM services |
| `src\mmccmd.c` | Host bring-up (reset, power, clock), command execution, ADMA2/SDMA/PIO data phases, SD and MMC card initialization (OCR, CID/CSD, SCR, EXT_CSD, bus width, high speed, cache), read/write/verify/flush, interrupt completion, recovery |
| `src\mmcsatl.c` | SCSI-to-SD/MMC translation, sense data, MODE SENSE/SELECT, the IOCTLs |
| `src\mmcacpi.c` | NT 3.1–4.0 and 9x: the ACPI table walk that finds SD host controllers (see [In-driver ACPI](#in-driver-acpi)) |
| `src\NT`, `src\2KXP`, `src\9X` | Build files per target |
| `bin\` | Prebuilt binaries and install files |

## Feature matrix by target

| | 95/98/Me (x86) | NT 3.1/3.50/3.51/4.0 (x86) | 2000/XP/2003 (x86) and XP x64/2003 x64 (amd64) |
|---|---|---|---|
| Build folder (sources are shared in `src\`) | `src\9X` | `src\NT` | `src\2KXP` |
| Target define | `MMC_W9X` (implies `MMC_NT4`) | `MMC_NT4` | none |
| Driver binary | `mmcsd9x.mpd` (`SCSIPORT.PDR` miniport) | `mmcsdnt.sys` | `mmcsdnt.sys` |
| Adapter instance | One per device node: a PCI function (all its slots), or an ACPI host found from an ACPI device node (98/Me) or from the manually installed "ACPI host" device (95) | One per PCI function (all its slots) or ACPI host | One per PCI function (all its slots) or ACPI device node |
| I/O model | Fast Polling by default; interrupt-completed ADMA2 with `IRQ=1` | Interrupt-completed ADMA2 where an interrupt is connected (PCI INTx on 3.50–4.0, ACPI hosts' ISA IRQ); NT 3.1: PCI hosts Fast Polling unless `IRQ=1`; Fast Polling with `IRQ=0` | Interrupt-completed ADMA2 on the interrupt PnP assigns; Fast Polling in the dump stack and with `IRQ=0` |
| Transfer methods | ADMA2 / SDMA / PIO | Same | Same |
| Host discovery | Bus:slot from `SCSIPORT.PDR`; ACPI tables for a non-PCI device node | PCI bus scan (HAL, or `CF8h`/`CFCh` on NT 3.1), then the ACPI tables (`BASE=` as an override) | PnP resources (PCI BARs, ACPI `_CRS`); bus:slot for the dump instance |
| Power management | Flush on shutdown; `HwAdapterState`; power IOCTL | Flush on shutdown; power IOCTL; state check on every request | `HwAdapterControl` stop/restart, hibernation, crash dump; power IOCTL |
| eMMC cache | Yes | Yes | Yes |
| UHS-I, DDR52, HS200, HS400 (`HS400=1`) | Yes | Yes | Yes |
| eMMC command queuing (`CQ=1`) | No | No | Yes (with an interrupt) |
| UNMAP, TRIM mode, LOG SENSE | Yes | Yes | Yes |
| Auto-sense on error (`SRB_STATUS_AUTOSENSE_VALID`) | No — the class driver issues REQUEST SENSE | No — the class driver issues REQUEST SENSE | Yes |
| Memory above 4 GB (PAE, x64) | Not used by 9x | Not used by NT 3.1–4.0 | 64-bit ADMA2 where every slot of the host has it (buffers above 4 GB go to the host directly); otherwise 32-bit ADMA2/SDMA: ScsiPort double-buffers pages above 4 GB, PIO for a buffer it reports above 4 GB (tested on XP x64 with 2 GB above 4 GB and on 2000 `/PAE` with 4 GB) |
| Hot-plug / card change | No | No | No |
| Max transfer size per I/O | 128 KB (64 KB on an SDMA-only host) | Same | Same |
| Debug output | VMM `_Debug_Printf_Service` (`DEBUG=1` builds), prefix `[mmcsd9x]` | `DbgPrint`, prefix `[MMCSDNT]` | `DbgPrint`, prefix `[MMCSDNT]` |

## Known Limitations

- **No hot-plug**: cards are found at `HwInitialize` and again after a power transition or reset. The driver is meant for an eMMC or an SD card that stays in its slot; a card pulled while Windows runs fails its requests (it is not reported as removed media), and a card inserted later is seen at the next boot
- **No UHS-II, UHS-III or SD Express**
- **HS400** is used only with `HS400=1`: the strobe and delay-line settings it needs are vendor specific and not programmed; **eMMC command queuing** only with `CQ=1`, on 2000/XP/x64, with the engine at the offset given (Intel 200h by default). Both were tested only against an emulated host (see `TESTS.TXT`)
- Tuning uses the host's tuning circuit (SDHCI 3.00 Execute Tuning); hosts that need software tuning or vendor sampling-clock settings stay at the next slower mode when tuning fails
- No eMMC hardware partitions (boot, RPMB, general purpose): only the user area is a disk
- No MSI; the interrupt path uses the function's INTx or the ACPI host's interrupt. Windows 9x uses Fast Polling unless `IRQ=1` is set; NT 3.1 uses Fast Polling for a PCI host unless `IRQ=1` is set
- Everything that is not a read or write under ADMA2 (PIO, SDMA, card commands, VERIFY, flush, the crash dump stack) is Fast Polling
- One command at a time per slot (the SD bus has no queuing), except eMMC command queuing with `CQ=1`
- Vendor-specific host controllers (programming interface `02`, or controllers that do not follow the SDHCI layout, such as some early Ricoh/TI/ENE readers in their own modes, and USB card readers) are not driven
- **In-driver ACPI (NT 3.1–4.0, 9x)**: the tables are read where the BIOS left them. A BIOS that reports the ACPI table area as usable memory through INT 15h E801/88 lets these systems overwrite it before the driver loads, and the host is not found; limit the memory the OS uses (`/MAXMEM=` in `boot.ini`) in that case. The AML is not executed: a host whose `_CRS` is computed by code (not a resource template or a named buffer) is not found; `BASE=` gives its register address by hand
- **Windows 9x and BIOS-served cards**: a card the BIOS also serves through INT 13h (an SD/eMMC boot BIOS) can appear twice on Windows 9x, once through MMCSD9X and once in MS-DOS compatibility mode, as with AHCINT and SeaBIOS's AHCI disks; seen with SeaBIOS's SD support, which Windows 9x cannot match to the protected-mode drive. Do not write to both
- Windows 95 has no ACPI device nodes: a host only ACPI describes needs the manual "ACPI host" install (see [Windows 95 / 98 / Me](#windows-95--98--me))
- On NT 3.1 and NT 4.0 the disk numbering follows the load order; see [Tag and disk numbering](#tag-and-disk-numbering)
- **Memory and PAE:** on 2000/XP/x64 a host whose slots all have 64-bit ADMA2 (SDHCI 3.00, 64-bit system bus capability) gets buffers above 4 GB directly (`DMA64=0` turns this off). Otherwise ADMA2 and SDMA take 32-bit addresses: the driver tells ScsiPort it is a 32-bit bus master, so on PAE and x64 systems ScsiPort keeps the DMA area below 4 GB and hands buffers above 4 GB through its own double buffering; a buffer it still reports above 4 GB is moved with PIO, and a DMA area above 4 GB makes the adapter run with PIO instead of failing. Tested with 4 GB RAM on Windows 2000 Professional (`/PAE`), NT 4.0, NT 3.1 and Windows 95, and with 2 GB of the RAM above the 4 GB line on XP x64; see `TESTS.TXT`. Windows 2000 Advanced Server/Datacenter and Server 2003 Enterprise with memory above 4 GB, and XP x86 with `/PAE`, were not tested
- A host whose 64-bit memory BAR the firmware placed above 4 GB is not used (the BIOS/CSM boot path of these systems leaves BARs below 4 GB)
- Windows 9x itself does not run with more than about 1 GB without `MaxPhysPage` in `SYSTEM.INI` (and `MaxFileCache` for the cache); with that set, the driver runs with 4 GB installed
- SDSC cards (byte addressing) are limited to 4 GB, which no SDSC card exceeds; SDHC/SDXC/eMMC to 2 TB
- 9X and NT builds do not fill auto-sense; the OS-era class drivers issue their own REQUEST SENSE
- Windows 95 RTM: see the [SCSIPORT.PDR note](#windows-95--98--me) below
- Tested in QEMU (SD card and eMMC models, PCI and ACPI hosts; see `TESTS.TXT`), not yet on real SD hosts; reports from real hardware are welcome

## Building from source

### Requirements

- **Windows 95 / 98 / Me (x86):**
  - A Windows 9x build machine (the build script is written for the Windows 9x `COMMAND.COM` batch interpreter)
  - Windows 95 DDK — expected at `C:\DDK` (provides `MASTER.MK`, `BLOCK\INC`, `INC32`, `BLOCK\LIB\scsiport.lib`)
  - Win32 SDK for Windows NT 4.0 / Windows 95 — expected at `C:\MSTOOLS`
  - Microsoft Macro Assembler (MASM) 6.11 — expected at `C:\MASM611` (`BIN\ML.EXE`, `BIN\NMAKE.EXE`)
  - Microsoft Visual C++ 2.0 — expected at `C:\MSVC20` (`BIN\CL.EXE`, `BIN\LINK.EXE`, `BIN\NMAKE.EXE`)
- **Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86):** Windows NT 4.0 DDK + Visual C++ 4.0 (MSVC 4.0)
- **Windows 2000 / XP (x86):** Windows Server 2003 SP1 DDK ("WDK 6001", build 3790.1830)
- **Windows XP x64 / Server 2003 x64 (amd64):** A DDK/WDK with an amd64 ("WNET") cross-compiler — Windows Server 2003 SP1 DDK ("WDK 6001", build 3790.1830), Windows Server 2003 R2 DDK, or the Windows 7 WDK (WinDDK 7600.16385.1)

### Build Instructions

#### For Windows 95 / 98 / Me (x86)

`build.bat` expects a copy of the repository's `src` folder in `C:\MMCSDNT` (shared sources in `C:\MMCSDNT`, build files in `C:\MMCSDNT\9X`), and `mmcsd9x.lnk` links against `C:\DDK\BLOCK\LIB\scsiport.lib`. If your tools live elsewhere, edit the `SET` lines at the top of `build.bat` (`MASM_ROOT`, `C16_ROOT`, `C32_ROOT`, `SDKROOT`, `DDKROOT`, `MMC_ROOT`) and the library path in `mmcsd9x.lnk`.

```bat
REM Copy the src folder (shared sources + 9X build files) to C:\MMCSDNT
mkdir C:\MMCSDNT
mkdir C:\MMCSDNT\9X
copy <path-to-MMCSDNT>\src\*.* C:\MMCSDNT
copy <path-to-MMCSDNT>\src\9X\*.* C:\MMCSDNT\9X

C:
cd \MMCSDNT\9X

REM Optional: debug build (debug output through VMM, see Debugging)
REM set DEBUG=1

REM Build
build.bat
```

`build.bat` sets `MASTER_MAKE=1` and the tool roots, puts MASM 6.11 and Visual C++ 2.0 on `PATH`, creates `TMP`/`TEMP` (default `C:\WINDOWS\TEMP`) if they are missing, warns if any expected tool is not found, and then runs `nmake` in `C:\MMCSDNT\9X`. The makefile pulls in the DDK's `MASTER.MK` (`BUILD_BITS=32`, `BUILD_TYPE=block`), compiles `..\mmcmain.c`, `..\mmccmd.c`, `..\mmcsatl.c` and `..\mmcacpi.c` with `-DMMC_W9X`, and links them with `mmcsd9x.lnk`.

Output: `C:\MMCSDNT\9X\mmcsd9x.mpd` (plus `mmcsd9x.map`). Run `nmake clean` to remove build output.

#### For Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86)

```bat
cd <path-to-MMCSDNT>\src\NT
build.bat
```

`build.bat` sets `MSVCDIR` (default `C:\MSDEV`) and `DDKDIR` (default `C:\NT4DDK`) — edit them if your tools live elsewhere — then compiles `..\mmcmain.c`, `..\mmccmd.c`, `..\mmcsatl.c` and `..\mmcacpi.c` with `-DMMC_NT4` and links against `scsiport.lib` and `ntoskrnl.lib`.

Output: `src\NT\mmcsdnt.sys`, for Windows NT 3.1 and later (`-subsystem:native,3.50`; the NT 3.1 loader does not check the subsystem version). The NT build imports only functions NT 3.1's `SCSIPORT.SYS` exports; PCI configuration space is written through ports `CF8h`/`CFCh`.

#### For Windows 2000 / XP (x86)

```bat
REM Set up build environment
Start Menu → Windows Server 2003 DDK → Build Environments → Windows 2000/XP Free Build Environment

REM Navigate to driver directory
cd <path-to-MMCSDNT>\src\2KXP

REM Build
build -cZ
```

`sources` compiles the shared `..\mmcmain.c`, `..\mmccmd.c`, `..\mmcsatl.c` and `..\mmcacpi.c` (no target define; `mmcacpi.c` compiles to nothing there) and links against `scsiport.lib`, `ntoskrnl.lib` and `hal.lib`. Output: `obj\i386\mmcsdnt.sys` (checked) or `obji386\mmcsdnt.sys` (free), depending on the environment chosen.

#### For Windows XP x64 / Server 2003 x64 (amd64)

The x64 driver is built from the same `src\2KXP` folder as the x86 driver; only the build environment differs.

```bat
Start Menu → Windows Server 2003 (WNET) DDK/WDK → Build Environments → x64 Free Build Environment
cd <path-to-MMCSDNT>\src\2KXP
build -cZ
```

Output: `mmcsdnt.sys` under the amd64 output directory (`objfre_wnet_amd64\amd64\` or equivalent). `mmcsdnt.rc` supplies version-resource information.

> The 2000/XP and x64 builds should first be verified in the **checked** build environment before being rebuilt and shipped from the **free** build environment.

## Installation

### Installation Media

Prebuilt binaries and install files live under `bin\`:

```
bin\
├── 9X\
│   ├── MMCSD9X.INF           (Windows 95/98/Me install file)
│   └── MMCSD9X.MPD           (SCSIPORT.PDR miniport)
├── NT\
│   ├── oemsetup.inf
│   ├── mmcsdnt.sys
│   └── floppy\               (NT 3.50/3.51/4.0 driver disk)
│       ├── disk1             (tag file)
│       ├── oemsetup.inf
│       ├── txtsetup.oem
│       └── mmcsdnt.sys
└── 2KXP\
    ├── mmcsdnt.inf           (one INF for x86 and x64)
    ├── mmcsdnt.sys           (x86 binary)
    ├── i386\
    │   └── mmcsdnt.sys       (x86 binary)
    ├── amd64\
    │   └── mmcsdnt.sys       (x64 binary)
    ├── floppy_i386\          (F6 disk, 2000/XP/2003 x86)
    │   ├── disk1             (tag file)
    │   ├── txtsetup.oem
    │   ├── mmcsdnt.inf       (flat: no i386\ subdirectory)
    │   └── mmcsdnt.sys
    └── floppy_amd64\         (F6 disk, XP x64/2003 x64)
        ├── disk1             (tag file)
        ├── txtsetup.oem
        ├── mmcsdnt.inf       (flat: no amd64\ subdirectory)
        └── mmcsdnt.sys
```

The F6 disks carry a flat copy of `mmcsdnt.inf`: text-mode setup copies the driver and the INF into `%SystemRoot%\OemDir` without subdirectories, and GUI-mode setup installs the controller again from there.

### Hardware IDs

| ID | Matches |
|---|---|
| `PCI\CC_080500`, `PCI\CC_080501` | Any PCI SD host controller following the SDHCI layout, without and with DMA |
| `ACPI\PNP0D40`, `*PNP0D40` | The generic ACPI id of an SDHCI host |
| `ACPI\80860F14`, `ACPI\80860F16` | Intel Bay Trail / Cherry Trail / Braswell SD and eMMC hosts |
| `ACPI\INT33BB`, `ACPI\INT3436` | Intel Bay Trail / Cherry Trail SD hosts (other firmware) |
| `ACPI\80865ACA`, `ACPI\80865ACC`, `ACPI\80865AD0` | Intel Apollo Lake SD, eMMC and SDIO hosts |
| `ACPI\AMDI0040` | AMD eMMC host |
| `MMCSD_ACPI_HOST` (9x only) | No PnP id: the manually installed device through which Windows 95 loads the driver for a host only ACPI describes |

The class code `08-05` with programming interface `02` (vendor-specific) is not listed: such controllers do not follow the SDHCI layout. No Windows in this range brings a driver for any of these ids, so there is nothing to outrank. The 2000/XP INF also writes critical device entries (`pci#cc_080500`, `pci#cc_080501` and the ACPI ids) so that a host that appears later (another slot, a new board) starts with MMCSDNT at boot.

### Installing

#### Windows 95 / 98 / Me

Windows 9x/Me setup itself runs through the BIOS (INT 13h), so there is no F6-style driver step — install MMCSD9x once Windows is up. Copy `bin\9X\MMCSD9X.INF` and `MMCSD9X.MPD` to a folder on the hard disk (for example `C:\MMC`), then:

- **PCI host:** Windows reports *New Hardware Found* (*PCI Card*, or *PCI SD Host Controller*) at the first boot with the controller. Choose **Driver from disk provided by hardware manufacturer**, enter the folder and select **MMCSD9x SD/MMC/eMMC Storage Controller**. If the device was installed without a driver before, use **Device Manager → (device) → Properties → Driver → Update Driver** (Windows 95: *Change Driver*) → **Have Disk**.
- **ACPI host on Windows 98/Me:** the same for the ACPI device node of the host (Device Manager lists it under *Other devices* or *System devices*).
- **ACPI host on Windows 95:** **Control Panel → Add New Hardware → No** (do not detect) → **SCSI controllers** → **Have Disk** → the folder → **MMCSD9x SD/MMC/eMMC Storage Controller (ACPI host, no PnP id)**. The driver then finds the host in the ACPI tables (or at `BASE=`).

The driver loads at once (no restart needed); `MMCSD9X.MPD` is copied to `WINDOWS\SYSTEM\IOSUBSYS`. The cards are driven in 32-bit mode (**Control Panel → System → Performance**).

> **Windows 95 RTM users:** if the controller shows a **yellow exclamation mark** in Device Manager, the original Windows 95 `SCSIPORT.PDR` is the problem. Either install the [AMDK6 Update](https://winworldpc.com/download/c3a0c2b0-c398-c2a8-3e02-11c3a6e28094), or extract `SCSIPORT.PDR` from that update and copy it to `WINDOWS\SYSTEM\IOSUBSYS` yourself — rename the existing `SCSIPORT.PDR` there to `SCSIPORT.BKP` first so you keep a backup.

#### GUI-mode setup (Windows 2000/XP/XP x64/Server 2003)

Use `bin\2KXP\mmcsdnt.inf` with **Have Disk** on the SD host controller in Device Manager (**Update Driver**), or let the **Found New Hardware** wizard pick it up. The same INF serves both architectures: it copies `i386\mmcsdnt.sys` on x86 and `amd64\mmcsdnt.sys` on x64. The device shows up as **MMCSDNT SD/MMC/eMMC Storage Controller** (x86) or **MMCSDNT SD/MMC/eMMC Storage Controller (x64)** under *SCSI and RAID controllers*, and the driver installs as service `MMCSDNT` under `LoadOrderGroup = SCSI Miniport`, boot start.

#### Text-mode (F6) setup — Windows 2000/XP/XP x64/Server 2003

Copy the contents of `bin\2KXP\floppy_i386\` (x86) or `bin\2KXP\floppy_amd64\` (x64) onto a floppy disk (or a virtual floppy image), press **F6** at the start of text-mode setup, and select **MMCSDNT SD/MMC/eMMC Storage Controller (ages2001)**. Setup then lists the card or eMMC as a disk, partitions, formats and installs onto it; GUI-mode setup installs the controller from the copy of the INF in `OemDir`. The F6 disks carry a flat copy of `mmcsdnt.inf` (the driver next to the INF, no `i386\`/`amd64\` subdirectories): text-mode setup copies the F6 files flat into `OemDir`, and an INF pointing into a subdirectory made GUI-mode setup ask for `OemDir\amd64\mmcsdnt.sys` ("Files Needed"), also later whenever another SD host was added.

The machine has to boot from the card after setup: that is the firmware's job (an SD/eMMC boot entry in the BIOS or CSM). A full (not quick) format makes text-mode setup read the whole card once; on a slow card this takes a while.

#### Windows NT 3.1 / 3.50 / 3.51 / NT 4.0

NT uses the older OEM Setup mechanism (`oemsetup.inf`). Use **"Have Disk"** in **Windows NT Setup → SCSI Adapters** (NT 3.x) or **Control Panel → SCSI Adapters → Drivers → Add** (NT 4.0), or the OEM prompt of text-mode setup, point it at `bin\NT\floppy\`, and select **MMCSDNT SD/MMC/eMMC Storage Controller (ages2001)**. Windows NT 3.1 has no *SCSI Adapters* applet for added drivers: create the service by hand (see [Configuration](#configuration)) and copy `mmcsdnt.sys` to `system32\drivers`.

**`boot.ini`:** `multi(0)disk(0)rdisk(0)partition(1)\WINNT` makes NTLDR read through the BIOS (INT 13h); this is the normal choice, since the BIOS boots from the card anyway. `scsi(0)disk(n)rdisk(0)partition(1)\WINNT` with `NTBOOTDD.SYS` (a copy of `mmcsdnt.sys`) also works; `disk(n)` is the slot number.

**Disk class driver:** NT 3.x's `scsidisk` (NT 4.0: `disk`) must be a boot driver (`Start = 0`) like the miniport, otherwise a system disk on MMCSDNT stops with `0x7B INACCESSIBLE_BOOT_DEVICE`. Text-mode setup sets this when it installs onto a SCSI disk; `oemsetup.inf` sets it too when the driver is added through *Windows NT Setup* / *SCSI Adapters*.

### Tag and disk numbering

`oemsetup.inf` and the `txtsetup.oem` files give the service `Tag = 41`, which is not in the `SCSI miniport` group order list of any of these systems, so MMCSDNT loads after the SCSI miniports that are there and its cards come after their disks: adding MMCSDNT for an SD card to a system that boots from another controller does not move `C:`. On NT 3.1 an untagged miniport (such as IDENT installed by hand) loads after a tagged one; when MMCSDNT is added there for data cards, remove MMCSDNT's `Tag` value too, or its card becomes the first disk and takes `C:`. With the system on the card it does not matter.

## Configuration

Registry values set at install time on the NT family (`mmcsdnt.inf` service section, `txtsetup.oem` `[Config.scsi.MMCSDNT]`, `oemsetup.inf`):

- **Tag** – `41` (see [Tag and disk numbering](#tag-and-disk-numbering))
- **Group** – `SCSI Miniport`
- **Type / Start / ErrorControl** – `1` (`SERVICE_KERNEL_DRIVER`) / `0` (boot start) / `1` (normal)
- **Event log** – `IoLogMsg.dll` registered as the event message file (`TypesSupported = 7`)
- **2000/XP/x64 only** – `Parameters\PnpInterface` with the values `0`, `1`, `5` and `15` (each `1`), in that order: PCI (`5`) and the bus types an ACPI device node can carry (see [Windows 2000/XP/x64 specifics](#windows-2000xpx64-specifics)). When the key is missing, the driver creates it itself at load

On Windows 9x/Me, `MMCSD9X.INF` registers the controller with `DevLoader = *IOS` and `PortDriver = MMCSD9X.MPD` (plus `DontLoadIfConflict = Y`).

### Driver parameters

On the NT family the driver reads the ScsiPort argument string, the `REG_SZ` value `DriverParameters` under `HKLM\SYSTEM\CurrentControlSet\Services\MMCSDNT\Parameters\Device` (create the keys if they are missing; reboot to apply):

| Value | Effect |
|---|---|
| `DMA=0` | No DMA: every transfer under PIO |
| `HS=0` | No high speed: SD at 25 MHz, MMC at 26 MHz (MMC 20 MHz before) at most |
| `CLOCK=n` | Highest card clock in MHz (decimal), for a slot with bad signal quality |
| `IRQ=0` | No interrupt: everything Fast Polling. Use this on a machine whose interrupt routing is wrong (the watchdog would get there on its own after five missed interrupts) |
| `IRQ=1` | Windows 9x: connect the device node's interrupt and complete ADMA2 transfers from it (Fast Polling by default there). Windows NT 3.1: also connect the INTx line of a PCI host |
| `BASE=xxxxxxxx` | NT 3.1–4.0 and 9x: the physical address (hexadecimal) of the register block of a host only ACPI describes, when the driver does not find it in the tables; with `IRQ=n` (hexadecimal, ISA IRQ 1–F) for its interrupt |
| `UHS=0` | No 1.8 V modes at all: no UHS-I for SD, no HS200/HS400/DDR52 for eMMC |
| `HS200=0` | eMMC: no HS200 (and so no HS400); High Speed 52 MHz / DDR52 instead |
| `DDR=0` | No double data rate: no DDR52 for eMMC, no DDR50 for SD |
| `HS400=1` | eMMC: HS400 after HS200 on an 8-bit slot (off by default; vendor strobe settings are not programmed) |
| `CQ=1` | 2000/XP/x64: eMMC command queuing through the host's CQHCI engine at offset 200h from the SDHCI registers (Intel); `CQ=xxx` gives another offset (hexadecimal). Needs an interrupt |
| `TRIM=0` | No UNMAP (and no TRIM mode): VPD B2h and READ CAPACITY(16) report no logical block provisioning |
| `DMA64=0` | 2000/XP/x64: no 64-bit ADMA2; buffers above 4 GB go through ScsiPort's double buffering |

Several values are separated by spaces or semicolons.

On Windows 95/98/Me `SCSIPORT.PDR` passes the string value `AdapterSettings` of the controller's driver key as the argument string. The key is `HKLM\System\CurrentControlSet\Services\Class\SCSIAdapter\000n` (the one whose `PortDriver` is `MMCSD9X.MPD`); add `"AdapterSettings"="IRQ=1"` there, for example with a `.reg` file, and restart.

### Build-time limits

The limits are compile-time constants in `src\mmcsd.h`:

```c
#define MMC_MAX_SLOTS                   6           /* per PCI function (SDHCI) */
#define MMC_MAX_CONTROLLERS             8
#define MMC_MAX_TRANSFER                0x20000     /* 128 KB */
#define MMC_MAX_PHYS_BREAKS             32
#define MMC_ADMA_ENTRIES                64          /* 8 or 12 bytes each, 1 KB per slot */
#define MMC_SCRATCH_SIZE                4096        /* EXT_CSD, SCR, switch status, VERIFY */
#define MMC_SDMA_BOUNCE_SIZE            0x10000     /* SDMA-only controllers */
#define MMC_MISSED_IRQ_LIMIT            5
#define MMC_DMA_ERROR_LIMIT             3           /* then PIO for the slot */
#define MMC_RETRY_LIMIT                 2           /* a failed transfer, card reinitialized */
```

- **`MMC_MAX_SLOTS`** follows the SDHCI specification (at most six slots per PCI function); the target ID of a card is its slot number
- **`MMC_MAX_CONTROLLERS`** sizes the table that records which hosts (by the physical address of the first register block) a driver instance has claimed, so that two instances (PCI pass and ACPI pass) never drive the same host
- **`MMC_MAX_TRANSFER`** and **`MMC_MAX_PHYS_BREAKS`** are what ScsiPort is told; 64 ADMA2 descriptors cover a 128 KB request split at every page
- `MMC_CQ_DEPTH` (8) is how many command queuing tasks the driver keeps in flight per card (at most the card's queue depth)
- **`MMC_MISSED_IRQ_LIMIT`**, **`MMC_DMA_ERROR_LIMIT`** and **`MMC_RETRY_LIMIT`** set when the driver gives up on interrupts (per adapter), on DMA (per slot) and on a failing transfer (it sets the card up again and retries twice)

After changing a value, rebuild every target you ship (`src\9X`, `src\NT`, `src\2KXP`).

## Debugging

The NT, 2000/XP and x64 variants log via `DbgPrint` (through the `MMC_DBG` macro), visible through a kernel debugger or `DebugView`. Debug output is prefixed `[MMCSDNT]`. The bring-up lines show what the driver found for each slot:

```
[MMCSDNT] FindAdapter: 1B36:0007 at bus 0 slot 00000004, progif 01, 1 slot(s) at FEBF1000
[MMCSDNT] FindAdapter: 1 slot(s), IRQ 11, interrupt
[MMCSDNT] Slot 0: SDHCI 3.00, caps 00000000:057834B4, base clock 52 MHz, ADMA2
[MMCSDNT] Slot 0: SDHC/SDXC, 4-bit, 26000 kHz high speed, 00000000:00800000 sectors, ADMA2
[MMCSDNT] ACPI: 80860F14 at 90000000, interrupt 5 (edge)
[MMCSDNT] Slot 0: MMC/eMMC (sector), 8-bit, 52000 kHz high speed, 00000000:01000000 sectors, ADMA2, cache on
```

The 9X variant prints through the VMM's `_Debug_Printf_Service` (WDEB386 on a serial port, SoftICE, or DebugView for Windows 9x); the calls are only compiled into `DEBUG=1` builds. Messages are prefixed `[mmcsd9x]`.

In code, all targets use one macro with double parentheses (no variadic macros needed for VC++ 2.0 / MSVC 4.0):

```c
MMC_DBG((MMC_PFX "Slot %lu: status %08lX\n", (ULONG)Slot->Index, status));
```

## Technical Notes

### I/O Model

- **Fast Polling (all targets):** a command is issued from `HwStartIo` with the slot's interrupt signals off and polled every 10 µs on the normal/error interrupt status register until the command, and the data phase, is complete; it is completed before `HwStartIo` returns. Completion never depends on an interrupt being delivered — this covers text-mode Setup, the crash dump stack, card initialization and hosts with broken interrupt routing.
- **Interrupt-completed ADMA2:** where an interrupt is connected, a disk read or write that can go by ADMA2 is started with the transfer-complete and error interrupts signalled, and `HwStartIo` returns `SRB_STATUS_PENDING`; `HwInterrupt` checks every slot of the adapter, acknowledges, completes the request and starts the next one if one is waiting. One transfer is outstanding per slot; while one is, a 100 ms ScsiPort timer (`RequestTimerCall`) also services the slots. If the timer finds completed transfers that no interrupt reported five times, the driver turns the interrupt path off for that adapter and continues with Fast Polling for good. An interrupt with nothing outstanding is acknowledged and claimed only if one of the driver's slots signalled it (PCI INTx is shared).
- **PIO:** 512-byte blocks through the buffer data port (32-bit accesses), waiting for buffer read/write ready between blocks.

### Card initialization

For each slot with a card present (an embedded slot is always treated as populated):

1. Software reset of the slot, 3.3 V bus power, 400 kHz clock; interrupt status enables set, signals off.
2. CMD0, then CMD8 (SD 2.00 interface condition). **SD:** ACMD41 with HCS until ready (the CCS bit tells SDHC/SDXC from SDSC), with S18R/XPC where the host has UHS-I modes; a card that accepts 1.8 V gets CMD11, the host switches its signalling to 1.8 V and checks that the card drives DAT[3:0] high again. **MMC:** CMD1 with the sector-mode bit until ready (bit 30 of the OCR: sector addressing).
3. CMD2 (CID), CMD3 (relative address: the card's own for SD, assigned for MMC), CMD9 (CSD: capacity of SDSC and byte-addressed MMC), CMD7 (select).
4. **SD:** ACMD51 (SCR), CMD16 512 for SDSC, ACMD6 4-bit, ACMD13 (SD status: allocation unit, erase timing, discard support). At 1.8 V: CMD6 current limit, then the fastest of SDR104, DDR50, SDR50 that card and host share, tuned with CMD19 where needed (SDR25 if none or if tuning fails). At 3.3 V: CMD6 check and switch to High Speed (SD 1.10+), clock 25 or 50 MHz (as the divider allows). **MMC:** CMD8 (EXT_CSD: sector count, card type, revision, cache size, erase and trim timing, life time, command queue support), CMD6 SWITCH to an 8-bit or 4-bit bus (as the host allows), the high-capacity erase group, power-off notification on (eMMC 4.5+), then **HS200** (1.8 V, HS_TIMING 2, 200 MHz, CMD21 tuning) and on request **HS400** (back to HS_TIMING 1 at 52 MHz, 8-bit DDR bus, HS_TIMING 3, 200 MHz), else High Speed by HS_TIMING at 52/26 MHz and **DDR52** (DDR bus width, at 1.8 V); the cache on (CACHE_CTRL) on eMMC 4.5+ with a cache.
5. Transfer method: ADMA2 (64-bit descriptors where the host has them and the driver runs on 2000/XP/x64) on a version 2.00+ host that has it, SDMA with the bounce buffer otherwise, PIO without DMA.
6. A mode that fails sets the card up again from the start without it: a failed 1.8 V switch powers the slot off (the card has to leave 1.8 V) and initializes at 3.3 V; a failed HS200 goes to High Speed/DDR52, a failed HS400 to HS200, a failed DDR52 to High Speed. The decision is kept for the slot until the next boot.

### Error handling and fallback

- **Command errors** (timeout, CRC, index, end bit) reset the CMD line; **data errors** (timeout, CRC, end bit, ADMA error) the DAT line too; a card in a wrong state gets CMD12.
- A failed read or write sets the card up again (as at bring-up) and is retried, twice at most; then it fails with sense data: MEDIUM ERROR (unrecovered read / write error) for data and CRC errors, ILLEGAL REQUEST / LBA OUT OF RANGE for the card's out-of-range and address errors, DATA PROTECT for a write-protect violation, NOT READY / MEDIUM NOT PRESENT when the card is gone, HARDWARE ERROR for anything else (a card that stops answering).
- **DMA errors** count against the slot; after three it switches to PIO. A buffer ADMA2 cannot address (not dword-aligned, above 4 GB) goes with PIO instead of failing.
- **Lost host state** (registers reading all ones, a power transition, a BIOS that used the controller behind the driver's back): the host and the card are set up again before the next request; outstanding requests are returned as `SRB_STATUS_BUS_RESET` and retried by ScsiPort.

### Power management

- **Shutdown and flush:** `SRB_FUNCTION_FLUSH` flushes the eMMC cache of every card (SD cards have nothing to flush); SYNCHRONIZE CACHE does the same for one card. `SRB_FUNCTION_SHUTDOWN` flushes and then sends the eMMC power-off notification (POWER_OFF_LONG, eMMC 4.5+) or puts the card to sleep (CMD5, eMMC 4.3+), so the card saves its tables before the power goes. The driver declares `CachesData`, without which ScsiPort passes neither request.
- **START STOP UNIT:** stop = flush, deselect (stand-by), clock off; start, or the next read or write, sets the card up again.
- **Windows 2000/XP/x64 — `HwAdapterControl`:** `ScsiStopAdapter` (sleep, hibernation, device removal) flushes, deselects and stops the slots (clock and bus power off); `ScsiRestartAdapter` sets them up again, including a cache setting made through MODE SELECT.
- **Hibernation and crash dump (2000/XP/x64):** the dump stack loads the miniport a second time without a driver object (or with `DUMP=` in the argument string). That instance uses Fast Polling only; it finds its host from the PCI bus and slot or the memory range the dump stack hands it (tested: hibernation with Windows 2000 on a PCI SD card).
- **NT 3.1–4.0 and 9x:** these ScsiPorts have no power callbacks. An ACPI or APM driver can use the `MMCSDNT` IOCTL (below): `POWER_DOWN` before the system enters a sleep state, `POWER_UP` (or simply the next request) after resume. On 9x a `HwAdapterState` restore makes the next request set the cards up again.

### IOCTL interface

`IOCTL_SCSI_MINIPORT` requests (an `SRB_IO_CONTROL` header followed by the data) sent to `\\.\ScsiN:` (or, on 2000/XP, to the disk):

| Signature | ControlCode | Function |
|---|---|---|
| `"MMCSDNT "` | `1` GET_INFO | Returns `MMC_IOCTL_INFO`: driver version (`01000000h`), flags (bit 0 interrupt path active, bit 1 powered down, bit 2 ACPI host, bit 3 TRIM mode, bit 4 64-bit ADMA2), slot count, PCI vendor/device (0 for an ACPI host), physical address of slot 0, IRQ, interrupt count; per slot the card type (none, SDSC, SDHC/SDXC, MMC, MMC/eMMC sector), bus width, transfer method (PIO/SDMA/ADMA2/ADMA2 64-bit), high speed, write protect, cache on, SDHCI version, DMA error count, clock, sector count, CID, CSD, OCR, host capabilities, the retry count, EXT_CSD revision, pre-EOL and life time estimates, bus timing (legacy, HS, SDR50, SDR104, DDR50, HS200, DDR52, SDR25, HS400), unmap method (erase, trim, discard), 1.8 V signalling, power-off notification, command queuing state (bit 0 engine found, bit 1 in use, bit 2 given up), depth and the most tasks seen in flight at once |
| `"MMCSDNT "` | `20h` TRIM_MODE_ON | A 4 KB pattern follows the header: from now on a write whose data is that pattern repeated is not written but unmapped (see [TRIM mode](#trim-mode)) |
| `"MMCSDNT "` | `21h` TRIM_MODE_OFF | Back to normal writes |
| `"MMCSDNT "` | `10h` POWER_DOWN | Flush, deselect, clock and bus power off on every slot. GET_INFO does not wake the controller; any other request does |
| `"MMCSDNT "` | `11h` POWER_UP | Set the slots up again (as after `ScsiRestartAdapter`) |

The structures are in `src\mmcsd.h` (`MMC_SRB_IO_CONTROL`, `MMC_IOCTL_INFO`, `MMC_IOCTL_SLOT`).

### Command queuing

eMMC 5.1 cards can queue up to 32 tasks; SoC hosts with a command queuing engine (CQHCI, JEDEC JESD84-B51) send them without the driver issuing every command. With `CQ=1` (2000/XP/x64, interrupt mode) the driver maps the engine at 200h from the SDHCI registers (Intel; `CQ=xxx` for another offset), checks its version register (5.x), and, for a sector-addressed card whose EXT_CSD reports command queue support, sends reads and writes as tasks:

- ScsiPort is set up for tagged queuing and several requests per LU, and INQUIRY reports CmdQue only for a card that uses the engine; up to 8 tasks are in flight (`MMC_CQ_DEPTH`, at most the card's depth). Every task has its own transfer descriptor list (the ADMA2 layout of the slot) and its task descriptor carries the block count, the address, the direction and forced programming for FUA writes.
- The card's command queue mode (CMDQ_MODE_EN) and the engine are turned on at the first read or write. Anything else (flush, UNMAP, MODE SELECT, the IOCTLs, power transitions) waits for the tasks in flight, halts the engine and turns the card's queue mode off; the next read or write turns both on again.
- An error reported by the engine or the host fails the tasks in flight with a bus reset (the class driver retries them), clears and disables the engine, sets the card up again and stops using command queuing on that card until the next boot.
- The engine's interrupts come through the SDHCI interrupt (normal interrupt status bit 14); the watchdog timer also completes tasks whose interrupt was lost.

### TRIM mode

Windows versions before 7 never send TRIM/UNMAP for deleted files. `TRIM_MODE_ON` (above) gives the driver a 4 KB pattern; while the mode is on, a write whose data is that pattern repeated over the whole request is unmapped (DISCARD, TRIM or ERASE, as UNMAP) instead of written. `tools\trimnt.exe <drive letter>` uses this: it switches the mode on with a random pattern on every AHCINT, IDENT, MMCSDNT and NVMENT adapter, fills the free space of the volume with files of the pattern (unbuffered, write-through), switches the mode off and deletes the files. Other writes are written normally all the time.

### In-driver ACPI

NT 3.1–4.0 and 9x know nothing of ACPI, so the driver reads the tables itself, only as far as finding SD hosts needs (`src\mmcacpi.c`):

1. The RSDP is searched in the EBDA's first kilobyte and in `E0000h`–`FFFFFh`; the RSDT (or the XSDT below 4 GB) gives the FADT, the FADT the DSDT; every SSDT is read too. The tables are mapped with `ScsiPortGetDeviceBase` (`_MapPhysToLinear` on 9x) and unmapped again.
2. The AML is scanned for `Device()` objects whose `_HID` or `_CID` (a string or a compressed EISA id) is one of the ids in [Hardware IDs](#hardware-ids).
3. The `_CRS` of such a device is read where it is a resource template: a `Name(_CRS, ResourceTemplate())`, or a `Method(_CRS)` that returns a named buffer of the same device (the usual `Return (RBUF)` form). `Memory32Fixed`, `Memory32`, `DWordMemory` or `QWordMemory` (below 4 GB) gives the register block, `Interrupt`, `IRQ` or `IRQNoFlags` the interrupt and its trigger mode.
4. The AML is never executed: `_STA` is not evaluated (a host the firmware disabled is still taken if its registers answer), and a `_CRS` computed by code is not understood. `BASE=` covers those cases.

The host is then driven like a PCI one: on NT 3.1–4.0 as an adapter on the EISA bus handler (a 32-bit bus master for the HAL, its IRQ as an ISA interrupt, edge or level as `_CRS` says); interrupts above 15 (APIC-only lines) are not connected and the host runs with Fast Polling.

### Windows 9x specifics

- **One adapter per device node:** `SCSIPORT.PDR` calls `HwFindAdapter` once per device node; a PCI function with several slots is one adapter (one target per slot). `*Again` is not honoured there, so a host only ACPI describes needs its own device node (98/Me: the ACPI node; 95: the manually installed "ACPI host" device).
- **Fast Polling by default**; `IRQ=1` in `AdapterSettings` turns the interrupt path on.
- **`HwAdapterState`:** provided because leaving it `NULL` makes `SCSIPORT.PDR` report "Init Failure" for the miniport. A restore makes the next request set the cards up again.
- **Memory-mapped registers:** mapped with `_MapPhysToLinear`, since `ScsiPortGetDeviceBase` of `SCSIPORT.PDR` does not map memory reliably (KB Q169584).
- **DMA area:** `SCSIPORT.PDR`'s uncached extension is not necessarily physically contiguous across pages; the scratch buffers, the ADMA2 tables and the SDMA bounce buffer are laid out page by page, each page with its own physical address (an earlier build that assumed contiguity sent ADMA2 data to the wrong memory and Windows 95 hung at boot).

### Windows NT 3.1 specifics

The NT build handles NT 3.1 at run time, so the same `mmcsdnt.sys` runs on NT 3.1, 3.50, 3.51 and 4.0:

- **`HW_INITIALIZATION_DATA` size:** NT 3.1's `ScsiPortInitialize` accepts only its own 40h-byte structure. `DriverEntry` first calls it with the NT 3.50–4.0 size on `PCIBus`; if that fails without `HwFindAdapter` ever being called, it retries with the 40h-byte layout on the ISA bus.
- **PCI configuration space:** NT 3.1's HAL has no PCI support, so the driver uses configuration mechanism #1 (ports `CF8h`/`CFCh`) for the bus scan, and on the whole NT build the PCI command register (memory space, bus master) is written through the same ports.
- **`PORT_CONFIGURATION_INFORMATION`:** 58h bytes on NT 3.1, without `SlotNumber` and `MaximumNumberOfTargets`; the driver checks `ConfigInfo->Length` before touching those fields.
- **DMA and interrupt:** a PCI host is reported on the EISA bus (32-bit bus master, no map registers). NT 3.1 has no PCI bus handler, so a PCI host's INTx is not connected by default (Fast Polling); `IRQ=1` reports it on the EISA bus. An ACPI host's ISA IRQ is connected as on NT 4.0.

### Windows NT 4.0 (and 3.50/3.51) specifics

- **Two passes:** `DriverEntry` calls `ScsiPortInitialize` for `PCIBus` (the PCI hosts; each adapter asks to be called again for the next one) and again for `Isa` (the hosts only ACPI describes, or `BASE=`). Hosts are recorded by register address so that no host is taken twice.

### Windows 2000/XP/x64 specifics

- **PnP adapter:** the PCI bus driver, or ACPI, enumerates the host and ScsiPort starts MMCSDNT with its resources: the memory BARs (or the `_CRS` memory range) and the interrupt.
- **ACPI device nodes and ScsiPort:** ScsiPort finds the `HW_INITIALIZATION_DATA` for a device by the device's bus type: a `LegacyInterfaceType` value under the device's hardware key, a bus type GUID it knows, or the legacy bus type the bus driver reports — and **Isa** when none of these gives one, which is the case for ACPI device nodes. `DriverEntry` therefore calls `ScsiPortInitialize` for `PCIBus`, `Internal`, `Isa` and `PNPBus`, and the service has `Parameters\PnpInterface` entries for all four (`5`, `0`, `1`, `15`), so that ScsiPort treats each as Plug and Play and runs no legacy detection. ScsiPort 5.0 reads the value names into one buffer without clearing it, so a short name after a longer one keeps the longer one's tail (`"1"` after `"15"` reads as 15): the names are written shortest first (`0`, `1`, `5`, `15`).
- **Text-mode setup (F6):** the setup kernel boots with a bare `MMCSDNT` service key; the `[Config]` values of `txtsetup.oem` are written only to the installed system. `DriverEntry` therefore creates `Parameters\PnpInterface` with the four values when the key does not exist yet, before `ScsiPortInitialize`.
- **Hibernation:** S3/S4 need every device to agree; a VGA adapter without a driver (VgaSave) vetoes them, which has nothing to do with MMCSDNT.

### Command timeout

A Fast Polling read or write is polled until the SRB's `TimeOutValue` minus one second (10 s when the SRB carries none, at most 60 s); card commands get 250 ms, the ACMD41/CMD1 power-up 1.5 s, busy (R1b) and programming 3 s, flush and the power IOCTL 30 s. An interrupt-completed transfer is timed by ScsiPort itself; when it expires, ScsiPort resets the bus and the driver resets the slot's lines and sets the card up again before the next request.

### One source tree

`src\mmcmain.c`, `src\mmccmd.c`, `src\mmcsatl.c`, `src\mmcacpi.c` and `src\mmcsd.h` build every target. The build files set the target:

| Define | Target | What it changes |
|---|---|---|
| `MMC_W9X` | Windows 95/98/Me | `_MapPhysToLinear`; Fast Polling unless `IRQ=1`; `HwAdapterState`; debug output via VMM `_Debug_Printf_Service`; implies `MMC_NT4` |
| `MMC_NT4` | NT 3.1/3.50/3.51/4.0 (and 9x) | Own host discovery (PCI scan, ACPI tables, `BASE=`); two `ScsiPortInitialize` passes; PCI config writes through `CF8h`/`CFCh`; no auto-sense |
| `MMC_NT31_COMPAT` | NT build only (set by `mmcsd.h` for `MMC_NT4` without `MMC_W9X`) | Windows NT 3.1 support in the same binary, see [Windows NT 3.1 specifics](#windows-nt-31-specifics) |
| none | 2000/XP/2003 x86 and x64 | PnP resources; auto-sense; `HwAdapterControl`; crash dump / hibernation; `Parameters\PnpInterface` |

### Memory Allocation

- **DMA area** – one uncached allocation per adapter (`ScsiPortGetUncachedExtension`): one page per slot for the scratch buffer (EXT_CSD, SCR, switch status, SD status, VERIFY data), the ADMA2 tables (1 KB per slot, four per page), one page of alignment slack, with `CQ=1` three pages per slot for the task descriptor list and the transfer descriptors, and on an SDMA-only host a 64 KB bounce buffer; every page gets its own physical address
- **SDMA** – one transfer goes to as many bounce pages as follow each other physically without crossing a 512 KB boundary (the SDMA buffer boundary the driver programs), at most 64 KB
- **Above 4 GB** – with 64-bit ADMA2 the DMA area may lie anywhere; otherwise a page of it without a physical address below 4 GB turns DMA off for the adapter (PIO, which uses the same pages through their virtual addresses); the adapter still starts
- **Device extension** – one `HW_DEVICE_EXTENSION` per adapter with six `MMC_SLOT`s

## Version history

### 1.0.0.0 — 2026-10-12

- Initial release: SD/SDHC/SDXC and MMC/eMMC on SDHCI hosts (PCI and ACPI-only) for Windows 95/98/Me, NT 3.1–4.0, 2000/XP/2003 x86 and XP x64/2003 x64 from one source tree; every card a fixed, bootable disk.
- SD High Speed and UHS-I (SDR104, DDR50, SDR50, SDR25) with the 1.8 V switch, tuning and a fallback to 3.3 V; MMC High Speed, eMMC DDR52, HS200 and HS400 (`HS400=1`), each falling back to the next slower mode.
- ADMA2 (64-bit on 2000/XP/x64), SDMA and PIO; interrupt-completed transfers with a Fast Polling watchdog.
- eMMC command queuing through the host's command queuing engine (`CQ=1`, 2000/XP/x64).
- UNMAP as eMMC discard/trim, SD discard or erase; TRIM mode and the `trimnt` tool; VPD B0h/B1h/B2h; LOG SENSE with the eMMC life time estimates.
- eMMC cache, power-off notification and sleep at shutdown; hibernation and crash dump support on 2000/XP, power IOCTL.
- In-driver ACPI table reading on NT 3.1–4.0 and 9x for hosts no PCI function shows.

## Related drivers

AHCINT, IDENT, MMCSDNT and NVMENT are one family: the same ScsiPort miniport design, SCSI translation layer, IOCTL conventions, Fast Polling fallback and TRIM mode, built from one source tree each for every system from Windows 95 and NT 3.1 to XP x64.

| Driver | Storage |
|---|---|
| **AHCINT** | SATA disks and optical drives on AHCI controllers |
| **IDENT** | PATA disks and optical drives, and SATA controllers in IDE mode |
| **MMCSDNT** | SD/SDHC/SDXC cards and MMC/eMMC on SD host controllers (SDHCI) |
| **NVMENT** | NVMe SSDs (based on [nvme2k](https://github.com/techomancer/nvme2k) by DominBear) |

`tools\trimnt.exe`, shipped with each of them, trims the free space of a volume on any of the four.

## License

Copyright (c) 2026 ages2001. All rights reserved.

**USE AT YOUR OWN RISK.** This driver talks directly to storage controller hardware on operating systems no longer supported or patched by Microsoft. Not recommended for any system holding data you can't afford to lose.

## Acknowledgments

- SD Specifications Part 1 Physical Layer and Part A2 SD Host Controller Simplified Specifications (SD Association)
- JEDEC JESD84 (e•MMC) standard
- ACPI Specification (tables, AML encoding, resource descriptors)
- Windows 95 DDK and Windows NT4/2000/XP DDK documentation

## Special Thanks

- infuscomus for porting the sibling drivers to x64
- Dietmar for Fast Polling idea
- DominBear for the [nvme2k](https://github.com/techomancer/nvme2k) project
- SweetLow for [nvme9x](https://github.com/LordOfMice/Tools) (Windows 9x VMM debug output technique)
- Windows 2000 Dev Community
- Testers
- And everyone who supports it
