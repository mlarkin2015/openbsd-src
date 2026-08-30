# LAPIC/IOAPIC test notes (2026-08-21)

## State

Local got commits on top of upstream 1420f74a (branch vmm-ng-new, NOT pushed):

- ff5492d9 — plan files (PLAN-000-MASTER + PLAN-001..008 with review findings)
- 90aab729 — per-vCPU LAPIC rewrite: timer CCR from host monotonic time,
  LVTs (timer/LINT0/LINT1/error/PCU), ESR/PPR/APRI/LDR/DFR, TPR-gated ack,
  highest-vector-first arbitration, debug-level logging; vCPU ID threaded
  through MMIO dispatch (mmio.h signature change); x86_vm.c now includes
  in-tree vmmvar.h (installed header is older, lacks vei_addr_size/
  vei_segment).
- aef7a8d7 — LAPIC timer expiry delivery: polling thread ("lapictmr",
  200us) in vm process sets IRR on expiry (periodic reload supported) and
  wakes halted vCPUs so timer interrupts inject while guest sleeps.
  IOAPIC fixes: EOI scans correct redtbl indices (pin*2, was pin/pin+1),
  consistent I82093AA_REDLO_* macros, RIRR cleared only for level entries;
  edge mode delivers only on rising edge; ack respects TPR class.

Build: `cd usr.sbin/vmd && make obj && make` -> obj/vmd. Clean, no new
warnings (LOWMEM_KB redefinition warning is pre-existing).

Kernel side untouched - no kernel rebuild needed for these changes.

## Follow-up validation and repairs (2026-08-27)

The original firmware-boot test did not prove that the APIC implementation
was active.  OpenBSD's packaged vmm SeaBIOS configuration disables both ACPI
and the fw_cfg romfile loader, so that firmware does not consume the generated
ACPI tables.  vmd now also publishes the RSDP with the SeaBIOS table-loader
protocol for firmware builds which enable that support.

Enabling the loader alone is not sufficient with unmodified SeaBIOS 1.16.3.
Its `qemu_cfg_init()` is gated by `runningOnQEMU()`, and its QEMU detection
rejects vmd's OpenBSD host bridge at PCI 00:00.0.  The reproducible port
overlay in `usr.sbin/vmd/seabios/` therefore makes two firmware changes:

- enable `CONFIG_FW_ROMFILE_LOAD` while leaving SeaBIOS's own ACPI generator
  disabled; vmd remains the sole owner of the guest ACPI table set; and
- recognize the OpenBSD vmm PCHB so SeaBIOS initializes vmd's QEMU-compatible
  fw_cfg interface.

The overlay builds through `/usr/ports/sysutils/firmware/vmm`; see its README.
The tested image was 266240 bytes with SHA256
`eccfc43801fef232ccd9c96ee25a8c8fe5f854f51a5840b29ad2baed80ecaefe`.
After both guest tests passed, the packaged image was preserved as
`/etc/firmware/vmm-bios.stock-1.16.3p1` and the tested image was installed as
`/etc/firmware/vmm-bios`.

A direct-kernel boot (`vmctl start -b /tmp/vmm-ng4-bsd -c
openbsd-amd64-test`) exercises the BDA RSDP path and now reports:

```
acpi0: tables DSDT FACP APIC
acpimadt0 at acpi0 addr 0xfee00000: PC-AT compat
ioapic0 at mainbus0: apid 2 pa 0xfec00000, version 11, 24 pins
cpu0: apic clock running at 100MHz
```

The same guest mounted its virtio root disk, completed rc, started its
daemons, and reached the login prompt.  Follow-up repairs made during that
test are:

- `0461f95` publishes the generated ACPI tables through the SeaBIOS loader.
- `7a28201` repairs IOAPIC register selection, redirection-table masks,
  logical line state, remote IRR, and EOI delivery.
- `abb61ff` decodes base-less SIB disp32 MMIO operands and fixes 32-bit MOV
  zero extension, with a regression using the guest's LAPIC EOI bytes.
- `55da1d2` returns the virtio device IRQ in synchronous ISR replies; without
  it an IRQ6 acknowledgement deasserted IRQ0 and caused an interrupt storm.
