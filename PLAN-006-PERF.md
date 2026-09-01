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
- Virtio-net now exposes up to four queue pairs when MQ is negotiated.  Each TX
  queue has its own worker and MSI-X queue vector; RX intentionally remains
  on queue 0 behind the single tap reader.  The control queue supports the
  standard queue-pair command and retains the queue-2 layout when MQ is not
  negotiated.  A fresh vmd build and the vmd regression suite pass.  Runtime
  tests at one, two, four and eight vCPUs validate selection of one, two, four
  and four pairs, MSI-X queue affinity, networking and clean shutdown.
  Moving from one to two workers improves parallel two-vCPU guest TX from
  1.25 to 2.34 Gbit/s, but four active workers remain at approximately the
  same 2.2-2.3 Gbit/s ceiling.  Parallel RX remains flat, as expected from the
  single tap reader.
- Virtual CPUID topology now describes one package containing one single-
  threaded core per configured vCPU.  The legacy Intel leaf 1/4 view, modern
  leaves 0x0b/0x1f and AMD leaves 0x80000008/0x8000001e use the same APIC-ID
  width, including non-power-of-two vCPU counts.  This prevents OpenBSD's
  interrupt mapper from discarding virtual CPUs as SMT siblings before
  virtio-net multiqueue negotiation.  The kernel builds, and guests at one,
  two, three, four and eight vCPUs report one package containing distinct
  single-threaded cores.  The three-vCPU case validates non-power-of-two
  topology decoding.
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
- Per-reason AVIC statistics showed that the remaining accelerated-APIC exits
  under two-vCPU TCP load were overwhelmingly x2AVIC incomplete IPIs whose
  target was not running.  The x2AVIC-only kernel HLT fast path is now runtime
  validated: normal HLT waits remain inside `VMM_IOC_RUN`, queued vectors wake
  that wait, and non-x2AVIC guests retain the vmd condition-variable path.
  Two-run four-stream guest-to-host means were 1.21, 1.05 and 1.18 Gbit/s at
  two, four and eight vCPUs respectively, while vmd reported zero HLT, AVIC and
  target-not-running exits at every CPU count.  This removes the targeted
  round trips but does not demonstrate a network-throughput gain.  The
  remaining device I/O is concentrated on vCPU0, and x2APIC timer exits scale
  linearly at approximately 250 per vCPU per five seconds.

## SMP network measurements (2026-08-28)

Matched 1/2/4/8-vCPU OpenBSD guest tests show that throughput peaks at two
vCPUs and then flattens or declines.  The queue-notification mutex averages
only about 0.06-0.10 us of wait time under load, while a representative
eight-vCPU, four-stream interval produces roughly 147,000 fixed ICR writes
and 495,000 LAPIC MMIO writes in five seconds.  A stats-disabled control run
matches the instrumented throughput.

## Two-pair TX measurements (2026-08-31)

The TX-only multiqueue experiment does remove the previous virtio-net
bottleneck for parallel guest transmission.  In the full two-vCPU matrix,
four-stream guest-to-host throughput rose from 1.25 to 2.34 Gbit/s (+87%).
Single-stream guest TX rose from 1.14 to 1.25 Gbit/s, while one- and
four-stream host-to-guest results remained effectively flat at 1.72 and 1.91
Gbit/s.  This direction split is consistent with the implementation: there
are two TX workers, but still only one tap reader feeding RX queue 0.

Against the matched pre-MQ four-stream scaling means of 1.21, 1.05 and 1.18
Gbit/s, the new two-pair means were 2.34, 1.86 and 2.19 Gbit/s at two, four and
eight vCPUs.  The four-vCPU mean conservatively includes a 1.09 Gbit/s
outlier; its following two runs were 2.26 and 2.22 Gbit/s.  vmd counters show
comparable traffic on both TX workers, and the eight-vCPU guest recorded
42,106 and 22,517 interrupts on the two queue-pair MSI-X handlers.

## Four-pair TX measurements (2026-08-31)

The device now advertises four pairs and OpenBSD guests select one, two or
four according to their CPU count.  Four-vCPU/eight-stream runs reached 2.31
and 2.34 Gbit/s; an eight-vCPU/eight-stream run reached 2.22 Gbit/s.  All four
TX workers carried comparable loads, including a representative interval of
261,726, 232,419, 245,827 and 238,734 packets, while RX remained on q0.

This establishes that queue negotiation and worker distribution are not the
reason throughput stops scaling beyond two pairs.  Keep the four-pair cap for
now, but do not increase it again before measuring TSO or finding a workload
that exceeds the current shared tap/write/network ceiling.

## Virtio-net TX TSO measurements (2026-08-31)

Virtio-net now offers `VIRTIO_NET_F_CSUM` and
`VIRTIO_NET_F_HOST_TSO4/6`.  Each TX worker converts the guest's
`virtio_net_hdr` checksum/GSO fields to a native tap(4) `tun_hdr`, allowing
the host network stack to consume TCP superpackets without vmd segmenting or
copying them.  TCP/UDP checksum offsets and TCPv4/TCPv6 segmentation are
validated; ECN, UFO and unrepresentable arbitrary checksum offsets are not
advertised.  TX frames may now carry up to 64 KiB of packet data.

`TUNSCAP` is enabled with zero receive capabilities.  This makes the tap
header available for writes but asks the host to materialize checksums and
segments before reads.  The existing single tap reader strips that zeroed
header and continues to feed RX queue 0; host-to-guest GSO, merged receive
buffers and RX multiqueue are not part of this milestone.

The two-vCPU OpenBSD guest negotiated two queues and reported checksum,
TSOv4 and TSOv6 hardware features.  Two four-stream guest-to-host runs reached
17.8 and 20.4 Gbit/s (19.1 Gbit/s mean), versus the prior 2.34 Gbit/s mean:
about 8.2 times the throughput.  Representative five-second device samples
reported 671,943 TSO packets out of 676,496 TX packets and 714,639 out of
717,263, proving the offload path was active.  A four-stream reverse-direction
control reached 2.06 Gbit/s with no retransmits, consistent with the earlier
1.91 Gbit/s single-reader RX result.  The guest booted, remained reachable by
ping and SSH, and shut down cleanly.

Before implementing the broader items below, prioritize:

1. decide whether host-to-guest offload support is worth the required merged
   receive-buffer and guest-offload-control work, without expanding into
   generic tap-layer receive scaling; and
2. retain legacy AMD AVIC validation and Intel APICv feasibility work as
   hardware-specific follow-ups.

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
