# PLAN-000-MASTER: Bringing vmm/vmd to Windows Guest Support

**Status**: implementation in progress. **Date**: 2026-09-01.
**Scope**: OpenBSD hypervisor (kernel `vmm(4)` + userspace `vmd(8)`) capable of
installing and running Microsoft Windows 10/11 as guests.

Each PLAN-00N file covers its area in detail; each has a **Review Findings**
section with audit corrections against this tree (2026-08). Where a plan and this
master disagree, this master wins.

---

## 2026-08-28 implementation milestone

The following work is complete and guest-validated; it supersedes stale gap
descriptions in the original 2026-08-21 audit below:

- per-vCPU xAPIC MMIO state, LVT timer, IRR/ISR/TMR/PPR arbitration and EOI;
- IOAPIC edge/level delivery, remote-IRR/EOI behavior, PIC-through-LINT0 and
  ACPI legacy routing;
- ACPI-capable SeaBIOS/fw_cfg handoff and corrected RSDP/XSDT/MADT/FADT/DSDT;
- generic PCI MSI and MSI-X delivery used by modern virtio devices;
- MMIO AND and REX.B/REX.X address decoding needed by FreeBSD; and
- ACPI COM1 enumeration plus virtio block GET_ID support.  FreeBSD 15.1 now
  reaches and operates its serial installer; OpenBSD and Alpine Linux reach
  login with APIC and MSI/MSI-X active.

The initial SMP gate is now implemented and validated through complete
2-, 4- and 8-vCPU OpenBSD GENERIC.MP boots.  `vmm(4)` admits up to 64 vCPUs;
vmd parks AP threads until INIT-SIPI; the LAPIC ICR handles physical fixed,
INIT and STARTUP IPIs; and CPUID/APICBASE identify the configured topology and
BSP correctly.  A two-vCPU `vmctl start -b` direct-kernel boot also attaches
the AP and reaches mountroot using vmd's BDA RSDP and generated ACPI tables,
without a SeaBIOS payload.
`vm.conf` and `vmctl` expose validated vCPU-count controls; `vmctl start -p`
now overrides a configured VM for one boot and restores its configured count
after stop.  Host builds and focused LAPIC/config regressions pass.  IOAPIC
fixed delivery resolves physical, flat-logical and cluster-logical
destinations, while lowest-priority delivery selects by PPR and rotates equal
ties.  AMD SVM reset now clears stale virtual-
interrupt-window state, which had delivered dummy vector zero when the AP first
enabled interrupts.  An atomic kernel assertion latch prevents concurrent
`VMM_IOC_INTR` calls from being lost to a stale `VMM_IOC_RUN` pending snapshot;
this fixed the four/eight-vCPU fsck/mountroot TLB-shootdown hang.  FreeBSD
15.1-p3 SMP testing exposed both an incorrect AMD extended-APIC-space bit in
the emulated LAPIC version and an x2AVIC unaccelerated-EOI bug: vmd cleared the
IOAPIC remote-IRR state, but the hardware LAPIC backing page retained the old
ISR bit and PPR class.  vmm now retires the reported in-service vector and
recomputes PPR before vmd completes the IOAPIC half.  FreeBSD boots, permits
login and shuts down cleanly with two vCPUs, and sustains guest-to-host iperf3
load with eight; four-vCPU OpenBSD and four/eight-vCPU Linux tests also pass.
SeaBIOS now receives the configured CPU count, parked APs no longer spin for a
uniprocessor guest, and VM-wide stop/reset coordination makes OpenBSD and
Ubuntu SMP reboot plus Ubuntu halt/poweroff reliable.  Repeated reset loops,
pause/unpause and Intel VMX validation remain.

The closeout also restored 32-bit guest support.  The userspace GVA walker now
handles non-PAE and PAE32 page-table formats and high physical frames
correctly; MMIO decoding covers the OpenBSD/i386 LAPIC `pushl`, `subl` and
`cmpl` sequences plus `popl` directly into the task-priority register; and
fixed or lowest-priority MSI/MSI-X messages resolve
xAPIC physical and flat/cluster logical destinations.  OpenBSD 7.9/i386 boots,
networks and powers off, while OpenBSD 8.0/i386 GENERIC.MP boots with four
vCPUs and shuts down cleanly.  Gentoo/i686 6.12 boots normally with virtio
MSI-X after a `pci=nomsi` control isolated the logical-delivery fault.
CentOS 7/i386 and amd64 now boot normally after adding a FACS for its older
ACPICA and accepting its xAPIC lowest-priority MSI-X messages.  The absent
ACPI PM timer remains a separate clock/platform gap.

