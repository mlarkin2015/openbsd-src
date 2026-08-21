# PLAN-001-OVMF: UEFI Firmware Support

## Review Findings (2026-08 source audit) — CORRECTIONS TO THIS PLAN

Verified against the tree (`fw_cfg.c`, `x86_vm.c`, `loadfile_elf.c`, `acpi.c`):

1. **No PE/COFF parsing or flat64 entry is needed (§1.4 is wrong).** OVMF is flashed
   ROM: SEC starts in 16-bit real mode at 0xFFFFFFF0 and performs its own mode
   transitions. The existing `loadfile_bios()` path (`x86_vm.c:302`) already loads an
   image ending at 4 GiB and sets `vcpu_init_flat16` — exactly what OVMF needs.
   Required changes are only:
   - map the firmware region read-only in EPT (currently guest-writable RAM);
   - raise/verify the 4 MiB BIOS size limit in `loadfile_bios()` for OVMF images;
   - add a second writable pflash-style region for the NVRAM store (see §1.5 fix).
2. **NVRAM placement (§1.5) is wrong**: 0x9E000 already holds the XSDT
   (`acpi.h:31-35`). The varstore must be a separate MMIO region just below the
   firmware flash (QEMU layout: vars flash at 4GB - fw_size*2), persisted by
   trapping EPT writes to that region, not mapped into low RAM.
3. **fw_cfg is thinner than assumed**: only SIGNATURE, ID, NOGRAPHIC, FILE_DIR and
   files "etc/e820", "etc/screen-and-debug", "bootorder" exist (`fw_cfg.c:97-120`).
   OVMF additionally needs `etc/acpi/tables`, `etc/acpi/rsdp` (or RSDP via its
   conventional scan locations — current 0x9D000 works), and ideally
   `etc/smbios/smbios-tables` + `etc/smbios/smbios-entry-point-64`.
   Note: no SMBIOS generation exists anywhere in vmd today (see PLAN-004).
4. **DSDT is loaded from `/etc/firmware/vmm.dsdt` and is optional**
   (`acpi.c:385-391`) — not C-generated as stated in Current State. A minimal DSDT
   may not satisfy OVMF's PCI/RTC expectations; author a real one first (PLAN-004).
5. **Secure Boot should be deferred**: Windows 10/11 install fine with Secure Boot
   disabled; MS key enrollment/licensing adds risk. Ship `ENABLE_SECURE_BOOT=FALSE`
   builds first, keep NVRAM layout SB-ready.
6. SMM: prefer an OVMF build with SMM disabled entirely (`SMM_REQUIRE=FALSE`)
   rather than relying on OVMF_SERIALIZE — vmm(4) does not emulate SMRAM/SMM at
   all, and OVMF runs fine without it under KVM-style VMMs.

## Goal

Enable vmm/vmd to boot VMs using OVMF (TianoCore EDK2) UEFI firmware alongside the existing SeaBIOS path, with support for UEFI NVRAM variable storage and Secure Boot.

## Current State

- SeaBIOS works: loaded as binary image into guest memory (4GB-top + 1MB-top regions)
- vCPU init: `vcpu_init_flat16` sets up 16-bit real mode for BIOS
- `fw_cfg` interface already exists at ports 0x510-0x518 — QEMU's firmware configuration interface, which OVMF uses
- ACPI tables already generated in C (`acpi.c`) — OVMF expects ACPI tables via fw_cfg or RSDP pointer

## What to Build

### 1.1 OVMF Firmware Build Integration

**Build OVMF binary for OpenBSD target**

TianoCore EDK2 needs to be cross-compiled. Options:
- **Option A**: Cross-compile EDK2 toolchain (BaseTools + GCC x86_64) on OpenBSD, then build OVMF
- **Option B**: Pre-build OVMF on a Linux build machine, ship `ovmf.fd` as a binary blob
- **Option C**: Build OVMF as part of the OpenBSD build using existing cross-compile infrastructure

Recommended: **Option B first** (get it working), then **Option A** (integrate into build system).

