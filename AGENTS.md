# vmm-ng4: OpenBSD Hypervisor (vmm(4) + vmd(8))

**Root**: OpenBSD virtual machine monitor source tree (based on OpenBSD `1420f74a`).

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│  Host Kernel (vmm(4))                                               │
│  ┌─────────────────────────────────────────────────────────────────┐│
│  │  /dev/vmm (character device, ioctl interface)                   ││
│  │  ├── vm_create_params / VMM_IOC_CREATE                         ││
│  │  ├── vm_run_params / VMM_IOC_RUN                               ││
│  │  ├── vm_rwregs_params / VMM_IOC_READREGS + WRITEREGS           ││
│  │  └── vm_terminate_params / VMM_IOC_TERM                        ││
│  └─────────────────────────────────────────────────────────────────┘│
│  ├── vmx_machdep.c    (Intel VT-x: VMCS, VMX transitions, EPT)     ││
│  ├── svm_machdep.c    (AMD-V: VMCB, SVM transitions, RVI/NPIE)     ││
│  └── vmm.c            (vm/vcpu lifecycle, memory, pools)           ││
└─────────────────────────────────────────────────────────────────────┘
                            │ ioctl
┌─────────────────────────────────────────────────────────────────────┐
│  Userland (vmd(8) — privilege-separated daemon)                     │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐            │
│  │ vmm process  │  │ vm process   │  │ virtio subproc│            │
│  │ (fork+exec)  │  │ (fork+exec)  │  │ (per-device)  │            │
│  │ - config     │  │ - vcpu run   │  │               │            │
│  │ - imsg IPC   │  │   loop       │  │ - tap fwd     │            │
│  │ - vm mgmt    │  │ - exit hdlr  │  │ - qcow2 read  │            │
│  └──────┬───────┘  └──────┬───────┘  └───────────────┘            │
│         │ imsg             │ /dev/vmm                              │
│         └──────────────────┘                                       │
│  ┌─────────────────────────────────────────────────────────────────┐│
│  │  Hardware Emulation (in vm process)                             ││
│  │  ├── x86_vm.c   (boot, memory map, vcpu_exit handler)           ││
│  │  ├── acpi.c     (ACPI tables: RSDP, FADT, MADT, XSDT, DSDT)    ││
│  │  ├── pci.c      (PIIX3/4 PCI, BAR management, config space)    ││
│  │  ├── virtio.*   (network, block, SCSI, RNG, vmmci, scsi)       ││
│  │  ├── i8259.c    (PIC: master/slave interrupt controller)       ││
│  │  ├── i8253.c    (PIT: programmable interval timer)             ││
│  │  ├── i8250.c    (ns8250 UART: serial console)                  ││
│  │  ├── mc146818.c (RTC/NVRAM)                                    ││
│  │  ├── ns8250.c   (serial port)                                  ││
│  │  ├── i82093aa.c (PIIX4 IDE ATA controller)                     ││
│  │  ├── fw_cfg.c   (QEMU fw_cfg interface for OVMF)               ││
│  │  └── loadfile_elf.c (kernel/BIOS loading)                      ││
│  └─────────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────────┘
```

## Directory Structure

| Directory | Purpose |
|-----------|---------|
| `sys/arch/amd64/amd64/vmm_machdep.c` | Intel VT-x hardware VMX support: VMCS, MSR intercepts, CPUID, EPT |
| `sys/arch/amd64/amd64/vmm_support.c` | AMD-V hardware SVM support: VMCB, MSR intercepts, CPUID |
| `sys/arch/amd64/amd64/vm_machdep.c` | Architecture-independent vCPU creation/run wrappers |
| `sys/dev/vmm/vmm.c` | Device driver: `/dev/vmm`, vm/vcpu lifecycle, memory management |
| `sys/dev/vmm/vmm.h` | Kernel-side data structures (struct vm, struct vcpu) |
| `sys/dev/pv/pvreg.h` | Paravirtualization definitions: KVM, Hyper-V, Xen CPUID leaves |
| `usr.sbin/vmd/vmd.c` | Main daemon, privilege separation setup |
| `usr.sbin/vmd/vmm.c` | Parent process: VM lifecycle, config, imsg IPC |
| `usr.sbin/vmd/vm.c` | VM child process: boot, vCPU run loop, hardware init |
| `usr.sbin/vmd/x86_vm.c` | x86-specific: memory map, boot firmware, vcpu_exit handler |
| `usr.sbin/vmd/pci.c` | PCI emulation: PIIX3/4 device, BAR allocation, config space I/O |
| `usr.sbin/vmd/virtio.c` | VirtIO framework: common infrastructure, MSI-X capability |
| `usr.sbin/vmd/vionet.c` | VirtIO network (backed by tap(4)) |
| `usr.sbin/vmd/vioblk.c` | VirtIO block device |
| `usr.sbin/vmd/vioscsi.c` | VirtIO SCSI controller |
| `usr.sbin/vmd/vioqcow2.c` | qcow2 disk image reader |
| `usr.sbin/vmd/acpi.c` | ACPI table generation: RSDP, FADT, MADT/XSDT/DSDT |
| `usr.sbin/vmd/i82093aa.c` | PIIX4 IDE ATA controller (legacy disk) |
| `usr.sbin/vmd/i8259.c` | 8259 PIC interrupt controller |
| `usr.sbin/vmd/i8253.c` | 8254 PIT timer |
| `usr.sbin/vmd/ns8250.c` | ns8250 UART (serial console) |
| `usr.sbin/vmd/fw_cfg.c` | QEMU fw_cfg interface for OVMF/TianoCore |
| `usr.sbin/vmd/psp.c` | AMD SEV-ES support |
| `usr.sbin/vmd/sev.c` | AMD SEV support |
| `usr.sbin/vmd/config.c` | vm.conf(5) parsing |
| `usr.sbin/vmd/dhcp.c` | Built-in DHCP server for local interfaces |
| `usr.sbin/vmd/loadfile_elf.c` | ELF kernel loader (also handles BIOS images) |
| `usr.sbin/vmd/vmctl/` | `vmctl` management command |

## Build Commands

```bash
# Build vmd(8) and vmctl(8) from the source tree
cd /usr/src/usr.sbin/vmd && make obj && make && make install
# or from the project directory:
cd /u/bin/src/OpenBSD/vmm-ng4/usr.sbin/vmd && make obj && make && make install DESTDIR=/path/to/root

