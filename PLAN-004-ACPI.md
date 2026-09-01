# PLAN-004-ACPI: Enhanced ACPI Tables for Windows

## Review Findings (2026-08 source audit) — CORRECTIONS TO THIS PLAN

1. **Current State is wrong about DSDT**: it is loaded from
   `/etc/firmware/vmm.dsdt` and is *optional* (silently skipped if missing,
   `acpi.c:385-391`), not "generated in C". The immediate priority is authoring a
   real DSDT in ASL and committing the compiled AML (plus the .asl source and a
   build rule invoking iasl, or a checked-in AML for reproducibility).
2. MADT already includes LAPIC and IOAPIC entries (`acpi.c`), contrary to
   "basic MADT with one LAPIC entry". Still needed: interrupt source overrides
   (ISA 0→2, 8→GSI8 active-low/level), LAPIC NMI, x2APIC entries when >255 CPUs.
3. **Missing from the plan entirely — SMBIOS**: vmd generates no SMBIOS tables and
   fw_cfg exposes none. OVMF reads SMBIOS via fw_cfg files
   (`etc/smbios/smbios-tables`, `etc/smbios/smbios-entry-point-64`). Windows
   licensing and Device Manager rely on it. Add `smbios.c` generating type 0,1,2,3,4
   (per vCPU),16,17,19,20,32 tables. High priority.
4. HPET (§4.3): no HPET emulation exists; note that Windows timekeeping works with
   PIT+RTC+PM-timer if the FADT/HPET table is consistent — an HPET table without
   emulation is worse than none. Either implement both together or defer both.
5. SRAT/SLIT (§4.5-4.6): single-NUMA VMs don't need them; defer.
6. HEST (§4.9): defer; WHEA is optional for installation.
7. Table placement: current addresses RSDP 0x9D000, XSDT 0x9E000, MADT 0x9F000,
   FADT 0xA0000, DSDT 0xA1000 (`acpi.h:31-35`) leave little headroom once HPET/
   MCFG/SSDT/TPM2 tables are added — plan a table area (e.g. reserve 0x9C000-
   0x9FFFF + high-memory placement for DSDT/SSDT) before adding tables ad hoc.
8. FADT: keep `pm1_cnt_len` compatible with the actual PM1a_CNT emulation in
   vmd (SLP_TYP/SLP_EN handling for clean Windows shutdown) — verify
   `acpi.c`/`i82093aa.c` PM register emulation exists before widening lengths.

## Goal

Implement comprehensive ACPI tables that Windows expects for hardware discovery, power management, CPU topology, and interrupt routing.

## Current State

- `acpi.c` generates: RSDP, XSDT, MADT, FADT, DSDT
- RSDP at 0x9D000
- XSDT at 0x9E000
- MADT at 0x9F000 (interrupt controller info)
- FADT at 0xA0000 (PM1a/b, SCI=IRQ9)
- DSDT at 0xA1000 (minimal)
- ACPI 2.0+ conventions used
- DSDT is loaded from a file (`acpi_load_table()` in `acpi.c`)

## What to Build

### 4.1 Enhanced FADT (Fixed ACPI Description Table)

**What**: Extend the FADT to provide Windows with accurate power management and interrupt information.

**Current**: Basic FADT with PM1a event/control, S5 poweroff and SCI=IRQ9;
FACS is present, but the PM timer is not implemented or advertised.

**Changes**:
```c
// acpi.c: acpi_create_fadt()

/* Add HPET base address */
fadt.hpet_base = hpet_gpa;

/* Update FADT revision to 6 (ACPI 6.3) */
fadt.hdr.revision = 6;

/* Add flags for Windows compatibility */
fadt.flags |= FADT_F_HW_SUPPRESSED;  /* HW-suppressed coords */
fadt.flags |= FADT_F_S4_RTC_STS_BIT; /* S4 RTC valid on sleep */

/* Add power button and sleep button handlers */
fadt.pwr_btn = pm1a_evt_blk;
fadt.slm_btn = pm1a_evt_blk;

/* Update PM1 event block length */
fadt.pm1_evt_len = 4;  /* Already set */

/* Update PM1 control block length */
fadt.pm1_cnt_len = 4;  /* Extend to 4 bytes for better compatibility */

/* Add GPE blocks */
fadt.gpe0_blk = 0xB020;
fadt.gpe0_blk_len = 0x20;
fadt.gpe1_blk = 0xB040;
fadt.gpe1_blk_len = 0x10;
```

