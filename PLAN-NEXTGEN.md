# PLAN-NEXTGEN: VMM/VMD Next-Generation Architecture

**Status**: authoritative roadmap as of 2026-09-09.

This document supersedes `PLAN-000-MASTER.md` through `PLAN-008-TEST.md` as
the planning source of truth.  Those files remain useful as design history,
implementation notes and test evidence, but their unqualified future-tense
statements no longer describe the tree.

The original objective was a path to a usable Windows guest.  That objective
has been met for Windows 10, and the remaining Windows 11 requirements are now
ordinary platform features rather than blockers for proving the VMM core.
The next phase should therefore be architecture-led: simplify ownership and
interfaces, restore privilege boundaries, add state serialization, and make
new accelerators, devices and host architectures plug into those interfaces.

## 1. Baseline already achieved

The following is the supported development baseline, not future work:

- SMP guests from one through eight vCPUs have been exercised with OpenBSD,
  Linux, FreeBSD and NetBSD.  The kernel ABI admits up to 64 vCPUs.  Firmware
  and direct OpenBSD kernel boot both support SMP.
- xAPIC, x2APIC, IOAPIC, INIT/SIPI, ordinary IPIs, PIC-through-LINT0 ExtINT,
  MSI and MSI-X are implemented.  AMD AVIC/x2AVIC accelerates interrupt
  delivery, EOI handling and halted-vCPU wakeup where hardware permits.
- SMP reboot, ACPI S5 poweroff and forced termination have been repaired and
  tested.  HPET is implemented and described by ACPI.
- SeaBIOS and an OpenBSD-built OVMF/EDK2 payload are supported.  UEFI guests
  have per-VM persistent or ephemeral variable stores, fw_cfg, SMBIOS and
  working optical and disk boot.
- ACPI supplies RSDP, XSDT, MADT, FADT, FACS, DSDT and HPET tables sufficient
  for the tested guests.  CPU topology reports distinct virtual cores.
- The graphical-console foundation is implemented as RFB over an owner-only
  Unix-domain socket.  A restricted display worker exports OVMF's ramfb/GOP
  surface.  i8042 keyboard input and a VirtIO absolute tablet work with
  Windows; OpenBSD has a matching `vioinput(4)` guest driver.
- VirtIO 1.x split queues, PCI MMIO capabilities, MSI-X, block, SCSI CD-ROM,
  network and input paths work with signed Windows VirtIO drivers.  Windows 10
  installs, boots and reboots from VirtIO storage.  The PowerShell media tool
  selects a coherent driver family for the target Windows release.
- VirtIO network supports up to four TX queue pairs, queue-affine MSI-X and
  guest TX checksum/TSO.  RX deliberately remains behind one tap reader.
- 32-bit non-PAE/PAE translation, protected-mode string I/O and the MMIO
  instructions encountered by OpenBSD, Linux and NetBSD i386 guests have been
  implemented.  The tested i386 guests boot.
- Focused regressions cover configuration, disk formats, RFB/display, ramfb,
  i8042, LAPIC, VirtIO input, VirtIO queues and x86 MMIO/MMU operations.

### Disposition of the original plans

| Original plan | Disposition |
| --- | --- |
| PLAN-001 OVMF | Core complete.  Secure Boot and crash-consistent variable persistence remain. |
| PLAN-002 Hyper-V | Not needed to boot Windows.  Recast as optional performance and compatibility work; do not advertise unimplemented enlightenments. |
| PLAN-003 Devices | Boot-critical VirtIO, display and input complete.  VirtIO GPU, sound and an inbox-driver storage controller remain optional additions. |
| PLAN-004 ACPI | Boot platform complete, including HPET.  PM timer/SCI events and richer PCI/NUMA tables are demand-driven follow-ups. |
| PLAN-005 TPM | Still open.  Implement by integrating a maintained TPM engine, not by writing TPM cryptography in vmd. |
| PLAN-006 Performance | AMD interrupt acceleration and TX scaling complete.  Intel APICv, clocks, ballooning, snapshots and migration remain. |
| PLAN-007 Management | Firmware, efivars, display configuration and media tooling complete.  A separate management plane remains. |
| PLAN-008 Testing | Substantial component and guest coverage exists.  Repeatable automation, Intel coverage and lifecycle stress remain. |

## 2. Architectural rules for new work

1. **One owner for each piece of emulated state.**  Kernel acceleration may
   cache or operate on state, but transitions between accelerated and software
   modes must use an explicit import/export contract.  Do not leave two LAPIC,
   timer or device models independently authoritative.
2. **Capability-driven hardware support.**  Detect architectural capability
   bits and validate the complete required set.  CPU model lists may encode a
   documented erratum, but must not be the normal feature-selection mechanism.
3. **Software fallback is part of the feature.**  APIC acceleration, clocks
   and optional device facilities must fail closed to the existing software
   implementation without changing the guest-visible machine unexpectedly.
