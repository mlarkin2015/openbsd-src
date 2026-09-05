# PLAN-003-DEVICE: Device Emulation

## Review Findings (2026-08 source audit) — CORRECTIONS TO THIS PLAN

1. **`i82093aa.c` is NOT a PIIX4 IDE controller — it is the Intel 82093AA IOAPIC**
   (full redir-table/MMIO emulation, registered as an MMIO device at
   `i82093aa.c:224`). The Current State section and §3.1 are based on a misreading.
   **There is no IDE/ATA/ATAPI emulation anywhere in the tree.** §3.1 must be
   reframed as "write a storage controller from scratch" or dropped entirely.
   The pragmatic Windows path is virtio storage with signed drivers injected into
   `boot.wim` and `install.wim`; a second virtio-scsi ISO cannot bootstrap the
   driver required to read that ISO.
2. **MSI/MSI-X and VirtIO 1.0 are now implemented and runtime-tested.** The stale
   implementation list below is retained only as historical scope; current status
   is recorded in §3.2.
3. **Missing critical devices not in this plan at all** (they gate Windows before
   anything listed here):
   - **Display**: no VGA/Bochs-GPU/virtio-gpu exists; the only concession to
     graphics is a comment about guests writing 0xb8000 (`x86_vm.c:189`). Windows
     Setup has no serial console mode.  Start with the OVMF ramfb/GOP surface and
     reuse the display backend for virtio-gpu later.
   - **Input**: i8042 keyboard and a virtio-input absolute tablet are now
     implemented and Windows-tested.  xHCI is not required merely to avoid
     relative mouse coordinates.
4. Disk formats are raw + qcow2 only (`VMDF_RAW`/`VMDF_QCOW2`, `vioblk.c:55`;
   `parse.y:1407`). No vhdx — document conversion instead of implementing it.
5. USB: defer xHCI and all host-device passthrough. A virtio-input absolute
   tablet provides the required pointer semantics with much less emulation.
6. SMBus/ICH9 (§3.6-3.8): keep, low priority; note that pairing ICH9 IDs with the
   existing PIIX3 PCI-ISA bridge (`pci.c`) will confuse guest drivers — either stay
   all-PIIX4 or move the whole platform profile to Q35/ICH9 as one coherent change.

## Goal

Add device emulation in vmd(8) in small, testable layers: a secure local display
service, firmware framebuffer, keyboard and absolute pointer first; virtio-gpu
2D and broader compatibility devices later.

## Current State

- **Storage**: virtio-blk and virtio-scsi only; `i82093aa.c` is the IOAPIC, not IDE
- **PIC**: `i8259.c` — master/slave 8259A interrupt controller
- **PIT**: `i8253.c` — 8254 programmable interval timer
- **RTC/NVRAM**: `mc146818.c` — real-time clock + CMOS memory
- **UART**: `ns8250.c` — ns16550 serial console
- **PCI**: `pci.c` — PIIX3 PCI-to-ISA bridge
- **VirtIO**: virtio-net, virtio-blk, virtio-scsi, virtio-rng,
  virtio-input absolute tablet (display VMs), virtio-vmmci
- **qcow2**: `vioqcow2.c` — qcow2 disk image reader

## What to Build

### 3.1 Windows Storage Compatibility

The first Windows install continues to use the existing virtio-blk and
virtio-scsi devices.  Produce a reproducible PowerShell/Windows ADK procedure
that:

1. injects the signed `vioscsi` and/or `viostor` drivers into every relevant
   index of `boot.wim`, so Setup retains access to both its media and target;
2. injects the storage drivers into the selected editions in `install.wim`, and
   optionally NetKVM, `vioinput`, and `viogpu` as those devices become usable;
3. exports `install.esd` to WIM when required and rebuilds the bootable ISO with
   `oscdimg`.

An unmodified installer is still useful before this work: reaching a visible,
interactive Setup screen is a separate display/input milestone even if no
installation disk is present.

Native Windows StorNVMe makes a minimal NVMe controller attractive later, but
it is not automatically less work than AHCI: admin and I/O queues, doorbells,
PRPs, identify data, reset/error behavior, MSI-X and flush semantics all must be
correct.  NVMe also does not provide an optical installer device.  AHCI/ATAPI,
NVMe and USB installation media therefore remain later compatibility projects,
not blockers for the injected-WIM path.

### 3.2 VirtIO 1.0 and MSI-X

