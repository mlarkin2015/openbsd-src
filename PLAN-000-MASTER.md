# PLAN-000-MASTER: Bringing vmm/vmd to Windows Guest Support

**Status**: implementation in progress. **Date**: 2026-08-31.
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
BSP correctly.
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
load with eight; four-vCPU OpenBSD and Linux smoke tests also pass.  Repeated
reset, pause/reboot and Intel VMX validation remain.

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
| SMP lifecycle/architecture coverage incomplete | OpenBSD 2/4/8, Linux 4 and FreeBSD 2/8 boot; repeated reset/pause/reboot and Intel VMX still need validation |
| No display device (VGA/virtio-gpu) | Windows Setup is graphical — cannot install blind |
| No input (i8042 PS/2, USB tablet) | Cannot interact with Setup |
| No IDE/AHCI storage (i82093aa is an IOAPIC, not IDE) | Storage = virtio only, needs driver ISO during setup |
| fw_cfg missing OVMF entries (`etc/acpi/*`, SMBIOS) | OVMF boots degraded or not at all |
| No SMBIOS generation | Windows licensing/hardware identity unhappy |
| Minimal DSDT, optional, no _PRT/_OSC/HPET | Device discovery/power management failures |
| No UEFI firmware/NVRAM support | Win11 hard-requires UEFI |
| No Hyper-V TLFS interface | Windows runs unenlightened (or refuses some features) |
| No TPM 2.0 | Win11 installer check fails |

## Design decisions to make first

1. **CPUID policy location**: Hyper-V signatures require per-VM CPUID policy.
   Either bake into kernel `vmm(4)` per-guest state, or add delegation
   (`VMM_IOC_SETCPUID`-style, like KVM_SET_CPUID2) so policy lives in vmd.
   Recommend the ioctl route: keeps policy in userspace, one kernel change.
2. **Platform profile**: stay all-PIIX4 (matches existing pci.c bridge) vs move to
   Q35/ICH9 for PCIe/MCFG/TPM coherence. Recommend: PIIX4 profile for the first
   Windows milestone; Q35 later as a separate coherent change.
3. **Storage strategy for installation**: virtio-only + driver ISO (recommended)
   vs implementing IDE/AHCI from scratch (months of work).
4. Secure Boot: defer (Win11 installs with SB off). TPM: implement after core
   platform works; it gates Win11 install checks only.

## Phases

### Phase 0 — Make the platform bootable for a graphical OS (critical path)

Everything else depends on these. Order within the phase:

1. **LAPIC/SMP completion** (kernel, `vmm.c`/`vmm_machdep.c` + userspace
   `vm.c`/`i82489dx.c`): multi-vCPU admission, AP wait-for-SIPI, physical ICR
   fixed/INIT/SIPI delivery, logical destinations, IOAPIC lowest-priority
   arbitration and CPUID/APICBASE topology are implemented.  OpenBSD guests
   boot with 2, 4 and 8 vCPUs, Linux with 4, and FreeBSD with 2 and 8; the
   eight-vCPU FreeBSD test sustained guest-to-host network load.  Continue
   with repeated INIT-SIPI, longer stress and pause/reboot behavior.
2. **OVMF as ROM** (userspace): load `ovmf.fd` through the existing bios path;
   map flash read-only in EPT; add NVRAM varstore pflash region (below firmware
   flash) with EPT write-trap persistence to `/var/vm/<vm>/nvram`;
   extend fw_cfg with `etc/acpi/tables`, `etc/acpi/rsdp`,
   `etc/smbios/smbios-{tables,entry-point-64}`; add `firmware "ovmf"` vm.conf
   option (`parse.y`, `config.c`, `vmd.h`).
3. **SMBIOS generation** (userspace, new `smbios.c`): types 0,1,2,3,4(per-vCPU),
   16,17,19,20,32 exposed via fw_cfg.
4. **Display**: virtio-gpu 2D scanout (`viogpu.c`), host-side framebuffer surface,
   EFI GOP under OVMF so firmware console + Windows basic display driver work.
5. **Input**: i8042 PS/2 keyboard+mouse (ports 0x60/0x64, IRQ 1/12) first;
   xHCI tablet later if needed.
6. **ACPI foundation** (PLAN-004 subset): real DSDT authored in ASL (PCI0 with
   _PRT/_CRS/_OSC, RTC, PIT, PS2K/PS2M, power button, _S5 sleep), interrupt
   source overrides in MADT, consistent FADT PM1 blocks with actual SLP_TYP/SLP_EN
   shutdown emulation.

**Milestone M0**: Windows 10 x64 installer reaches the graphical partitioning
screen (virtio storage driver loaded from ISO), keyboard/mouse work, VM can be
powered off cleanly from Setup.

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

1. TPM 2.0: ACPI `_HID MSFT0101` + TIS MMIO @0xFED40000 + TPM2 table
   (corrected PLAN-005); minimal command subset using libc SHA/libcrypto;
   state persisted by parent process.
2. **Complete:** MSI-X support in `virtio.c` + per-device vectors (corrected
   PLAN-003 §3.2), including MSI fallback and guest boot validation.
3. Config validation warnings (PLAN-007 §7.2), vm.conf(5) docs, qcow2 conversion
   guidance instead of vhdx support.
4. Optional here: minimal xHCI with tablet device replacing/augmenting i8042.

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
