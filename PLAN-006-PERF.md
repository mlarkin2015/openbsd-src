# PLAN-006-PERF: Performance and Optimization

## Review Findings (2026-08 source audit) — CORRECTIONS TO THIS PLAN

1. MSR numbers used throughout must match the corrected TLFS list in
   PLAN-002 (REFERENCE_TSC is 0x40000021, not 0x40000004; VP_RUNTIME 0x40000010).
2. Paravirtualized EOI (§6.2) depends on SynIC/SIEFP/SIMP pages (MSRs
   0x40000080/82/83) which in turn depend on LAPIC/LVT emulation that does not
   exist yet — sequence after the Phase-0 LAPIC work in PLAN-000.
3. Balloon device (§6.3) requires a `VMM_IOC_SETMEM`-style ioctl that does not
   exist in vmm(4); this is a kernel interface addition plus guest-page hole
   management in EPT. Larger than it looks; keep last.
4. Migration (§6.4): vmd's privilege-separated architecture (vmm/vm/viodev
   subprocesses with imsg) makes full live migration a cross-process state
   serialization problem; recommend checkpoint/restart (stop-copy) first.
5. Windows guests get most of their timekeeping win from the Hyper-V reference
   TSC page + invariant TSC exposure; KVM pvclock is irrelevant to Windows —
   prioritize accordingly.

## Goal

Improve VM performance through paravirtualized clock, VP EOI, balloon device, and live migration support.

## Current State

- KVM clocksource partially implemented (`KVM_MSR_SYSTEM_TIME`, `KVM_MSR_WALL_CLOCK`)
- pvclock infrastructure exists (`pvreg.h` — `struct pvclock_time_info`, `struct pvclock_wall_clock`)
- No paravirtualized EOI
- No memory balloon device
- No live migration support
- Each vCPU has its own run-loop pthread; emulated device and LAPIC work still
  returns to the VM process
- Event-driven I/O via `event(3)`
- `vmctl log stats` provides five-second vCPU-exit, LAPIC/IPI and virtio-net
  queue/lock counters for controlled performance work
- The current virtio-net device exposes one queue pair, even to SMP guests
- An initial AMD xAPIC AVIC implementation is build-tested.  It accelerates
  fixed edge IPIs plus LAPIC timer, IOAPIC and MSI/MSI-X vector delivery,
  while retaining userspace assistance for INIT/SIPI, LAPIC timer
  programming, level-triggered IOAPIC EOI and access faults.  It intentionally
  excludes x2AVIC, IOMMU-posted device interrupts, SEV/SEV-ES and Family 17h.
  Runtime guest validation and the before/after network matrix are next.
- A software-only x2APIC control path is build- and runtime-tested with a
  two-vCPU OpenBSD guest.  It exposes
  x2APIC only when legacy AVIC is inactive, tracks guest APICBASE state and
  sends x2APIC MSR accesses through a dedicated vmm/vmd exit to the existing
  LAPIC model.  This intentionally leaves x2AVIC disabled so guest-visible
  x2APIC is validated independently on bit-18-only Zen 4 hardware.  Exit
  statistics confirm x2APIC activity and zero AVIC activity on that host.
- Runtime testing also fixed an IPI-versus-HLT lost-wakeup race in the vCPU
  run loop and implemented ACPI S5 shutdown through PM1A.  Repeated kernel
  relinks and guest poweroffs now complete without hangs.

## SMP network measurements (2026-08-28)

Matched 1/2/4/8-vCPU OpenBSD guest tests show that throughput peaks at two
vCPUs and then flattens or declines.  The queue-notification mutex averages
only about 0.06-0.10 us of wait time under load, while a representative
eight-vCPU, four-stream interval produces roughly 147,000 fixed ICR writes
and 495,000 LAPIC MMIO writes in five seconds.  A stats-disabled control run
matches the instrumented throughput.

Before implementing the broader items below, prioritize:

1. mode-gated x2AVIC on the bit-18-only AMD host, using the recorded software
   x2APIC run as the control and never enabling legacy AVIC there;
2. legacy AMD AVIC validation on a bit-13-capable host, followed by Intel
   APICv/virtual-interrupt delivery feasibility work;
3. virtio-net multiqueue, MSI-X queue affinity and interrupt batching; and
4. another identical scaling matrix to separate APIC savings from useful
   queue parallelism.

x2APIC alone changes MMIO register access into intercepted MSR access; without
kernel or hardware acceleration it does not remove the dominant exit and
userspace-emulation cost.  Posted interrupts are most useful as part of the
hardware LAPIC design and do not eliminate userspace virtio processing.

## What to Build

### 6.1 Paravirtualized Clocksource

**What**: Provide a fast, accurate time source to the guest without requiring a VM exit for every time query.

**Current**: KVM clocksource (`KVM_MSR_SYSTEM_TIME`) exists in `vmm_machdep.c` but may not be fully functional.

**Implementation**:

1. **KVM clocksource** (`KVM_MSR_SYSTEM_TIME` = 0x4B564D01):
   - Guest reads this MSR to get a GPA
   - Host writes a `pvclock_time_info` structure at that GPA
   - Structure is read by guest without VM exit
   - Uses version numbers for atomic reads

2. **Hyper-V TSC reference page** (`HV_X64_MSR_REFERENCE_TSC` = 0x40000004):
   - Guest writes GPA to this MSR
   - Host writes a `hyperv_tsc_page` structure
   - Includes TSC-to-time conversion data

3. **pvclock structure** (from `pvreg.h`):
   ```c
   struct pvclock_time_info {
       uint32_t   ti_version;
       uint32_t   ti_pad0;
       uint64_t   ti_tsc_timestamp;    // TSC at last update
       uint64_t   ti_system_time;      // System time in nanoseconds
       uint32_t   ti_tsc_to_system_mul; // Fixed-point multiplier
       int8_t     ti_tsc_shift;         // Right shift for TSC
       uint8_t    ti_flags;             // PVCLOCK_FLAG_*
       uint8_t    ti_pad[2];
   } __packed;
   ```

4. **Wall clock** (`KVM_MSR_WALL_CLOCK` = 0x4B564D00):
   ```c
   struct pvclock_wall_clock {
       uint32_t wc_version;
       uint32_t wc_sec;       // Seconds since epoch
       uint32_t wc_nsec;      // Nanoseconds
   } __packed;
   ```

**Implementation**:
- In `vmm_hyperv_runtime.c` (from PLAN-002):
  ```c
  void vmm_hv_runtime_update(struct vcpu *vcpu) {
      if (vcpu->hv_tsc_page_gpa == 0) return;
      
      paddr_t gpa = vcpu->hv_tsc_page_gpa;
      void *hva = vmm_translate_gpa(gpa, PAGE_SIZE);
      if (!hva) return;
      
      struct hyperv_tsc_page *page = hva;
      
      // Update in two steps with sequence numbers
      page->tsc_sequence1 = 1;  // odd = updating
      page->tsc_time_base = rdtsc();
      page->ref_time = getnanotime();
      page->tsc_frequency = tsc_frequency;
      page->tsc_sequence2 = 0;  // even = valid
  }
  ```
- Call `vmm_hv_runtime_update()` from the VM entry path (before `VMM_IOC_RUN` re-entry)
- Call `vmm_hv_runtime_update_all()` periodically (e.g., every 10ms) from the vmd event loop

**Files to modify:**
- `sys/arch/amd64/amd64/vmm_hyperv_runtime.c` — add update logic
- `sys/arch/amd64/amd64/vmm_machdep.c` — integrate clock update into VM entry/exit path

### 6.2 Paravirtualized EOI (VP EOI)

**What**: Allow the guest to acknowledge interrupts without a VM exit, improving interrupt latency.