4. **Preserve privilege separation.**  Network, display, audio, storage, TPM
   and rendering parsers belong in narrow workers with pre-opened descriptors,
   `unveil(2)` and `pledge(2)`.  vmd itself must not acquire a remote listening
   socket merely to support a management UI.
5. **Define state before migration.**  Every new device must have reset,
   quiesce, drain, save, restore and versioning semantics.  This is useful for
   suspend and reliable reset even before checkpoint/migration exists.
6. **Keep MI policy separate from MD mechanisms.**  VM lifecycle, memory-slot
   management, VirtIO, worker protocols and configuration should be machine
   independent.  Register sets, firmware boot, interrupt controllers and
   virtualization entry/exit remain machine dependent.
7. **Expose only implemented interfaces.**  In particular, Hyper-V CPUID/MSR
   bits, Secure Boot state, TPM capabilities and VirtIO feature bits must be
   backed by working behavior.
8. **Land small, bisectable milestones.**  Refactors should preserve behavior
   and tests before feature changes depend on them.

## 3. Priority 0: consolidation, security and repeatability

This is the recommended next milestone.

### 3.1 Restore process restrictions

Several processes still contain `DSDT DEBUG: pledge disabled` blocks,
including the main daemon, vmm process, VM process, control process, agentx
process and some VirtIO workers.  Audit each process and restore its least-
privilege boundary.  Firmware/DSDT data should be loaded or opened before the
restriction is applied and passed by descriptor where necessary.

Acceptance criteria:

- no temporary DSDT-debug privilege bypass remains;
- BIOS and UEFI guests, persistent efivars, display, networking and all disk
  formats pass with pledge enabled;
- the display socket remains local, mode 0600 and owned by the VM owner (root
  for an ownerless/ephemeral VM); and
- malformed RFB, fw_cfg and worker messages cannot make the parent open new
  paths or network sockets.

**Completed 2026-09-09.**  The DSDT is now read through its fixed unveil before
pledge, bounded to the 4 KB guest-physical slot reserved for it, and copied to
guest memory only after the VM memory map exists.  Pledge is restored in the
parent, control, vmm, VM, agentx, vioblk and vionet processes; vioscsi already
retained its restrictions.  Because tap(4)'s `TUNSCAP` ioctl is outside the
pledge ioctl allowlist, vionet performs only that ioctl on the parent-opened tap
descriptor before pledging, after receiving the fixed-size trusted device
message and before mapping or processing guest-controlled memory.  Path and
display-socket creation remain confined to the privileged parent.

The complete vmd regression suite passed.  Live smoke tests reached a login
prompt with four-vCPU BIOS and UEFI OpenBSD guests using VirtIO disks and
networking.  The UEFI test also exercised persistent efivars and the display
worker; its display socket was mode 0600, owned by the configured VM owner, and
removed when the VM stopped.

### 3.2 Make LAPIC acceleration vendor-neutral at the vmd boundary

`i82489dx.c` now contains x2APIC plus functions named for AMD AVIC.  The
problem is more than the historical Intel filename: a userland architectural
LAPIC model has learned the name and transfer details of one kernel backend.

Refactor in two behavior-preserving steps:

1. Rename the software model to `lapic.c`/`lapic.h` and keep only architectural
   xAPIC/x2APIC state, arbitration, timer and ICR behavior there.
2. Define a generic LAPIC-acceleration transition interface: capabilities,
   mode, register-state import/export, accelerated writes/IPIs, injection and
   fallback.  Keep AVIC/x2AVIC implementation details in the SVM MD code and
   later implement the same contract for VMX APICv.

This interface must make state ownership and locking explicit.  It must not
move only a second, partial LAPIC implementation into the kernel.

### 3.3 Split oversized functions and files along existing boundaries

Do not perform a cosmetic rewrite.  Begin with measured hotspots whose roles
are already separable:

- split the 10,000-line amd64 `vmm_machdep.c` into common x86, VMX, SVM,
  MSR/CPUID and interrupt-acceleration units without changing the ioctl ABI;
- split VM-entry control construction, register reset and diagnostic dumps
  from the VMX/SVM run loops;
- separate VirtIO PCI transport/configuration from queue and worker protocol
  code in `virtio.c`;
- separate descriptor validation/accounting from device-specific SCSI and
  network command execution; and
- break large lifecycle dispatch functions in `vmd.c`, `vm.c` and `vmm.c`
  into parse/validate/transition/cleanup operations.

The first audit list includes `vcpu_reset_regs_vmx`, `vcpu_run_vmx`,
`vcpu_run_svm`, `vmm_handle_cpuid`, `vionet_tx`, `virtio_io_cfg_field`,
`virtio_init`, `run_vm`, `vcpu_run_loop` and `i82489dx_mmio`.  Function length
alone is not a reason to split code; multiple state transitions or unrelated
failure unwinds are.

