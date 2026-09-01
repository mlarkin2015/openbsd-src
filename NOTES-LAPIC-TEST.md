# LAPIC/IOAPIC test notes (updated 2026-09-01)

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
daemons, and reached the login prompt.  A later direct-kernel test used
`vmctl start -i 1 -c -b /bsd -d openbsd-amd64-test.qcow2 -p 2 x` with no
firmware payload.  OpenBSD 8.0 GENERIC.MP found the same ACPI tables, attached
cpu1 through INIT/SIPI, selected the APIC clock, negotiated two virtio-net
queues and MSI-X, mounted root and completed filesystem checks.  This confirms
that the BDA RSDP path supports SMP directly and is not SeaBIOS-dependent.

Follow-up repairs made during the original direct-kernel test are:

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

The FADT no longer advertises `FADT_NO_MSI`.  The message decoder implements
the conventional xAPIC physical- and logical-destination formats at
0xfee00000 with fixed and lowest-priority delivery.  Logical messages use the
LAPIC model's flat/cluster target resolution; lowest-priority delivery selects
one eligible target by PPR and rotates equal-priority ties.  x2APIC/remapped
MSI remains future work rather than a prerequisite for ordinary MSI.

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
- xAPIC ICR physical destinations, flat/cluster logical destinations and
  self/all destination shorthands deliver fixed, INIT and STARTUP IPIs.
  PIC-through-LINT0 ExtINT is implemented separately.  Lowest-priority, NMI,
  SMI and ExtINT ICR delivery modes remain unsupported and are ignored rather
  than misrouted.
- CPUID leaf 1 reports the configured logical-processor count and APIC ID,
  leaf 0x0b describes one thread per core in one package, and MSR_APICBASE sets
  the BSP bit only on vCPU 0.
- `vm.conf` accepts `cpus count`; `vmctl start -p count` provides the equivalent
  command-line control and overrides a configured VM for one boot.  Both
  validate the 1-64 range, while VM instances inherit their parent's count
  unless explicitly permitted to override it.
- The new `regress/usr.sbin/vmd/lapic` test covers ICR readback, physical,
  logical and shorthand targets, fixed/INIT/SIPI dispatch, INIT deassert and
  LAPIC reset.  Config regressions cover valid and excessive vCPU counts.
  GENERIC.MP, vmd and vmctl all build successfully.

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

- CPUID exposes x2APIC for software-LAPIC VMs and for hosts offering x2AVIC.
  It remains suppressed for legacy-AVIC-only and SEV-ES VMs, whose
  state-sharing paths do not support x2APIC;
- every vCPU maintains a guest `MSR_APICBASE` shadow and validates xAPIC,
  x2APIC and disabled-mode transitions.  x2APIC register accesses outside
  enabled x2APIC mode, reserved registers and invalid-width accesses inject
  `#GP`;
- intercepted x2APIC MSRs use a dedicated `/dev/vmm` exit and the existing
  per-vCPU vmd LAPIC model.  The model provides unshifted x2APIC IDs, derived
  cluster-logical IDs, the combined 64-bit ICR, physical and cluster-logical
  destinations, self-IPIs, EOI and the existing timer/LVT registers; and
- `vmctl log stats` reports `x2apic` exits separately.  This exit remains the
  complete fallback on hosts without hardware acceleration and handles the
  configuration/timer registers deliberately left in software by x2AVIC.

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
recorded the halt, and `vcpu_halt()` then overwrote the runnable state.  The
software path rechecks the LAPIC while holding the run mutex; the hardware
path additionally compares a per-vCPU wakeup generation captured immediately
before `VMM_IOC_RUN`, since a hardware IRR is intentionally absent from vmd's
software LAPIC.  Repeated guest kernel relinks no longer hang.

The same testing found that `halt -p` reached ACPI shutdown but did not stop
the VM.  The DSDT now publishes `_S5` with sleep type 5, and PM1A control
writes carrying that type plus `SLP_EN` terminate the VM cleanly.  Repeated
shutdown tests no longer hang, and vmd logs the guest's transition to ACPI S5.

### AMD x2AVIC (two-vCPU host runtime validated)