- `c40808e` corrects the LAPIC's 100 MHz timer timebase, keeps CCR reads from
  consuming expiry, adds synchronized ISR/IRR/TMR state and PPR arbitration,
  and wakes running as well as halted vCPUs.
- `a5357a0` lowers the UART interrupt line after the guest acknowledges it,
  allowing edge-triggered IRQ4 to retrigger and the serial console to drain.
- `1daeca6` disables unconditional MMIO decoder tracing in normal builds.

The firmware-path test with the custom SeaBIOS image now produces the same
APIC lines, enumerates the virtio devices as `apic 2 int 3/5/6/7`, completes
rc, and reaches the login prompt.  A final ordinary `vmctl start -c` with no
`-b` override passed after installation as the default firmware.  This proves
the fw_cfg RSDP handoff rather than relying on the direct-kernel BDA path.

## Linux firmware-boot validation (Alpine 3.23)

Alpine Linux 3.23 with kernel 6.18.33 booted from its virtio disk using the
same custom SeaBIOS image and reached the login prompt.  Positive evidence:

```
ACPI: RSDP 0x00000000000F1CF0 000024 (v02 VMD   )
ACPI: APIC 0x000000000009F000 000040 (v01 VMD ...)
IOAPIC[0]: apic_id 2, version 17, address 0xfec00000, GSI 0-23
APIC: Switch to symmetric I/O mode setup
..TIMER: vector=0x30 apic1=0 pin1=0 apic2=-1 pin2=-1
ACPI: Using IOAPIC for interrupt routing
```

The guest mounted its virtio root filesystem, obtained a DHCP lease over the
virtio network, started sshd, and reached the serial login prompt.

The initial audit established that vmd had no guest MSI/MSI-X implementation:
virtio exposed selector registers but returned `VIRTIO_MSI_NO_VECTOR`, and all
device interrupts used legacy INTx.  The implementation and validation which
supersede that result are recorded below.

## REP INS/OUTS audit

Commit `8f6006ac` ("vmm-ng: integrate dv@'s ins/outs diff") added string-I/O
support before the LAPIC/IOAPIC work.  Both VMX and SVM report the string and
REP attributes, data/address sizes, port, and segment to vmd.  vmd walks guest
memory through RSI/RDI, invokes the normal port handler once per element, and
returns updated RCX/RSI/RDI state.  The normal amd64 `rep outsb` serial-console
path is therefore present, and the FreeBSD test reached the kernel panic with
serial output intact.

The following correctness work is deferred until after the current ACPI/APIC
bring-up unless a guest demonstrates that it is on the critical path:

- fix VMX's 64-bit address-size decode (currently reported as 48 bits);
- preserve RCX and honor DF for non-REP string I/O;
- apply CX/ECX/RCX count width and SI/ESI/RSI wrapping according to the
  instruction's address size;
- repair protected-mode `read_vmem()` segment setup and paging semantics; and
- add an end-to-end REP OUTS regression (the existing vmm regression only
  checks decoding of one non-REP INS instruction).

Commit `d31f7a9` added the DSDT source as `usr.sbin/vmd/vmm-dsdt.asl` and
fixed the firmware description.  Follow-up validation added the root bridge
resource windows and stopped advertising unimplemented PM timer/secondary PM
blocks.  The final tables now:

- set FADT revision 6.5 (the later MSI work clears `FADT_NO_MSI`);
- clear the unused GPE0 address and length;
- remove the duplicate `\_SI` object and the unusable LNKA-D link devices;
- map PCI slots 1-10 INTA directly to vmd's fixed GSIs
  3,5,6,7,9,10,11,12,14,15; and
- describe the PCI bus, I/O window 0x1000-0xffff, and MMIO window
  0xf0000000-0xffbfffff in `_CRS`.

The DSDT compiles with ACPICA `iasl -we` with no errors, warnings, or remarks,
and a disassemble/recompile round trip is byte-identical.  The final Alpine
boot has no duplicate-object, link `_CRS`, GSI-derivation, PCI BAR-window, GPE,
or PM-timer errors.  It reports `PCI: Using ACPI for IRQ routing`, then boots
virtio-blk and virtio-net through legacy INTx and reaches login.  OpenBSD also
accepts the root bridge resources, enumerates all four virtio devices as
`apic 2 int 3/5/6/7`, completes rc, and reaches login.