**What**: Ensure all VirtIO devices properly support VirtIO 1.0 feature negotiation and MSI-X interrupts, which Windows VirtIO drivers require.

**Current state**:
- VirtIO 1.0 feature negotiation exists (`virtio.h` has `VIRTIO_F_VERSION_1`)
- generic PCI MSI and MSI-X capabilities are implemented for the modern
  virtio devices, including MMIO table/PBA masking and pending delivery;
  fixed xAPIC messages support physical and flat/cluster logical destinations
- PCI config space capabilities are present
- Virtio-net offers up to four queue pairs with a control virtqueue.
  Independent TX workers and per-pair MSI-X vectors are runtime-validated at
  one, two and four active pairs; RX intentionally remains on queue 0 behind
  one tap reader.  Moving from one to two workers nearly doubles parallel
  guest TX, but four workers remain at the same 2.2-2.3 Gbit/s ceiling.
- Virtio-net now negotiates checksum offload plus host TCPv4/TCPv6
  segmentation offload.  vmd translates guest virtio offload metadata into
  tap(4)'s native offload header while retaining its scatter/gather payload
  path.  Two-vCPU four-stream guest TX is runtime-validated at 17.8-20.4
  Gbit/s, versus the previous 2.34 Gbit/s mean.  Host receive offloads and
  merged receive buffers remain deliberately disabled, so RX is unchanged.
- VirtIO 1.x split queues map their descriptor, available and used areas from
  the three independent guest addresses supplied by the modern transport.
  Queue reset/reinitialization no longer depends on the negotiated feature
  bitmap after that bitmap has been cleared.  This is runtime-validated by
  NetBSD 11 with one and two vCPUs, including DHCP, MSI-X network interrupts,
  bridged ICMP traffic and clean shutdown.
- Virtio-scsi data-in replies are bounded by both the allocation length in the
  SCSI CDB and the guest's complete writable descriptor chain.  This prevents
  inquiry replies from overrunning shorter guest buffers and scatters READ(6)
  payloads across split buffers.  NetBSD 11 CD-ROM probing and byte-for-byte
  whole-image reads at 2 KiB and 64 KiB request sizes are runtime-validated;
  SYNCHRONIZE CACHE is accepted as a successful no-op for read-only media.
  REQUEST SENSE returns fixed-format NO SENSE data after a successful command,
  allowing OVMF's ScsiDiskDxe to enumerate the optical device.

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

### 3.3 Graphical Display Stack

The display path has three deliberately separate layers:

1. **Scanout producers.**  Start with OVMF ramfb/GOP so firmware and an
   unmodified Windows installer are visible.  Add a single-scanout virtio-gpu
   2D device later.  Both publish pixels and damage into the same backend.
2. **Staging surface.**  The VM process copies only the bounded framebuffer into
   shared staging memory.  The consumer gets neither arbitrary guest memory nor
   `/dev/vmm`.  Ramfb may initially use periodic comparison; virtio-gpu supplies
   explicit transfer/flush rectangles.
3. **Display worker.**  A separate, restricted process serves ordinary RFB on a
   mode-0600 Unix socket.  It has no Internet socket and no filesystem access
   after setup.  Remote users tunnel the Unix socket with SSH.  Direct TCP/TLS
   can be designed later around a tiny listener which passes accepted fds.

The RFB implementation starts single-client and omits clipboard, file transfer
and resize extensions.  All rectangles, pixel formats, message lengths, input
rates and output queues are bounded; malformed-client and slow-client behavior
must be regression-tested.  Do not reuse the old `/u/bin/src/OpenBSD/viogpu`
process/security design wholesale: it listens on all interfaces without
authentication and places network privileges in the VM process.  Its command
coverage and dirty-rectangle code are useful only as references.

Virtio-gpu 2D requires resource create/unref, attach/detach backing,
transfer-to-host, set-scanout, resource-flush, display-info and cursor commands.
The Windows virtio-win `viogpu` driver is a display-only/2D path; 3D/VirGL and
host renderer integration are explicitly deferred.

**Files to create:**
- `usr.sbin/vmd/display.c` / `display.h` — socket lifecycle, staging surface and
  typed IPC
- `usr.sbin/vmd/display_worker.c` — restricted Unix-socket RFB worker
- `usr.sbin/vmd/ramfb.c` / `ramfb.h` — OVMF fw_cfg producer and capture
- `usr.sbin/vmd/i8042.c` / `i8042.h` — keyboard and PS/2 fallback
- `usr.sbin/vmd/viinput.c` / `viinput.h` — absolute tablet (implemented)
- `usr.sbin/vmd/viogpu.c` / `viogpu.h` — later 2D scanout producer