NetBSD 11/amd64 and i386 now exercise the modern VirtIO split-ring paths.
Virtio-scsi data-in replies are bounded by both the CDB allocation length and
the complete writable descriptor chain, preventing short inquiry buffers from
being overrun and allowing READ(6) payloads to span multiple descriptors.
NetBSD CD-ROM probing and full-image reads at 2 KiB and 64 KiB request sizes
are runtime-validated without corruption.

## Deferred cleanup

- **MSR handling cleanup**: replace the temporary chained facility dispatch in
  the VMX and SVM MSR-exit paths (for example, `handle_mtrr() || handle_mce() ||
  handle_mca()`) with a clearer indexed or classified dispatch scheme.  Keep
  MTRR, PAT, MCE, MCA and future Hyper-V MSR policy separated by architectural
  facility while avoiding an ever-growing chain in each RDMSR/WRMSR default
  case.  This is cleanup rather than a blocker for the current Windows boot
  investigation.

## Verified current state (original source audit, 2026-08-21)

What exists and works today:

- SeaBIOS boot path: raw BIOS image loaded ending at 4 GiB, `vcpu_init_flat16`
  (`usr.sbin/vmd/x86_vm.c:302`, `loadfile_bios()`); ELF kernels via flat64.
- fw_cfg at 0x510-0x518 incl. DMA interface; files: `etc/e820`,
  `etc/screen-and-debug`, `bootorder` (`fw_cfg.c:97-120`).
- ACPI: RSDP/XSDT/MADT(LAPIC+IOAPIC)/FADT generated in C (`acpi.c`); DSDT loaded
  from `/etc/firmware/vmm.dsdt`, optional.
- IOAPIC emulation (`i82093aa.c`) + local APIC (`i82489dx.c`).  The original
  skeletal implementation has since been replaced as described above.
- VirtIO 1.0 feature negotiation (`virtio.c:1005+`); devices: net, blk, scsi,
  rng, vmmci. Disks raw + qcow2.  MSI/MSI-X has since been implemented.
- Kernel CPUID leaves: "OpenBSDVMM58" @0x40000000, KVM compat @0x40000100
  (`vmm_machdep.c:6563+`). VMCALL/VMMCALL exits currently inject #UD.
- EPT-violation → userspace MMIO dispatch (`x86_mmio.c`). SEV/SEV-ES scaffolding.

What does **not** exist (gaps that gate Windows):

| Gap | Impact on Windows |
|---|---|
| SMP lifecycle/architecture coverage incomplete | OpenBSD 2/4/8, Linux 4/8 and FreeBSD 2/8 boot; SMP reboot and halt/poweroff pass, while repeated reset loops, pause/unpause and Intel VMX still need validation |
| No display device or display service | Windows Setup is graphical — cannot install blind |
| No input (i8042 or virtio-input tablet) | Cannot interact with Setup |
| No IDE/AHCI/NVMe storage | Storage is virtio-only; Windows drivers must be injected into installation media |
| Minimal DSDT, optional, no _OSC/HPET or PM timer | Device discovery/power management limitations |
| UEFI lacks a graphical host path | OVMF boots, but its GOP framebuffer cannot yet be viewed |
| No Hyper-V TLFS interface | Windows runs unenlightened (or refuses some features) |
| No TPM 2.0 | Win11 installer hardware check fails |
| OVMF built without Secure Boot support | Win11 installer hardware check fails; UEFI alone is insufficient |

## Design decisions to make first

1. **CPUID policy location**: Hyper-V signatures require per-VM CPUID policy.
   Either bake into kernel `vmm(4)` per-guest state, or add delegation
   (`VMM_IOC_SETCPUID`-style, like KVM_SET_CPUID2) so policy lives in vmd.
   Recommend the ioctl route: keeps policy in userspace, one kernel change.
