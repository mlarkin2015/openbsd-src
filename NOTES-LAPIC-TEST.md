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

## Test plan: uniprocessor OpenBSD or Linux guest, SeaBIOS

Watch for:

1. Boot progress:
   - OpenBSD: should pass clock calibration / "cpu0: TSC frequency" without
     hanging.
   - Linux: LAPIC timer calibration should complete
     ("tsc: Refined TSC clocksource calibration").
2. Clock accuracy: if guest clock runs fast/slow, the assumed 100 MHz LAPIC
   bus clock may disagree with what CPUID leaves 0x15/0x16 advertise.
   Fix = align them (i82489dx.c i82489dx_timer_ccr uses ns/(1000/div)).
3. Level IRQs under load: virtio-blk/net are level-triggered via IOAPIC.
   The RIRR fix should prevent wedging; if virtio stalls under load,
   suspect i82093aa_evaluate_pin/i82093aa_eoi interaction first.
4. Logging: interrupt delivery is now log_debug; run vmd verbose to see
   traces.

## Known remaining gaps (not yet done)

- Timer thread only wakes HALTED vcpus (vcpu_hlt check in lapic_timer_thread);
  running guests pick up expiry via the normal run loop. OK for unicpu.
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