### 3.4 Input Devices

Implement an i8042 keyboard first because OVMF and Windows Setup have inbox PS/2
keyboard support.  Validate controller commands, output-buffer status, keyboard
enable/reset/LED/scancode commands and IRQ 1 behavior.  A relative PS/2 mouse
on IRQ 12 is an optional fallback.

The keyboard portion is complete.  Unit coverage exercises controller and
keyboard commands, scan-set translation and IRQ state.  RFB input remains
responsive while an unchanged incremental framebuffer request is pending, and
the display worker no longer sends unchanged full-screen updates in a busy
loop.  Runtime validation passed in the Ubuntu 26 graphical UEFI installer.
Windows 8 checked and Windows 11 installers also reach graphical Setup and
accept keyboard navigation and button activation.  Windows exposed one
additional PS/2 semantic: keyboard reset and Set Defaults restore scanning,
rather than leaving it disabled until an explicit Enable command.  Controller
disable is also cleared by an ordinary keyboard-device write.  Both behaviors
now have regression coverage.

The preferred pointing device is a virtio-input tablet advertising absolute X
and Y axes plus buttons.  Absolute coordinates avoid pointer capture and edge
warping in a VNC window.  The Windows `vioinput` driver must eventually be
injected into the installer/installed image; the PS/2 fallback covers the
unmodified-media display milestone.  Do not invent a vendor-specific PS/2
touchpad protocol merely to obtain absolute coordinates.

The device implementation is complete, host-regression tested and Windows 10
runtime-tested.  Display
VMs expose the standard modern PCI ID `1af4:1052`, a byte-addressable 136-byte
device configuration, and event/status queues with MSI-X.  The RFB worker's
bounded pointer messages are scaled into a stable 0..32767 absolute range;
button, wheel, axis and SYN events are delivered as atomic reports.  The
device remains in the existing VM process and adds no listener or privilege
surface.

### 3.5 USB Controllers (Deferred)

xHCI is not needed for the first graphical or Windows-install milestones.
Implement it later only for a concrete USB-device requirement.  Full host USB
passthrough, EHCI companions and USB-over-network are separate projects with a
large parser and privilege surface.

### 3.6 SMBus Controller (ICH9)

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

### 3.7 ICH9 LPC Bridge

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

### 3.8 PCI Bridge (ICH9 PCH)

**What**: Emulate the ICH9 Platform Controller Hub (PCH) for PCI-to-PCI bridging.

**Files to create:**
- `usr.sbin/vmd/ich9_pch.c` — ICH9 PCH

## Dependencies

- None external — all device emulation is pure C within vmd
- Existing VirtIO infrastructure
- Existing PCI infrastructure

## Risks

- **Display attack surface**: Treat RFB input as untrusted.  Keep it in a worker
  without guest-memory, `/dev/vmm`, filesystem or Internet privileges and use
  bounded typed messages to the VM process.
- **USB complexity**: xHCI is complex (event rings, bandwidth scheduling and
  multiple transaction types), so it is deferred rather than used as the first
  absolute-pointer solution.
- **GPU complexity**: 3D renderer integration is substantially larger than the
  2D display-only Windows path and is not part of the initial implementation.
- **PCI device ID conflicts**: Windows has strict PCI device ID matching. Must use known-good PCI vendor/device IDs that Windows VirtIO drivers recognize.

## Implementation Order

1. Define display configuration, Unix-socket lifecycle, bounded staging
   surface and least-privilege process/IPC contract (complete).
2. Export OVMF ramfb/GOP through the RFB worker (complete; raw RFB encoding,
   1024x768 live smoke test).
3. Implement i8042 keyboard (complete); retain a PS/2 relative-pointer fallback
   as optional follow-up.
4. Boot an unmodified Windows ISO to a visible, interactive Setup screen.
5. Implement virtio-input absolute tablet (implemented, regression-tested and
   runtime-tested with Windows 10).
6. Inject storage/input drivers into Windows WIMs and reach disk selection.
7. Add virtio-gpu 2D on the same display backend.
8. Consider NVMe, AHCI/ATAPI or xHCI only after a specific compatibility need.
9. Keep SMBus/Q35/ICH9 and VirGL as coherent later platform projects.