2. **Platform profile**: stay all-PIIX4 (matches existing pci.c bridge) vs move to
   Q35/ICH9 for PCIe/MCFG/TPM coherence. Recommend: PIIX4 profile for the first
   Windows milestone; Q35 later as a separate coherent change.
3. **Storage strategy for installation**: keep the existing virtio block/SCSI
   devices and inject the signed Windows virtio drivers into `boot.wim` and
   `install.wim`.  A second virtio-scsi driver ISO cannot bootstrap its own
   controller.  AHCI/ATAPI or NVMe remains a later compatibility project.
4. Secure Boot and TPM 2.0 remain deferred until the core Windows 10 platform
   works.  Both are independent requirements for a supported Windows 11 VM;
   UEFI alone does not satisfy the installer hardware checks.  An unsupported
   installer bypass is useful only for development and is not a completion
   criterion.

## Graphical console decisions (2026-09)

The first display implementation will keep the network protocol out of the VM
process and will not expose a TCP listener by default:

1. OVMF's ramfb/GOP output and, later, virtio-gpu 2D scanout feed a small,
   bounded staging framebuffer.  The VM process copies only the display surface
   into that object; the display worker does not receive arbitrary guest RAM or
   `/dev/vmm`.
2. A separate display worker serves ordinary RFB over an `AF_UNIX` socket.
   TigerVNC can open such a socket directly.  Remote access uses an SSH local
   forward to the Unix socket; direct TCP service is deferred.
3. The proposed per-VM configuration is:

   ```
   firmware uefi
   display {
       socket "/var/run/vmd/windows11.vnc"
   }
   ```

   The current ramfb producer is specific to OVMF, so `display` requires
   `firmware uefi`.  A future BIOS-capable VGA or display device can relax this
   restriction once it provides a real scanout.

   If `socket` is omitted, vmd derives a collision-free name beneath
   `/var/run/vmd`.  The socket is mode 0600.  Its owner is the owner explicitly
   assigned to the VM in `vm.conf`; it is root when no owner was configured and
   for every manually started/ephemeral VM.  The configured path must be
   absolute.  vmd rejects symlinks and pre-existing non-sockets, records the
   device/inode of sockets it creates, and only unlinks that exact socket during
   teardown.
4. The worker is initially single-client and implements only the bounded RFB
   messages needed for framebuffer updates plus keyboard and pointer input.  No
   clipboard, file transfer, resize extension, or unauthenticated non-local TCP
   listener is in the first version.  The RFB parser and input IPC require
   strict length/rate checks and fuzz/regression coverage.
5. i8042 provides the first firmware/Setup keyboard.  A virtio-input absolute
   tablet (`ABS_X`/`ABS_Y`) is the preferred pointer after its Windows
   `vioinput` driver can be injected; this avoids both relative-pointer warping
   and implementing xHCI solely for a tablet.  A relative PS/2 mouse may remain
   a fallback.
6. Virtio-gpu is a later 2D device using the same staging/display backend.  The
   available Windows driver is principally a WDDM display-only path; VirGL/3D is
   not part of the initial Windows milestone.

## Phases

### Phase 0 — Make the platform bootable for a graphical OS (critical path)

Everything else depends on these. Order within the phase:

1. **LAPIC/SMP completion** (kernel, `vmm.c`/`vmm_machdep.c` + userspace
   `vm.c`/`i82489dx.c`): multi-vCPU admission, AP wait-for-SIPI, physical ICR
   fixed/INIT/SIPI delivery, logical destinations, IOAPIC lowest-priority
   arbitration and CPUID/APICBASE topology are implemented.  OpenBSD guests
   boot with 2, 4 and 8 vCPUs, Linux with 4 and 8, and FreeBSD with 2 and 8;
   the eight-vCPU FreeBSD test sustained guest-to-host network load.  OpenBSD
   and Ubuntu SMP reboot and Ubuntu halt/poweroff also pass.  Continue with
   repeated reset loops, longer stress and pause/unpause behavior.
2. **OVMF as ROM — complete** (userspace): OVMF boots OpenBSD from an EFI disk
   and installer image; `firmware uefi|bios`, persistent per-VM `efivars`,
   ephemeral variables, fw_cfg ACPI handoff, reset and firmware-setup behavior
   are implemented and runtime-tested.  Graphical output is deliberately the
   next layer rather than part of firmware loading.
