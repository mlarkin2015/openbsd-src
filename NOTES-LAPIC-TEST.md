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

An audit of vmd's PCI and virtio code established that guest MSI/MSI-X is not
implemented.  vmd does not add a PCI MSI or MSI-X capability.  The virtio
transport's MSI-X selector fields explicitly ignore writes and return
`VIRTIO_MSI_NO_VECTOR`; device interrupts use assigned legacy INTx lines via
`vcpu_assert_irq()`.  A `pci=nomsi` comparison was therefore unnecessary and
would not select a different interrupt path.

Commit `d31f7a9` added the DSDT source as `usr.sbin/vmd/vmm-dsdt.asl` and
fixed the firmware description.  Follow-up validation added the root bridge
resource windows and stopped advertising unimplemented PM timer/secondary PM
blocks.  The final tables now:

- set FADT revision 6.5 and `FADT_NO_MSI`, which Linux explicitly honors;
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

Linux still repeatedly reports `No irq handler for 0.52` (vector 0x34).
Console input triggers the message once per character, tying it to legacy
IRQ4/UART routing rather than PCI INTx.  Linux also reports that it cannot
disable RTC fixed events because vmd advertises PM1A event/control registers
but does not emulate them.  Both are separate follow-up items.

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

- ICR/IPIs (INIT-SIPI) not implemented - SMP bringup still missing.
  ICRLO writes are logged and dropped.
- LINT0/virtual-wire routing not done: PIC and LAPIC remain parallel paths;
  intr_ack() in x86_vm.c prefers LAPIC then falls back to PIC.
- IOAPIC destination-mode handling still ignores logical mode / lowest-prio
  delivery (dest used directly as vcpu id).
- Delivery modes other than Fixed delivered as fixed.
- PCI MSI and MSI-X are not implemented.  Virtio uses legacy INTx despite
  exposing the standard virtio common-configuration vector selector fields.
- ACPI PM1A event/control and SCI delivery are not implemented.  The FADT
  still advertises the PM1A blocks, which makes Linux attempt unsupported RTC
  fixed-event operations.

## Next steps after testing

1. Trace the Linux IRQ4/UART vector-0x34 failure, then implement LINT0
   virtual-wire routing (PIC through LAPIC ExtINT delivery mode).
2. Implement ACPI PM1A/SCI semantics, or provide complete hardware-reduced
   sleep-control/status registers before setting `FADT_HW_REDUCED_ACPI`.
3. ICR/INIT-SIPI + per-vCPU state machine (RUNNING/INIT/WAIT_SIPI/HALTED)
   for SMP guests; needs kernel-side SMP audit too (see PLAN-000-MASTER
   working notes re: kernel big lock).
4. IOAPIC logical destination mode.
5. Implement PCI MSI-X delivery for modern virtio guests.
