# PLAN-005-TPM: TPM 2.0 Support (Windows 11)

## Review Findings (2026-08 source audit) — CORRECTIONS TO THIS PLAN

1. **Interface (§5.2) is wrong.** Windows does not discover a vTPM via a PCI device
   with class 0x0C0330 (that's USB). The standard mechanisms are:
   - ACPI device `_HID "MSFT0101"` (TPM 2.0) plus a **memory-mapped TIS 1.3
     interface at 0xFED40000** (or the newer CRB interface), and
   - an ACPI "TPM2" table in the XSDT describing the interface type/addresses.
   No PCI function is needed; drop `tpm2_pci.c` in favor of
   `tpm2_tis.c` registered as an MMIO device via `mmio_dev_add()` like the LAPIC.
2. **Scope reduction**: implementing the full command set of §5.1 including RSA/ECC
   key generation is a multi-month crypto project. For Windows 11 installation the
   practical minimum is: GetCapability, GetRandom, PCR_Read/Event/Extend,
   NV_DefineSpace/Read/Write, CreatePrimary (with cached deterministic keys),
   Hash/HashSequenceStart+Update+Complete, SelfTest, StartAuthSession/policy subset.
   Use OpenBSD libc SHA-{1,256} and libcrypto for RSA rather than hand-rolling.
   Alternatively evaluate porting a minimal software TPM (e.g. ibmswtpm2-style)
   into a vmd privilege-separated subprocess.
3. **Windows 11 also enforces Secure Boot + UEFI + 4GB RAM + 2 vCPUs** in its
   installer checks — TPM alone is not sufficient; sequence after PLAN-001 and the
   Phase-0 platform work.
4. PCR banks (§5.3): use 24 PCRs in each of SHA-1 and SHA-256 banks (the plan's
   0-15/16-23 split description is confused; banks are per-algorithm, each 24 deep).
5. State persistence (§5.8): fine as file-backed, but must respect vmd's pledge/
   unveil model — do persistence in the parent process or a dedicated
   privilege-separated subprocess, not the pledged vm child.
6. Measured boot (§5.7): OVMF itself extends PCRs via the TIS interface once it
   detects the TPM2 table — host-side extension at load time is redundant; drop.

## Goal

Implement a virtual TPM 2.0 device (PTPD — Platform TPM for virtualization) to satisfy Windows 11's TPM requirement.

This is an explicitly deferred, long-term Windows 11 compliance milestone.
The current Windows 10 installation work does not depend on it.  A supported
Windows 11 VM requires both this vTPM and the independent Secure Boot work in
PLAN-001; neither requirement is supplied merely by running UEFI firmware.

- [ ] Select or import a maintained software TPM core rather than hand-writing
  cryptographic primitives.
- [ ] Implement the ACPI `MSFT0101` device, TPM2 table and TIS/CRB MMIO
  transport expected by Windows.
- [ ] Put command processing and persistent secrets in a suitably restricted
  subprocess/parent-owned state path, with one private state store per VM.
- [ ] Validate Windows provisioning, reboot, shutdown, snapshot policy and
  corrupt/mismatched state handling before advertising TPM support.

## Current State

- No TPM emulation exists in vmm/vmd
- ACPI does not include TPM devices (TCG ACPI tables missing)
- No PCI device for TPM
- No shared memory interface for TPM commands

## What to Build

### 5.1 TPM 2.0 Command Emulation

**What**: Emulate a TPM 2.0 device that accepts and processes TPM commands from the guest OS.

**Reference**: TPM 2.0 Library Specification (TCG spec), PTPD (TPM for virtualization) specification

**Minimum command set for Windows 11**:
```c
// Commands Windows requires for installation and basic operation:
TPM2_CC_NV_Define_Space      — Define NV indices
TPM2_CC_NV_Undefine_Space    — Undefine NV indices
TPM2_CC_NV_Read              — Read NV data
TPM2_CC_NV_Write             — Write NV data
TPM2_CC_ActivateIdentity     — Activate identity (for Windows Hello)
TPM2_CC_Certify              — Certify PCR values
TPM2_CC_CertifyX509          — Certify using X.509 certificate
TPM2_CC_Create               — Create a key
TPM2_CC_CreatePrimaryKey     — Create a primary key (RSA/ECC)
TPM2_CC_ECC_Parameters       — Get ECC parameters
TPM2_CC_GetCapability        — Get capability (supported features)
TPM2_CC_GetRandom            — Get random bytes
TPM2_CC_Hash                 — Hash data
TPM2_CC_HMAC                 — HMAC operation
TPM2_CC_NV_IsWritten         — Check if NV index is written
TPM2_CC_PolicyGetDigest      — Get policy digest
TPM2_CC_PolicyRestart        — Restart a policy
TPM2_CC_PCR_Event            — Extend PCR with event
TPM2_CC_PCR_Read             — Read PCR values
TPM2_CC_PCR_Reset            — Reset PCR values
TPM2_CC_PCR_SetAuthValue     — Set PCR auth value
TPM2_CC_PolicyAuthorize      — Authorize using policy
TPM2_CC_PolicyCommandCode    — Policy command code
TPM2_CC_PolicyCpHash         — Policy CP hash
TPM2_CC_PolicyLocality       — Policy locality
TPM2_CC_PolicyNV_Read        — Policy NV read
TPM2_CC_PolicyOR             — Policy OR
TPM2_CC_PolicyPassword       — Policy password
TPM2_CC_PolicyRestart        — Policy restart
TPM2_CC_PolicySecret         — Policy secret
TPM2_CC_PolicyTicket         — Policy ticket
TPM2_CC_PolicyTemplate       — Policy template
TPM2_CC_PolicyTicket         — Policy ticket
TPM2_CC_RSA_Decrypt          — RSA decrypt
TPM2_CC_RSA_Encrypt          — RSA encrypt
TPM2_CC_RSA_PublicKey        — Generate RSA key pair
TPM2_CC_SelfTest             — Self-test TPM
TPM2_CC_SequenceComplete       — Complete a hash sequence
TPM2_CC_SequenceUpdate       — Update a hash sequence
TPM2_CC_StirRandom           — Stir in random data
TPM2_CC_Vendor_TCG_Test      — Vendor test (required for some checks)
```

**Implementation approach**:
- Use the open-source tpm2-tss (Trusted Platform Module 2.0 Software Stack) or implement a subset directly
- **Option A**: Integrate tpm2-tss (complex, heavy dependency)
- **Option B**: Implement a minimal TPM 2.0 emulator (recommended for first pass)
  - Implement TPM 2.0 command parser/interpreter
  - Support RSA-2048 key generation (required for Windows)
  - Support PCR banks: SHA-1 (PCR 0-15) and SHA-256 (PCR 16-23)
  - Support NV storage
  - Support HMAC and hashing
  - Support self-test

**Pseudocode for command dispatch**:
```c
int tpm2_command_handler(struct tpm2_cmd *cmd, struct tpm2_resp *resp) {
    switch (cmd->header.command_code) {
    case TPM2_CC_GetCapability:
        return tpm2_get_capability(cmd, resp);
    case TPM2_CC_GetRandom:
        return tpm2_get_random(cmd, resp);
    case TPM2_CC_CreatePrimaryKey:
        return tpm2_create_primary_key(cmd, resp);
    case TPM2_CC_PCR_Read:
        return tpm2_pcr_read(cmd, resp);
    case TPM2_CC_PCR_Event:
        return tpm2_pcr_event(cmd, resp);
    case TPM2_CC_PCR_Reset:
        return tpm2_pcr_reset(cmd, resp);
    // ... more cases
    default:
        resp->header.return_code = TPM2_RC_COMMAND_CODE;
        return 0;
    }
}
```

**Files to create:**
- `usr.sbin/vmd/tpm2.c` — TPM 2.0 command emulation
  - `tpm2_init()` — initialize TPM state
  - `tpm2_command_handler()` — command dispatch
  - `tpm2_get_capability()`
  - `tpm2_get_random()`
  - `tpm2_create_primary_key()`
  - `tpm2_pcr_read()`
  - `tpm2_pcr_event()`
  - `tpm2_pcr_reset()`
  - `tpm2_hash()`
  - `tpm2_hmac()`
  - `tpm2_nv_read()`
  - `tpm2_nv_write()`
  - `tpm2_rsa_encrypt()`
  - `tpm2_rsa_decrypt()`
  - `tpm2_rsa_keygen()`
  - `tpm2_ecc_keygen()`
  - `tpm2_certify()`
  - `tpm2_certify_x509()`
  - `tpm2_activate_identity()`
  - `tpm2_stir_random()`
  - `tpm2_self_test()`
  - `tpm2_sequence_update()`
  - `tpm2_sequence_complete()`
- `usr.sbin/vmd/tpm2.h` — header with types, constants, function declarations

### 5.2 PTPD Interface (PCI)

**What**: Provide a PCI interface that Windows uses to communicate with the virtual TPM.

**Reference**: PTPD specification, Microsoft PTPD documentation

**PCI Device**:
- Vendor ID: `0x1AF4` (Virtio) or `0x8086` (Intel)
- Device ID: `0x2400` (Intel PTT) or `0x1051` (VirtIO TPM)
- Class: `0x0C0330` (USB controller — PTPD is implemented as a USB device over PCI)
  - Actually, PTPD uses a PCI interface with a specific class code

**Alternative**: Use the **TPM TIS (TCG Interface Specification)** interface:
- PCI device class: `0x0C0330` (USB controller) or `0x0C0500` (Smart Card)
- Or use the **TPM 2.0 Device Interface** (TTX interface) via PCI MMIO

**Implementation choice**: Use the **Virtual TCG (vTPM) PCI interface**:
- PCI device class code: `0x0C0330` (USB controller) — Windows expects this
- Vendor ID: `0x1AF4` (Virtio)
- Device ID: `0x1051` (VirtIO TPM 2.0)
- MMIO BAR: TPM command/status register block
- MSI-X interrupts for command completion

**Registers**:
```c
#define TPM2_REG_BASE       0x0000
#define TPM2_REG_CAPABILITY 0x0000
#define TPM2_REG_DATA       0x0008
#define TPM2_REG_STATUS     0x0010
#define TPM2_REG_INT_STATUS 0x0018
#define TPM2_REG_INT_ENABLE 0x0020
#define TPM2_REG_ENABLE     0x0028
#define TPM2_REG_DISABLE    0x0030

#define TPM2_STATUS_READY       (1 << 0)
#define TPM2_STATUS_VALID       (1 << 1)
#define TPM2_STATUS_NEEDS_COMMAND (1 << 3)
#define TPM2_STATUS_COMMAND_SUCCESS (1 << 4)
```

**Files to create:**
- `usr.sbin/vmd/tpm2_pci.c` — TPM PCI interface (PTPD/vTPM)
  - `tpm2_pci_init()` — initialize PCI device
  - `tpm2_pci_mmio_read()` — MMIO read handler
  - `tpm2_pci_mmio_write()` — MMIO write handler
  - `tpm2_pci_interrupt_handler()` — interrupt handling

### 5.3 PCR (Platform Configuration Register) Implementation

**What**: Implement PCR storage with hash accumulation.

**Data structure**:
```c
#define TPM2_PCR_COUNT_SHA1   24  // PCR 0-23
#define TPM2_PCR_COUNT_SHA256 24  // PCR 0-23

struct tpm2_pcr {
    uint8_t sha1[TPM2_PCR_COUNT_SHA1][20];  // SHA-1 PCR bank
    uint8_t sha256[TPM2_PCR_COUNT_SHA256][32]; // SHA-256 PCR bank
};

void tpm2_pcr_extend(struct tpm2_pcr *pcrs, int bank, int index, const uint8_t *data, int len) {
    // PCR_new = hash(PCR_old || data)
    // For SHA-256:
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, pcrs->sha256[bank][index], 32);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(pcrs->sha256[bank][index], &ctx);
}
```

**Initial PCR state**: All zeros (as per TPM spec)

**Files to modify:**
- `usr.sbin/vmd/tpm2.c` — add PCR storage and extend functions

### 5.4 NV Storage

**What**: Implement NV RAM storage for TPM-persistent data (keys, certificates, etc.).

**Implementation**:
- Fixed NV index range (0x00010000 - 0x0001FFFF)
- Each index has: attributes, size, data
- Data persisted to file on the host (`/var/vm/<vm>/tpm2_nv.dat`)

**Files to modify:**
- `usr.sbin/vmd/tpm2.c` — add NV storage functions

### 5.5 RSA Key Generation

**What**: Windows requires RSA key generation (2048-bit) for BitLocker, Windows Hello, and device attestation.

**Implementation**:
- Implement RSA key generation (2048-bit, 4096-bit optional)
- Support RSA-PKCS1 padding
- Support RSA-OAEP padding (for BitLocker)
- Support RSA-SHA256 signing

**Files to modify:**
- `usr.sbin/vmd/tpm2.c` — add RSA key generation and crypto functions

### 5.6 ACPI TPM Devices

**What**: Add TPM devices to ACPI tables so Windows discovers the TPM.

**Required ACPI tables**:
1. **TPM2 table** (signature "TPM2"):
   ```c
   struct acpi_tpm2 {
       uint8_t signature[4];     // "TPM2"
       uint32_t length;
       uint8_t revision;
       uint8_t checksum;
       uint8_t oemid[6];
       uint8_t oem_tableid[8];
       uint32_t oem_revision;
       uint32_t creator_id;
       uint32_t creator_revision;
       uint8_t interface_type;   // 2 = MMIO
       uint16_t reserved;
       uint64_t tpm_start_address; // MMIO base address
       uint32_t tpm_control_area_length;
       uint64_t region_physical_address;
       uint64_t region_virtual_address;
       uint32_t region_size;
       uint8_t flags;
       uint32_t vendor_info_size;
       uint8_t vendor_info[];    // "OpenBSD vmm TPM"
   };
   ```

2. **TCG ACPI Table** (signature "TGCA"):
   - TCG Platform Specification version
   - Firmware features

**Implementation**:
- In `acpi.c`, add `acpi_create_tpm2()` function
- Place TPM2 table in XSDT
- Add TPM device to DSDT:
  ```asl
  Device (TPM) {
      Name (_HID, "ACPI000E")  // TPM 2.0 device
      Name (_CID, "PNP0C31")   // ACPI TPM
      Name (_UID, 1)
      Name (_STA, 0x0F)
      Method (_CRS, 0, Serialized) {
          Return (ResourceTemplate() {
              Memory32Fixed(ReadWrite, TPM_MMIO_BASE, TPM_MMIO_SIZE)
          })
      }
  }
  ```

**Files to modify:**
- `usr.sbin/vmd/acpi.c` — add TPM2 table, TPM device to DSDT
- `usr.sbin/vmd/acpi.h` — add function declarations

### 5.7 PCR Extension from Firmware

**What**: Extend PCRs with firmware hash during boot for measured boot.

**Implementation**:
- On firmware load (SeaBIOS or OVMF), compute hash of firmware image
- Extend PCR 0-7 with firmware hash
- This requires calling into the TPM2 emulation from the boot path

**Implementation approach**:
- Add a function in `x86_vm.c` that is called after firmware is loaded:
  ```c
  void vm_extend_pcr_on_boot(struct vmd_vm *vm, uint8_t *firmware, size_t len) {
      // Hash firmware image
      // Extend PCR 0-7 with hash
      tpm2_pcr_event(&vm->tpm, TPM2_ALG_SHA256, 0, firmware, len);
      tpm2_pcr_event(&vm->tpm, TPM2_ALG_SHA256, 1, firmware, len);
      // ...
  }
  ```

**Files to modify:**
- `usr.sbin/vmd/x86_vm.c` — call PCR extension after firmware load

### 5.8 TPM State Persistence

**What**: Persist TPM state across VM reboots for BitLocker recovery.

**Implementation**:
- Serialize TPM state (PCR values, NV storage, RSA keys) to a file
- File: `/var/vm/<vm>/tpm_state.bin`
- On VM start: load state from file
- On VM stop: save state to file
- Provide a "clear" option to reset TPM (for fresh Windows installations)

**Files to create:**
- `usr.sbin/vmd/tpm2_state.c` — TPM state serialization/deserialization
- `usr.sbin/vmd/tpm2_state.h` — header

## Dependencies

- SHA-256 implementation (OpenBSD libc has `sha256.h`, `shavar.h`)
- SHA-1 implementation (OpenBSD libc has `sha1.h`)
- RSA implementation (OpenBSD has `evp.h` with RSA support, or `libcrypto`)
- RNG (OpenBSD's `arc4random()` or `/dev/urandom`)

## Risks

- **TPM complexity**: Full TPM 2.0 spec is very large. Start with the minimal command set and add more as needed.
- **Security implications**: A buggy TPM emulator could allow guest OS to bypass TPM checks. Ensure proper isolation of TPM state between VMs.
- **Performance**: RSA operations can be slow. Use OpenBSD's `libcrypto` for efficient crypto.

## Implementation Order

1. Implement TPM 2.0 command parser and basic commands (GetCapability, GetRandom, PCR read/write)
2. Implement PCR storage (SHA-1 and SHA-256 banks)
3. Implement PCR extension
4. Add PCI interface (PTPD/vTPM)
5. Add TPM2 ACPI table and TPM device to DSDT
6. Implement NV storage
7. Implement RSA key generation (2048-bit)
8. Implement remaining commands (Certify, CreatePrimaryKey, etc.)
9. Implement TPM state persistence
10. Implement PCR extension on firmware boot
11. Test with Windows 11 installer (TPM requirement check)
12. Test with Windows 11 running (BitLocker, Windows Hello)