3. **SMBIOS generation — complete** (userspace, `smbios.c`): types 0,1,2,3,4,
   16,17,19,20,32 and the end marker are exposed via fw_cfg.  Type 4 describes
   the single virtual package with one core/thread per vCPU, matching the CPUID
   topology.  The Type 1 UUID is deterministic for the VM name and instance.
4. **Display service foundation — complete**: `display { socket ... }`,
   collision checks, mode-0600 ownership, privileged listener creation, fd
   handoff and inode-safe teardown are implemented.  The single-client RFB
   worker is isolated by fork/exec and pledge and receives only its listener,
   a read-only bounded staging surface and typed input channel.  Raw RFB
   rectangles, strict native pixel-format negotiation and bounded key/pointer
   messages have regression coverage.
5. **Firmware display — complete**: OVMF's QemuRamfbDxe writes the standard
   big-endian `etc/ramfb` descriptor through fw_cfg DMA.  The VM process polls
   that bounded guest range into the staging surface.  A live 1024x768 OVMF
   boot produced nonblank pixels through the RFB Unix socket; forced VM stop
   also tears down the worker and socket.
6. **PS/2 keyboard — complete**: the i8042 controller implements ports
   0x60/0x64, IRQ 1, controller and keyboard command responses, scan-set
   translation, and RFB keysym input.  An Ubuntu 26 UEFI installer accepted
   keyboard input throughout its graphical installer.  Windows 8 checked and
   Windows 11 installers now reach graphical Setup and accept keyboard
   navigation and button activation.  Reset and Set Defaults correctly restore
   keyboard scanning, as required by the Windows i8042 driver.  A virtio-input
   absolute tablet remains next; retain a relative PS/2 mouse only as a
   fallback.
7. **Visible Windows Setup milestone — keyboard complete**: an unmodified
   Windows 11 installer reaches graphical Setup and accepts PS/2 keyboard
   input.  Absolute pointer input remains before M0a is complete.  Setup need
   not discover the virtio installation disk yet.
8. **Windows installation-media workflow**: provide a reproducible Windows
   PowerShell/ADK procedure to inject `vioscsi` and/or `viostor` into every
   relevant `boot.wim` index and selected `install.wim` editions.  Add NetKVM,
   `vioinput`, and `viogpu` to `install.wim` as those devices land.
9. **Virtio-gpu 2D**: implement one scanout with explicit transfer/flush damage
   and reuse the display worker.  Defer 3D/VirGL.
10. **ACPI foundation** (PLAN-004 subset): real DSDT authored in ASL (PCI0 with
   _PRT/_CRS/_OSC, RTC, PIT, PS2K/PS2M, power button, _S5 sleep), interrupt
   source overrides in MADT, consistent FADT PM1 blocks with actual SLP_TYP/SLP_EN
   shutdown emulation.

**Milestone M0a**: An unmodified Windows 10/11 x64 installer reaches a visible
graphical Setup screen; keyboard and absolute pointer input work.  Storage may
still be absent.

**Milestone M0b**: Setup media with injected virtio drivers reaches the
partitioning screen, sees the installation disk, and can power off cleanly.

### Phase 1 — Enlighten the guest (Hyper-V TLFS, corrected PLAN-002)

