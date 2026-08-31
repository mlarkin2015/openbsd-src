# PLAN-003-DEVICE: Device Emulation

## Review Findings (2026-08 source audit) — CORRECTIONS TO THIS PLAN

1. **`i82093aa.c` is NOT a PIIX4 IDE controller — it is the Intel 82093AA IOAPIC**
   (full redir-table/MMIO emulation, registered as an MMIO device at
   `i82093aa.c:224`). The Current State section and §3.1 are based on a misreading.
   **There is no IDE/ATA/ATAPI emulation anywhere in the tree.** §3.1 must be
   reframed as "write IDE from scratch" or dropped entirely; the pragmatic Windows
   path is virtio storage with drivers loaded from a second ISO during setup.
2. **MSI-X is not "partially implemented" (§3.2) — it is absent.** Every MSI-X path
   returns `VIRTIO_MSI_NO_VECTOR` (`virtio.c:407,473,575,593`; `vionet.c:1015`).
   All devices use legacy INTx via the IOAPIC. VirtIO 1.0 feature negotiation DOES
   exist (`virtio.h:46`; `VIRTIO_F_VERSION_1` advertised in `virtio.c:1005+`),
   so that part of §3.2 is already done.
3. **Missing critical devices not in this plan at all** (they gate Windows before
   anything listed here):
   - **Display**: no VGA/Bochs-GPU/virtio-gpu exists; the only concession to
     graphics is a comment about guests writing 0xb8000 (`x86_vm.c:189`). Windows
     Setup has no serial console mode. viogpu (§3.3) is critical-path, not item 7,
     and needs EFI GOP/simple-framebuffer integration so OVMF exposes a console.
   - **Input**: no i8042 PS/2 keyboard/mouse, no USB tablet. Without input you
     cannot get through Windows Setup even with a display. Add i8042 first (cheap,
     ports 0x60/0x64), xHCI tablet later.
4. Disk formats are raw + qcow2 only (`VMDF_RAW`/`VMDF_QCOW2`, `vioblk.c:55`;
   `parse.y:1407`). No vhdx — document conversion instead of implementing it.
5. USB (§3.4): if USB is done at all early, do a minimal **xHCI with only a
   virtual tablet device**, skipping real host-device passthrough. Full
   passthrough is a large standalone project.
6. SMBus/ICH9 (§3.5-3.7): keep, low priority; note that pairing ICH9 IDs with the
   existing PIIX3 PCI-ISA bridge (`pci.c`) will confuse guest drivers — either stay
   all-PIIX4 or move the whole platform profile to Q35/ICH9 as one coherent change.

## Goal

Add and enhance device emulation in vmd(8) to provide the hardware surface that Windows expects: enhanced IDE controller, USB controllers, GPU, SMBus, PCI bridges, and improved VirtIO support.

## Current State

- **PIIX4 IDE**: `i82093aa.c` — basic PIIX4 IDE controller with interrupt routing
- **PIC**: `i8259.c` — master/slave 8259A interrupt controller
- **PIT**: `i8253.c` — 8254 programmable interval timer
- **RTC/NVRAM**: `mc146818.c` — real-time clock + CMOS memory
- **UART**: `ns8250.c` — ns16550 serial console
- **PCI**: `pci.c` — PIIX3 PCI-to-ISA bridge
- **VirtIO**: virtio-net, virtio-blk, virtio-scsi, virtio-rng, virtio-vmmci
- **qcow2**: `vioqcow2.c` — qcow2 disk image reader

## What to Build

### 3.1 Enhanced PIIX4 IDE Controller

**What**: The PIIX4 IDE controller already exists but needs DMA support and better ATAPI handling for Windows.

**Current limitations**:
- No DMA mode support (PIO only)
- Limited ATAPI command support
- No NCQ
- Interrupt routing may not be fully IOAPIC-compliant

**Enhancements needed**:

1. **DMA support (UDMA/66)**:
   - Implement IDE DMA registers (Base Address Register, Command Register, Status Register)
   - Support PIO mode 0-4 and DMA mode 0-2 (UDMA/33 is sufficient for most use cases)
   - Implement DMA scatter-gather (IORDY not required for basic support)

2. **ATAPI passthrough**:
   - Implement ATAPI-specific commands: READ_TOC, READ_CAPACITY, MODE_SENSE
   - Support CD/DVD-ROM mode (for Windows installation ISOs)
   - Implement DMA for ATAPI (required for DVD playback)

3. **NCQ (Native Command Queuing)**:
   - At minimum: implement NCQ enable/disable
   - Full NCQ: implement command queue with reordering (optional, can be added later)