### 3.4 Clean up MSR and CPUID policy

Replace chains such as `handle_mtrr() || handle_mce() || handle_mca()` with a
table/range dispatcher that names the MSR class and returns an explicit result
(`handled`, `inject #GP`, or host error).  VMX and SVM should share emulated
MSR behavior while retaining MD interception setup.  Keep distinct handlers
and state for MTRR, PAT, MCE/MCA and paravirtual MSRs.

Add a per-VM CPUID policy object before Hyper-V or nested virtualization adds
more guest-visible leaves.  Preserve the current tested CPUID values during
the refactor.

### 3.5 Comments, diagnostics and test matrix

- Audit changed vmm/vmd/vmctl functions for OpenBSD-style preambles, accurate
  arguments, return values, locking, preconditions and postconditions.
- Remove stale debug prose and unconditional/chatty diagnostics.  Retain
  rate-limited diagnostics and counters that answer a concrete question.
- Add a single documented smoke matrix: OpenBSD amd64/i386, Linux amd64/i686,
  FreeBSD and NetBSD; BIOS/UEFI; 1/2/4/8 vCPU where relevant.
- Automate reset, halt, forced stop and repeated start/stop loops.  Add an
  Intel VMX host to the matrix before changing APICv.  Exercise pause/unpause
  with APs both running and blocked in HLT.
- Runtime-test legacy xAPIC AVIC on a host that advertises it; current field
  coverage is strongest on the x2AVIC-only Zen 4 mobile path.
- Turn the pure parsers and decoders (RFB, fw_cfg, MMIO instruction decode,
  VirtIO descriptors and SCSI CDBs) into reusable fuzz/regression harnesses.

## 4. Priority 1: Intel interrupt acceleration

Implement Intel APICv through the generic LAPIC acceleration contract from
3.2.  Select it by the VMX capability MSRs, not by marketing generation.

The coherent first level requires the relevant combination of TPR shadow,
APIC-access or x2APIC virtualization, APIC-register virtualization, virtual
interrupt delivery and EOI-exit bitmaps.  The software LAPIC remains the
fallback if the complete selected mode is unavailable.  Posted interrupts are
a second milestone; they require a posted-interrupt descriptor, notification
vector and scheduler/vCPU migration synchronization and must not be confused
with VT-d interrupt remapping for assigned devices.

Practical validation floors:

- exercise basic APIC virtualization on Haswell-era or newer hardware that
  advertises the complete selected control set; and
- treat posted interrupts as Broadwell-server-era or newer capability-driven
  work, with at least one older non-posted APICv machine in the fallback test.

Acceptance criteria:

- software xAPIC/x2APIC and APICv modes produce identical guest-visible LAPIC
  state across reset, INIT/SIPI, mode switches and EOI;
- OpenBSD, Linux, FreeBSD and Windows pass at 1/2/4/8 vCPUs on Intel;
- halted vCPUs return the physical CPU to the scheduler and wake directly for
  a deliverable interrupt without an avoidable userland round trip; and
- exit counters demonstrate the intended reduction without interrupt loss,
  storms or worse idle utilization.

## 5. Priority 2: display and desktop devices

### 5.1 VirtIO GPU 2D before legacy VGA

The existing ramfb/GOP path already supplies the Windows and Unix firmware
display.  Basic VGA would mainly benefit legacy BIOS graphical guests and is
not the best next step for the proven UEFI path.

Implement the standard VirtIO GPU 2D resource, transfer, flush and scanout
commands first.  Keep ramfb as the firmware/early-boot display, then hand the
same display worker a new scanout when the guest driver binds.  Put command
validation and any renderer in a dedicated worker; do not map untrusted guest
resource sizes directly into the RFB process.

The upstream Windows virtio-win tree contains a `viogpu` display driver, so a
standards-compatible 2D device can use the existing signed-driver ecosystem.
Do not make 3D a condition of landing the 2D device.

### 5.2 Configurable and runtime-resizable displays

Implement this in two stages:

1. Add validated fixed `width` and `height` display configuration, feed the
   selected mode to OVMF/GOP and ramfb, and size the initial RFB ServerInit
   surface accordingly.  Retain conservative defaults and the existing
   maximum allocation limits.
2. Add runtime resizing with VirtIO GPU display events and the RFB
   DesktopSize/ExtendedDesktopSize negotiation.  Viewers that do not negotiate
   resizing retain the prior surface or reconnect.

This belongs to the display/scanout abstraction, not VGA.  A future VGA
device can become another producer of the same scanout interface.

### 5.3 Accelerated 3D as an isolated experiment

VirtIO GPU 3D requires a renderer, host graphics API integration, fencing and
untrusted shader/command-stream handling.  It must run outside the VM and vmd
parent processes.  First prove resource lifetime, scanout and migration/reset
semantics with 2D; then prototype virgl or a newer context type in a sandboxed
renderer worker.