The later LINT0 and PIT pulse repairs removed Linux's repeated
`No irq handler for 0.52` (vector 0x34) report while retaining serial input.
PM1A event/control register emulation also removed the RTC fixed-event error.
No active PM1 event source drives SCI yet.

FreeBSD's subsequent XSDT failure came from placing the table at 0x9e000,
inside SeaBIOS's POST-time scratch/stack region.  Commit `e6ee385` moves the
XSDT, MADT, FADT, and DSDT down to 0x90000-0x93000 while leaving the RSDP at
0x9d000.  FreeBSD then accepted the XSDT and reached CPU and device attach.

The remaining FreeBSD 15.1 bring-up exposed three independent compatibility
problems, all included in commit `71e5221`:

- FreeBSD used opcode 0x23 (`AND r/m -> reg`) while probing MMIO.  The MMIO
  emulator now implements 16/32/64-bit AND, including architectural result
  width and status flags.
- The address decoder ignored REX.B/REX.X while selecting ModR/M and SIB
  registers.  In the captured failure, `41 c7 06 01 00 00 00` should have
  written the IOAPIC selector through `%r14`, but was decoded through `%rsi`
  and produced a bogus non-MMIO GPA.  Exact-byte regressions now cover that
  instruction and a REX.B/REX.X SIB operand.
- FreeBSD 15 enumerates the legacy UART from ACPI.  Adding a `PNP0501` COM1
  object at I/O 0x3f8, IRQ 4 allowed `uart0` to attach and let the console
  switch from early polled output to the tty driver.  With
  `console=comconsole`, FreeBSD reached and operated the installer over the
  serial console.  This also exercises the previously implemented REP OUTS
  and IOAPIC IRQ4/UART-deassert paths after the tty handoff.

FreeBSD also probes virtio block identifiers.  vmd now implements
`VIRTIO_BLK_T_GET_ID` with stable per-slot IDs and reports the correct number
of device-written bytes in the used ring.  A separate apparent qcow2 capacity
failure was an installation mismatch, not an image-format bug: the old
`/usr/sbin/vmctl` encoded QCOW2 as disk type 2, while the newer vmd interpreted
2 as RAW and therefore exposed the qcow2 file's 256 KiB physical length.  vmd
and vmctl must be built and installed together; the matching vmctl encodes
QCOW2 as type 3 and vmd reads the image's 50 GiB virtual size.

## PCI MSI and MSI-X validation

vmd now provides generic PCI MSI and MSI-X support and uses it for modern
virtio RNG, network, block, and SCSI devices:

- one 64-bit, single-message MSI capability per device;
- an MSI-X capability with one configuration vector plus one vector per
  virtqueue;
- an MMIO MSI-X table and PBA, including per-vector and function masking,
  pending-bit recording, and delivery when a vector is unmasked;
- virtio configuration/queue selector storage across the device subprocess
  boundary; and
- MSI-X, then MSI, then legacy INTx interrupt selection.  MSI/MSI-X messages
  bypass the PIC and IOAPIC and enter the destination LAPIC as edge-triggered
  vectors.

The FADT no longer advertises `FADT_NO_MSI`.  The message decoder currently
implements the conventional xAPIC physical-destination format at
0xfee00000, with fixed delivery.  This is sufficient for vmd's current
64-vCPU maximum because its LAPIC IDs fit in the xAPIC destination field.
x2APIC, interrupt remapping, logical destinations, and lowest-priority
delivery remain future work rather than prerequisites for ordinary MSI.

An OpenBSD firmware boot enumerated all three boot devices on MSI-X and
reached login:

```
virtio0: msix per-VQ
virtio1: msix per-VQ
virtio2: msix per-VQ
```

The virtio block root filesystem and virtio network were both active.  Alpine
Linux 3.23 (kernel 6.18.33) also advertised OS MSI support through ACPI,
assigned the MSI-X MMIO BARs, mounted its virtio root disk, obtained DHCP over
virtio-net, and reached login without the former `MSI disabled` report.