4. **IOAPIC interrupt routing**:
   - Ensure IDE interrupts (IRQ 14, 15) are properly routed through IOAPIC
   - Implement level-triggered mode (Windows requires level-triggered for IDE)
   - Implement EOI handling

**Files to modify:**
- `usr.sbin/vmd/i82093aa.c` — extend with DMA support, ATAPI improvements
- `usr.sbin/vmd/i82093aa.h` — add new register definitions, DMA state

**New structures to add to `i82093aa.h`**:
```c
#define I82093AA_IDEDMA_BASE 0x00
#define I82093AA_IDEDMA_STAT 0x02
#define I82093AA_IDEDMA_CMD  0x02

#define I82093AA_IDEDMA_REG_STAT_DRDY  (1<<0)
#define I82093AA_IDEDMA_REG_STAT_DSC    (1<<1)
#define I82093AA_IDEDMA_REG_STAT_DF     (1<<2)
#define I82093AA_IDEDMA_REG_STAT_RDY    (1<<3)
#define I82093AA_IDEDMA_REG_STAT_BSY    (1<<7)

#define I82093AA_UDMA_MODE_0  0x40
#define I82093AA_UDMA_MODE_1  0x50
#define I82093AA_UDMA_MODE_2  0x60
#define I82093AA_UDMA_MODE_3  0x70
#define I82093AA_UDMA_MODE_4  0x80
#define I82093AA_UDMA_MODE_5  0x90
#define I82093AA_UDMA_MODE_6  0xA0
```

### 3.2 VirtIO 1.0 and MSI-X

**What**: Ensure all VirtIO devices properly support VirtIO 1.0 feature negotiation and MSI-X interrupts, which Windows VirtIO drivers require.

**Current state**:
- VirtIO 1.0 feature negotiation exists (`virtio.h` has `VIRTIO_F_VERSION_1`)
- MSI-X capability is partially implemented
- PCI config space capabilities are present
- Virtio-net offers two queue pairs with a control virtqueue.  Independent TX
  workers and per-pair MSI-X vectors are runtime-validated at one, two, three,
  four and eight vCPUs; RX intentionally remains on queue 0 behind one tap
  reader.  Four-stream guest TX nearly doubles, while RX performance remains
  flat.  TSO is the next vionet performance item once its external diff is
  available.

**Enhancements**:
1. **MSI-X for all VirtIO devices**:
   - virtio-blk: at least 1 queue (req + optional compl)
   - virtio-net: at least 2 queues (rx + tx), support multiqueue (up to 4 queues)
   - virtio-scsi: request queue + 2 completion queues
   - Each queue should use a separate MSI-X vector

2. **VirtIO 1.0 feature flags**:
   - Ensure all devices expose correct VirtIO 1.0 features:
     - `VIRTIO_F_VERSION_1` (1)
     - `VIRTIO_F_RING_INDIRECT_DESC` (28)
     - `VIRTIO_F_RING_EVENT_IDX` (29)
     - `VIRTIO_F_VERSION_1` (35)

**Files to modify:**
- `usr.sbin/vmd/virtio.c` — MSI-X vector management, VirtIO 1.0 compliance
- `usr.sbin/vmd/vionet.c` — multiqueue support, MSI-X per queue
- `usr.sbin/vmd/vioblk.c` — MSI-X support
- `usr.sbin/vmd/vioscsi.c` — MSI-X support

### 3.3 VirtIO GPU Device

**What**: Emulate a VirtIO GPU device for graphical Windows output.

**Windows driver**: Already exists — `viogpu.sys` in the Fedora `virtio-win` driver package.
Vendor `0x1AF4` / Device `0x1050`. Used in production by Proxmox, XCP-ng, oVirt.
No need to write a Windows driver.

