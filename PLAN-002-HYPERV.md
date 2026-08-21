# PLAN-002-HYPERV: Hyper-V Paravirtualization Interface

## Review Findings (2026-08 source audit) — CORRECTIONS TO THIS PLAN

Most MSR numbers and several CPUID leaf layouts in this plan are **wrong**. Use the
genuine TLFS (Hypervisor Top-Level Functional Specification) values below.

**Correct MSR numbers** (replace §2.2 and §2.8 lists):

```
HV_X64_MSR_GUEST_OS_ID      0x40000000
HV_X64_MSR_HYPERCALL        0x40000001
HV_X64_MSR_VP_INDEX         0x40000002
HV_X64_MSR_RESET            0x40000003
HV_X64_MSR_VP_RUNTIME       0x40000010
HV_X64_MSR_TIME_REF_COUNT   0x40000020
HV_X64_MSR_REFERENCE_TSC    0x40000021
HV_X64_MSR_TSC_FREQUENCY    0x40000022
HV_X64_MSR_APIC_FREQUENCY   0x40000023
HV_X64_MSR_EOI              0x40000070
HV_X64_MSR_ICR              0x40000071
HV_X64_MSR_SCONTROL         0x40000080
HV_X64_MSR_SVERSION         0x40000081
HV_X64_MSR_SIEFP            0x40000082
HV_X64_MSR_SIMP             0x40000083
HV_X64_MSR_EOM              0x40000084
HV_X64_MSR_STIMER0_CONFIG   0x400000B0   (B0/B2/B4/B6 config, B1/B3/B5/B7 count)
HV_X64_MSR_CRASH_P0..P4     0x40000100..0x40000104
HV_X64_MSR_GUEST_IDLE       0x400000F0
```

**Correct CPUID layout** (replace §2.1):

```
0x40000000: EAX = max leaf (0x4000000A typical), EBX/ECX/EDX = "Microsoft Hv"
0x40000001: EAX = "Hv#1" interface signature (NOT "WN01")
0x40000002: version (EAX build, EBX major.minor) — optional, omit
0x40000003: feature identification:
    EAX: VP runtime(0), time ref count(1), SynIC(2), synth timers(3),
         APIC access virt(4), hypercall(5), VP index(6), reset(7), stats(8),
         reference TSC(9), guest idle(10)
    EBX: crash regs(0), debug(1), xmm fast hypercall(4), guest debug(10)
    ECX: bit 0 = enlightened VMCS (eVMCS)
    EDX: reserved
0x40000004: recommendations (EBX): relaxed timing(0), crash hint(10)...
0x40000005: implementation limits (EAX = max supported spinwait)
Leaves 0x40000006..0x4000000F as described in §2.1 DO NOT EXIST in the TLFS.
```

**Other corrections:**

1. Current state verified: leaf 0x40000000 returns "OpenBSDVMM58" and 0x40000100
   "KVMKVMKVM" (`vmm_machdep.c:6563-6590`, `vmmvar.h:26`). There is no existing
   Hyper-V leaf handling; `hv_guest_os_id` state tracking (§2.1) is a sound
   gating mechanism — keep it.
2. **Hypercall mechanism (§2.3)**: the guest writes HV_X64_MSR_HYPERCALL (GPA |
   enable bit), the hypervisor fills that page with `vmcall`/`vmmcall` thunks, and
   the guest invokes them with control + input/output GPAs in RCX/RDX/R8. There is
   no "hv_invept instruction". Interception point: existing VMX_EXIT_VMCALL /
   SVM_VMEXIT_VMMCALL handlers (`vmm_machdep.c:4747`, `:4307`) currently inject
   #UD — extend them instead of adding a new exit path.
3. **eVMCS (§2.4): drop from scope.** It changes VMCS layout and interacts with
   VMX controls; it is an optional optimization, not a Windows requirement. Do not
   advertise ECX bit 0 of leaf 0x40000003 in v1.