To test the fallback independently, MSI-X capability publication was
temporarily suppressed in an otherwise identical build.  OpenBSD reported
`msi` for virtio RNG, network, and block, then mounted root, configured the
network, and reached login.  The suppression was removed and the full MSI-X
build restored afterward.

## Test plan: uniprocessor OpenBSD or Linux guest, SeaBIOS

Watch for:

1. Boot progress (passed with an OpenBSD direct-kernel boot):
   - OpenBSD: should pass clock calibration / "cpu0: TSC frequency" without
     hanging.
   - Linux: LAPIC timer calibration should complete
     ("tsc: Refined TSC clocksource calibration").
2. Clock accuracy: the LAPIC now advances at 100 MHz before applying DCR;
   the OpenBSD guest calibrated it at 100 MHz.  Longer wall-clock drift tests
   are still useful.
3. Level IRQs under load: basic virtio-blk boot I/O passed.  Virtio-blk/net
   are level-triggered via IOAPIC.
   The RIRR fix should prevent wedging; if virtio stalls under load,
   suspect i82093aa_evaluate_pin/i82093aa_eoi interaction first.
4. Logging: interrupt delivery is now log_debug; run vmd verbose to see
   traces.

## Initial SMP implementation (2-vCPU OpenBSD boot validated)

The first SMP bring-up milestone is implemented, host-build/regression tested,
and exercised through a complete boot by a 2-vCPU OpenBSD guest:

- `vmm(4)` now admits 1-64 vCPUs and clears stale exit/injection state when a
  stopped vCPU is reset.
- vmd resets only vCPU 0 to the firmware entry point.  AP threads are created
  but remain parked in `WAIT_SIPI`; INIT resets the target LAPIC and vCPU, and
  SIPI starts it at `CS.base = vector << 12`, `RIP = 0`.
- xAPIC ICR physical destinations and self/all destination shorthands deliver
  fixed, INIT and STARTUP IPIs.  Logical destinations and other delivery modes
  remain unsupported and are ignored rather than misrouted.
- CPUID leaf 1 reports the configured logical-processor count and APIC ID,
  leaf 0x0b describes one thread per core in one package, and MSR_APICBASE sets
  the BSP bit only on vCPU 0.
- `vm.conf` accepts `cpus count`; `vmctl start -p count` provides the equivalent
  command-line control.  Both validate the 1-64 range, while VM instances
  inherit their parent's count unless explicitly permitted to override it.
- The new `regress/usr.sbin/vmd/lapic` test covers ICR readback, physical and
  shorthand targets, fixed/INIT/SIPI dispatch, INIT deassert, LAPIC reset and
  rejection of logical-destination IPIs.  Config regressions cover valid and
  excessive vCPU counts.  GENERIC.MP, vmd and vmctl all build successfully.

The first 2-vCPU OpenBSD field test attached both `cpu0` and `cpu1`, proving
that INIT-SIPI reached and ran the AP trampoline, but then entered a tight IPI
wait loop.  The compiler emits the LAPIC ICR delivery-status test as group-3
opcode `f7 /0` (`testl $0x1000, ...`), which the MMIO decoder rejected.  vmd
now decodes and emulates the 16/32/64-bit `TEST r/m, immediate` form, including
logical-instruction flags and 64-bit immediate sign extension without operand
writeback.  An exact RIP-relative LAPIC ICR regression passes.

The next run completed autoconfiguration but the AP took a divide-fault trap at
`cpu_hatch` immediately after `sti`.  This exposed stale AMD SVM virtual-
interrupt-window state across vCPU reset: `vcpu_reset_regs_svm()` reset the
VINTR intercept but could leave the dummy `V_IRQ` vector zero armed.  The reset
path now clears V_TPR, V_IRQ, virtual-interrupt metadata/shadow and EVENTINJ,
then marks every rewritten VMCB group dirty.  The vmd run loop also refuses to
truncate the `0xffff` no-vector result from a raced interrupt acknowledge into
vector `0xff`; a real PIC vector zero remains faithfully delivered and logged.

With both fixes installed, an OpenBSD GENERIC.MP guest attached cpu0 and cpu1,
mounted root and completed boot with two vCPUs.  A concurrent in-guest kernel
build/stress run was started successfully.