**Required OVMF modules** (from `OvmfPkg/OvmfPkgX64.dsc`):
- `OvmfPkg/Sec/SecMain.inf` — SEC phase (entry point, CPU init)
- `OvmfPkg/PeiCore/PeiCore.inf` — PEI phase (early initialization, memory)
- `OvmfPkg/DxeCore/DxeCore.inf` — DXE phase (driver discovery, memory services)
- `OvmfPkg/Smm/SmiEntry/SmiEntry.inf` — SMM support
- `OvmfPkg/Library/DynamicTableLib/AcpiTableInfra.c` — ACPI table generation
- `OvmfPkg/Library/MemoryAllocationLib/PoolAllocation.c` — memory allocation

**EDK2 build flags** (DSC file or env):
```
SECURITY_UPDATE_CAPSULE = TRUE
FEATURE_ENABLE_PK = TRUE
FEATURE_ENABLE_PLATFORM_CERT_LIST = TRUE
OVMF_SERIALIZE = TRUE  # Single-threaded SMM (critical for OpenBSD vmm)
ENABLE_SECURE_BOOT = TRUE
```

**Output**: `Build/OVMF/DEBUG_GCC5/X64/OVMF.fd` — 64MB or 128MB firmware image

**Files to create:**
- `usr.sbin/vmd/ovmf/build-ovmf.sh` — shell script to build OVMF
- `usr.sbin/vmd/ovmf/ovmf.fd` — the built firmware blob (or path reference)
- `usr.sbin/vmd/ovmf/README` — build instructions and dependencies

### 1.2 NVRAM Variable Store

**What**: Emulate UEFI variable storage (NV RAM) backed by a file on the host filesystem.

**Protocol**: UEFI runtime services — `EfiRuntimeServices->SetVariable()` and `GetVariable()`.

**Data format**: Standard UEFI NVRAM variable format:
```c
struct uefi_variable {
    uint8_t  data[];        // Variable data
    uint32_t attributes;    // EFI_VARIABLE_NON_VOLATILE, etc.
    uint32_t data_size;
    uint64_t time_stamps;   // (optional)
    guid_t   vendor_guid;   // {8be4df61-93ca-11d2-aa0d-00e098032b8c} for "EFI"
    char     name[];        // Variable name (Unicode)
};
```