**Power management**:
- Implement PM1a event register (power button, sleep button, power button interrupt)
- Implement PM1a control register (SLP_TYPa, SLP_EN)
- Implement PM Timer (already partially present via i8253)

### 4.2 MADT (Multiple APIC Description Table)

**What**: Extend the MADT to provide complete APIC/IOAPIC information.

**Current**: Basic MADT with one LAPIC entry

**Changes**:
1. **Local APIC entries** (one per vCPU):
   ```c
   struct madt_lapic {
       uint8_t type;       // 0 (Local APIC)
       uint8_t reserved;
       uint16_t flags;
       uint8_t acpi_id;    // APIC ID = vCPU ID
       uint32_t local_apic_id;
   }
   ```
   - Create one entry per vCPU (up to 64)

2. **IOAPIC entry**:
   ```c
   struct madt_ioapic {
       uint8_t type;       // 1 (IOAPIC)
       uint8_t reserved;
       uint16_t flags;
       uint8_t ioapic_id;
       uint8_t ioapic_uid;
       uint32_t ioapic_gasi;  // Global system interrupt base
       uint64_t ioapic_addr;  // IOAPIC memory-mapped address
   }
   ```

3. **Interrupt source overrides** (for PIC IRQs):
   ```c
   struct madt_intsrcovr {
       uint8_t type;       // 2 (Interrupt Source Override)
       uint8_t bus;        // 0 (ISA)
       uint8_t irq;        // IRQ 0-15
       uint32_t gsi;       // GSI (same as IRQ for ISA)
       uint16_t flags;     // Trigger mode, polarity
   }
   ```

4. **NMI sources**:
   ```c
   struct madt_nmi {
       uint8_t type;       // 3 (NMI Source)
       uint16_t flags;
       uint32_t gsi;
       uint8_t irq_flags;
   }
   ```

5. **Local APIC NMI entries**:
   ```c
   struct madt_lapic_nmi {
       uint8_t type;       // 4 (Local APIC NMI)
       uint16_t flags;
       uint8_t acpi_uid;
       uint8_t lapic_flags;
   }
   ```

### 4.3 HPET (High Precision Event Timer)

**What**: Add an HPET table. Windows requires HPET for high-resolution timer support.

**Implementation**:
1. Create HPET emulation in vmm(4):
   - HPET registers (General Capability, General Configuration, Timer 0-3)
   - Timer 0 = main HPET counter (acts as replacement for PIT)
   - Timer 1 = RTC fallback
   - Timer 2-3 = reserved

2. Add HPET entry to MADT:
   ```c
   struct madt_hpet {
       uint8_t type;       // 0xF (HPET)
       uint8_t reserved;
       uint16_t flags;
       uint8_t hpet_id;
       uint8_t reserved2;
       uint16_t event_cap_blk;
       uint64_t event_cap_blk_addr;
   }
   ```

**Files to create:**
- `sys/arch/amd64/amd64/vmm_hpet.c` — HPET emulation in kernel
- `usr.sbin/vmd/hpet.c` — HPET userspace interface (if needed)

**Files to modify:**
- `usr.sbin/vmd/acpi.c` — add `acpi_create_hpet()`
- `usr.sbin/vmd/acpi.h` — add function declaration

### 4.4 MCFG (Memory-mapped PCI Configuration)

**What**: Add MCFG table for PCIe enumeration. Windows requires this for PCIe device discovery.

**Implementation**:
```c
struct acpi_mcfg {
    uint8_t signature[4];     // "MCFG"
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t oemid[6];
    uint8_t oem_tableid[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint64_t reserved;
    struct acpi_mcfg_segment segments[];
};

struct acpi_mcfg_segment {
    uint64_t base_address;    // MMCONFIG base
    uint16_t pci_segment_group;
    uint8_t start_bus_number;
    uint8_t end_bus_number;
    uint32_t reserved;
};
```

- Base address: 0xE0000000 (standard for x86)
- PCI segment group: 0
- Start bus: 0
- End bus: 255 (or number of buses)

**Files to modify:**
- `usr.sbin/vmd/acpi.c` — add `acpi_create_mcfg()`

### 4.5 SRAT (System Resource Affinity Table)

**What**: SRAT provides NUMA topology information. Windows uses this for CPU affinity and memory locality.