Increasing the guest to four and eight vCPUs then exposed a lost interrupt
wakeup during fsck/mountroot I/O.  One vCPU could snapshot an empty vmd LAPIC
queue just before another vCPU queued an IPI and issued `VMM_IOC_INTR`.  If the
target entered `VMM_IOC_RUN` in between, the stale userspace snapshot
overwrote the kernel assertion; the host IPI could also miss because the
target had not yet published its current physical CPU.  This stranded guest
TLB shootdowns and left several vCPU threads spinning.

`vmm(4)` now records asynchronous assertions in an atomic per-vCPU latch.
VMX and SVM merge that latch with vmd's level snapshot at run entry and also
consult assertions arriving after the merge.  Deassertion is reconciled at
the next run entry, so its only possible stale effect is one extra harmless
interrupt-window exit.  After installing the new host kernel, four- and
eight-vCPU OpenBSD guests passed the former fsck/mountroot failure point; the
eight-vCPU guest remained running under load.

## SMP network scaling instrumentation (2026-08-28)

`vmctl log stats` now selects a diagnostic level between `brief` and
`verbose`.  It reports five-second deltas without enabling the existing
per-event debug trace:

- per-vCPU returns to userspace, split into I/O, MMIO, interrupt-window, HLT
  and other exits, plus interrupt injection and `VMM_IOC_INTR` activity;
- xAPIC MMIO reads/writes, EOIs, ICR writes and fixed-IPI target count, LAPIC
  timer expiry, queued vectors and acknowledged vectors;
- virtio-net queue kicks and interrupts in both the VM and device processes,
  packet/byte batching and packets per interrupt; and
- time waiting for and holding the VM process's global virtio synchronization
  mutex around a modern virtio-net queue notification.

The counters are intended for measurement, not a stable management API.
`vmctl log brief` disables their hot-path collection.  `vmctl log verbose`
retains the former full debug-log behavior.

The test guest's `/bsdm` GENERIC.MP kernel was installed as `/bsd`; normal
firmware boots then reported the requested two, four and eight CPUs.  TCP
throughput was measured with iperf3 between that guest and the host on the
same vport, for 10-12 seconds per direction.  The results are noisy but
reproduce the reported tendency for two vCPUs to be the best point:

| vCPUs | guest -> host, 1 stream | host -> guest, 1 stream | guest -> host, 4 streams | host -> guest, 4 streams |
|------:|-------------------------:|-------------------------:|--------------------------:|--------------------------:|
| 1 | 0.72 Gbit/s | 1.32 Gbit/s | 1.12 Gbit/s | 1.05 Gbit/s |
| 2 | 0.82 Gbit/s | 1.71 Gbit/s | 1.62 Gbit/s | 2.27 Gbit/s |
| 4 | 0.80 Gbit/s | 1.45 Gbit/s | 1.06 Gbit/s | 2.18 Gbit/s |
| 8 | 0.58 Gbit/s | 1.43 Gbit/s | 1.21 Gbit/s | 2.17 Gbit/s |

An eight-vCPU, four-stream guest-to-host control run produced 1.21 Gbit/s
with statistics enabled and 1.20 Gbit/s with them disabled, so the diagnostic
collection is not the source of the scaling loss.

The virtio mutex is also not the observed bottleneck.  Its average wait was
generally 59-103 ns under load and did not increase with vCPU count; the
average serialized queue service was about 3-5 us.  In contrast, the xAPIC
work grows very large during parallel TCP load.  Representative five-second
samples recorded approximately 56,000 ICR writes with two vCPUs, 116,000
with four, and 147,000 with eight.  The eight-vCPU sample also contained
495,000 LAPIC MMIO writes--about 99,000 userland-assisted LAPIC writes per
second.  Idle LAPIC timer traffic scales linearly as well: approximately 500,
1,000 and 2,000 timer interrupts per five seconds at two, four and eight
vCPUs respectively.

This supports the IPI-overhead hypothesis, especially for multiple flows,
and identifies the userspace xAPIC path as a more important target than the
virtio queue mutex.  The guest currently reports `vio0: 1 queue`, so adding
vCPUs does not add network queues; it instead adds scheduler/network-stack
cross-CPU coordination and per-vCPU timer traffic around one device queue.