**Implementation approach**:
- Use the `OvmfPkg/Library/GovmVarLib` variable store format (QEMU-compatible)
- Store as a file: `/var/vm/<vm-name>/ovmf_vars.fd`
- File format: fixed-size buffer with variable entries, each marked as "valid" or "deleted"
- On VM start: map this file into the VM's memory as the "variables runtime memory range"
- On VM stop: file already reflects updates (it's a regular file)

**Files to create:**
- `usr.sbin/vmd/ovmf_nvram.c` — NVRAM variable store implementation
  - `nvram_init()` — load/create NVRAM file, set up in VM memory
  - `nvram_find_variable()` — search variable store for a variable
  - `nvram_set_variable()` — set/update a variable (handle delete/merge)
  - `nvram_get_variable()` — get variable value
  - `nvram_delete_variable()` — mark variable as deleted
  - `nvram_reset()` — clear all variables (for factory reset)
  - `nvram_sync()` — flush to file (or rely on file mapping)
- `usr.sbin/vmd/ovmf_nvram.h` — header with types and function declarations

**Secure Boot specific**: The NVRAM must store:
- `db` — signature database (allowed boot signatures)
- `dbx` — forbidden signature database (revoked signatures)
- `KEK` — Key Exchange Key (used to sign updates to db/dbx)
- `PK` — Platform Key (owner of the secure boot chain)
- `SetupMode` — 0=deployed mode (Secure Boot on), 1=setup mode (allow signing)

### 1.3 Firmware Selection in vm.conf

**What**: Allow administrators to select firmware type per VM.

**vm.conf syntax addition**:
```
vm "win11" {
    firmware "ovmf"       # or "seabios" (default)
    memory 8192
    cpus 4
    disk "hd0" "/path/to/disk.vhd"
    cdrom "/path/to/win11.iso"
}
```

**Implementation**:
1. Add `vmc_firmware` field to `struct vmop_create_params` in `vmd.h`:
   ```c
   unsigned int vmc_firmware;
   #define VMD_FW_SEABIOS  0
   #define VMD_FW_OVMF     1
   ```
2. Parse in `config.c` — add `firmware "ovmf"` / `firmware "seabios"` to vm.conf parser
3. Pass firmware type through imsg to the vm process

**Files to modify:**
- `usr.sbin/vmd/vmd.h` — add `vmc_firmware` field, constants
- `usr.sbin/vmd/parse.y` — add `firmware` keyword
- `usr.sbin/vmd/config.c` — validate firmware selection, handle path lookups

### 1.4 UEFI Boot Path

**What**: When OVMF is selected, set up VM differently than SeaBIOS boot.

**SeaBIOS boot** (existing):
1. Load SeaBIOS binary to 4GB-top + 1MB-top regions
2. Set vCPU to 16-bit real mode (`vcpu_init_flat16`): CS=0xF000:0xFFF0, RIP=0xFFF0
3. BIOS POST → reads ACPI tables → boots from disk/cdrom
4. Loader reads MBR, finds kernel, loads into memory

**OVMF boot** (new):
1. Load OVMF.fd to memory:
   - PEI Core typically runs from 0xE0000-0xFFFFF
   - DXE Core runs at higher addresses
   - OVMF memory map includes:
     - EfiLoaderCode: firmware code
     - EfiLoaderData: firmware data
     - EfiRuntimeServicesCode: runtime firmware code
     - EfiRuntimeServicesData: runtime firmware data
     - EfiACPIMemoryNVS: ACPI NVS
     - EfiACPIReclaimMemory: ACPI reclaim
     - EfiConventionalMemory: available RAM
2. Set vCPU to 64-bit mode (already in `vcpu_init_flat64`):
   - CR0 = PE | PG | NW | CD
   - CR3 = PML4 page (set up identity mapping)
   - CS.L = 1 (long mode)
   - RIP = OVMF entry point (typically 0xFF800000 or similar — depends on firmware image)
   - GDTR, IDTR properly configured
3. Pass ACPI tables via RSDP pointer (same as SeaBIOS)
4. Pass fw_cfg data (ACPI tables, kernel cmdline, etc.) at fw_cfg ports 0x510-0x518
5. OVMF reads fw_cfg → builds its own EFI memory map → boots EFI boot manager

**Implementation**:
- In `load_firmware()` in `x86_vm.c`:
  - If `vmc_firmware == VMD_FW_OVMF`:
    - Load OVMF.fd via `gzdopen()` (already done)
    - Determine firmware entry point (from PE header or config)
    - Set up EFI memory map in guest RAM
    - Populate fw_cfg with: ACPI tables, kernel cmdline (if any), NVRAM file pointer
    - Set vCPU state: use `vcpu_init_flat64` with adjusted RIP for OVMF entry
  - Else: existing SeaBIOS path

**Files to modify:**
- `usr.sbin/vmd/x86_vm.c` — modify `load_firmware()` for OVMF path
- `usr.sbin/vmd/loadfile_elf.c` — possibly extend `loadfile_bios()` to handle PE/EFI binaries
  - OVMF.fd is a PE/COFF image (not raw binary)
  - Need to parse PE header to find entry point and sections
  - Map sections to appropriate guest memory addresses
- `usr.sbin/vmd/fw_cfg.c` — ensure fw_cfg contains correct data for OVMF:
  - `QEMU_FW_CFG_NAME` → "OVMF"
  - `QEMU_FW_CFG_UUID` → VM UUID
  - ACPI tables via `QEMU_FW_CFG_ACPI_TABLES`
  - Kernel cmdline via `QEMU_FW_CFG_KERNEL_CMDLINE` (if booting a kernel directly)

**EFI memory map layout** (for OVMF):
```
0x00000000 - 0x000A0000: EfiConventionalMemory (low RAM)
0x000A0000 - 0x000C0000: EfiReservedMemoryType (VGA BIOS area)
0x000C0000 - 0x000E0000: EfiReservedMemoryType (ACPI NVS)
0x000E0000 - 0x00100000: EfiReservedMemoryType (reserved)
0x00100000 - 0xE0000000: EfiConventionalMemory (high RAM)
0xE0000000 - 0xFF800000: EfiRuntimeServicesCode (OVMF firmware code)
0xFF800000 - 0xFFA00000: EfiRuntimeServicesData (OVMF runtime data)
0xFFA00000 - 0xFFC00000: EfiACPIMemoryNVS (ACPI NVS)
0xFFC00000 - 0xFFE00000: EfiACPIReclaimMemory (ACPI reclaim)
0xFFE00000 - 0xFFEC0000: EfiLoaderData (fw_cfg data area)
0xFFEC0000 - 0xFFF00000: EfiReservedMemoryType (reserved)
0xFFF00000 - 0xFFFE0000: EfiReservedMemoryType (reserved)
0xFFFE0000 - 0xFFFF0000: EfiReservedMemoryType (reserved)
0xFFFF0000 - 0xFFFFF000: EfiReservedMemoryType (ACPI data)
0xFFFFF000 - 0x100000000: EfiReservedMemoryType (MMIO above 4GB)
```

### 1.5 NVRAM File in VM Memory

**What**: Place the NVRAM variable store file at a known location in guest memory that OVMF can find.

**Implementation**:
- When NVRAM is initialized, map the file into the VM's guest physical memory
- Use the existing `VM_MEM_RESERVED` memory type
- Place NVRAM file at a fixed address (e.g., just below 1MB: `0x9E000` or in high memory)
- Tell OVMF about it via fw_cfg: `QEMU_FW_CFG_NVRAM` with address and size
- On VM shutdown, the file on disk is already up-to-date (it's a regular file)

### 1.6 Testing

**Manual testing checklist**:
1. Build OVMF.fd
2. Boot Linux VM with OVMF firmware — verify EFI shell works, ACPI tables present, fw_cfg accessible
3. Boot Windows 10 ISO with OVMF — verify UEFI installer starts
4. Boot Windows 11 ISO with OVMF — verify Secure Boot checks pass
5. Verify NVRAM variables persist across reboots (file-backed)
6. Verify Secure Boot: boot signed EFI binary (should pass), unsigned EFI binary (should fail in deployed mode)

**Testing files:**
- `regress/sbin/vmd/test_ovmf_boot` — test script for OVMF boot
- `regress/sbin/vmd/test_ovmf_nvram` — test NVRAM variable persistence

## Dependencies

- TianoCore EDK2 build toolchain (BaseTools, GCC x86_64-unknown-elf)
- Linux build machine for pre-building OVMF (if using Option B)
- `ovmf.fd` binary blob (64MB or 128MB)

## Risks

- **SMM complexity**: OVMF uses System Management Mode for some operations. OpenBSD vmm does not fully emulate SMRAM/SMM. `OVMF_SERIALIZE` must be enabled to avoid multi-threaded SMM.
- **PE/COFF parsing**: OVMF.fd is a PE/COFF image, not a raw binary. Need to parse PE header to extract sections and entry point.
- **Memory layout**: OVMF has specific memory map expectations. Mismatches can cause boot failure.
- **Secure Boot keys**: For testing, need to generate or obtain UEFI keys (PK, KEK, db, dbx).

## Implementation Order

1. Build OVMF (get `ovmf.fd` working)
2. Add fw_cfg data for OVMF (ACPI tables, NVRAM path)
3. Add firmware selection to vm.conf
4. Implement `ovmf_nvram.c` (NVRAM variable store)
5. Implement OVMF boot path in `load_firmware()` (PE/COFF parsing, memory map)
6. Test with Linux VM first (easier to debug)
7. Test with Windows 10 ISO
8. Test with Windows 11 ISO (Secure Boot)