The mode-gated AMD x2AVIC path is implemented and build-tested.  It treats
CPUID `0x8000000a` bit 18 as an independent x2AVIC capability, so a Zen 4+
mobile CPU which lacks legacy AVIC bit 13 starts with the software xAPIC and
accelerates only after the guest enables x2APIC:

- vmm(4) reports `/x2AVIC` only when every RVI CPU advertises bit 18, is not
  Family 17h, and has a host APIC ID representable by the 12-bit physical
  table field.  A bit-18-only host never enters legacy bit-31-only AVIC mode;
- the guest's valid `MSR_APICBASE` transition enables VMCB AVIC and x2AVIC
  bits 31+30 together.  ID/version, TPR/PPR, ISR/TMR/IRR, EOI, ICR and
  SELF_IPI MSRs then run through x2AVIC, while SVR/LVT/timer accesses remain
  intercepted for the existing vmd timer and device model;
- a `VM_EXIT_X2APIC` mode-transition record transfers the complete LAPIC
  register image between vmd's software model and the hardware backing page.
  The backing page becomes the direct interrupt target before userspace is
  entered, pending IRR/TMR bits are merged on import, and x2APIC MSR bypass is
  enabled only after the transfer completes;
- acceleration is tracked per vCPU rather than by one VM-wide boolean, so
  interrupt, timer and acknowledge paths remain correct while different CPUs
  pass through reset or APIC mode transitions;
- incomplete x2AVIC IPI exits preserve the full 32-bit physical or
  cluster-logical destination.  INIT/SIPI and unsupported types fall back to
  the software 64-bit ICR path, while stopped destinations are woken without
  re-queuing a vector already placed in hardware; and
- x2APIC disable exports ISR/IRR/TMR and configuration state back to vmd.
  Direct-vector failures racing teardown fall back to the software IRR, and
  the wake-generation check closes the corresponding hardware-HLT race.

A clean staged-header vmd build, the LAPIC/x2AVIC regression, the x86 MMIO
regression and a full `GENERIC.MP` build pass.  The bit-18-only Zen 4 mobile
test host identifies vmm as `SVM/RVI/x2AVIC`.  A two-vCPU OpenBSD
`GENERIC.MP` guest boots, completes relinking, permits login and shuts down
cleanly with `halt -p`.  Repeated boots and shutdowns also completed without
the former lost-wakeup or ACPI S5 hangs.

`vmctl log stats` confirms that the guest enters the accelerated path:

- x2AVIC exits are nonzero while software LAPIC ICR, fixed-IPI target and EOI
  counts remain zero, including under TCP load;
- intercepted x2APIC traffic at idle is approximately 500 exits per five
  seconds across two vCPUs, accompanied by approximately 500 timer expiries.
  This is the deliberately unaccelerated timer/configuration plane, not IPI
  delivery; and
- during the measured TCP runs aggregate x2AVIC exits varied from roughly
  10,000 to 30,000 per five seconds.  Thus hardware removes the dominant
  userspace ICR path, but incomplete-IPI/EOI handling and virtio transitions
  can still return to vmd.

Two 12-second iperf3 runs in each two-vCPU test cell gave the following
results.  The old software-LAPIC column is the earlier single run, while the
x2AVIC column shows the new range and mean, so the percentage is indicative
rather than a controlled statistical confidence interval:

| Direction / streams | software LAPIC | x2AVIC range (mean) | mean change |
|---------------------|---------------:|----------------------:|------------:|
| guest -> host, 1 | 0.82 Gbit/s | 1.07-1.20 (1.14) Gbit/s | +39% |
| host -> guest, 1 | 1.71 Gbit/s | 1.44-1.65 (1.55) Gbit/s | -9% |
| guest -> host, 4 | 1.62 Gbit/s | 1.19-1.31 (1.25) Gbit/s | -23% |
| host -> guest, 4 | 2.27 Gbit/s | 1.91-1.99 (1.95) Gbit/s | -14% |

The performance outcome was mixed: removing software ICR traffic substantially
improved the single-stream guest-transmit case, but did not by itself fix SMP
network scaling or improve the parallel-flow cases in this noisy sample.
These initial measurements preceded the x2AVIC HLT fast path and virtio-net
multiqueue work described below; the later tests cover two, four and eight
vCPUs.  x2AVIC still does not eliminate userspace device processing or every
interrupt-related exit.