**Reference**: VirtIO GPU spec (https://docs.oasis-open.org/virtio/virtio/virtio-gpu/v1.1/cs01/virtio-gpu-v1.1-cs01.html)

**Features to implement (host-side emulation only)**:
1. **2D framebuffer (scanout)**:
   - Single scanout resource (linear memory)
   - Guest maps framebuffer memory via virtio queue
   - Host reads framebuffer and displays (or forwards to host display)
   - This is the **critical path** — required for a usable Windows desktop

2. **3D acceleration (VirGL)**: *deferred*
   - Uses Mesa3D's virglrenderer to render OpenGL in guest
   - Requires Mesa3D with virgl support on the OpenBSD host — non-trivial
   - Not required for Windows to install or function; only for 3D gaming
   - Mark as Phase 8 optional

**Implementation**:
- Create VirtIO GPU PCI device (vendor 0x1AF4, device 0x1050)
- Implement VirtIO features:
  - `VIRTIO_GPU_F_VIRGL` — 3D acceleration (optional, for VirGL)
  - `VIRTIO_GPU_F_RESOURCE_UUID` — resource UUID (optional)
  - `VIRTIO_GPU_F_SCANOUT` — scanout support
- Implement resource creation/deletion commands
- Implement framebuffer scanout
- Implement cursor support

**Files to create:**
- `usr.sbin/vmd/viogpu.c` — VirtIO GPU device emulation
- `usr.sbin/vmd/viogpu.h` — header

### 3.4 USB Controllers (xHCI + EHCI)

**What**: Emulate USB 3.0 (xHCI) and USB 2.0 (EHCI) controllers for Windows installation media (USB flash drives) and peripheral support.

**Reference**: Intel xHCI 1.1 spec, Intel EHCI 1.0 spec

**Features to implement**:
1. **xHCI (USB 3.0)**:
   - xHCI host controller (PCI device 0x8086:0x1e31 for Intel)
   - USB 3.0 root hub
   - Port power management
   - USB 2.0 companion controller (xHCI has built-in EHCI for legacy devices)

2. **EHCI (USB 2.0 companion)**:
   - EHCI host controller for USB 2.0 devices
   - USB 2.0 root hub

3. **USB tap device backend**:
   - Each USB port connected to a host USB device via tap
   - Forward USB traffic to/from host USB subsystem

**Implementation approach**:
- xHCI register emulation: base registers, cap length, oper regs, run regs
- Doorbell registers for endpoint notification
- Event rings for transfer completion
- Bandwidth scheduling (basic)

**Files to create:**
- `usr.sbin/vmd/usb_xhci.c` — xHCI USB 3.0 controller
- `usr.sbin/vmd/usb_ehci.c` — EHCI USB 2.0 companion controller
- `usr.sbin/vmd/usb.h` — common USB definitions
- `usr.sbin/vmd/usb_dev.c` — USB device abstraction

### 3.5 SMBus Controller (ICH9)

**What**: Emulate the Intel ICH9 SMBus controller for hardware enumeration and virtualization features.

**Reference**: Intel ICH9 datasheet, SMBus 2.0 spec

**Features**:
- SMBus Host Controller (PCI device 0x8086:0x2930 for ICH9)
- SMBus 2.0 compliance
- Host Command Interface (HCNT)
- Interrupt generation
- Block write/read support

**Files to create:**
- `usr.sbin/vmd/smbus_ich9.c` — ICH9 SMBus emulation

### 3.6 ICH9 LPC Bridge

**What**: Emulate the Intel ICH9 Low Pin Count (LPC) bridge for PnP, legacy I/O, and ACPI integration.

**Reference**: Intel ICH9 datasheet

**Features**:
- PCI-to-LPC bridge (PCI device 0x8086:0x291E for ICH9)
- Legacy IDE (fallback)
- COM port access
- Keyboard controller (8042)
- PS/2 mouse
- Serial mouse
- PIT/RTC access
- ACPI power management

**Files to create:**
- `usr.sbin/vmd/ich9_lpc.c` — ICH9 LPC bridge
- `usr.sbin/vmd/ich9_lpc.h` — header

### 3.7 PCI Bridge (ICH9 PCH)

**What**: Emulate the ICH9 Platform Controller Hub (PCH) for PCI-to-PCI bridging.

**Files to create:**
- `usr.sbin/vmd/ich9_pch.c` — ICH9 PCH

## Dependencies

- None external — all device emulation is pure C within vmd
- Existing VirtIO infrastructure
- Existing PCI infrastructure

## Risks

- **USB complexity**: xHCI is complex (event rings, bandwidth scheduling, multiple transaction types). Start with basic USB 1.1 support (uhci) and add USB 2.0/3.0 later.
- **GPU complexity**: VirtIO GPU spec is still evolving. VirGL integration requires Mesa3D build on OpenBSD (may not be available or may be difficult to build).
- **PCI device ID conflicts**: Windows has strict PCI device ID matching. Must use known-good PCI vendor/device IDs that Windows VirtIO drivers recognize.

## Implementation Order

1. Enhance PIIX4 IDE with DMA support
2. Improve VirtIO MSI-X support
3. Add ICH9 LPC bridge (enables legacy I/O, keyboard, PS/2)
4. Add SMBus controller
5. Add EHCI USB controller (USB 2.0, easier than xHCI)
6. Add xHCI USB controller (USB 3.0)
7. Add VirtIO GPU (2D only first, VirGL later)
8. Add ICH9 PCH
9. Test each device with Windows (check Device Manager for unrecognized devices)