**Implementation**:
```c
struct acpi_srat {
    uint8_t signature[4];     // "SRAT"
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t oemid[6];
    uint8_t oem_tableid[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint64_t reserved;
    struct acpi_srat_entry entries[];
};

struct acpi_srat_entry {
    uint8_t type;           // 0 = CPU, 1 = Memory
    uint8_t length;
    uint16_t flags;
    uint8_t apic_id;
    uint32_t local_sapic_eid;
    uint32_t local_sapic_id;
    uint32_t local_x2apic_id;
    uint32_t x2apic_uid;
    uint32_t flags;         // CPU ONLINE_OK
    uint8_t processor_hardware_id[8];
};
```

- Create one CPU entry per vCPU
- Create one memory entry per memory range
- Set `flags` bit 0 (online capable) for all entries
- Set `local_x2apic_id` = vCPU ID (for x2APIC support)

**Files to modify:**
- `usr.sbin/vmd/acpi.c` — add `acpi_create_srat()`

### 4.6 SLIT (System Locality Information Table)

**What**: SLIT provides memory locality distances between NUMA nodes.

**Implementation**:
```c
struct acpi_slit {
    uint8_t signature[4];     // "SLIT"
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t oemid[6];
    uint8_t oem_tableid[8];
    uint32_t oem_revision;
    uint33_t creator_id;
    uint32_t creator_revision;
    uint64_t locality_count;
    uint8_t entry[];          // Distance matrix
};
```

- For a single NUMA node (typical VM):
  - `locality_count = 1`
  - `entry[0] = 10` (distance from node 0 to itself = 10)

### 4.7 SSDT (Secondary SDT)

**What**: Additional embedded ACPI tables for dynamic device configuration, CPU hotplug, and device power states.

**Implementation**:
1. **CPU hotplug SSDT** (even if hotplug is not supported, Windows expects it):
   - `_OST` — Operating System Notifications
   - `_Lxx` — Level-triggered methods
   - `_Exx` — Edge-triggered methods
   - `GPE` — General Purpose Events for CPU hotplug

2. **Device power states SSDT**:
   - `_P0_`, `_P1_`, `_P2_`, `_P3_`, `_P4_` — processor power states
   - `_C0_`, `_C1_`, `_C2_`, `_C3_` — processor C-states
   - `_T_` — thermal zone

3. **PCI device enumeration SSDT** (for devices not discoverable via PCI config space):
   - `_SB.PCI0.RST_` — reset method for PCI devices
   - `_SB.PCI0.Slots` — PCI slot information

### 4.8 DSDT Enhancements

**What**: The DSDT (Discrete System Description Table) needs significant enhancement.

**Current**: Minimal DSDT (loaded from file, likely very basic)

**Changes**:
1. **System Bus device (_SB)**:
   ```asl
   Scope (_SB) {
       Name (_HID, "PNP0A08")        // PCI Express bus
       Name (_CID, "PNP0A03")        // Compatible with ACPI 1.0 PCI
       Name (_ADR, 0)
       
       Device (PCI0) {
           Name (_ADR, 0)
           Name (_CID, "PNP0A03")
           
           // Power resources
           Method (_PRW, 0, Serialized) {
               Return (Package() { 0x05, 0x03 })  // GPE0, D3
           }
       }
       
       Device (LAPIC) {
           Name (_ADR, 0)
           Name (APIC, 0)
       }
   }
   ```

2. **ACPI devices**:
   ```asl
   Device (ACPI) {
       Name (_HID, "ACPI000A")
       Method (_STA, 0, NotSerialized) { Return (0x0F) }
   }
   ```

3. **Timer devices**:
   ```asl
   Device (TIMR) {
       Name (_HID, "PNP0100")      // 8254 PIT
       Name (_CRS, ResourceTemplate() {
           IoDecode16(0x0000, 0x0040, 0x00, 0x0040, 0x10)
       })
   }
   ```

4. **PIC devices**:
   ```asl
   Device (PIC) {
       Name (_HID, "PNP0000")
       Name (_CRS, ResourceTemplate() {
           IoDecode16(0x0020, 0x0020, 0x00, 0x0020, 0x08)
           IoDecode16(0x00A0, 0x00A0, 0x00, 0x00A0, 0x08)
       })
   }
   ```

5. **RTC device**:
   ```asl
   Device (RTC) {
       Name (_HID, "PNP0B00")
       Name (_CRS, ResourceTemplate() {
           IoDecode16(0x0070, 0x0070, 0x00, 0x0070, 0x08)
           Interrupt(ResourceConsumer, Level, ActiveLow, Shared) { 8 }
       })
   }
   ```