### x2AVIC HLT fast path (host runtime validated)

The aggregate `avic` statistic was split by hardware exit cause before moving
another part of the LAPIC into the kernel.  In a representative five-second
interval during a two-vCPU, four-stream guest-to-host TCP run, vCPU 0 reported
32,699 AVIC exits and vCPU 1 reported 19,437.  All 52,136 were incomplete IPIs
with `target not running`; EOI, invalid-type and other AVIC causes were zero.
The same interval contained 60,182 HLT exits.  A 12-second diagnostic run
reached 1.47 Gbit/s, but its purpose was to classify exits rather than establish
a new performance baseline.

The resulting fast path is deliberately limited to active x2AVIC vCPUs:

- an interruptible guest HLT sleeps in vmm(4) without returning from
  `VMM_IOC_RUN`;
- a target-not-running incomplete IPI uses the full x2APIC ICR destination to
  wake matching vCPUs after hardware has already queued their IRR bits, then
  resumes the sender without vmd assistance;
- direct LAPIC vectors wake the same kernel wait, while INIT/SIPI, unsupported
  IPI types, LAPIC timer programming and other uncommon exits still use vmd;
- a pre-VMRUN IRR snapshot plus a deliverable-IRR/PPR check covers the
  IsRunning-to-HLT transition without re-queuing hardware vectors; and
- explicit kicks preserve pause, INIT/legacy-interrupt and termination
  behavior.  A paused vCPU retains its architectural HLT state in vmm(4),
  re-enters the kernel wait after unpause and resumes only when a real
  interrupt becomes deliverable.

Non-x2AVIC AMD guests and Intel guests continue to use the existing userspace
HLT condition variable.  A staged-header vmd build, full `GENERIC.MP` build and
LAPIC regression pass.  With the matching kernel and vmd installed, a two-vCPU
OpenBSD guest booted, permitted login and shut down cleanly.

Two guest-to-host, four-stream, 12-second iperf3 runs were repeated at two,
four and eight vCPUs:

| vCPUs | run 1 | run 2 | mean | HLT/AVIC userspace exits | aggregate x2APIC exits/5s |
|------:|------:|------:|-----:|-------------------------:|---------------------------:|
| 2 | 1.20 Gbit/s | 1.21 Gbit/s | 1.21 Gbit/s | 0 | 492-498 |
| 4 | 1.02 Gbit/s | 1.07 Gbit/s | 1.05 Gbit/s | 0 | 982-1,002 |
| 8 | 1.16 Gbit/s | 1.20 Gbit/s | 1.18 Gbit/s | 0 | 1,968-2,008 |

Every five-second sample also reported zero for all AVIC subreasons, including
`ipi-not-running`.  Before the fast path, a representative two-vCPU interval
reported 60,182 HLT exits and 52,136 target-not-running incomplete-IPIs.  The
remaining x2APIC exits are the expected unaccelerated timer/configuration
traffic and scale at approximately 250 exits per vCPU per five seconds.

The fast path therefore eliminates the targeted vmm-to-vmd round trips at all
three tested CPU counts.  Throughput remains within the earlier noisy results,
however, so it does not demonstrate a gain for the current single-queue
virtio-net path.  Under load, vCPU0 continues to handle most device I/O and
interrupt assertions; at eight vCPUs, vCPUs 4 through 7 are often timer-only.
This points at serialized userspace device processing and the single virtio-net
queue rather than residual IPI/HLT exits.  Pause/unpause is intentionally
deferred; forced termination remains an optional lifecycle check.

### Virtual CPU topology for interrupt affinity (1/2/3/4/8-vCPU validated)

Virtual CPUID now reports one package with one single-threaded core per vCPU.
OpenBSD/amd64 currently derives Intel topology from legacy leaves 1 and 4 and
AMD topology from leaves `0x80000008` and `0x8000001e`, rather than relying on
leaf `0x0b`.  All of those views, plus leaf `0x1f`, now agree.  The legacy
Intel view advertises the next-power-of-two APIC-ID capacity so configurations
such as three or six vCPUs still decode as distinct cores in package zero;
ACPI continues to enumerate only the configured processors.