The likely optimization order is:

1. reduce or eliminate userspace LAPIC EOI/ICR traffic, first by moving the
   common fixed-IPI/EOI fast path into vmm(4), then considering AMD AVIC and
   Intel APICv/virtual-interrupt delivery;
2. add virtio-net multiqueue and MSI-X vector/queue affinity, with interrupt
   batching or moderation, so extra guest CPUs can perform useful network
   work; and
3. evaluate posted interrupts as part of the hardware LAPIC work.  They do
   not by themselves remove the userspace virtio device transition.

x2APIC would simplify APIC register decoding, but an intercepted x2APIC MSR
still exits unless it is handled in the kernel or accelerated by hardware.
It is therefore not expected to remove the dominant overhead on its own.

### Software x2APIC baseline (host runtime validated)

A deliberately unaccelerated x2APIC path is now build- and runtime-tested.
Its purpose is to establish that the guest-visible x2APIC interface works
before enabling AMD x2AVIC:

- CPUID exposes x2APIC when the VM is using the software LAPIC path.  It is
  temporarily suppressed for legacy-AVIC and SEV-ES VMs, whose state-sharing
  paths need separate synchronization;
- every vCPU maintains a guest `MSR_APICBASE` shadow and validates xAPIC,
  x2APIC and disabled-mode transitions.  x2APIC register accesses outside
  enabled x2APIC mode, reserved registers and invalid-width accesses inject
  `#GP`;
- intercepted x2APIC MSRs use a dedicated `/dev/vmm` exit and the existing
  per-vCPU vmd LAPIC model.  The model provides unshifted x2APIC IDs, derived
  cluster-logical IDs, the combined 64-bit ICR, physical and cluster-logical
  destinations, self-IPIs, EOI and the existing timer/LVT registers; and
- `vmctl log stats` reports `x2apic` exits separately.  No VMX APICv or SVM
  AVIC/x2AVIC control is enabled by this path.

A clean `GENERIC.MP` build, staged-header vmd build and LAPIC regression pass.
The regression covers x2APIC IDs, TPR access, physical and logical 64-bit ICR
delivery and self-IPI.  A two-vCPU OpenBSD `GENERIC.MP` guest also boots and
runs with this path.  The expected host `vmm0` line remains `SVM/RVI`, and
verbose stats show nonzero `x2apic` exits with zero `avic` exits.  This is the
software control result needed before mode-gated x2AVIC is activated.

The first runtime test exposed a lost-wakeup race between an IPI and guest
`HLT`: vCPU 0 waited in `pmap_tlb_shootwait` while vCPU 1 remained parked in
`cpu_idle_cycle_hlt`, even though its LAPIC IRR held the IPI.  The interrupt
had signalled the vCPU after its hardware HLT exit but before the vmd thread
recorded the halt, and `vcpu_halt()` then overwrote the runnable state.  It now
rechecks the LAPIC for a pending interrupt while holding the run mutex before
marking the vCPU halted.  Repeated guest kernel relinks no longer hang.

The same testing found that `halt -p` reached ACPI shutdown but did not stop
the VM.  The DSDT now publishes `_S5` with sleep type 5, and PM1A control
writes carrying that type plus `SLP_EN` terminate the VM cleanly.  Repeated
shutdown tests no longer hang, and vmd logs the guest's transition to ACPI S5.

## Known remaining gaps

### AMD AVIC prototype (host runtime validation pending)

A first xAPIC AVIC implementation is now build-tested on the AMD path.  It is
deliberately narrower than x2AVIC or IOMMU guest-mode interrupt remapping:

- vmm(4) detects CPUID `0x8000000a` AVIC support and enables it only when
  every RVI CPU is eligible; Family 17h is conservatively excluded because of
  the lost-IPI AVIC erratum, and encrypted SEV/SEV-ES guests retain the
  software LAPIC path;
- every VM has AVIC logical/physical-ID tables and every vCPU has a hardware
  LAPIC backing page; the physical table's running/host-APIC-ID fields are
  updated atomically around VMRUN;