4. **Kernel/userspace split decision required first**: CPUID is handled entirely in
   the kernel without exiting to vmd, so Hyper-V CPUID requires kernel changes and a
   per-VM policy (which signature to expose). Consider adding a CPUID-delegation
   exit or a `VMM_IOC_SETCPUID`-style interface so policy lives in vmd, mirroring
   KVM_SET_CPUID2. Decide before implementing §2.1.
5. Windows also needs the **hypervisor-present bit (leaf 1 ECX bit 31)** and
   invariant-TSC/leaf 0x80000007 EDX bit 8 exposure — audit `vmm_handle_cpuid()`
   for these before any Hyper-V leaf works usefully.
6. Synthetic timers/SynIC (§2.5) depend on working LAPIC/LVT emulation which does
   not exist yet (see PLAN-000 Phase 0). Sequence accordingly.

## Goal

Implement the Hyper-V paravirtualization interface (CPUID leaves, MSRs, hypercalls) so that Microsoft Windows detects vmm as a Hyper-V compatible hypervisor and enables paravirtualized drivers and performance features.

## Current State

- CPUID leaf 0x40000000 returns "OpenBSDe" hypervisor signature
- CPUID leaf 0x40000001 returns KVM features (clocksource2, stable bit)
- CPUID leaf 0x40000100 returns "KVMKVMKVM" signature
- CPUID leaf 0x40000101 returns KVM features (nop-io-delay, clocksource)
- MSR intercepts: `vmx_handle_wrmsr()` / `vmx_handle_rdmsr()` handle various MSRs
- `pvreg.h` already defines Hyper-V CPUID leaf constants and feature flags
- MSR definitions present: `HV_X64_MSR_*` macros likely in machine headers

## What to Build

### 2.1 Hyper-V CPUID Leaves

**What**: Windows queries CPUID to detect Hyper-V and determine which paravirtualized features are available.

**Implementation**: Add a new case block in `vmm_handle_cpuid()` in `sys/arch/amd64/amd64/vmm_machdep.c` that handles Hyper-V CPUID leaves (0x40000000-0x4000000F) and related hypercall leaves (0x4000000E-0x4000000F).

**Required CPUID leaves**:

```
CPUID 0x40000000: Hypervisor ID and max leaf
  EAX = 0x4000000D  (highest supported leaf)
  EBX = 'Msft'      (Microsoft signature)
  ECX = 'hyperv'    (hypervisor identifier)
  EDX = ' '         (trailing space)
  → Signifies "Microsoft hypervisor detected"

CPUID 0x40000001: Interface identification
  EAX = 'WN01'      (Windows 2008 R2 compatibility)
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x40000002: Version
  EAX = 0x000F000C  (Hyper-V version 10.0, build 26100 — Windows 11 24H2)
  EBX = major.minor (e.g., 0x000F0001 for v10.0)
  ECX = service pack / build flags
  EDX = service branch / number

CPUID 0x40000003: Feature flags
  EAX = bit 0 = VP runtime available
        bit 1 = Time Reference Count available
        bit 2 = Synthetic Interrupt Controller (SynIC) available
        bit 3 = Synthetic Timers available
        bit 4 = Hypercall interface available
        bit 5 = VP index MSR available
        bit 6 = MSR reset available
        bit 7 = Enlightened VMCS available
        bit 8 = Stats pages available
        bit 9 = Reference TSC available
        bit 10 = Guest Idle halt available
  EBX = feature bits (extended)
  ECX = feature bits (extended)
  EDX = feature bits (extended)

CPUID 0x40000004: Enlightened VMCS info
  EAX = bit 0 = Enlightened VMCS supported
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x40000005: Implementation limits
  EAX = 0
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x40000006: Hypervisor capabilities
  EAX = 0
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x40000007: APIC access virtualization
  EAX = 0
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x40000008: Guest features
  EAX = 0
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x40000009: Hypervisor identity (long format)
  EAX = 'Msft'
  EBX = 'hyperv'
  ECX = '     '    (padding)
  EDX = 0

CPUID 0x4000000A: Enlightened MSR bitmap
  EAX = 0
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x4000000B: Partition counter
  EAX = 0
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x4000000C: Partition counters available
  EAX = 0
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x4000000D: Feature bits (extended)
  EAX = same as 0x40000003
  EBX = same as 0x40000003
  ECX = same as 0x40000003
  EDX = same as 0x40000003

CPUID 0x4000000E: Hypercall interface
  EAX = 'HV01'      (Hypercall interface version 1)
  EBX = 0
  ECX = 0
  EDX = 0

CPUID 0x4000000F: Feature bits (additional)
  EAX = 0
  EBX = 0
  ECX = 0
  EDX = 0

Extended leaves (0x80000000+):
  0x80000001: EAX bit 31 = hypervisor present (already set by existing code)
  0x80000001: EBX bit 31 = hypervisor interface (set if Hyper-V detected)
```