This is a prerequisite for OpenBSD virtio-net multiqueue: `intrmap` excludes
CPUs with a nonzero SMT ID, so the former Intel cache-topology result reduced
an SMP guest to one usable interrupt CPU.  AMD no longer presents every vCPU
as core zero in a separate package.  A full `GENERIC.MP` kernel build passes.
With the kernel installed, OpenBSD guests at one, two, three, four and eight
vCPUs boot, permit login, exchange network traffic and shut down cleanly.
Every CPU reports `smt 0`, a unique core ID equal to its vCPU ID, and
`package 0`.  The three-vCPU run specifically validates the non-power-of-two
APIC-ID-width case.

### Four-pair virtio-net TX path (runtime and performance validated)

Virtio-net now advertises `VIRTIO_NET_F_CTRL_VQ` and `VIRTIO_NET_F_MQ`, up to
four queue pairs, and the standard `max_virtqueue_pairs` device-config field.
The control virtqueue implements `VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET`, defaults
to one active pair after reset, and accepts one through four pairs.  Its index
follows the negotiated layout: queue 2 when MQ was declined and queue 8 when
MQ was accepted.

Each TX queue has an independent worker thread, event loop, notification pipe,
descriptor scratch space, used-ring accounting and MSI-X queue interrupt.
This is intentionally a TX-only scaling experiment: the one tap descriptor is
still read by one RX thread, all host packets are placed on RX queue 0, and
notifications for every other RX queue are left idle.  No tap(4) interface
changes are part of this milestone.  Five-second verbose statistics split
kicks, interrupts, packets and bytes by queue pair and include the control
queue.  Compile-time assertions keep the queue/vector count within the
16-entry MSI-X table and keep the TX-worker interrupt messages synchronized
with the configured pair count.

A fresh vmd build and the complete `regress/usr.sbin/vmd` suite pass.  With
the topology kernel and four-pair vmd installed, OpenBSD guests at one, two,
four and eight vCPUs boot, exchange network traffic and shut down cleanly.
They select one, two, four and four pairs respectively.  A four-pair guest
exposes six handlers: `vio0:0` configuration, `vio0:1` control, and
`vio0:2` through `vio0:5` for pairs 0 through 3.  An eight-vCPU/eight-stream
run incremented every pair handler by 10,961, 3,267, 4,617 and 3,433
interrupts respectively.

The one-vCPU guest reports one queue, passes network traffic and shuts down
cleanly.  This confirms that declining MQ leaves the data queues usable with
the alternate queue-2 control layout.  No control command is expected in that
configuration, so the queue-2 control handler itself was not directly driven.

The initial two-pair implementation produced the following two-vCPU results,
using two 12-second runs per cell:

| Direction / streams | pre-MQ x2AVIC mean | MQ range (mean) | change |
|---------------------|--------------------:|----------------:|-------:|
| guest -> host, 1 | 1.14 Gbit/s | 1.13-1.37 (1.25) Gbit/s | +10% |
| guest -> host, 4 | 1.25 Gbit/s | 2.33-2.34 (2.34) Gbit/s | +87% |
| host -> guest, 1 | 1.55 Gbit/s | 1.66-1.77 (1.72) Gbit/s | +11% |
| host -> guest, 4 | 1.95 Gbit/s | 1.80-2.01 (1.91) Gbit/s | -2% |

The strong gain is confined to parallel guest TX, where the two independent
workers can do useful work.  Host-to-guest traffic remains effectively flat,
as expected from the deliberately single tap reader and q0-only RX path.

The established four-stream guest-to-host scaling test was also repeated:

| vCPUs | single-queue mean | MQ runs | MQ mean | mean change |
|------:|------------------:|--------:|--------:|------------:|
| 2 | 1.21 Gbit/s | 2.34, 2.33 Gbit/s | 2.34 Gbit/s | +93% |
| 4 | 1.05 Gbit/s | 1.09, 2.26, 2.22 Gbit/s | 1.86 Gbit/s | +77% |
| 8 | 1.18 Gbit/s | 2.19, 2.19 Gbit/s | 2.19 Gbit/s | +86% |