6. **PCI resources**:
   ```asl
   Name (_CRS, ResourceTemplate() {
       IoDecode16(0x0000, 0x0000, 0x00, 0x0FFF, 0x10)
       IoDecode16(0xC000, 0xC000, 0x00, 0xC000, 0x4000)
   })
   ```

7. **GPE devices** (General Purpose Events):
   ```asl
   Device (GPE0) {
       Name (_HID, "ACPI0008")
       Name (_STA, 0x0F)
   }
   ```

8. **Power management**:
   ```asl
   Device (PWRB) {
       Name (_HID, "PNP0B00")
       Name (_STR, "Power Button")
   }
   
   Device (SLPB) {
       Name (_HID, "PNP0C0E")
       Name (_STR, "Sleep Button")
   }
   ```

**Implementation approach**:
- Write the DSDT in ASL (ACPI Source Language) and compile with iASL (Intel ACPI Compiler)
- Or: generate DSDT in C (like the other tables) — more consistent with existing codebase
- C generation approach: create `acpi_dsc.c` with functions to build DSDT nodes

**Files to create/modify:**
- `usr.sbin/vmd/acpi.c` — extend DSDT generation
- `usr.sbin/vmd/acpi_dsc.c` — new file for DSDT generation (or add to acpi.c)
- `usr.sbin/vmd/acpi.h` — add DSDT constants

### 4.9 HEST (Hardware Error Source Table)

**What**: HEST provides hardware error source information. Windows uses this for error reporting and WHEA (Windows Hardware Error Architecture).

**Implementation**:
```c
struct acpi_hest {
    uint8_t signature[4];     // "HEST"
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    uint8_t oemid[6];
    uint8_t oem_tableid[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    struct acpi_hest_entry entries[];
};

struct acpi_hest_entry {
    uint32_t type;            // 0 = Hardware Error Source
    uint32_t length;
    uint32_t reserved;
    uint32_t header_revision;
    uint8_t configured;
    uint8_t reserved2;
    uint16_t source_id;
    uint32_t error_block_size;
    uint8_t type_data[...];
};
```

- Create one hardware error source entry for "Generic Error Source"
- Enable WHEA support

### 4.10 OSPM Interface (OSC)

**What**: The `_OSC` (OS Support/Features Control) method allows the guest OS to negotiate control of PCI features with the firmware.

**Implementation**:
- In the DSDT, add an `_OSC` method under the PCI root bus
- Windows sends an _OSC query to determine what features the firmware supports
- Response includes support for:
  - PCIe ASPM
  - PCIe PM
  - PCIe AER
  - PCIe Hot-Plug
  - PCIe PCIe capabilities
  - PCIe INTx
  - PCIe HP native IRQ
  - PCIe native hot plug

**Files to modify:**
- `usr.sbin/vmd/acpi.c` — add `_OSC` method to DSDT
- `usr.sbin/vmd/pci.c` — implement PCI feature negotiation (ASPM, PM, etc.)

## Dependencies

- Intel ACPI Compiler (iASL) for compiling ASL to DSDT binary (if using ASL approach)
- Existing ACPI infrastructure in `acpi.c`

## Risks

- **DSDT complexity**: Writing a complete DSDT is complex and error-prone. Windows ACPI parser is strict.
- **ACPI table validation**: Windows has strict ACPI table validation. Use `acpidt` or `acpidump` from a Linux system to validate tables before shipping.
- **ASL vs C**: Compromise on C-based DSDT generation to stay consistent with existing code, but consider ASL for complex sections (power management, PCI enumeration).

## Implementation Order

1. Enhance FADT with HPET base, GPE blocks, power buttons
2. Enhance MADT with per-vCPU LAPIC entries, IOAPIC, interrupt source overrides
3. Add HPET table and HPET emulation
4. Add MCFG table
5. Add SRAT table (CPU + memory affinity)
6. Add SLIT table
7. Add HEST table
8. Enhance DSDT (PCI, power, timer, PIC, RTC, GPE devices)
9. Add `_OSC` method for PCI feature negotiation
10. Add SSDT entries (CPU hotplug, power states)
11. Validate all tables with `acpidump` / ACPI checker
12. Test with Windows (Device Manager, ACPI diagnostics)