**Implementation details**:
- The existing CPUID handler in `vmm_machdep.c` uses `switch(leaf)` to handle different leaves
- Add a new `switch` block for the `0x40000000` range
- The handler needs to check whether the guest has signaled itself as Hyper-V by:
  - Writing to `HV_X64_MSR_GUEST_OS_ID` (0x40000000) — this is the guest's way of identifying itself
  - If guest OS ID MSR is written, the guest is Hyper-V aware (Windows or Linux with Hyper-V passthrough)
- Track state: have we seen a Hyper-V guest OS ID? If so, return Hyper-V leaves. Otherwise, return OpenBSDe/KVM leaves (for non-Windows guests)

**State tracking**:
- Add a field to `struct vcpu`: `uint64_t hv_guest_os_id;` — set when guest writes MSR 0x40000000
- In `vmm_handle_cpuid()`, check if `hv_guest_os_id != 0`:
  - If yes: return Hyper-V leaves
  - If no: return OpenBSDe/KVM leaves (current behavior)

### 2.2 Hyper-V MSRs

**What**: Windows uses MSRs to communicate with the hypervisor — providing guest identity, hypercall page address, time reference pages, etc.

**Implementation**: Add MSR intercept handling in `vmx_handle_rdmsr()` and `vmx_handle_wrmsr()` for Hyper-V MSRs.

**Required MSRs**:

```
MSR 0x40000000 (HV_X64_MSR_GUEST_OS_ID):
  - Guest writes: guest OS identity (e.g., for Windows = specific build number)
  - Hypervisor reads: to detect Hyper-V guest
  - Implementation: store value in vcpu struct, don't trap (or trap and store)
  - Also write back: hypervisor can read this to know which guest OS is running

MSR 0x40000001 (HV_X64_MSR_HYPERCALL):
  - Guest writes: GPA of hypercall page (struct hv_hypercall_page)
  - Hypervisor reads: to map the hypercall page into guest memory
  - Implementation: store GPA, validate it's writable, create shadow mapping

MSR 0x40000002 (HV_X64_MSR_TSC):
  - Guest reads: TSC frequency info
  - Implementation: return current TSC value
  - Guest writes: 0 to reset TSC reference

MSR 0x40000003 (HV_X64_MSR_EPOCH):
  - Guest reads: current epoch counter
  - Implementation: return epoch (seconds since 1970 or similar)

MSR 0x40000004 (HV_X64_MSR_REFERENCE_TSC):
  - Guest writes: GPA of TSC reference page (struct hyperv_tsc_page)
  - Hypervisor reads: to know where to write TSC conversion data
  - Implementation: store GPA, create shared page mapping

MSR 0x40000005 (HV_X64_MSR_TSC_FREQUENCY):
  - Guest reads: TSC frequency in Hz
  - Implementation: return tsc_frequency

MSR 0x40000006 (HV_X64_MSR_APIC_FREQUENCY):
  - Guest reads: APIC timer frequency
  - Implementation: return APIC timer frequency (typically 10MHz)

MSR 0x40000007 (HV_X64_MSR_CRASH_P0-P4):
  - Used for crash dump parameters
  - Guest writes: crash dump info
  - Implementation: store values for crash dump support

MSR 0x40000008 (HV_X64_MSR_VP_INDEX):
  - Guest reads: VP (virtual processor) index
  - Guest writes: VP index
  - Implementation: read returns vcpu->vc_id, write stores value

MSR 0x40000009 (HV_X64_MSR_RESET):
  - Guest writes: resets all Hyper-V MSRs to their default values
  - Implementation: clear all hv_* fields in vcpu struct

MSR 0xE0000000 (HV_X64_MSR_VP_INDEX):
  - Same as 0x40000006 — VP index (alternate name)

MSR 0xE0000001 (HV_X64_MSR_TIME_REF_COUNT):
  - Guest reads: time reference count (for synthetic timer)
  - Implementation: return current time reference count

MSR 0xE0000002 (HV_X64_MSR_CYCLE_COUNTER):
  - Guest reads: cycle counter
  - Implementation: return current TSC

MSR 0xE0000003 (HV_X64_MSR_TSC_PAGE):
  - Guest reads: GPA of TSC page
  - Implementation: return stored TSC page GPA

MSR 0xE0000004 (HV_X64_MSR_VP_RUNTIME):
  - Guest reads: VP runtime (nanoseconds spent running)
  - Implementation: accumulate runtime on each VCPU run, return value

MSR 0xE0000005 (HV_X64_MSR_GUEST_IDLE):
  - Guest reads: 1 if guest is idle, 0 if running
  - Implementation: track guest idle state

MSR 0xE0000006 (HV_X64_MSR_GUEST_CRASH_P0-P4):
  - Guest crash dump registers (same as 0x40000007)

MSR 0xE0000007 (HV_X64_MSR_VP_INDEX):
  - VP index (alternate)

MSR 0xE0000008 (HV_X64_MSR_SYNTHETIC_TIMER):
  - Timer control register

MSR 0xE0000009 (HV_X64_MSR_APIC_VERSION):
  - APIC version

MSR 0xE000000A (HV_X64_MSR_APIC_EOI):
  - APIC EOI write (for synthetic interrupt controller)

MSR 0xE000000B (HV_X64_MSR_SYNTHETIC_TIMER_ADVANCE):
  - Timer advance value
```

**Implementation approach**:
- In `vmm_machdep.c`, extend `vmx_handle_rdmsr()`:
  ```c
  switch (msr) {
  case HV_X64_MSR_GUEST_OS_ID:
      *value = vcpu->hv_guest_os_id;
      break;
  case HV_X64_MSR_HYPERCALL:
      *value = vcpu->hv_hypercall_gpa;
      break;
  case HV_X64_MSR_VP_INDEX:
      *value = vcpu->vc_id;  // or stored value
      break;
  case HV_X64_MSR_VP_RUNTIME:
      *value = vcpu->hv_runtime_ns;
      break;
  case HV_X64_MSR_REFERENCE_TSC:
      *value = vcpu->hv_tsc_page_gpa;
      break;
  case HV_X64_MSR_TSC_FREQUENCY:
      *value = tsc_frequency;
      break;
  // ... more cases
  }
  ```
- Similarly extend `vmx_handle_wrmsr()` to store values

### 2.3 Hypercall Page Handling

**What**: Windows writes a GPA to the Hypercall MSR, then uses the `hv_invept` instruction (or `HYPERV_HYPERCALL_EXIT` in SVM) to invoke hypercalls. The hypervisor must intercept the hypercall instruction, look up the GPA, and execute the requested hypercall.