The first four-vCPU run was an isolated low outlier; the following two runs
were stable at a 2.24 Gbit/s mean.  It is retained in the table rather than
discarded.  Even including it, all tested SMP sizes improve materially over
the matched single-queue baseline.

Raising the cap from two to four pairs did not produce another comparable
gain.  A four-vCPU/four-stream run reached 2.19 Gbit/s, versus the earlier
stable two-pair mean of 2.24 Gbit/s.  Four-vCPU/eight-stream runs reached 2.31
and 2.34 Gbit/s, while one eight-vCPU/eight-stream run reached 2.22 Gbit/s.
During the eight-stream load, all four TX workers carried comparable packet
counts; one representative five-second interval reported 261,726, 232,419,
245,827 and 238,734 packets.  RX remained entirely on q0.

The four-pair support is functional and removes queue count as the reason for
the observed ceiling, but the current shared tap/write/network path saturates
at roughly 2.2-2.3 Gbit/s.  Do not increase the cap again without evidence
from a different workload or after offload work.

### Virtio-net guest TX TSO (runtime and performance validated)

Virtio-net now negotiates checksum offload and host TCPv4/TCPv6 segmentation
offload.  Each TX worker validates and translates the guest's
`virtio_net_hdr` into tap(4)'s native `tun_hdr`, then prepends that header to
the existing zero-copy payload iovecs.  TCP and UDP partial checksums plus
TCPv4/TCPv6 GSO are representable.  Unsupported ECN/UFO types, invalid bounds
and arbitrary checksum offsets are rejected.  The accepted TX size grows from
the tap MRU to the virtio/tap 64 KiB segmentation limit.

The tap descriptor is placed in header mode with zero receive capabilities.
Consequently, tap writes accept offload metadata, while host packets are still
segmented and checksummed before vmd reads them.  Both copy and zero-copy RX
paths strip the resulting empty tap header and otherwise retain the existing
single-reader, queue-0 behavior.  This milestone does not negotiate guest TSO,
merged RX buffers, the guest-offload control class or RX multiqueue.

The supplied `mq.diff` was treated only as historical multiqueue context: it
contains no virtio TSO/checksum feature negotiation, tap capability ioctl or
offload-header translation.  The implementation instead follows the current
tree's `if_vio(4)` producer and tap(4) consumer ABIs.

A fresh vmd build and the complete `regress/usr.sbin/vmd` suite pass.  With
the new vmd installed, a two-vCPU OpenBSD guest booted with two queues and
reported `CSUM_TCPv4`, `CSUM_UDPv4`, `CSUM_TCPv6`, `CSUM_UDPv6`, `TSOv4` and
`TSOv6` in `vio0 hwfeatures`.  Ping, SSH and clean `halt -p` all pass.

Two four-stream, 12-second guest-to-host iperf3 runs reached 17.8 and 20.4
Gbit/s, for a 19.1 Gbit/s mean.  This is approximately 8.2 times the prior
four-stream two-vCPU mean of 2.34 Gbit/s.  Representative five-second vionet
samples reported 671,943 TSO packets out of 676,496 TX packets and 714,639 out
of 717,263; every TX packet in those samples requested checksum offload.  A
four-stream host-to-guest control reached 2.06 Gbit/s with zero retransmits,
consistent with the earlier 1.91 Gbit/s result and confirming the tap-header
change did not break the unaccelerated RX path.

## LAPIC/SMP closeout fixes (2026-08-31)

Two remaining table-stakes items were completed and tested:

- `vmctl start -p count configured-vm` now applies a one-boot vCPU-count
  override without treating the request as a new diskless VM.  vmd preserves
  the configured count separately and restores it after stop or failed start.
  A configured one-vCPU OpenBSD VM was started with `-p 2`; `vmctl status`
  reported two vCPUs while running and one again after forced stop.
- IOAPIC fixed delivery now resolves xAPIC physical, flat-logical and
  cluster-logical destinations.  Lowest-priority delivery selects the enabled
  target with the lowest PPR class and rotates equal-priority ties.  The LAPIC
  regression uses the real IOAPIC model and covers logical multicast,
  PPR-based selection and round-robin arbitration.