1. Decide/implement CPUID delegation (design decision #1).
2. CPUID leaves 0x40000000-0x40000005 with genuine TLFS values ("Microsoft Hv",
   "Hv#1", feature bits limited to what we implement); hypervisor-present bit +
   invariant TSC exposure audit.
3. MSRs: GUEST_OS_ID, HYPERCALL, VP_INDEX, RESET, REFERENCE_TSC, TSC/APIC
   frequency, TIME_REF_COUNT, CRASH_P0-P4 (numbers per PLAN-002 corrections).
4. Hypercall page fill + intercept VMCALL/VMMCALL exits (extend existing
   handlers at `vmm_machdep.c:4747`/`:4307`); implement HVCALL-post-message /
   signal-event minimal set; TLB flush hypercalls mapped to EPT invalidation.
5. SynIC + synthetic timers (needs Phase 0 LAPIC) — biggest single win for
   Windows interrupt/timer load.
6. eVMCS: explicitly out of scope.

**Milestone M1**: Linux guest runs with hv_stable clocksource and hv drivers;
Windows reports "Hyper-V - Microsoft Corporation" in msinfo32 and uses
enlightened timers/EOI.

### Phase 2 — Windows 11 requirements

1. [ ] **vTPM 2.0**: ACPI `_HID MSFT0101` + TIS MMIO @0xFED40000 + TPM2
   table (corrected PLAN-005); provide the command set required by Windows,
   isolate the software-TPM implementation from the VM process, and persist
   private TPM state per VM through the privileged parent or a dedicated
   subprocess.
2. [ ] **Secure Boot**: build an OVMF variant with Secure Boot and
   authenticated-variable support; provision Microsoft-compatible keys in a
   reproducible template; preserve each VM's key databases in its existing
   `efivars` store; and validate signed boot, db/dbx updates and recovery from
   malformed variable state.  Do not require host firmware Secure Boot or a
   host TPM—the guest-facing implementations are virtual.
3. **Complete:** MSI-X support in `virtio.c` + per-device vectors (corrected
   PLAN-003 §3.2), including MSI fallback and guest boot validation.
4. Config validation warnings (PLAN-007 §7.2), vm.conf(5) docs, qcow2 conversion
   guidance instead of vhdx support.
5. Optional here: minimal xHCI with tablet device replacing/augmenting i8042.

**Milestone M2**: Windows 11 installs and passes hardware checks.

### Phase 3 — Performance & operability (PLAN-006, trimmed)

pv-EOI via SynIC, reference-TSC page tuning, VP runtime accounting, balloon
device (requires new kernel ioctl), stop-copy checkpoint/migration before any
live migration.

### Phase 4 — Nice-to-haves

IDE/AHCI emulation, USB passthrough, VirGL 3D, SRAT/SLIT/HEST, guest detection
(DHCP-fingerprint based only), Q35 platform profile, live migration.

## Testing strategy (details: PLAN-008 as corrected)

- Kernel-side: extend `regress/sys/arch/amd64/vmm*/` for LAPIC timer/IPI, Hyper-V
  CPUID/MSR via /dev/vmm directly — no guest needed.
- Pure-component unit tests: fw_cfg dir contents, SMBIOS checksums, NVRAM store
  CRUD, PCR extend math, DSDT/AML golden diffs.
- Boot tests: SMP Linux guest as the automated canary; Windows ISOs manual,
  licensed-media-gated, skipped by default.

## Alternative ordering: "improve vmm/vmd generally" (Windows as horizon)

The phase order above optimizes for reaching Windows fastest. If the goal is
restated as *general hypervisor improvement with Windows as an eventual
horizon*, most critical-path items turn out to be guest-agnostic and reorder
as follows:

### Universal track (benefits every guest)

1. **LAPIC completion** — timer/LVT/ICR(INIT-SIPI)/EOI. SMP guests of any OS
   are degraded without it. Unchanged from Phase 0; still first.
2. **MSI-X in virtio** — moves up from Phase 2: cleaner interrupts for
   Linux/OpenBSD guests immediately.
3. **ACPI quality** (real DSDT, correct MADT overrides, consistent FADT) —
   OpenBSD/Linux guests parse our tables as strictly as Windows; same bugs,
   same fixes, higher payoff than Windows-only framing suggests.
4. **fw_cfg expansion + SMBIOS generation** — consumed by SeaBIOS, OVMF and
   guest kernels alike.
5. **pvclock/KVM-leaf completion** — direct timekeeping win for OpenBSD/Linux
   guests; infrastructure shared with the Hyper-V reference-TSC work later.
6. **Display (virtio-gpu) + i8042 input** — any graphical guest, not just
   Windows Setup.
7. **Balloon device, checkpoint/migration, EPT/MMIO robustness** — pure
   platform features.

### Firmware track (mid priority, cross-architecture)

UEFI is *not* a Windows-only item: this tree carries `arm64_vm.c` and
`riscv64_vm.c`, and **arm64 guests have no BIOS option at all — UEFI is the
only firmware path**. The OVMF groundwork (flash mapping in EPT, fw_cfg file
plumbing, NVRAM region design) transfers directly to EDK2-on-arm64. So:

8. OVMF-as-ROM for x86 (without NVRAM persistence / Secure Boot, which stay
   deferred) — enables GPT/modern-loader x86 guests and validates the
   machinery for #9.
9. EDK2 firmware bring-up for arm64 guests — unlocks a whole architecture.

### Guest-enlightenment track (last)

10. Hyper-V TLFS (Windows), TPM 2.0 + Secure Boot/NVRAM (Win11 checks),
    IDE/AHCI and vhdx (niche compatibility).

Under this ordering the Windows-specific surface shrinks to roughly steps 10
and the eVMCS-free subset of PLAN-002, while ~80% of the effort lands as
general platform improvement. The milestones become: M0 = SMP Linux + OpenBSD
guests rock-solid with graphics/input; M1 = UEFI boot working on x86 and
started on arm64; M2 = Windows installer runs unenlightened; M3 =
enlightened Windows.

## Working notes (LAPIC/SMP track, 2026-08)

1. **Current admission boundary**: the former `vcp_ncpus != 1` rejection is
   removed, and vmd pairs multi-vCPU creation with an AP `WAIT_SIPI` state so
   only the BSP executes the firmware reset vector.  Counts are selected with
   `vm.conf`'s `cpus` option or `vmctl start -p` and validated from 1 through 64.
2. **Kernel-side SMP work required (vmm(4))**: userspace SMP support alone is not
   sufficient. Known kernel concerns to audit:
   - vcpu ID assumptions: code paths may implicitly assume one vcpu per VM
     (e.g. `VMM_IOC_RUN` dispatch, interrupt/INTR-flag plumbing via
     `vrp_irqready`, TLB/shootdown handling keyed on vm not vcpu).
   - The earlier big-lock concern was incorrect for this tree: `vmmioctl()`
     calls `KERNEL_UNLOCK()` before dispatching `VMM_IOC_RUN` and reacquires it
     only on return.  Each vCPU has its own `vc_lock`; concurrent execution still
     needs measurement and race auditing, but no global run serialization is
     currently known.
   - Per-vcpu state in `struct vm_info`/`struct vcpu` (IRq injection flags,
     halt/pause state) must become strictly per-vcpu; check `vcpu_intr()`,
     `vcpu_halt()`, msr bitmap sharing, and EPT invalidation scope (single-EPT
     per VM means IPI-based shootdowns must target all vCPUs).
   - The first AMD SMP hatch exposed VMCB reset state that UP-only testing did
     not exercise: stale V_IRQ/vector-zero state could survive while the VINTR
     intercept was reset.  SVM reset now clears virtual-interrupt state and
     dirties all VMCB groups.  A 2-vCPU OpenBSD guest subsequently booted.
3. **IOAPIC sufficiency assessment** (`i82093aa.c`): redirection indexing,
     edge detection, remote IRR and EOI re-evaluation have been repaired and
     validated with OpenBSD, Linux and FreeBSD.  Physical, flat-logical and
     cluster-logical destinations are resolved through the LAPIC model;
     lowest-priority delivery selects by PPR class and rotates equal ties.
     Remaining work:
   - NMI, ExtINT, SMI and Init IOAPIC-redirection delivery modes are rejected
     rather than misdelivered as fixed vectors.  This does not include the
     implemented 8259 PIC-through-LINT0 ExtINT compatibility path.
   - EOI broadcast suppression is not implemented.

## Risk register

| Risk | Mitigation |
|---|---|
| LAPIC/IPI rework destabilizes existing guests | OpenBSD, Linux and FreeBSD SMP boots pass; counts remain per-VM opt-in pending lifecycle and long-stress validation |
| OVMF build/toolchain on OpenBSD painful | Ship prebuilt ovmf.fd initially (Option B in PLAN-001); integrate build later |
| Windows strictness on ACPI/SMBIOS bugs | Validate offline against golden dumps; use Linux guest acpidump once |
| Scope creep (TPM crypto, USB, IDE) | Strict phase gating; M0 needs none of them beyond viogpu+i8042 |
| Kernel/userspace split churn on CPUID | Decide delegation design before writing Hyper-V leaf code |