Windows 2D support is active upstream, but upstream Windows 3D remains an
experimental area rather than a dependable signed-driver path.  Development
of a new Windows driver can use test-signing, but a distributable driver needs
upstream acceptance or WHQL/Microsoft signing.  A downstream driver submitted
independently for signing must use distinct hardware IDs as requested by the
virtio-win project.  Do not make an OpenBSD-specific Windows 3D driver the
default roadmap.

### 5.4 Audio: separate protocol from transport

RFB has no interoperable standard audio channel.  Do not add private audio
messages to the display socket or expand the RFB worker's privileges.

Define a generic PCM/mixer backend owned by a dedicated audio worker and feed
it to sndio through a descriptor or narrowly unveiled Unix socket.  Remote
audio, if desired, should use sndio's own transport or a separate authenticated
proxy.  VM ownership and access policy should mirror the display socket.

Choose the guest-facing device after a short driver spike:

- VirtIO sound is the clean paravirtual design and is standardized, but there
  is no upstream virtio-win sound driver in the current driver tree and
  OpenBSD would also need a guest driver; or
- a minimal Intel HDA controller/codec is more emulation work but uses inbox
  Windows drivers and OpenBSD's existing `azalia(4)` stack.

If Windows audio is the goal, prototype HDA first.  If a small, auditable
device and Unix guests are the goal, implement VirtIO sound first.  Both can
share the sndio worker.

## 6. Priority 3: complete the standard Windows 11 platform

### 6.1 Virtual TPM 2.0

Do not implement the TPM command set or cryptography in vmd.  Port and isolate
`libtpms`/`swtpm` (or an equivalently maintained engine) and implement only the
guest transport, lifecycle and ACPI integration in this tree.

The preferred structure is one TPM worker per configured VM, a pre-opened
control/data channel, owner-only persistent state, and explicit startup,
shutdown, reset, save and restore operations.  Evaluate TPM2 CRB versus TIS
against OVMF and Windows inbox-driver behavior, then add the ACPI TPM2 table
for the selected transport.  Ephemeral TPM state must be opt-in and clearly
identified because replacing state invalidates sealed secrets.

### 6.2 Secure Boot

Build a Secure-Boot-capable OVMF variant with authenticated variable support.
Separate immutable firmware code from each VM's writable variable store and
provide an enrolled template containing the selected PK, KEK, db and dbx.
Define explicit `secure-boot` configuration rather than inferring it from the
guest name.

Before enabling it by default:

- make efivars updates crash-consistent (temporary file, fsync and atomic
  replacement or an equivalent journaled format);
- define ownership, cloning, backup and reset-to-template behavior;
- document key/dbx update policy; and
- validate signed OpenBSD/Linux media plus Windows 11 installation,
  `Confirm-SecureBootUEFI`, TPM provisioning, BitLocker/sealing and recovery
  after an intentional variable-store rollback.

TPM and Secure Boot are separate features.  Measured boot connects them only
after both individual paths are correct.

### 6.3 Hyper-V enlightenments are optional, not a boot dependency

Profile Windows exits before selecting a subset.  Likely useful first pieces
are VP index, reference TSC/time reference count, crash/reset reporting and
eventually targeted TLB flush or synthetic timers.  SynIC is justified only
if a real consumer is added.  Enlightened VMCS is nested-Hyper-V work, not a
requirement for an ordinary Windows guest.

Each CPUID bit must land with its MSR/hypercall behavior and regression.  Do
not change the hypervisor signature or claim the Microsoft interface merely
to make Windows choose a path that vmm cannot complete.

## 7. Priority 4: storage compatibility and guest hibernation

VirtIO storage is the preferred high-performance path and already works.  An
inbox-driver controller is useful for recovery media and installations that
cannot be rebuilt with VirtIO drivers, but is no longer a Windows blocker.

Implement a minimal NVMe controller before a new AHCI stack unless ATAPI or a
specific legacy guest becomes the requirement.  NVMe gives modern Windows and
Unix inbox drivers and lets the existing vioscsi device continue to serve
optical media.  Start with one namespace, admin identify/create queues, basic
read/write/flush, PRP validation and MSI-X; omit optional namespaces, SR-IOV
and advanced management.  Choose AHCI instead only when combined disk/ATAPI
legacy compatibility is worth its larger register/state-machine surface.

The hibernation note is guest-kernel work, not a request to make ordinary I/O
literally side-effect-free.  Add a `vioblk_hibernate_io` path analogous to
NVMe/AHCI/WD/SDMMC: preallocate all state, use a minimal polling virtqueue with
interrupts and most of the kernel unavailable, and support HIB_INIT, read,
write and HIB_DONE.  Register it in amd64/i386 hibernate device selection and
test S4 suspend/resume on a vioblk root disk.  vmd must continue servicing the
queue correctly while the guest is quiesced and must preserve flush ordering.