**Reference**: KVM paravirtualized EOI (`KVM_MSR_EOI_EN`), Hyper-V SynIC EOI

**Implementation**:

1. **KVM-style paravirtualized EOI**:
   ```c
   #define KVM_MSR_EOI_EN  0x4B564D04
   
   // Guest writes GPA of EOI page
   // Guest writes interrupt vector to that GPA
   // Hypervisor acknowledges interrupt without VM exit
   ```

2. **Hyper-V SynIC EOI**:
   ```c
   #define HV_X64_MSR_APIC_EOI  0xE000000A
   
   // Guest writes interrupt vector to this MSR
   // Hypervisor acknowledges via SynIC
   ```

**Implementation**:
- In `vmm_machdep.c`, intercept writes to `KVM_MSR_EOI_EN`:
  ```c
  case KVM_MSR_EOI_EN:
      if (write) {
          vcpu->kvm_eoi_gpa = value;
          // Map the EOI page
          vmm_eoi_map_page(vcpu);
      }
      break;
  ```
- In the guest memory write path, check if the write is to the EOI page:
  ```c
  // In the write path (before writing to guest memory):
  if (gpa == vcpu->eoi_page_gpa && data == expected_vector) {
      // Acknowledge interrupt
      vmm_ack_interrupt(vcpu, data);
      // Don't actually write to memory
      return;
  }
  ```

**Files to create:**
- `sys/arch/amd64/amd64/vmm_eoi.c` — paravirtualized EOI handling
  - `vmm_eoi_init()` — initialize EOI structures
  - `vmm_eoi_handle()` — process EOI writes
  - `vmm_eoi_map_page()` — map EOI page
  - `vmm_eoi_ack()` — acknowledge interrupt
- `sys/arch/amd64/amd64/vmm_eoi.h` — header

### 6.3 Memory Balloon Device

**What**: Allow the host to dynamically adjust VM memory usage (balloon up/down).

**Reference**: VirtIO balloon device spec

**Implementation**:

1. **VirtIO balloon PCI device**:
   - Vendor: `0x1AF4` (Virtio)
   - Device: `0x1003` (VirtIO Balloon)
   - Features: `VIRTIO_BALLOON_F_DEFLATE_ON_OOM`, `VIRTIO_BALLOON_F_STATS_VQ`

2. **Virtqueue for balloon commands**:
   - Queue 0: Balloon commands (inflate/deflate)
   - Queue 1: Statistics (optional)

3. **Balloon command format**:
   ```c
   struct balloon_command {
       uint8_t type;      // 0=INFLATE, 1=DEFLATE, 2=FREE_PAGE_REPORT
       uint8_t reserved;
       uint16_t pages;    // Number of pages
   };
   ```

**Implementation**:
- In `vmd`, the balloon device is a regular VirtIO device
- When guest inflates the balloon (asks for memory):
  - Host allocates pages, maps them into VM memory
  - Pages are "taken" from the VM's available memory
  - These pages can be used for host-side workloads
- When guest deflates:
  - Pages are returned to the VM

**Host-side memory management**:
- Maintain a pool of "balloon pages"
- When VM inflates: allocate from host pool, map into VM GPA space
- When VM deflates: unmap, return to host pool

**Files to create:**
- `usr.sbin/vmd/balloon.c` — VirtIO balloon device
  - `balloon_init()` — initialize balloon device
  - `balloon_handle_command()` — process inflate/deflate commands
  - `balloon_inflate()` — allocate pages for guest
  - `balloon_deflate()` — return pages from guest
  - `balloon_stats()` — report balloon statistics
- `usr.sbin/vmd/balloon.h` — header

### 6.4 Live Migration Support

**What**: Allow a running VM to be migrated to another host with minimal downtime.

**Reference**: VM live migration protocols (QEMU migration, libvirt)

**Implementation approach**:

1. **Save VM state**:
   - VCPU register state (already available via `VMM_IOC_READREGS`)
   - Guest memory (already mapped, can be read)
   - Device state (emulated devices' internal state)

2. **Transfer state**:
   - Send memory pages (dirty page tracking for incremental transfers)
   - Send device state
   - Send register state

3. **Resume on destination**:
   - Set up memory on destination host
   - Set device state
   - Set VCPU register state (via `VMM_IOC_RESETCPU`)
   - Start VM

**Implementation**:

1. **Dirty page tracking**:
   - Use EPT to mark pages as dirty when written
   - Periodically scan dirty pages and send only changed pages
   - Iterate: send dirty pages → VM runs → repeat → send final state → stop VM → resume on destination

2. **VM state serialization**:
   ```c
   struct vm_migration_state {
       uint32_t magic;         // Magic number for version detection
       uint32_t version;       // Migration state version
       uint32_t vm_id;
       uint32_t vcpu_count;
       uint8_t registers[VMM_MAX_VCPUS][sizeof(struct vcpu_reg_state)];
       uint8_t memory[];       // Guest memory (padded to page size)
       // Device state follows...
   };
   ```

3. **Transfer protocol**:
   - Use a simple TCP-based protocol (similar to QEMU migration)
   - Send magic + version
   - Send memory pages (page-by-page, with checksums)
   - Send device state
   - Send register state
   - Acknowledge completion

**Files to create:**
- `sys/arch/amd64/amd64/vmm_migrate.c` — kernel migration support
  - `vmm_migrate_save()` — save VM state
  - `vmm_migrate_load()` — load VM state
  - `vmm_migrate_dirty_page_scan()` — scan for dirty pages
  - `vmm_migrate_register_state()` — get/set register state
- `sys/arch/amd64/amd64/vmm_migrate.h` — header
- `usr.sbin/vmd/migrate.c` — userspace migration support
  - `migrate_start()` — begin migration
  - `migrate_receive_state()` — receive migration state
  - `migrate_send_state()` — send migration state
  - `migrate_transfer_memory()` — transfer guest memory
  - `migrate_transfer_devices()` — transfer device state
  - `migrate_transfer_registers()` — transfer register state
  - `migrate_resume()` — resume VM on destination
- `usr.sbin/vmd/migrate.h` — header

### 6.5 VCPU Runtime Tracking

**What**: Track how much CPU time each VCPU has consumed.

**Implementation**:
- On each `VMM_IOC_RUN` call:
  - Record timestamp before entering guest
  - Record timestamp after returning from guest
  - Accumulate into vcpu runtime counter
- Expose runtime via Hyper-V VP runtime MSR (`HV_X64_MSR_VP_RUNTIME`)
- Expose runtime via KVM clocksource (`pvclock_time_info`)

**Files to modify:**
- `sys/arch/amd64/amd64/vmm_machdep.c` — add runtime tracking in VM entry/exit
- `sys/arch/amd64/amd64/vmm_hyperv_runtime.c` — expose runtime via Hyper-V MSR

## Dependencies

- None external — all implemented within vmm codebase
- Requires timing infrastructure (`getnanotime()`, `rdtsc()`)

## Risks

- **Memory ballooning**: Taking pages from a running VM can cause performance impact. Pages must be properly unmapped and remapped.
- **Live migration complexity**: Device state is the hardest part. Each emulated device must be serializable.
- **Dirty page tracking**: Must use EPT dirty logging. Not all hardware supports this.

## Implementation Order

1. Implement paravirtualized clocksource (pvclock update)
2. Implement Hyper-V TSC reference page update
3. Implement paravirtualized EOI
4. Implement VCPU runtime tracking
5. Implement memory balloon device
6. Implement basic live migration (save/load full state, no incremental)
7. Implement dirty page tracking for incremental migration
8. Test with Linux guest (easier to verify clock accuracy)
9. Test with Windows VM (measure interrupt latency, clock accuracy)