**Implementation**:
- When guest writes `HV_X64_MSR_HYPERCALL` with a GPA:
  - Validate the GPA maps to a writable page in the guest memory
  - Store the GPA in the vcpu struct
  - Map this page into the host address space (via `vmm_translate_gpa()`)
  - The guest can then write hypercall arguments and execute `vmmcall`/`hypercall`
- Intercept `vmmcall` / `vmcall` instruction in the VMX exit handler:
  - Check if RIP points to the hypercall page
  - Extract hypercall number from RAX (top 12 bits = hypercall number, bit 31 = indirect)
  - For simple hypercalls: execute directly
  - For indirect hypercalls: load arguments from the hypercall page, execute, store results

**Hypercall numbers to implement (at minimum)**:
```c
HV_X64_MCALL_GET_SYSTEM_INFORMATION   (0)
HV_X64_MCALL_GET_PARTITION_INFORMATION (1)
HV_X64_MCALL_POST_EVENT               (4)
HV_X64_MCALL_RESET_EVENT              (5)
HV_X64_MCALL_SET_TIMER                (6)
HV_X64_MCALL_SIGNAL_EVENT             (7)
HV_X64_MCALL_FLUSH_VIRTUAL_ADDRESS    (12)
HV_X64_MCALL_FLUSH_VIRTUAL_ADDRESS_LIST (13)
HV_X64_MCALL_NESTED_VP_JOIN           (17)
HV_X64_MCALL_CLEAR_EVT                (19)
HV_X64_MCALL_CREATE_EVT               (20)
```

**Simple hypercalls** (that vmm can handle directly):
- `POST_EVENT` — set an event flag
- `RESET_EVENT` — clear an event flag
- `SET_TIMER` — program a synthetic timer
- `SIGNAL_EVENT` — signal an event (wake up waiting vCPU)

**TL;DR for first iteration**: Implement the hypercall interception mechanism and handle only `POST_EVENT`, `RESET_EVENT`, `SET_TIMER`, `SIGNAL_EVENT`. Other hypercalls can return "not supported" (E_NOT_SUPPORTED).

**Files to create:**
- `sys/arch/amd64/amd64/vmm_hypercall.c` — Hyper-V hypercall handling
  - `vmm_hypercall_init()` — initialize hypercall structures
  - `vmm_hypercall_handle()` — main hypercall dispatcher
  - `vmm_hypercall_post_event()`
  - `vmm_hypercall_reset_event()`
  - `vmm_hypercall_set_timer()`
  - `vmm_hypercall_signal_event()`
  - `vmm_hypercall_flush_tlb()`
  - `vmm_hypercall_get_system_info()`
  - `vmm_hypercall_get_partition_info()`
- `sys/arch/amd64/amd64/vmm_hypercall.h` — header

### 2.4 VP Index MSR

**What**: Simple MSR that returns the vCPU index.

**Implementation**:
- In `vmx_handle_rdmsr()`:
  ```c
  case HV_X64_MSR_VP_INDEX:
      *value = vcpu->vc_id;
      break;
  ```
- In `vmx_handle_wrmsr()`:
  ```c
  case HV_X64_MSR_VP_INDEX:
      vcpu->hv_vp_index = (uint32_t)(*value);
      break;
  ```
- This is straightforward — no shared memory needed.

### 2.5 Time Reference Page

**What**: Guest writes a GPA to `HV_X64_MSR_REFERENCE_TSC`. The hypervisor writes to this page each time it processes a VM entry, providing TSC-to-time conversion data.

**Data structure** (`struct hyperv_tsc_page`):
```c
struct hyperv_tsc_page {
    uint64_t tsc_page;           // Unused
    uint64_t tsc_sequence1;      // Sequence (even = valid)
    uint64_t tsc_time_base;      // TSC value when ref_time was set
    uint32_t ref_time;           // Reference time in 100ns units
    uint32_t tsc_sequence2;      // Sequence (even = valid)
    uint32_t tsc_frequency;      // TSC frequency in Hz (fixed-point)
};
```