Retain the deferred REP INS/OUTS architectural corner cases and 80386 hardware
task-switch exits under legacy compatibility.  Implement them when a small
DOS/extender regression image is available; the task-switch path requires a
full descriptor/TSS validation and atomic state transition, not just advancing
RIP after the exit.

## 8. Priority 5: snapshots, memory control, checkpoint and migration

### 8.1 VirtIO balloon (`viomb`) host support

OpenBSD already has the guest `viomb(4)` driver; vmd lacks the device and the
kernel lacks the complete reclaim/control interface.  Implement the mechanism
before VVP rather than coupling it to VVP.  VVP later becomes a policy
consumer.

The first version should report requested versus actually reclaimed pages,
pin pages while ballooned, prevent device DMA mappings to reclaimed pages,
and return pages deterministically on reset/deflate.  Overcommit policy must
reserve host headroom and react to pressure; a balloon alone does not make
unbounded overcommit safe.

### 8.2 VM snapshots and disk image graphs

Treat a snapshot as a first-class VM state object, not as a synonym for a
qcow2 operation.  The user-visible model should distinguish:

- a **disk snapshot**, which preserves all writable disks and persistent
  platform state but not RAM or vCPU state;
- a **full VM snapshot**, which additionally preserves RAM, vCPU and emulated
  device state and resumes at the captured instruction;
- a **clone**, which creates a new VM identity and writable branch from a
  snapshot; and
- a **checkpoint**, which uses the same machine-state stream but is normally
  transient input to suspend, recovery or migration rather than a retained
  user-visible branch.

The qcow2 format supports internal snapshots by retaining alternate L1 tables,
and can store a VM-state blob.  Do not make that the first implementation.
vmd already supports external qcow2 backing files through `vmctl create -b`,
and external overlays provide a better initial control plane: each immutable
node is independently named, permissioned, checked and recovered; branching
does not rewrite a shared image's internal metadata; and a multi-disk snapshot
can be committed by publishing one manifest only after every disk operation
succeeds.  The current `VM_MAX_BASE_PER_DISK` limit is four, so snapshot work
must also provide bounded-chain validation and offline flatten/merge operations
rather than merely increasing the limit or allowing an unbounded recursive
chain.  Reading, importing or exporting qcow2 internal snapshots can be later
interoperability work.

A snapshot manifest should have a stable snapshot UUID and record its parent,
VM UUID, name/description and time; snapshot kind and power state; machine
configuration, CPU and firmware compatibility; every disk layer's path,
format, virtual size and backing identity; and the associated memory/device
state stream when present.  Persistent efivars and future TPM state must be
versioned with the snapshot.  Display sockets, tap descriptors and other host
resources are recreated on restore and are never serialized.  The manifest,
not a path inferred from `vm.conf`, is the atomic record of a multi-disk point
in time.  Validate graph cycles, missing or changed ancestors, ownership and
permissions before starting or restoring a VM.

Snapshot creation should initially be stop-the-world:

1. pause all vCPUs at the common barrier and quiesce/drain every device worker;
2. flush and synchronize writable disks, efivars and TPM state;
3. create a new writable qcow2 child for each active disk while preserving the
   frozen layers as the snapshot node;
4. save machine state for a full snapshot and atomically publish the manifest;
   and
5. switch to the new pre-opened disk descriptors and resume, or roll back the
   entire operation without publishing a partial snapshot.

This produces a crash-consistent guest filesystem.  Application-consistent
snapshots require a later guest-agent freeze/thaw protocol.  Restoring a
snapshot should branch a fresh writable child rather than modify the retained
node, so repeated restores are safe.  Raw disks initially support stopped,
full-copy snapshots only unless a separately trusted storage provider supplies
atomic snapshots.

Add an explicit `vmctl snapshot` command family, with unambiguous abbreviation
to `vmctl snap` if the command parser permits it:

- `create [-m] vm [name]`, where `-m` includes memory/device state;
- `list vm` and `show vm`, with `show` rendering an ASCII parent/child tree and
  marking the active branch and snapshots that contain memory state;
- `revert vm snapshot`, `clone vm snapshot newvm` and `delete vm snapshot`;
  and
- offline `merge`/`flatten` operations for collapsing chains.

The human-readable view should make branches and the selected head obvious,
for example:

```
$ vmctl snapshot show win10
base-install
`-- pre-update [memory]
    |-- test-driver
    `-- patched
        `-- * current