A fresh two-vCPU FreeBSD 15.1-p3 test found one additional compatibility bug
before AP startup.  Both the userspace LAPIC model and the AMD AVIC backing
page advertised bit 31 in the LAPIC version register.  On AMD, that bit means
the extended APIC register space exists; FreeBSD consequently read x2APIC MSR
0x840 (`LAPIC_EXT_FEATURES`), which vmm correctly rejected with #GP because
that register space is not implemented.  The advertised version is now
0x00060010 in both models, and the regression checks it exactly.

That correction allowed AP startup but did not resolve a later ZFS mountroot
hang.  Instrumenting the x2AVIC backing page found vector 48 simultaneously
present in ISR and IRR on CPU 1 with PPR stuck at class 0x30.  The first vtblk
interrupt's AVIC unaccelerated-EOI exit had reached vmd, which cleared the
IOAPIC remote-IRR state, but neither hardware nor vmm had retired the vector
from the LAPIC backing-page ISR.  The second instance could therefore remain
pending indefinitely.  vmm now clears the EOI exit's reported ISR bit,
recomputes PPR from the remaining ISR and TPR state, and leaves a concurrent
same-vector IRR/TMR request intact; vmd continues to complete the IOAPIC half.

The matched kernel completed a FreeBSD 15.1-p3 two-vCPU boot, login and clean
shutdown with x2APIC/x2AVIC enabled.  A software-xAPIC control boot with
`hw.apic.x2apic_mode=0` also passed during diagnosis.  Four-vCPU OpenBSD and
Linux guests booted normally with the final kernel.  FreeBSD subsequently ran
with eight vCPUs under varied guest-to-host iperf3 loads without a fault; the
highest observed result was 27.2 Gbit/s.  This is a field-test peak rather than
a controlled multi-run benchmark mean.

### SMP halt and reboot lifecycle (2026-08-31)

Final lifecycle testing exposed three independent teardown and reset issues:

- vmd did not supply SeaBIOS's `FW_CFG_NB_CPUS` and `FW_CFG_MAX_CPUS` entries.
  A uniprocessor guest kernel in a VM configured with four vCPUs therefore
  left the three firmware APs spinning on SeaBIOS's SMP lock and consumed
  roughly 300% host CPU.  vmd now publishes both CPU counts, and an AP that
  halts with interrupts disabled remains restartable by INIT/SIPI while the
  BSP's terminal HLT behavior is retained on both SVM and VMX.
- a concurrent termination request could be overwritten when `vm_run()`
  changed a vCPU from running to stopped.  The transition now preserves
  `VCPU_STATE_REQTERM`, reports `VM_EXIT_TERMINATED`, and completes forced
  termination without leaving an x2AVIC HLT waiter behind.
- a reset or terminal exit on one vCPU did not stop its siblings.  The first
  terminal vCPU now marks the VM as stopping, kicks or wakes every peer, and
  prevents them from re-entering the guest.  `run_vm()` joins each thread once
  and preserves an `EAGAIN` reset request instead of replacing it with a
  sibling's successful status.

The final build passed a four-vCPU OpenBSD VM running a GENERIC uniprocessor
guest without the firmware AP spin, OpenBSD SMP reboot, and Ubuntu 24 SMP
reboot and halt/poweroff.  The earlier forced-stop test also completed without
requiring `kill -9`.

### i386/i686 paging, LAPIC MMIO and MSI-X (2026-09-01)

The earlier i386 resets were reproducible with one vCPU and were therefore
not an SMP-startup failure.  Three independent 32-bit compatibility problems
were fixed:

- the userspace GVA walker retained stale upper bits after 32-bit PTE reads,
  discarded physical-address bit 31, applied the wrong CR3 masks to non-PAE
  and PAE32 paging, and treated PAE PDPTE permission/A-D fields like ordinary
  entries.  The walker is now isolated in `x86_mmu.c` and has focused non-PAE
  4 KiB/4 MiB and PAE32 high-frame regressions;
- the MMIO opcode tables had 255 entries, making opcode `0xff` an out-of-bounds
  lookup.  The exact OpenBSD/i386 LAPIC sequences now decode and emulate
  `pushl` from MMIO, `popl` from the stack directly to MMIO, `subl` from MMIO
  and `cmpl` against MMIO with the required register, stack and flags
  semantics; and