- vmd injects LAPIC timer, IOAPIC and MSI/MSI-X vectors directly into the
  hardware IRR/TMR backing page.  A running remote vCPU receives an AVIC
  doorbell, while the existing vCPU condition variable wakes a stopped one;
- fixed edge IPIs are handled by AVIC.  Incomplete IPI exits wake stopped
  destinations or send INIT/SIPI and unsupported delivery modes through the
  existing userspace ICR model;
- completed AVIC register-write traps keep vmd's configuration/timer shadow
  synchronized, and level-triggered EOI exits retain IOAPIC remote-IRR
  handling.  Genuine unaccelerated access faults use the existing MMIO
  instruction decoder; and
- AVIC exit counts are included in `vmctl log stats` output.

`GENERIC.MP`, a clean staged-header vmd build, and the LAPIC regression all
pass.  The regression now covers direct AVIC vector injection, stopped-target
wakeup and invalid-type IPI fallback.  The matched host kernel and vmd were
installed for software-x2APIC testing, but this x2AVIC-only host cannot
runtime-test the legacy AVIC path.  Legacy AVIC validation still requires a
host advertising CPUID `0x8000000a` bit 13.

After reboot, an eligible AMD host should identify vmm as `SVM/RVI/AVIC`; a
verbose VM-process log should also say `AMD AVIC enabled`.  The first field
test should boot the known-good OpenBSD guest at 2, 4 and 8 vCPUs, repeat the
kernel-build stress test, and then repeat the existing iperf3 matrix while
collecting `vmctl log stats`.  The expected signal is a sharp reduction in
LAPIC MMIO/ICR exits; device-process transitions remain because this prototype
does not implement IOMMU-posted interrupts.

- Concurrent execution, INIT-SIPI and interrupt delivery through boot are
  validated with OpenBSD guests at two, four and eight vCPUs.  Longer stress,
  repeated AP reset, pause/unpause, guest reboot, and SMP Linux/FreeBSD remain
  to be validated.
- Four/eight-vCPU field validation was on AMD SVM.  The common assertion latch
  and Intel VMX run path are both updated and build-tested, and VMX reset
  reconstructs its interrupt-window control without SVM's dummy vector, but an
  SMP boot on Intel hardware remains to be exercised.
- IOAPIC destination-mode handling still ignores logical mode / lowest-prio
  delivery (dest used directly as vcpu id).
- LAPIC ICR fixed delivery supports physical destinations plus flat/cluster
  logical destinations.  Lowest-priority, NMI, SMI and ExtINT delivery modes
  are not implemented.
- MSI supports one 64-bit message per device.  MSI-X supports at most 16
  vectors per device.  x2APIC/remapped MSI, logical destinations, and
  lowest-priority delivery are not implemented.
- PM1A S5 poweroff is implemented, but no active PM1 event source asserts SCI.
- REP INS/OUTS has the deferred architectural corner cases listed above.
- Virtio-net exposes one queue pair; SMP guests have no receive/transmit
  multiqueue or queue-affinity scaling yet.

## Next steps after testing

1. Continue longer OpenBSD stress at eight vCPUs, then boot SMP Linux and
   FreeBSD guests; confirm every AP attaches and stress timer/IPI/virtio
   interrupt delivery.
2. Exercise pause/unpause and guest reboot with APs both parked and running,
   including repeated INIT-SIPI sequences.
3. Audit device and EPT paths under genuinely concurrent vCPU exits; the
   `/dev/vmm` ioctl path already drops the kernel big lock before VMM_IOC_RUN,
   so no global execution serialization is currently known.
4. Add mode-gated AMD x2AVIC activation (VMCB bits 31+30 only), repeat the
   same guest tests, and compare its exit profile against the software
   baseline.  Never enable the legacy bit-31-only mode on this host.
5. Runtime-test the legacy AMD AVIC prototype on a bit-13-capable host and
   repeat the instrumented network matrix.
6. Add virtio-net multiqueue and MSI-X queue affinity, then repeat the same
   single- and four-stream matrix.
7. Add active ACPI PM1 event sources and SCI delivery as they become needed.
8. Add LAPIC/IOAPIC lowest-priority delivery.
9. Extend MSI routing only when a guest needs logical/x2APIC delivery or
   interrupt remapping.