```

Deleting an interior node or modifying a shared ancestor must never happen
implicitly.  Prefer flattening into a new image followed by an atomic rename;
an in-place merge requires exclusive ownership and an explicit destructive
operation.  Interrupted create, restore and merge operations must leave either
the old graph or the new graph usable, with orphaned temporary layers clearly
reported and recoverable.

Stage this work as offline disk snapshots and tree inspection first, stopped
full-VM snapshots second, and short-pause snapshots of running VMs third.
Incremental RAM snapshots, persistent dirty bitmaps, retention policy and live
block commit come only after the simple graph and recovery rules are proven.

Acceptance tests must cover multiple writable disks, branching and repeated
restore, efivars/TPM generations, maximum chain depth, shared read-only bases,
missing or replaced ancestors, and host failure during each publication step.

### 8.3 Stop-copy checkpoint before live migration

Define a versioned state stream containing:

- VM identity, memory layout and negotiated feature/capability masks;
- all vCPU registers, pending exceptions and virtual interrupt state;
- LAPIC/IOAPIC/PIC, timers, RTC, PCI configuration and MSI/MSI-X state;
- every VirtIO queue index, in-flight request policy and worker state;
- display/input state, UEFI variables and TPM state when present; and
- a compatibility description for the destination CPU and firmware.

Implement local stop-copy save/restore first.  Require devices to drain or
fail checkpoint explicitly; never serialize live host pointers or file
descriptors.  Then add dirty-page logging and iterative pre-copy.  Network
transport, authentication and orchestration belong to VVP; vmm/vmd should
expose a local descriptor-based stream.

PCI-assigned devices and experimental 3D contexts are non-migratable until
they implement a device-specific contract.

## 9. Priority 6: PCI device assignment

The current `acpidmar` has substantial Intel DMAR and AMD IVRS/AMD-Vi domain,
mapping and invalidation groundwork, including prototype guest-page mapping
helpers.  It is not sufficient by itself: it is disabled in GENERIC and has no
complete vmm ownership, detach/reset, interrupt-remapping, unmap or recovery
contract.  Those helpers should be audited rather than exposed as an ABI.

VT-d is Intel's IOMMU architecture; AMD-Vi (AMD IOMMU, described by IVRS/IVHD)
is the AMD counterpart.  Require an IOMMU and assign the complete isolation
group, not a single function that shares an unsafe requester context.

Staged implementation:

1. make acpidmar/AMD-Vi reliable under normal host DMA, fault reporting,
   suspend/resume and detach on supported lab systems;
2. define a kernel PCI ownership API that quiesces and detaches the host
   driver, verifies the isolation group, performs FLR or an appropriate bus
   reset and grants one VM an opaque device handle;
3. pin and map only that VM's guest pages into a fresh IOMMU domain, with
   complete unmap/invalidation and teardown on every failure path;
4. mediate PCI config space and BAR placement instead of exposing arbitrary
   host configuration writes;
5. add safe MSI/MSI-X delivery.  Require interrupt remapping or an equally
   constrained design before exposing a device capable of arbitrary MSI
   writes; and
6. reset and return the device to the host only after DMA is disabled and the
   guest domain is destroyed.

Start with a dedicated USB controller or simple NIC, not a GPU.  A GPU adds
ROM/firmware, reset, large/rebar mappings and host-console ownership problems.
For a passed-through display, pass a dedicated USB controller containing the
guest's physical keyboard and mouse.  Keep the management console on
RFB/virtio-input; do not synthesize host desktop input into a VM behind vmd.

Initial assigned devices are incompatible with host suspend, hibernation,
checkpoint and migration, and configuration must say so.

## 10. Priority 7: nested virtualization

Recent-CPU-only support is a reasonable initial contract, but it does not make
nested virtualization small.  The hypervisor must virtualize VMXON/VMCS or
VMRUN/VMCB state, permitted control masks, nested EPT/NPT, ASID/VPID and TLB
invalidation, event injection, nested exits and the interaction with virtual
APIC acceleration.

Prerequisites are the MD split, CPUID policy, explicit interrupt-state owner,
and save/restoreable vCPU state.  Then:

1. expose neither VMX nor SVM unless the complete chosen baseline is present;
2. implement one backend at a time on the hardware available for continuous
   testing, initially without nested APICv/AVIC, SEV, TDX or device assignment;
3. boot a minimal L1 hypervisor self-test and one-vCPU L2 guest before SMP;
4. add nested paging composition and invalidation stress; and
5. test Linux KVM and a second independent L1 before calling the ABI stable.

This is a research-sized project and should not block ordinary VM features.

## 11. Priority 8: machine-independent core and other host architectures

The arm64, riscv64 and powerpc64 MD files in this tree are currently small
stubs, not partial functioning hypervisors.  The first cross-architecture
milestone is an MI/MD contract, not copying amd64's PC platform.

Keep MI:

- VM/vCPU lifecycle and ownership, memory slots and guest-page pinning;
- ioctl versioning and generic exit envelope;
- worker process model, disk/network backends and VirtIO transport core;
- configuration, control protocol, statistics and state serialization.

Keep MD or platform-specific:

- register and exception formats, entry/exit assembly and second-stage MMU;
- firmware/boot protocol, interrupt controller, timer and PCI host bridge;
- CPU feature policy and interrupt acceleration.

### arm64

Both VHE and non-VHE implementations are architecturally possible.  Start
with Armv8.1 VHE-capable systems because running the host kernel at EL2 avoids
much of the non-VHE host/EL2 trampoline and register-switching machinery.
Bring up one vCPU with stage-2 translation, generic timer, PSCI, GICv3 and an
EDK2/DT or ACPI platform, then SMP.  Add non-VHE Armv8.0 only after the MD
interface is stable; it is a separate entry/exit backend, not a small flag.

### riscv64

Target the ratified H extension first: HS/VS execution, `hgatp` two-stage
translation, virtual timer/interrupt state and an SBI presented to the guest.
The specification permits classic S-mode virtualization with M-mode traps and
shadow page tables, but that requires suitable machine firmware and is a much
slower, separate backend.  Treat it as later research, not a peer bring-up
mode.  Add AIA virtualization only after a basic interrupt model works.

### powerpc64

POWER9 is plausible but needs a fresh platform design around the privilege
model/firmware contract, radix second-stage translation, XIVE interrupt
virtualization and guest boot ABI.  The present stub supplies none of these.
Proceed only with dedicated Talos/POWER9 hardware, documentation and an owner
for long-term testing; otherwise arm64 and riscv64 provide more reusable
payoff first.

### octeon III

Limit this port to VZ-capable Octeon III; older Octeon generations are not
implementation targets.  A field probe on the 16-core CN72xx/CN73xx Infinity
test machine reported PRId `0x000d9703` and Config3.VZ set, confirming that it
provides CPU-level MIPS VZ hardware virtualization.  Its CIU3 interrupt
controller and 32 GB of memory make it the initial positive development and
regression target.  Marvell's CN7xxx documentation and upstream Linux's
explicit `CPU_CAVIUM_OCTEON3` handling independently agree with that result.

The dual-core CN50xx USG predates VZ and is not a viable vmm host.  Retain it
only as a negative test: a VZ probe and any future vmm attachment must reject
it cleanly without attempting to access GuestCtl registers.

The Infinity reports GuestCtl0 `0x0c0c0280`: guest-controlled hardware address
translation (`AT=3`), GuestCtl2, GuestCtl0Ext and pending-interrupt passthrough
are present.  GuestCtl1 is absent and `RAD=1`, so the CPU has no GuestID-tagged
TLB contexts and the guest TLB holds only one context at a time.  The backend
must therefore save, restore or invalidate guest TLB state when a physical CPU
switches between vCPUs instead of relying on a hardware GuestID.  Probe the
optional writable `DRG` mode separately before depending on direct root access
to guest mappings.

OpenBSD currently has no GuestCtl, guest-TLB, root/guest exception or vCPU
entry/exit support.  Obtain the programming manual before fixing an ABI.
Product variation within Octeon III may still matter, so do not assume that
every CN7xxx part exposes an identical optional VZ feature set.

With suitable hardware and documentation, begin with a narrow MIPS VZ
feasibility milestone: enter one guest context, virtualize CP0 and guest TLB
state, route a timer interrupt, and boot a single-vCPU flattened-device-tree
guest with a minimal VirtIO MMIO platform.
Only then decide whether the CIU3, PCI host bridge and platform maintenance
cost justify a supported port.  Octeon remains behind arm64/riscv64 in roadmap
priority because test hardware and documentation are scarce, not because the
silicon lacks virtualization.

## 12. Priority 9: VVP management plane

Build VVP as a separate port and daemon, not as a network listener inside vmd.
vmd remains a local privilege-separated mechanism.  A per-host VVP agent uses
the local control socket and narrowly scoped privileged helpers; the API/UI
process handles authenticated remote clients without receiving raw VM, disk,
tap or IOMMU descriptors.

VVP v1:

- inventory, create/start/stop/reboot and console-token lifecycle;
- storage image creation, cloning, snapshot ownership and retention policy;
- snapshot-tree visualization and orchestration of the local
  `vmctl snapshot` operations defined in section 8;
- bridge/vether/tap inventory and declarative virtual-switch attachment;
- event/audit log, local users or delegated authentication, roles and
  object-scoped permissions; and
- a versioned API plus reconciliation after either daemon restarts.

VVP v2:

- host federation, capability and resource inventory, placement policy;
- image distribution and shared-storage adapters;
- balloon/pressure policy and maintenance evacuation; and
- authenticated checkpoint transfer and live migration after section 8 is
  complete.

Do not make VVP's database the only record of a runnable VM.  Its desired
state must reconcile with vmd's actual state and survive partial operations.

## 13. Demand-driven compatibility work

These are real gaps but should not displace the architectural milestones
without a reproducing guest:

- ACPI PM timer and active PM1/SCI event sources.  HPET already exists; repair
  the prior PM-timer/PIT calibration disagreement before advertising another
  clock to CentOS or Windows.
- Remaining LAPIC/IOAPIC delivery modes: lowest-priority variants, NMI, SMI,
  INIT and IOAPIC-redirection ExtINT, plus EOI broadcast suppression.
- LAPIC TSC-deadline timer mode and x2APIC/remapped-MSI destination formats.
- REP INS/OUTS address-width, DF, segment-limit, cross-page and fault-injection
  corner cases.
- 80386 hardware task-switch exits and virtual-8086 MMIO/string-I/O details.
- Legacy BIOS VGA, relative PS/2 mouse and CSM-like compatibility.  Prefer an
  explicit `firmware bios` VM over adding a CSM to OVMF.
- Advanced ACPI such as `_OSC`, MCFG, SRAT/SLIT and HEST only when PCIe, NUMA,
  hotplug or error-reporting features actually consume them.
- VirtIO RX scaling.  One tap reader can classify packets to multiple guest
  RX queues, but adding readers alone does not remove the tap serialization
  point or provide stable RSS affinity.  Revisit only with measurements and a
  bounded vmd design; generic tap-layer redesign remains a separate project.
- Optional VirtIO storage discard/write-zeroes and network ECN/UFO offloads,
  only with precise feature negotiation and negative-path regressions.

## 14. Recommended milestone order

1. **N0 -- hardening and refactor scaffolding**: restore pledge/unveil, remove
   stale debug paths, establish the smoke matrix, genericize LAPIC acceleration
   state transfer, and clean up MSR dispatch.
2. **N1 -- Intel parity**: validate current SMP on Intel, implement APICv,
   then posted interrupts if supported and measurable.
3. **N2 -- desktop platform**: VirtIO GPU 2D, configurable initial resolution,
   runtime resize, then select HDA versus VirtIO sound with the shared sndio
   worker.
4. **N3 -- Windows 11 compliance**: external TPM 2.0 engine integration,
   crash-consistent efivars and Secure Boot templates; complete an unmodified
   Windows 11 installation.
5. **N4 -- stateful VM lifecycle**: vioblk guest hibernation, offline external
   disk snapshots and graph tools, device quiesce/save/restore contracts,
   stopped full-VM snapshots and local stop-copy checkpoint, short-pause
   running snapshots, then ballooning.
6. **N5 -- management**: VVP v1 on the stable local API; VVP v2 only after
   checkpoint and dirty logging support migration.
7. **N6 -- research tracks**: PCI assignment, nested virtualization, 3D
   rendering and non-amd64 hosts, each behind its own capability and test gate.

The best immediate next task is N0.  It reduces risk in every later item and
turns AMD AVIC and future Intel APICv into two implementations of one contract
instead of accumulating another vendor-specific path in the userland LAPIC.

## 15. Explicit non-goals for the next phase

- no direct TCP VNC or management listener in the vmd VM process;
- no home-grown TPM cryptographic implementation;
- no claim of Hyper-V, Secure Boot or migration capability before the entire
  advertised contract works;
- no Q35/ICH9, xHCI, SR-IOV or full USB stack without a demonstrated guest
  requirement;
- no 3D renderer in the privileged vmd parent or VM process;
- no live migration before local, versioned stop-copy restore; and
- no software-only architecture port presented as equivalent to supported
  hardware virtualization.

## 16. Primary references for future design work

- Intel 64 and IA-32 Architectures Software Developer's Manual, volume 3C:
  <https://cdrdv2-public.intel.com/825750/326019-sdm-vol-3c.pdf>
- Intel APICv feature summary:
  <https://edc.intel.com/content/www/us/en/design/products-and-solutions/processors-and-chipsets/core-ultra-200h-and-200u-series-processors-datasheet-volume-1-of-2/002/intel-apic-virtualization-technology-intel-apicv/>
- VirtIO 1.2 specification, including sound, GPU, input and balloon devices:
  <https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html>
- Current upstream Windows VirtIO drivers and status:
  <https://github.com/virtio-win/kvm-guest-drivers-windows>
- Maintained TPM engine and process wrapper:
  <https://github.com/stefanberger/libtpms>
  and <https://github.com/stefanberger/swtpm>
- Armv8-A virtualization and VHE overview:
  <https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Armv8-A%20virtualization.pdf>
- Ratified RISC-V H extension:
  <https://docs.riscv.org/reference/isa/priv/hypervisor>
- Power ISA specifications:
  <https://openpowerfoundation.org/specifications/isa/>
- qcow2 image format, including backing files, internal snapshots and VM-state
  records:
  <https://www.qemu.org/docs/master/interop/qcow2.html>
- Marvell Octeon III CN7xxx hardware-virtualization product brief:
  <https://www.marvell.com/content/dam/marvell/en/public-collateral/embedded-processors/marvell-infrastructure-processors-octeon-iii-cn70xx-71xx-ap-product-brief-2019.pdf>
- Linux MIPS VZ handling, including Octeon III-specific guest-TLB workarounds:
  <https://github.com/torvalds/linux/blob/master/arch/mips/kvm/tlb.c>
