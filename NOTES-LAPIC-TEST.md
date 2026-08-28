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

## Known remaining gaps (not yet done)

- The kernel still rejects `vcp_ncpus != 1` in `vm_create()`.  Its VM/vCPU
  allocation, per-vCPU locks, run ioctl and interrupt-kick interfaces already
  carry vCPU IDs, but none have yet been validated with concurrent guest CPUs.
- AP lifecycle is missing.  vmd currently resets every vCPU to the BSP boot
  state and starts every vCPU thread immediately; APs instead need to remain
  in a distinct wait-for-SIPI state until selected by the BSP.
- ICR/IPIs (especially INIT-SIPI) are not implemented.  ICRLO/ICRHI writes are
  logged and dropped.
- CPUID leaf 1 does not advertise a logical-processor count or HTT, topology
  leaf 0x0b is zeroed, and MSR_APICBASE incorrectly reports the BSP bit on
  every vCPU.  These must reflect the configured vCPU topology.
- IOAPIC destination-mode handling still ignores logical mode / lowest-prio
  delivery (dest used directly as vcpu id).
- Delivery modes other than Fixed delivered as fixed.
- MSI supports one 64-bit message per device.  MSI-X supports at most 16
  vectors per device.  x2APIC/remapped MSI, logical destinations, and
  lowest-priority delivery are not implemented.
- PM1A registers are implemented, but no active PM1 event source asserts SCI.
- REP INS/OUTS has the deferred architectural corner cases listed above.

## Next steps after testing

1. Permit 2+ vCPUs in `vmm(4)` and add a vmd AP state machine that creates APs
   but parks them in `WAIT_SIPI`; prove that a 2-vCPU VM runs only the BSP.
2. Implement physical-destination ICR fixed IPIs, INIT and SIPI, including
   destination shorthand, AP reset state, SIPI `CS.base = vector << 12`, and
   waking/kicking the selected vCPU thread.
3. Correct CPUID leaf 1/topology and per-vCPU MSR_APICBASE BSP reporting, then
   boot 2-vCPU OpenBSD, Linux and FreeBSD guests and stress timer/IPI/virtio
   interrupt delivery.
4. Audit device and EPT paths under genuinely concurrent vCPU exits; the
   `/dev/vmm` ioctl path already drops the kernel big lock before VMM_IOC_RUN,
   so no global execution serialization is currently known.
5. Add active ACPI PM1 event sources and SCI delivery as they become needed.
6. Add IOAPIC logical destination mode.
7. Extend MSI routing only when a guest needs logical/x2APIC delivery or
   interrupt remapping.
