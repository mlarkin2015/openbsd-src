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

## Next steps after testing

1. Fix any timer-frequency mismatch found in testing.
2. LINT0 virtual-wire routing (PIC through LAPIC ExtINT delivery mode).
3. ICR/INIT-SIPI + per-vCPU state machine (RUNNING/INIT/WAIT_SIPI/HALTED)
   for SMP guests; needs kernel-side SMP audit too (see PLAN-000-MASTER
   working notes re: kernel big lock).
4. IOAPIC logical destination mode.