# Build just the kernel vmm(4) module (requires full sys tree)
cd /usr/src/sys/arch/amd64/compile/vmm && make clean && make

# Run regression tests
cd /u/bin/src/OpenBSD/vmm-ng4/regress && make regress

# Test individual component
cd regress/sbin/vmd && make regress
```

## Code Patterns and Conventions

**Kernel side (vmm(4)):**
- Dual backends: Intel VT-x (VMX) via `vmx_*` functions, AMD-V (SVM) via `svm_*` functions
- All hardware-specific code in `sys/arch/amd64/amd64/vmm_machdep.c` (VMX) and `vm_support.c` (SVM)
- Common device logic in `sys/dev/vmm/vmm.c`
- Exit handlers: `vmx_handle_exit()` / `svm_handle_exit()` dispatch by exit reason
- MSR intercepts use MSR bitmap (VMX) or MSR bitmap array (SVM) for fast path
- CPUID intercepts: `vmm_handle_cpuid()` — returns host CPUID for most leaves, fakes hypervisor info at `0x40000000`
- **EPT (Extended Page Tables)**: Hardware-assisted address translation. EPT violations exit to userspace for MMIO and device emulation.
- `vcpu_run()` returns on any exit; vmd(8) handles the exit and re-enters.

**Userspace side (vmd(8)):**
- Privilege separation: parent process (config/IPC), vmm process (VM lifecycle), vm process (vCPU run loop + device emulation)
- Communication: `imsg` library over Unix domain sockets or pipe pairs
- vCPU run loop: `vcpu_run_loop()` in `vm.c` — calls `VMM_IOC_RUN`, handles exits, dispatches to emulation
- Device emulation: each I/O port or MMIO range maps to a handler function via `ioports_map[]` array
- PCI config space accesses go through `pci_handle_config_space()` callback
- VirtIO devices: each device type has a subprocess (`vionet`, `vioblk`, `vioscsi`) communicating via pipe with message protocol (`viodev_msg`)
- **Exit handling** (`x86_vm.c:vcpu_exit()`): dispatches by exit reason — I/O, EPT violation, HLT, shutdown, etc.
- MMIO: `mmio.c` handles memory-mapped I/O through `vmx_handle_mmio()` / `svm_handle_mmio()`

**Key data flow for a VM boot:**
1. `vmd` parent reads `vm.conf`, opens disk images, tap devices
2. Forks + execs a `vm` child, sends it configuration via imsg
3. `vm` child calls `vmm_create_vm()` via ioctl
4. Calls `create_memory_map()` — sets up GPA ranges: low RAM, reserved BIOS area, MMIO window, RAM above 4G
5. Calls `load_firmware()` — if ELF, loads kernel; if binary, loads as BIOS image into 4GB-top area
6. Calls `init_emulated_hw()` — initializes PIC, PIT, RTC, UART, PCI, virtio, fw_cfg
7. Enters `run_vm()` — launches vCPU threads, each calling `VMM_IOC_RUN` in a loop
8. On each exit, `vcpu_exit()` dispatches to appropriate handler
9. VM continues until triple fault or explicit shutdown

## Key Files for Hardware Emulation

| File | What it emulates |
|------|-----------------|
| `i8259.c/h` | Intel 8259A PIC (master + slave, IRQs 0-15) |
| `i8253.c/h` | Intel 8254 PIT (timer channels 0-2) |
| `i82093aa.c/h` | PIIX4 IDE controller (ATA/ATAPI, IRQ 14+15) |
| `ns8250.c/h` | ns16550 UART (COM1 serial console) |
| `mc146818.c/h` | RTC/NVRAM (CMOS memory, IRQ 8) |
| `i82093aa.c` | PIIX4 ACPI registers (GPE, SMI) |
| `pci.c/h` | PIIX3/4 PCI-to-ISA bridge, config space access |
| `acpi.c/h` | Full ACPI tables (RSDP, FADT, MADT, XSDT, DSDT) |
| `fw_cfg.c/h` | QEMU fw_cfg interface (required for OVMF) |
| `virtio.c/h` | VirtIO PCI framework with capability chaining |
| `vionet.c/h` | VirtIO network device (tap backend) |
| `vioblk.c/h` | VirtIO block device |
| `vioscsi.c/h` | VirtIO SCSI controller |
| `vioqcow2.c` | qcow2 image format reader |

## Architectural Constraints and Limits

- **Max vCPUs per VM**: 64 (`VMM_MAX_VCPUS_PER_VM`)
- **Max VMs**: 512 (`VMM_MAX_VCPUS`)
- **Max VM memory**: 128 GB (`VMM_MAX_VM_MEM_SIZE`)
- **Max disks per VM**: 4 (`VMM_MAX_DISKS_PER_VM`)
- **Max NICs per VM**: 4 (`VMM_MAX_NICS_PER_VM`)
- **PCI BAR sizes**: fixed — MMIO 64KB, I/O 4KB
- **BIOS image limit**: 4 MB (in `loadfile_bios()`)
- **Memory map layout**: LOWMEM_KB(576) - 1MB reserved, 1MB - 0xE000 (PCI MMIO), 0xF000 (BIOS copy), 4GB - 4GB+BIOS

## BIOS / Firmware

- **Current**: SeaBIOS (`/etc/firmware/vmm-bios`)
- **Loading**: BIOS images are loaded into two locations: top of 4GB physical address space and the traditional 1MB-128KB region
- **fw_cfg**: QEMU fw_cfg interface already exists (`fw_cfg.c`) at ports 0x510-0x518 — this is the interface OVMF expects
- **ACPI**: ACPI 2.0+ tables are generated in C (not ASL/SSDT compiled). DSDT is minimal.
- **Boot flow**: BIOS → POST → PCI enumeration → boot device → load kernel/OS

## Paravirtualization Interface

The kernel already exposes CPUID leaves for multiple hypervisors (in `sys/dev/pv/pvreg.h` and `vmm_machdep.c`):

- **KVM**: CPUID leaves 0x40000000, 0x40000001 — exposes clocksource2, async PF, steal time, pv_eoi
- **Hyper-V**: CPUID leaves 0x40000000-0x4000000f — defines interface, version, features, enlightenment info
- **Xen**: CPUID leaves 0x40000000 — version and hypercall page
- **vmm(4)**: CPUID leaf 0x40000000 — reports signature "OpenBSDe"

Hyper-V features defined but not fully implemented:
- `HYPERV_FEATURE_EAX_VP_RUNTIME` (time reference)
- `HYPERV_FEATURE_EAX_SYNIC` (Simplified Interrupt Remapping)
- `HYPERV_FEATURE_EAX_STIMER` (Synthetic Timers)
- `HYPERV_FEATURE_EAX_VP_INDEX`

## ACPI Implementation

Current ACPI implementation (`acpi.c`):
- Generates RSDP at 0x9D000
- XSDT at 0x9E000
- MADT at 0x9F000 (interrupt controller info)
- FADT at 0xA0000 (PM1a/b event/control, SCI=IRQ9)
- DSDT at 0xA1000 (minimal, ASL-generated table loaded into guest)
- Uses ACPI 2.0+ conventions (both 32-bit and 64-bit addresses for extended tables)
- FADT int_model = `FADT_INT_MULTI_APIC`

## Gotchas

- **vCPU register state**: CR0, CR3, CR4 are modified by vmm(4) during VM entry/exit. Never assume they're preserved across `VMM_IOC_RUN` calls.
- **EPT shadowing**: vmm(4) handles EPT for guest physical memory. vmd only handles MMIO regions and EPT-assisted MMIO.
- **MSR interception**: Both VMX and SVM use MSR bitmaps. MSR intercepts are expensive — only intercept what's needed.
- **SEV-ES**: With SEV-ES, the hypervisor cannot directly read/write guest registers or flags — uses GHCB page for synchronization.
- **Thread safety**: vCPU run loops use pthreads. `vm_pause_barrier` synchronizes all vCPUs for pause/resume.
- **Pledge**: vmd drops privileges to `pledge("stdio vmm")` in the vm process before the run loop.

---

## Windows VM Support: Implementation Plan

### Overview

To run Microsoft Windows as a guest, vmm/vmd must support:
1. **UEFI firmware** (OVMF/TianoCore EDK2) — Windows 10/11 require UEFI with Secure Boot
2. **Microsoft Hypervisor interface (HVCI/Hyper-V)** — Windows expects specific hypervisor CPUID leaves and MSR interfaces
3. **Windows VirtIO drivers** — already available as open-source
4. **Additional device emulation** beyond what's currently provided
5. **Proper ACPI tables** for Windows PnP/Power management
6. **TPM support** for Windows 11 (PTM — Platform TPM)

### Phase 1: UEFI/OVMF Firmware Support

**Priority: Critical** — Windows 11 requires UEFI; Windows 10 can work with either BIOS or UEFI but UEFI is strongly preferred for driver compatibility.

#### 1.1 Integrate OVMF (TianoCore EDK2) BIOS image
- **File to create**: `/etc/firmware/ovmf.fd` — pre-built OVMF firmware blob
- **Build process**: Cross-compile TianoCore EDK2 for x86_64-unknown-openbsd
  - Source: https://github.com/tianocore/edk2
  - Target: `OvmfPkg/OvmfPkgX64.dsc` with OpenBSD platform overrides
  - Must include: `OvmfPkg/Sec/SecMain.inf` (PEI core), `OvmfPkg/Smm/SmiEntry/SmiEntry.inf` (SMM), `OvmfPkg/PeiCore/PeiCore.inf`
  - Required libraries: `OvmfPkg/MdePkg/MdePkg.inf` (core UEFI firmware library), `OvmfPkg/MdeModulePkg/MdeModulePkg.inf` (drivers)
- **Configuration**: OVMF must be built with:
  - `SECURITY_UPDATE_CAPSULE` — for Windows Update firmware updates
  - `FEATURE_ENABLE_PK` — Platform Key support for Secure Boot
  - `FEATURE_ENABLE_PLATFORM_CERT_LIST` — allow vendor certificates
  - `OVMF_SERIALIZE` — single-threaded SMM (simplifies OpenBSD vmm integration)
  - `ENABLE_SECURE_BOOT` — required for Windows 11

**Files to create:**
- `usr.sbin/vmd/ovmf/` — build scripts for OVMF integration

#### 1.2 Add firmware selection to vmd configuration
- **File**: `usr.sbin/vmd/vmd.h` — add `vmc_firmware` field to `struct vmop_create_params`
- **File**: `usr.sbin/vmd/config.c` — parse `firmware "ovmf"` or `firmware "seabios"` in vm.conf
- **File**: `usr.sbin/vmd/vm.c` — `load_firmware()` must detect firmware type and handle differently:
  - SeaBIOS: existing code path (16-bit real mode boot)
  - OVMF: 64-bit EFI payload boot (set RIP to OVMF entry point, setup EFI memory map)

#### 1.3 Add OVMF NVRAM variable store
- **File**: `usr.sbin/vmd/ovmf_nvram.c` — emulated NV storage for UEFI variables
- **Protocol**: UEFI variable services (`EfiRuntimeServices->SetVariable/GetVariable`)
- **Implementation**: Memory-backed variable store backed by a file on the host (`/var/vm/<vm>/nvram`)
- **Format**: UEFI NVRAM variable format (GUID + name + attributes + data + time service)
- **Secure Boot**: Must support signature database (db/dbx) management

**Files to create:**
- `usr.sbin/vmd/ovmf_nvram.c` — NVRAM variable store
- `usr.sbin/vmd/ovmf_nvram.h` — header

#### 1.4 Update boot sequence for UEFI
- **File**: `usr.sbin/vmd/x86_vm.c` — modify `load_firmware()` for OVMF boot path
  - Set up EFI memory map (per UEFI spec)
  - Place OVMF firmware at correct address (typically 0xE0000 or use fw_cfg to pass)
  - Set vCPU state for 64-bit EFI execution (already mostly in `vcpu_init_flat64`)
  - Configure GDT for long mode (already present)
  - Set up IDT for exceptions (GPF, #PF, triple fault)

### Phase 2: Hypervisor Interface (Hyper-V / HvCI)

**Priority: High** — Windows expects specific CPUID leaves and MSR interfaces that identify the hypervisor and provide paravirtualized interfaces.

#### 2.1 Implement Hyper-V CPUID leaves
- **File**: `sys/arch/amd64/amd64/vmm_machdep.c` — add Hyper-V CPUID handling in `vmm_handle_cpuid()`
- **Required leaves**:
  - `CPUID_HV_SIGNATURE (0x40000000)`: Return "MicrosoftHv" signature, max leaf 0x4000000D
  - `CPUID_OFFSET_HYPERV_INTERFACE (0x40000001)`: Return "WN01" (Windows compatibility)
  - `CPUID_OFFSET_HYPERV_VERSION (0x40000002)`: Return Hyper-V version (e.g., Windows 11 = 0x000F000C for version 10.0 build 26100)
  - `CPUID_OFFSET_HYPERV_FEATURES (0x40000003)`: Bitmask of supported features:
    - VP runtime (leaf 0xE0000005)
    - Time reference count (leaf 0xE0000007)
    - Synthetic timers (leaf 0xE000000B)
    - Hypercall interface (leaf 0x4000000F)
    - VP index
  - `CPUID_OFFSET_HYPERV_ENLIGHTENMENT_INFO (0x40000004)`:
    - `HYPERV_ENLIGHT Enlightened MSR support (bit 0)`
    - `HYPERV_ENLIGHT Enlightened VMCS (bit 1)` — required for Windows 8+
    - `HYPERV_ENLIGHT Enlightened Timer Interrupts (bit 2)`
    - `HYPERV_ENLIGHT Enlightened Reload TLB (bit 3)`
  - `CPUID_OFFSET_HYPERV_IMPL_LIMITS (0x40000005)`: Maximum hypercalls, MSR ranges

**Files to modify:**
- `sys/arch/amd64/amd64/vmm_machdep.c` — new case block for Hyper-V leaves in `vmm_handle_cpuid()`

#### 2.2 Implement Hyper-V MSRs
- **File**: `sys/arch/amd64/amd64/vmm_machdep.c` — add MSR intercept handlers
- **Required MSRs**:
  - `HV_X64_MSR_GUEST_OS_ID (0x40000000)`: Guest OS identifier MSR — written by guest to provide identity
  - `HV_X64_MSR_HYPERCALL (0x40000001)`: Hypercall page GPA — written by guest to provide GPA of hypercall page
  - `HV_X64_MSR_TSC (0x40000002)`: Time stamp counter reference
  - `HV_X64_MSR_EPOCH (0x40000003)`: Epoch counter
  - `HV_X64_MSR_REFERENCE_TSC (0x40000004)`: TSC reference page (struct hyperv_tsc_page)
  - `HV_X64_MSR_TIME_REF_COUNT (0xE0000007)`: Hyper-V time reference count
  - `HV_X64_MSR_APIC_ACCESS (0xE0000001)`: APIC base address (for synthetic APIC)

**Files to modify:**
- `sys/arch/amd64/amd64/vmm_machdep.c` — `vmx_handle_wrmsr()` and `vmx_handle_rdmsr()` cases

#### 2.3 Implement Hyper-V hypercall page
- **File**: `sys/arch/amd64/amd64/vmm_machdep.c` — handle hypercall page GPA mapping
- **Hypercalls to implement**:
  - `HV_X64_MCALL_SYNtheticTIMER (3)`: Set timer advance (for synthetic timers)
  - `HV_X64_MCALL_POST_EVENT (4)`: Post event
  - `HV_X64_MCALL_RESET_EVENT (5)`: Reset event
  - `HV_X64_MCALL_SET_TIMER (6)`: Set timer deadline
  - `HV_X64_MCALL_SIGNAL_EVENT (7)`: Signal event
  - `HV_X64_MCALL_NESTED_VP (17)`: Nested VM join (optional, later)
  - `HV_X64_MCALL_FLUSH_VIRTUAL_ADDRESS (12)`: Flush TLB
  - `HV_X64_MCALL_FLUSH_VIRTUAL_ADDRESS_LIST (13)`: Flush TLB list

**Files to create:**
- `sys/arch/amd64/amd64/vmm_hypercall.c` — hypercall implementation
- `sys/arch/amd64/amd64/vmm_hypercall.h` — hypercall interface header

#### 2.4 Implement Enlightened VMCS (eVMCS)
- **Priority**: Required for Windows 10+ performance (Windows will refuse to run with poor performance without it)
- **Reference**: "Enlightened VMCS" specification, Hyper-V v10 API
- **Key concept**: vmm(4) maps the guest's VMCS to the host — host VMM (vmm) reads/writes VMCS directly, avoiding double-VNIR
- **Implementation**:
  - Guest provides GPA of eVMCS region via MSR
  - vmm(4) maps eVMCS GPA to host virtual address
  - On VMX vm-entry/vm-exit, use eVMCS instead of normal VMCS
  - On VMCS reload, sync eVMCS with VMCS
- **File**: `sys/arch/amd64/amd64/vmm_evmpc.c` — enlightened VMCS support

#### 2.5 Implement Hyper-V VP runtime features
- **File**: `sys/arch/amd64/amd64/vmm_hv_runtime.c`
- **Required features**:
  - VP index MSR (`HV_X64_MSR_VP_INDEX (0x40000006)`): Returns vCPU index
  - Time reference page: Shared memory page with TSC → time conversion
  - Synthetic timers: Per-VP synthetic timer (via hypercall)
  - Event flags: Per-VP event flag array (1 page, 4096 flags)
  - SynIC (Simplified Interrupt Remapping): Message-signaled interrupts via MSIX

### Phase 3: Device Emulation

**Priority: High** — Windows requires standard legacy devices for boot and modern virtio drivers for performance.

#### 3.1 Enhanced PIIX4 IDE controller
- **Current**: `i82093aa.c` provides basic PIIX4 IDE emulation
- **Missing for Windows**:
  - DMA mode support (UDMA/66)
  - ATAPI command passthrough (for CD/DVD ROM)
  - NCQ (Native Command Queuing) support (at least basic)
  - Proper interrupt routing through IOAPIC
  - IDE bus master status register
- **File**: `usr.sbin/vmd/i82093aa.c` — extend DMA support
- **Note**: Windows has built-in drivers for PIIX4 IDE, so this should work with enhancements

#### 3.2 VirtIO driver support expansion
- **Current**: virtio-net, virtio-blk, virtio-scsi, virtio-rng, virtio-vmmci
- **Windows VirtIO drivers**: Available from Fedora, include drivers for all virtio devices
- **Required VirtIO features for Windows**:
  - VirtIO 1.0 (not just 0.9.1) — `VIRTIO_F_VERSION_1`
  - PCI config space notification (VIRTIO_PCI_CAP_PCI_CFG capability)
  - MSI-X interrupts (already partially supported)
  - Legacy PCI device IDs for Windows INF files
- **Files to modify**:
  - `usr.sbin/vmd/virtio.c` — ensure VirtIO 1.0 feature negotiation
  - `usr.sbin/vmd/vionet.c` — add MAC address passthrough for driver detection
  - `usr.sbin/vmd/vioblk.c` — add discard/TRIM support (VIRTIO_BLK_T_DISCARD)
  - `usr.sbin/vmd/vioscsi.c` — add SCSI passthrough improvements

#### 3.3 VirtIO GPU device
- **Priority**: Required for graphical Windows installation and use
- **Reference**: virtio-gpu spec, VirGL for 3D acceleration
- **Features**:
  - 2D framebuffer (scanout)
  - 3D acceleration via VirGL (Mesa3D)
  - Display output hotplug
- **Files to create:**
  - `usr.sbin/vmd/viogpu.c` — VirtIO GPU device emulation
  - `usr.sbin/vmd/viogpu.h` — header

#### 3.4 USB controller (xHCI)
- **Priority**: Required for USB-based Windows installation (USB flash drives)
- **Reference**: Intel xHCI 1.1 spec
- **Features**:
  - USB 3.0 host controller
  - USB 2.0 EHCI companion (for legacy device compatibility)
  - USB over TCP/IP for remote VMs
- **Files to create:**
  - `usr.sbin/vmd/usb_xhci.c` — xHCI emulation
  - `usr.sbin/vmd/usb_ehci.c` — EHCI companion controller
  - `usr.sbin/vmd/usb.h` — common USB definitions

#### 3.5 SMBus controller
- **Priority**: Required for Windows to enumerate hardware and for virtualization features
- **Reference**: Intel ICH9 SMBus controller
- **Features**:
  - SMBus 2.0 compliance
  - Host controller interface
- **Files to create:**
  - `usr.sbin/vmd/smbus_ich9.c` — ICH9 SMBus emulation

#### 3.6 Additional PCI devices
- **ICH9 LPC** — Low Pin Count bus (required for ACPI, PnP, legacy I/O)
- **ICH9 South Bridge** — PCI-to-PCI bridge, USB, SMBus integration
- **Intel E1000e NIC** — fallback for non-virtio scenarios (less performant)

**Files to create:**
- `usr.sbin/vmd/ich9_lpc.c` — ICH9 LPC bridge
- `usr.sbin/vmd/ich9_pch.c` — ICH9 PCH (Platform Controller Hub)

### Phase 4: ACPI Tables for Windows

**Priority: Medium-High** — Windows uses ACPI extensively for PnP enumeration, power management, CPU topology, and hardware discovery.

#### 4.1 Enhanced ACPI tables
- **File**: `usr.sbin/vmd/acpi.c` — major rewrite/enhancement
- **Changes needed**:
  - **FADT**: Add HPET base address, add flags for PME, ACPI 6.3+ compliance
  - **MADT**: Add local APIC override, IOAPIC entries for each IOAPIC, NMI sources
  - **HPET**: Add High Precision Event Timer table (required for Windows timekeeping)
  - **GTDT**: Generic Timer Descriptors (for ARM, not needed for x86)
  - **MCFG**: Memory-mapped PCI configuration (required for PCIe enumeration)
  - **SRAT**: System Resource Affinity Table (NUMA/topology)
  - **SLIT**: System Locality Distance Information Table
  - **SSDT**: Additional Embedded ACPI tables (CPU hotplug, device power states)
  - **HEST**: Hardware Error Source Table (for Windows error reporting)

#### 4.2 ACPI Device Objects (DSDT/SSDT)
- **Current**: DSDT is minimal
- **Needed for Windows**:
  - `_PCT` — Performance Control (CPU frequency scaling)
  - `_PSS` — Passive Performance States
  - `_PS0`/_PS3 — Device power states
  - `_OSC` — OS Control Handoff (for Windows to control PCIe features)
  - `_STA` — Device status
  - `_CRS` — Current Resource Settings
  - `_SB` — System Bus (root device)
  - `_PIC` — Interrupt Controller mode (APIC vs PIC)
  - `SCI_INT` — SCI interrupt number
  - `GPE0_blk`, `GPE1_blk` — General Purpose Events

#### 4.3 IOAPIC emulation
- **File**: `sys/arch/amd64/amd64/vmm_ioapic.c` (kernel) or `usr.sbin/vmd/ioapic.c` (userspace)
- **Current**: Uses MPBIO for IOAPIC but may not fully emulate
- **Needed**:
  - IOAPIC Redirection Table entries (one per interrupt)
  - Redirection table update mechanism
  - EOI handling
  - Level/edge trigger modes
  - Remote IRR
  - NMI delivery

### Phase 5: TPM Support (Windows 11)

**Priority: High for Windows 11** — Windows 11 requires TPM 2.0 (or a virtual TPM 2.0) for installation.

#### 5.1 PTPD (Platform TPM) emulation
- **Reference**: TPM 2.0 specification, PTPD specification (TPM for virtualization)
- **Features**:
  - TPM 2.0 command set (subset: NV initialize, create primary, RSA key generation, PCR extend/read, AIK quote)
  - PTPD interface (migrated commands over PCI)
  - PCR banks: SHA-1 (PCR 0-15), SHA-256 (PCR 16-23)
  - Endorsement hierarchy (seeded from host entropy)
  - Storage hierarchy
  - NV storage
- **PCI device**: TPM 2.0 TIS interface (TTX interface)
- **Files to create:**
  - `usr.sbin/vmd/tpm2.c` — TPM 2.0 emulation
  - `usr.sbin/vmd/tpm2.h` — header
  - `sys/arch/amd64/amd64/vmm_tpm.c` — kernel TPM device interface

#### 5.2 Secure Boot integration with TPM
- **PCR extension on boot**: Extend boot components to PCR 0-7
- **Measured boot**: Log boot components to TPM for attestation
- **Files to modify**:
  - `usr.sbin/vmd/ovmf_nvram.c` — integrate with TPM PCR extension
  - `usr.sbin/vmd/x86_vm.c` — TPM PCR update on firmware load

### Phase 6: Performance and Optimization

**Priority: Medium** — Make Windows VMs perform well enough for practical use.

#### 6.1 Paravirtualized clocksource
- **File**: `sys/arch/amd64/amd64/vmm_clock.c`
- **Implement**:
  - KVM clocksource (`KVM_MSR_SYSTEM_TIME`) — already partially present
  - Hyper-V time reference page (`HV_X64_MSR_REFERENCE_TSC`)
  - Xen clocksource (` Xen vtime`)
- **Features**:
  - Stable TSC (already handled by invariant TSC check)
  - pvclock structure with version numbers for atomic reads
  - Wall clock integration

#### 6.2 Paravirtualized interrupt delivery (VP EOI)
- **File**: `sys/arch/amd64/amd64/vmm_eoi.c`
- **Implement**:
  - KVM paravirtualized EOI (`KVM_MSR_EOI_EN`)
  - Hyper-V synthetic interrupt controller (MSI-based)
  - Guest writes to EOI MSR to acknowledge interrupt (no exit needed)

#### 6.3 Memory ballooning / overcommit
- **File**: `usr.sbin/vmd/balloon.c`
- **Features**:
  - VirtIO balloon device for dynamic memory management
  - Guest memory pressure reporting
  - Dynamic memory adjustment via `VMM_IOC_SETMEM` ioctl

#### 6.4 Live migration support
- **File**: `sys/arch/amd64/amd64/vmm_migrate.c` (kernel)
- `usr.sbin/vmd/migrate.c` (userspace)
- **Features**:
  - Save/restore VM state (register state, memory, device state)
  - Checkpoint/restore
  - Memory dirty page tracking

### Phase 7: Management and Tooling

**Priority: Medium**

#### 7.1 Windows-specific vm.conf options
- **File**: `usr.sbin/vmd/config.c` — parse new directives
- **New vm.conf options**:
  ```
  vm "windows11" {
      firmware "ovmf"          # or "seabios"
      memory 8192              # 8GB recommended minimum
      cpus 4                   # minimum 2 for Windows
      virtio                   # use virtio for disk/network
      disk "hd0" "/path/to/vhdx"
      disk "cdrom" "/path/to/en_windows_11.iso"
      secure-boot on           # enable Secure Boot (required Win11)
      tpm on                   # enable virtual TPM 2.0
      acpi-tables "windows"    # use Windows-optimized ACPI tables
  }
  ```

#### 7.2 VM configuration validation
- **File**: `usr.sbin/vmd/config_validate.c`
- Validate VM configuration for Windows compatibility:
  - Minimum 2 vCPUs for Windows 10/11
  - Minimum 4GB RAM (8GB recommended)
  - UEFI firmware required for Windows 11
  - TPM required for Windows 11
  - Secure Boot enabled for Windows 11
  - virtio drivers available for device types

#### 7.3 Guest OS detection and auto-configuration
- **File**: `usr.sbin/vmd/guest_detect.c`
- Detect guest OS from:
  - CPUID vendor string
  - ACPI table signatures
  - Bootloader identification
  - DHCP client FQDN/hostname
- Auto-enable Windows-specific features when Windows detected

#### 7.4 virtio driver package
- **File**: `usr.sbin/vmd/virtio-drivers/` — include Windows VirtIO drivers
- **Contents**:
  - `viostor.inf` — VirtIO block storage driver
  - `viorng.inf` — VirtIO random number generator
  - `viostor.inf` — VirtIO SCSI storage driver
  - `netkvm.inf` — VirtIO network driver
  - `balloon.inf` — VirtIO memory balloon driver
  - `viorng.inf` — VirtIO RNG driver
  - `vioGPU.inf` — VirtIO GPU driver (when implemented)

### Phase 8: Testing and Verification

**Priority: Medium**

#### 8.1 Regression tests
- **File**: `regress/sbin/vmd/` — add Windows-specific regression tests
- Test cases:
  - BIOS vs UEFI boot
  - VirtIO device enumeration
  - ACPI table validation (ACPI table checker)
  - Hyper-V CPUID/MSR leaf validation
  - TPM PCR measurements
  - Secure Boot verification

#### 8.2 Guest-side testing
- **File**: `regress/guest/` — scripts for guest VM testing
- Test suites:
  - Windows Hardware Compatibility Kit (WHCK) subset
  - Linux KVM self-tests (for basic virtio/ACPI validation)
  - Acpi-test (for ACPI compliance)
  - Windows Driver Kit (WDK) verification tools

### Summary of Files to Create/Modify

#### New files (creation):

| File | Description |
|------|-------------|
| `usr.sbin/vmd/ovmf_nvram.c` | UEFI NVRAM variable store |
| `usr.sbin/vmd/ovmf_nvram.h` | NVRAM header |
| `usr.sbin/vmd/ovmf/` | OVMF build scripts |
| `usr.sbin/vmd/viogpu.c` | VirtIO GPU device |
| `usr.sbin/vmd/viogpu.h` | GPU header |
| `usr.sbin/vmd/usb_xhci.c` | xHCI USB controller |
| `usr.sbin/vmd/usb_ehci.c` | EHCI USB controller |
| `usr.sbin/vmd/usb.h` | USB common header |
| `usr.sbin/vmd/smbus_ich9.c` | ICH9 SMBus controller |
| `usr.sbin/vmd/ich9_lpc.c` | ICH9 LPC bridge |
| `usr.sbin/vmd/tpm2.c` | TPM 2.0 emulation |
| `usr.sbin/vmd/tpm2.h` | TPM header |
| `usr.sbin/vmd/balloon.c` | Memory balloon device |
| `usr.sbin/vmd/migrate.c` | Live migration |
| `usr.sbin/vmd/config_validate.c` | VM config validation |
| `usr.sbin/vmd/guest_detect.c` | Guest OS detection |
| `sys/arch/amd64/amd64/vmm_hypercall.c` | Hyper-V hypercall handling |
| `sys/arch/amd64/amd64/vmm_hypercall.h` | Hypercall header |
| `sys/arch/amd64/amd64/vmm_evmpc.c` | Enlightened VMCS |
| `sys/arch/amd64/amd64/vmm_hv_runtime.c` | Hyper-V VP runtime |
| `sys/arch/amd64/amd64/vmm_tpm.c` | Kernel TPM interface |
| `sys/arch/amd64/amd64/vmm_eoi.c` | Paravirtualized EOI |
| `sys/arch/amd64/amd64/vmm_clock.c` | Paravirtualized clocksource |
| `sys/arch/amd64/amd64/vmm_migrate.c` | Migration support |

#### Modified files:

| File | Description |
|------|-------------|
| `usr.sbin/vmd/vmd.h` | Add firmware selection, TPM flags, Windows config options |
| `usr.sbin/vmd/config.c` | Parse firmware, TPM, secure-boot, Windows-specific options |
| `usr.sbin/vmd/vm.c` | Update boot flow for UEFI, TPM, balloon |
| `usr.sbin/vmd/x86_vm.c` | Handle OVMF boot path, TPM integration |
| `usr.sbin/vmd/acpi.c` | Major rewrite: Windows ACPI tables, HPET, MCFG, SRAT, SSDT |
| `usr.sbin/vmd/pci.c` | Add ICH9 devices, enhanced PCI config |
| `usr.sbin/vmd/virtio.c` | VirtIO 1.0, MSI-X improvements, PCI config caps |
| `usr.sbin/vmd/vionet.c` | MAC address passthrough, improved features |
| `usr.sbin/vmd/vioblk.c` | TRIM/discard support, VirtIO 1.0 |
| `usr.sbin/vmd/vioscsi.c` | SCSI passthrough improvements |
| `usr.sbin/vmd/i82093aa.c` | DMA mode support, ATAPI improvements |
| `usr.sbin/vmd/fw_cfg.c` | Enhance for OVMF NVRAM passthrough |
| `sys/arch/amd64/amd64/vmm_machdep.c` | Hyper-V CPUID leaves, Hyper-V MSRs, eVMCS, paravirtualized EOI, clocksource |
| `sys/arch/amd64/amd64/vmm_support.c` | AMD-V Hyper-V CPUID/MSR handling |
| `sys/dev/pv/pvreg.h` | Add missing Hyper-V feature definitions |
| `usr.sbin/vm.conf.5` | Document new vm.conf options |