- Linux/i686 programmed virtio MSI-X messages to xAPIC logical destination
  bit 0 (`fee01004`).  vmd previously discarded every logical MSI.  Fixed MSI
  and MSI-X delivery now resolve physical or flat/cluster logical targets
  through the LAPIC model before asserting the vector.

An OpenBSD 7.9 GENERIC i386 guest now boots through IOAPIC/APIC clock setup,
mounts root, obtains DHCP, passes SSH and gateway ping tests, and powers off
cleanly.  Gentoo/i686 6.12 first booted only with GRUB `pci=nomsi`, which
isolated interrupt delivery from paging and MMIO decoding; after the logical
MSI fix it boots normally with MSI-X, mounts its Btrfs root, obtains DHCP and
reaches login.  The complete `regress/usr.sbin/vmd` suite passes with the new
MMU, MMIO and LAPIC coverage.

OpenBSD 8.0/i386 GENERIC.MP exposed one additional compiler-generated LAPIC
access at mountroot: `8f 05 80 e0 f6 d0` is `popl 0xd0f6e080`, which restores a
saved interrupt priority directly from the stack into the LAPIC TPR mapping.
Opcode `8f /0` now reads `SS:ESP`, performs the MMIO write, and advances ESP
and EIP only after success.  A regression uses the exact failing instruction
and checks its stack read, TPR write and register updates.  The installed vmd
then booted the guest with four vCPUs through filesystem checks and userland to
login, followed by a clean vmmci shutdown.

CentOS 7/i386 with Linux 3.10 provided two additional legacy controls.  Its
older ACPICA disabled the interpreter because the FADT pointed to no FACS;
vmd now creates a revision-1 FACS at 0x94000 and populates both FADT pointers.
The guest then evaluated the existing fixed `PCI0._PRT` routes successfully,
proving that no new link-device or INTx routing model was required.

The guest next stopped after finding the virtio disk.  A `pci=nomsi` boot
immediately found its partitions and reached login, isolating MSI-X.  CentOS
programs xAPIC logical-destination, lowest-priority messages (for example,
address `fee0100c`, data `4141`); vmd previously accepted only fixed delivery
and silently discarded them.  PCI MSI/MSI-X now shares the LAPIC target/PPR
selection logic, selects one eligible target for lowest-priority delivery and
rotates equal-priority ties.  Normal CentOS 7 i386 and amd64 boots both reach
login, and a two-vCPU OpenBSD GENERIC.MP regression still reaches login with
virtio MSI-X active.

CentOS still warns that the FADT has no PM timer.  An experimental 24-bit
3.579545 MHz counter advanced correctly, but CentOS rejected it while its
LAPIC/PIT calibration was already off by roughly 20x.  The timer was therefore
removed instead of advertising a clock the guest would not trust.  ACPI PM
timer implementation and the underlying timer-calibration mismatch remain a
separate platform follow-up.

### NetBSD VirtIO 1.x split queues (2026-09-01)

NetBSD 11/amd64 booted with both one and two vCPUs, but its network transmit
path initially produced invalid or all-zero Ethernet frames.  vmd treated the
three independently addressed areas of a modern split virtqueue as offsets
from one host mapping.  That is valid for the legacy contiguous layout but is
not guaranteed by the VirtIO 1.x PCI transport.  The descriptor, available
and used guest addresses and host mappings are now retained separately by all
modern virtio devices (RNG, network, block and SCSI).

A second failure appeared when NetBSD brought vioif down and back up.  Device
reset clears the negotiated driver-feature bitmap, while NetBSD can program
modern queues during reinitialization before renegotiating features.  Queue
setup now selects the immutable transport offered by the device rather than
the temporarily empty negotiated bitmap, and reset clears each virtqueue
before refreshing its selected PCI configuration registers.

The installed vmd completed a two-vCPU NetBSD test with both CPUs online.  The
guest renewed a DHCP lease, captured frames had valid Ethernet, ARP and ICMP
headers, and a bridged host ping completed three of three replies.  The vioif
MSI-X queue counter advanced from zero to 31 with no reported receive,
transmit or control-queue errors, followed by a clean ACPI S5 shutdown.  The
virtio block and RNG queues were also exercised by the successful boot.