**Implementation**:
- Store the GPA when guest writes `HV_X64_MSR_REFERENCE_TSC`
- On each VM entry (or periodically in the run loop):
  - Translate GPA to host VA via `vmm_translate_gpa()`
  - Write current TSC, ref_time (host time), and tsc_frequency to the page
- Use `tsc_sequence` odd/even to indicate valid vs invalid data (guest reads twice and compares)

**Files to create:**
- `sys/arch/amd64/amd64/vmm_hv_runtime.c` — Hyper-V runtime features
  - `vmm_hv_runtime_init()` — initialize shared memory structures
  - `vmm_hv_runtime_update()` — update TSC reference page
  - `vmm_hv_runtime_update_all()` — update for all VCPUs

### 2.6 MSR Intercept Configuration

**What**: The hypervisor must intercept reads/writes to Hyper-V MSRs so that the guest can configure the interface.

**Implementation**: Add Hyper-V MSR numbers to the MSR intercept bitmap in both VMX and SVM.

**VMX (MSR bitmaps)**:
```c
// In vcpu_init_vmx():
vmx_setmsrbrw(vcpu, HV_X64_MSR_GUEST_OS_ID);
vmx_setmsrbrw(vcpu, HV_X64_MSR_HYPERCALL);
vmx_setmsrbr(w, vcpu, HV_X64_MSR_TSC);
vmx_setmsrbr(w, vcpu, HV_X64_MSR_EPOCH);
vmx_setmsrbr(w, vcpu, HV_X64_MSR_REFERENCE_TSC);
vmx_setmsrbr(r, vcpu, HV_X64_MSR_TSC_FREQUENCY);
vmx_setmsrbr(r, vcpu, HV_X64_MSR_APIC_FREQUENCY);
vmx_setmsrbr(w, vcpu, HV_X64_MSR_VP_INDEX);
vmx_setmsrbr(w, vcpu, HV_X64_MSR_RESET);
// ... etc.
```

**SVM (MSR bitmap)**:
```c
// In vcpu_init_svm():
svm_setmsrbrw(vcpu, HV_X64_MSR_GUEST_OS_ID);
svm_setmsrbr(w, vcpu, HV_X64_MSR_HYPERCALL);
// ... etc.
```

### 2.7 AMD-V (SVM) Support

**What**: Ensure all Hyper-V CPUID/MSR handling works for AMD-V as well as Intel VT-x.

**Implementation**: Mirror the VMX changes in the SVM code paths:
- `svm_handle_cpuid()` — add Hyper-V case block
- `svm_handle_rdmsr()` / `svm_handle_wrmsr()` — add Hyper-V MSR cases
- SVM MSR intercept — configure MSR bitmaps in VMCB

**Files to modify:**
- `sys/arch/amd64/amd64/vmm_support.c` — SVM (AMD-V) support

### 2.8 pvreg.h Updates

**What**: Add any missing Hyper-V MSR and CPUID definitions to `sys/dev/pv/pvreg.h`.