## Phase closeout (2026-09-01)

The LAPIC/IOAPIC, SMP, x2APIC/x2AVIC, MSI/MSI-X and virtio-net TX milestone
is functionally complete on the AMD SVM/x2AVIC test host.  Firmware and
direct-kernel SMP boots, 32- and 64-bit guests, INIT/SIPI and ordinary IPIs,
edge and level device interrupts, reboot/poweroff, MSI-X queue affinity,
multiqueue TX and TSO all have runtime coverage.  The items below are
explicitly deferred cross-vendor validation, uncommon architectural modes or
independent ACPI/RX performance work rather than blockers for closing this
phase.

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
  validated with OpenBSD guests at two, four and eight vCPUs, Linux at four
  and eight, and FreeBSD at two and eight.  The eight-vCPU FreeBSD test
  sustained guest-to-host network load.  OpenBSD and Ubuntu SMP reboot, Ubuntu
  halt/poweroff, and forced termination are also validated.  Longer
  cross-guest stress, repeated reset loops and pause/unpause remain.
- Four/eight-vCPU field validation was on AMD SVM.  The common assertion latch
  and Intel VMX run path are both updated and build-tested, and VMX reset
  reconstructs its interrupt-window control without SVM's dummy vector, but an
  SMP boot on Intel hardware remains to be exercised.
- IOAPIC fixed and lowest-priority delivery support physical, flat-logical and
  cluster-logical destinations.  LAPIC ICR fixed delivery supports the same
  destinations.  PIC-through-LINT0 ExtINT is implemented, but
  lowest-priority, NMI, SMI and ExtINT ICR delivery and IOAPIC-redirection
  ExtINT are not implemented.
- MSI supports one 64-bit message per device.  MSI-X supports at most 16
  vectors per device.  xAPIC fixed and lowest-priority delivery support
  physical and flat/cluster logical destinations; x2APIC/remapped MSI is not
  implemented.
- PM1A S5 poweroff is implemented, but no ACPI PM timer is advertised and no
  active PM1 event source asserts SCI.
- REP INS/OUTS has the deferred architectural corner cases listed above.
- Virtio-net exposes up to four queue pairs with independent TX workers and
  MSI-X queue affinity.  Runtime testing validates one, two and four active
  pairs, all four TX workers and clean shutdown.  TX checksum and TCPv4/TCPv6
  segmentation offload raise two-vCPU parallel guest TX to a 19.1 Gbit/s mean.
  RX deliberately remains on queue 0 behind one tap reader and does not yet
  accept host offload metadata.

## Next steps after testing

1. Extend the passing Linux four-vCPU smoke test with sustained
   timer/IPI/virtio interrupt stress; FreeBSD network-load coverage now reaches
   eight vCPUs.
2. Exercise pause/unpause with APs both parked and running, and extend the
   passing OpenBSD/Ubuntu reboot tests into repeated reset loops.
3. Audit device and EPT paths under genuinely concurrent vCPU exits; the
   `/dev/vmm` ioctl path already drops the kernel big lock before VMM_IOC_RUN,
   so no global execution serialization is currently known.
4. Optionally finish the x2AVIC kernel HLT lifecycle checks with pause/unpause.
   Boot, login, clean shutdown and forced termination pass, and two-,
   four- and eight-vCPU four-stream network tests reduce `ipi-not-running` and
   HLT userspace exits to zero.  Pause/unpause is deferred for now.
5. Runtime-test the legacy AMD AVIC prototype on a bit-13-capable host and
   repeat the instrumented network matrix.
6. If further host-to-guest work is justified, add virtio guest offload
   control and merged RX buffers before enabling tap RX offloads; keep generic
   tap-layer scaling outside this effort.
7. Resolve the LAPIC/PIT calibration mismatch before adding the 3.579545 MHz
   ACPI PM timer; add active PM1 event sources and SCI delivery as needed.
8. Add LAPIC ICR lowest-priority and remaining non-fixed delivery modes when a
   guest requires them.
9. Extend MSI routing only when a guest needs x2APIC/remapped delivery.