**Additions**:
```c
/* Hyper-V MSR definitions */
#define HV_X64_MSR_GUEST_OS_ID          0x40000000
#define HV_X64_MSR_HYPERCALL            0x40000001
#define HV_X64_MSR_TSC                  0x40000002
#define HV_X64_MSR_EPOCH                0x40000003
#define HV_X64_MSR_REFERENCE_TSC        0x40000004
#define HV_X64_MSR_TSC_FREQUENCY        0x40000005
#define HV_X64_MSR_APIC_FREQUENCY       0x40000006
#define HV_X64_MSR_CRASH_P0             0x40000007
#define HV_X64_MSR_CRASH_P1             0x40000008
#define HV_X64_MSR_CRASH_P2             0x40000009
#define HV_X64_MSR_CRASH_P3             0x4000000A
#define HV_X64_MSR_CRASH_P4             0x4000000B
#define HV_X64_MSR_VP_INDEX             0x40000006
#define HV_X64_MSR_RESET                0x40000007
#define HV_X64_MSR_VP_RUNTIME           0xE0000004
#define HV_X64_MSR_GUEST_IDLE           0xE0000005
#define HV_X64_MSR_TIME_REF_COUNT       0xE0000001
#define HV_X64_MSR_CYCLE_COUNTER        0xE0000002

/* Hyper-V CPUID sub-leaves */
#define CPUID_OFFSET_HYPERV_ENLIGHTENED_VMCS   0x4
#define CPUID_OFFSET_HYPERV_IMPL_LIMITS        0x5
#define CPUID_OFFSET_HYPERV_CAPABILITIES       0x6
#define CPUID_OFFSET_HYPERV_APIC_ACCESS        0x7
#define CPUID_OFFSET_HYPERV_GUEST_FEATURES     0x8
#define CPUID_OFFSET_HYPERV_IDENTITY           0x9
#define CPUID_OFFSET_HYPERV_MSR_BITMAP         0xA
#define CPUID_OFFSET_HYPERV_PARTITION_COUNTER  0xB
#define CPUID_OFFSET_HYPERV_COUNTER_AVAILABLE  0xC
#define CPUID_OFFSET_HYPERV_FEATURE_BITS       0xD
#define CPUID_OFFSET_HYPERV_HYPERCALL_IFACE    0xE

/* Hyper-V feature flags for CPUID 0x40000003 */
#define HYPERV_FEATURE_EAX_VP_RUNTIME       0
#define HYPERV_FEATURE_EAX_TIME_REF_COUNT   1
#define HYPERV_FEATURE_EAX_SYNIC            2
#define HYPERV_FEATURE_EAX_STIMER           3
#define HYPERV_FEATURE_EAX_HYPERCALL        5
#define HYPERV_FEATURE_EAX_VP_INDEX         6
#define HYPERV_FEATURE_EAX_MSR_RESET        7
#define HYPERV_FEATURE_EAX_ENLIGHTED_VMCS   7  /* same bit as MSR_RESET */
#define HYPERV_FEATURE_EAX_REF_TSC          9
#define HYPERV_FEATURE_EAX_GUEST_IDLE       10
```

**Files to modify:**
- `sys/dev/pv/pvreg.h` — add Hyper-V definitions

## Dependencies

- No external dependencies (all implemented within vmm codebase)
- Existing MSR intercept infrastructure must be working
- Existing CPUID intercept infrastructure must be working

## Risks

- **Guest detection**: Need a reliable way to determine if the guest is Hyper-V-aware. The standard way is to check if the guest has written to `HV_X64_MSR_GUEST_OS_ID`. However, the guest might not do this until after boot.
- **Hypercall page mapping**: Mapping guest GPAs requires careful handling of EPT. The hypercall page must be mapped as writable.
- **Performance**: Every hypercall is a VM exit, which is expensive. Simple hypercalls (event signaling) should be handled quickly without full memory access if possible.

## Implementation Order

1. Add Hyper-V definitions to `sys/dev/pv/pvreg.h`
2. Add Hyper-V CPUID handling in `vmm_handle_cpuid()` (Intel VMX only first)
3. Add Hyper-V MSR handling in `vmx_handle_rdmsr()` / `vmx_handle_wrmsr()`
4. Add Hyper-V CPUID handling in `svm_handle_cpuid()` (AMD SVM)
5. Add Hyper-V MSR handling in `svm_handle_rdmsr()` / `svm_handle_wrmsr()`
6. Implement hypercall page mapping (store GPA, validate)
7. Implement basic hypercall interception (vmmcall/vmcall)
8. Implement simple hypercalls: POST_EVENT, RESET_EVENT, SET_TIMER, SIGNAL_EVENT
9. Add VP runtime tracking (nanoseconds in guest)
10. Implement TSC reference page update
11. Test with Linux guest (easier to debug than Windows)
12. Test with Windows VM
