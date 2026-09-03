# PLAN-007-MGMT: Management and Tooling

## Review Findings (2026-08 source audit) — CORRECTIONS TO THIS PLAN

1. **Guest detection (§7.3) cannot work as written**: it inspects CPUID/register
   state before boot, but the hypervisor signature is what *we* return, not the
   guest's. Pre-boot guest OS detection is not feasible; either drop it or detect
   via DHCP client fingerprints (vmd's dhcp.c already sees client requests) and
   document manual configuration as the primary path. Auto-flipping
   firmware/secure-boot/tpm silently is also bad UX — prefer validation warnings.
2. vm.conf example uses `disk "hd0" "...vhdx"` but **vhdx is unsupported** (raw +
   qcow2 only, `vioblk.c:55`, `parse.y:1407`); use qcow2 and document
   `qemu-img convert` for existing vhdx images.
3. Driver bundling (§7.4): redistributing Fedora virtio-win binaries in the
   OpenBSD tree raises license/redistribution questions; better to document an
   official download URL and optionally provide an ISO-builder script.
4. Config validation (§7.2) should be warnings, not hard failures, and must live
   in `config.c`/`parse.y` (no separate config_validate.c needed — the parser
   already does semantic checks there).
5. `firmware "ovmf"` parsing (§7.1) is prerequisite plumbing for PLAN-001 and
   should be implemented early, not in Phase 7.
6. A graphical console must not default to an unauthenticated TCP listener.
   Configure a per-VM Unix-domain display socket, mode 0600, and isolate RFB in
   a worker which has neither `/dev/vmm` nor access to arbitrary guest memory.

## Goal

Add Windows-specific configuration options, validation, guest detection, and driver package integration to make vmm/vmd easy to use for Windows VMs.

## Current State

- `vm.conf(5)` supports memory, cpus, boot, disk, cdrom, network, local
  interface, owner, `firmware bios|uefi`, `efivars`, and vmmci
- `vmctl(8)` manages VM lifecycle (start, stop, pause, resume, reboot, info)
- `vmd(8)` parses config, manages VMs
- UEFI firmware and persistent/ephemeral variable stores are implemented
- No graphical display endpoint is implemented
- No Windows-specific configuration options
- No guest OS detection
- No configuration validation for Windows compatibility

## What to Build

### 7.1 Windows-Specific vm.conf Options

**What**: Add configuration directives that optimize VM setup for Windows.

**New vm.conf options**:

```
vm "windows11" {
    // Existing options...
    memory 8192
    cpus 4
    disk "/path/to/disk.qcow2"
    
    // New options...
    firmware uefi                // Use OVMF instead of SeaBIOS
    secure-boot on               // Enable Secure Boot (required for Win11)
    tpm on                       // Enable virtual TPM 2.0
    virtio                       // Prefer virtio devices (default)
    acpi-profile "windows"       // Use Windows-optimized ACPI tables
    balloon on                   // Enable memory balloon device
}
```

**Implementation**:

1. Add fields to `struct vmop_create_params` in `vmd.h`:
   ```c
   // In struct vmop_create_params:
   unsigned int vmc_firmware;
   #define VMD_FW_SEABIOS  0
   #define VMD_FW_OVMF     1
   
   unsigned int vmc_secure_boot;
   #define VMD_SECURE_BOOT_OFF  0
   #define VMD_SECURE_BOOT_ON   1
   
   unsigned int vmc_tpm;
   #define VMD_TPM_OFF  0
   #define VMD_TPM_ON   1
   
   unsigned int vmc_balloon;
   #define VMD_BALLOON_OFF  0
   #define VMD_BALLOON_ON   1
   
   unsigned int vmc_acpi_profile;
   #define VMD_ACPI_DEFAULT  0
   #define VMD_ACPI_WINDOWS  1
   
   unsigned int vmc_virtio_prefer;
   #define VMD_VIRTIO_OFF   0
   #define VMD_VIRTIO_ON    1
   ```

2. Parse new options in `parse.y`:
   ```yacc
   firmware: FIRMWARE string {
       if (strcmp($2, "ovmf") == 0)
           vmc->vmc_firmware = VMD_FW_OVMF;
       else if (strcmp($2, "seabios") == 0)
           vmc->vmc_firmware = VMD_FW_SEABIOS;
       else
           warnx("unknown firmware type: %s", $2);
       free($2);
   }
   
   secure_boot: SECURE_BOOT BOOL {
       vmc->vmc_secure_boot = $3 ? VMD_SECURE_BOOT_ON : VMD_SECURE_BOOT_OFF;
   }
   
   tpm: TPM BOOL {
       vmc->vmc_tpm = $3 ? VMD_TPM_ON : VMD_TPM_OFF;
   }
   
   balloon: BALLOON BOOL {
       vmc->vmc_balloon = $3 ? VMD_BALLOON_ON : VMD_BALLOON_OFF;
   }
   
   acpi_profile: ACPI_PROFILE string {
       if (strcmp($2, "windows") == 0)
           vmc->vmc_acpi_profile = VMD_ACPI_WINDOWS;
       else if (strcmp($2, "default") == 0)
           vmc->vmc_acpi_profile = VMD_ACPI_DEFAULT;
       else
           warnx("unknown ACPI profile: %s", $2);
       free($2);
   }
   
   virtio: VIRTIO BOOL {
       vmc->vmc_virtio_prefer = $3 ? VMD_VIRTIO_ON : VMD_VIRTIO_OFF;
   }
   ```

3. Add keywords to `parse.y` tokens:
   ```yacc
   %token FIRMWARE SECURE_BOOT TPM BALLOON ACPI_PROFILE VIRTIO
   ```

**Files to modify:**
- `usr.sbin/vmd/vmd.h` — add new fields to `struct vmop_create_params`
- `usr.sbin/vmd/parse.y` — add grammar rules, keywords, token definitions
- `usr.sbin/vmd/config.c` — validate new options, pass through to VM

### 7.1A Display Endpoint Configuration

The proposed display configuration is deliberately small:

```
vm "windows11" {
    owner build
    firmware uefi
    display {
        socket "/var/run/vmd/windows11.vnc"
    }
}
```

`display` enables the graphical console.  Its optional `socket` entry overrides
the RFB Unix-domain socket path.  If omitted, vmd derives a unique path beneath
`/var/run/vmd` from the VM/instance identity.  A caller can connect locally with
`vncviewer /var/run/vmd/windows11.vnc`, or remotely without opening a host TCP
port by forwarding a local TCP port to the Unix socket with SSH.

The socket rules are part of the interface, not implementation details:

- the path must be absolute and fit within `sockaddr_un.sun_path`;
- the final socket has mode 0600;
- if the VM stanza explicitly specifies `owner`, the socket UID is that owner;
  if it does not, the UID is root;
- a VM started manually by `vmctl start` always receives a root-owned display
  socket, even if its lifecycle ownership internally records the vmctl caller;
- vmd validates the parent directory and rejects a symlink or an existing
  non-socket at the final path;
- vmd must not blindly unlink the configured name.  It records the device and
  inode of a socket it successfully bound and unlinks only that same object at
  VM teardown;
- two configured VMs/instances may not resolve to the same socket path.

The privileged/configuration side creates and owns the listener before passing
only the required fd to the display worker.  The worker serves normal RFB over
`AF_UNIX`, handles one client initially, and re-pledges after receiving its fds.
It has no `inet`, filesystem, `/dev/vmm`, disk-image or guest-wide-memory access.
Input is returned to the VM process as bounded typed keyboard/pointer messages.

Direct TCP support is deferred.  If added, the default bind is loopback-only and
a tiny `stdio inet sendfd` listener passes established connections to the same
RFB worker.  Non-loopback service requires an authenticated encrypted design;
legacy VNC password authentication is not sufficient.

### 7.2 VM Configuration Validation

**What**: Validate VM configuration for Windows compatibility before starting.

**Validation rules**:
```c
int validate_windows_vm(struct vmop_create_params *vmc) {
    int errors = 0;
    
    // Windows 11 requires OVMF
    if (vmc->vmc_firmware != VMD_FW_OVMF) {
        warnx("Windows 11 requires firmware \"ovmf\"");
        errors++;
    }
    
    // Windows 11 requires at least 2 vCPUs
    if (vmc->vmc_ncpus < 2) {
        warnx("Windows requires at least 2 vCPUs (has %zu)", vmc->vmc_ncpus);
        errors++;
    }
    
    // Windows 11 requires at least 4GB RAM
    if (vmc->vmc_memranges[0].vmr_size < 4 * GB(1)) {
        warnx("Windows requires at least 4GB RAM (has %zu)", 
              vmc->vmc_memranges[0].vmr_size);
        errors++;
    }
    
    // Windows 11 requires TPM
    if (vmc->vmc_tpm != VMD_TPM_ON) {
        warnx("Windows 11 requires tpm on");
        errors++;
    }
    
    // Windows 11 requires Secure Boot
    if (vmc->vmc_secure_boot != VMD_SECURE_BOOT_ON) {
        warnx("Windows 11 requires secure-boot on");
        errors++;
    }
    
    // Disk image must exist
    if (access(vmc->vmc_disks[0], R_OK) != 0) {
        warnx("disk image does not exist: %s", vmc->vmc_disks[0]);
        errors++;
    }
    
    return errors;
}
```

**Usage**: Call this in `config.c` after parsing, before creating the VM. If validation fails, skip the VM or warn but still allow start (for flexibility).

**Files to create:**
- `usr.sbin/vmd/config_validate.c` — validation functions
  - `validate_windows_vm()` — Windows-specific validation
  - `validate_vm()` — general VM validation
  - `validate_firmware()` — validate firmware type
  - `validate_memory()` — validate memory settings
  - `validate_disks()` — validate disk images
  - `validate_network()` — validate network configuration
- `usr.sbin/vmd/config_validate.h` — header

### 7.3 Guest OS Detection

**What**: Automatically detect guest OS and enable Windows-specific features.

**Detection methods**:
1. **CPUID signature**: Check for Microsoft CPUID leaf at `0x40000001` (already set in PLAN-002)
2. **ACPI table signatures**: Check for Windows-specific ACPI tables (e.g., `MSFT` in OEM table ID)
3. **Bootloader identification**: Detect Windows bootloader from boot signature
4. **DHCP client FQDN**: Check for Windows-style hostnames

**Implementation**:
- In `x86_vm.c`, after boot, check CPUID for Hyper-V signature
- If Hyper-V signature detected:
  - Enable Windows-optimized ACPI tables (if not already set)
  - Enable TPM (if not already set)
  - Enable balloon (if not already set)
  - Log: "Detected Windows guest — enabling Windows-optimized configuration"

**Detection code**:
```c
int detect_guest_os(struct vmd_vm *vm) {
    // Check CPUID for Hyper-V signature
    struct vcpu_reg_state vrs;
    vcpu_readregs(vm->vm_vmmid, 0, &vrs);
    
    if (vrs.vrs_gprs[VCPU_REGS_RAX] == 0x40000000 &&
        vrs.vrs_gprs[VCPU_REGS_RBX] == 'Msft') {
        log_debug("Detected Hyper-V guest (Windows)");
        return GUEST_WINDOWS;
    }
    
    // Check for Linux via CPUID
    if (vrs.vrs_gprs[VCPU_REGS_RAX] == 0x40000000 &&
        vrs.vrs_gprs[VCPU_REGS_RBX] == 'Open') {
        log_debug("Detected OpenBSD/KVM guest");
        return GUEST_OPENBSD;
    }
    
    // Check for other guests...
    return GUEST_UNKNOWN;
}
```

**Auto-enable Windows features**:
```c
void auto_enable_windows_features(struct vmd_vm *vm) {
    if (vm->vmc_firmware != VMD_FW_OVMF)
        vm->vmc_firmware = VMD_FW_OVMF;
    if (vm->vmc_secure_boot != VMD_SECURE_BOOT_ON)
        vm->vmc_secure_boot = VMD_SECURE_BOOT_ON;
    if (vm->vmc_tpm != VMD_TPM_ON)
        vm->vmc_tpm = VMD_TPM_ON;
    if (vm->vmc_balloon != VMD_BALLOON_ON)
        vm->vmc_balloon = VMD_BALLOON_ON;
    if (vm->vmc_acpi_profile != VMD_ACPI_WINDOWS)
        vm->vmc_acpi_profile = VMD_ACPI_WINDOWS;
}
```

**Files to create:**
- `usr.sbin/vmd/guest_detect.c` — guest OS detection
  - `detect_guest_os()` — main detection function
  - `detect_windows()` — detect Windows guest
  - `detect_linux()` — detect Linux guest
  - `detect_openbsd()` — detect OpenBSD guest
  - `auto_enable_features()` — enable features based on detected guest
- `usr.sbin/vmd/guest_detect.h` — header

### 7.4 VirtIO Driver Package

**What**: Include Windows VirtIO drivers in the source tree for easy distribution.

**Contents**:
```
usr.sbin/vmd/virtio-drivers/
├── viostor/
│   ├── viostor.inf      — INF file for VirtIO storage driver
│   ├── viostor.cat      — Catalog file (optional)
│   ├── viostor.sys      — Driver binary
│   └── README.md        — Installation instructions
├── netkvm/
│   ├── netkvm.inf       — INF file for VirtIO network driver
│   ├── netkvm.cat       — Catalog file
│   ├── netkvm.sys       — Driver binary
│   └── README.md
├── Balloon/
│   ├── Balloon.inf      — INF file for VirtIO balloon driver
│   ├── Balloon.cat
│   ├── Balloon.sys
│   └── README.md
├── viogpu/
│   ├── viogpu.inf       — INF file for VirtIO GPU driver
│   ├── viogpu.cat
│   ├── viogpu.sys
│   └── README.md
├── viorng/
│   ├── viorng.inf       — INF file for VirtIO RNG driver
│   ├── viorng.cat
│   ├── viorng.sys
│   └── README.md
└── README.md              — Overall driver package documentation
```

**Sources**:
- Windows VirtIO drivers are available from [Fedora's virtio-win project](https://fedorahosted.org/virtio-win/)
- These can be included as pre-built binaries or built from source
- **Option A**: Include pre-built binaries from Fedora (easier)
- **Option B**: Build from source (requires Windows build environment)

**Distribution**:
- Drivers are bundled with OpenBSD when building vmd
- Can be installed via `pkg_add` or manually copied to guest
- Documentation included

**Files to create:**
- `usr.sbin/vmd/virtio-drivers/` — driver package directory
- `usr.sbin/vmd/virtio-drivers/README.md` — documentation
- `usr.sbin/vmd/virtio-drivers/viostor/` — storage driver
- `usr.sbin/vmd/virtio-drivers/netkvm/` — network driver
- `usr.sbin/vmd/virtio-drivers/Balloon/` — balloon driver
- `usr.sbin/vmd/virtio-drivers/viogpu/` — GPU driver
- `usr.sbin/vmd/virtio-drivers/viorng/` — RNG driver
- `usr.sbin/vmd/virtio-drivers/vioscsi/` — SCSI driver (if not using viostor)

### 7.5 vm.conf(5) Documentation

**What**: Update the man page to document all new options.

**Sections to add**:
- `Windows Configuration` — describe Windows-specific options
- `Secure Boot` — describe Secure Boot configuration
- `TPM 2.0` — describe virtual TPM configuration
- `VirtIO Drivers` — describe how to install Windows VirtIO drivers
- `Example` — add a complete Windows 11 example configuration

**Example configuration**:
```
vm "windows11" {
    # Required for Windows 11
    firmware uefi
    secure-boot on
    tpm on
    
    # Performance
    memory 8192
    cpus 4
    balloon on
    
    # Storage
    disk "/var/vm/windows11/disk.qcow2"
    
    # Network
    interface tap0 switch mybridge
    
    # Optional: install drivers from CD
    cdrom "/usr/share/vmm/virtio-drivers.iso"
}
```

**Files to modify:**
- `usr.sbin/vm.conf.5` — update man page

## Dependencies

- Windows VirtIO drivers from Fedora (binary blobs or source)
- Existing vm.conf parsing infrastructure

## Risks

- **Driver licensing**: Windows drivers may have specific licensing requirements. Ensure compliance with Fedora's virtio-win project license.
- **Configuration complexity**: Too many options can overwhelm users. Provide sensible defaults.

## Implementation Order

1. Add new vm.conf options (firmware, secure-boot, tpm, balloon, acpi-profile)
2. Implement configuration validation
3. Implement guest OS detection
4. Add virtio driver package (download from Fedora)
5. Update vm.conf(5) man page
6. Test with Windows VM (verify auto-detection works, drivers install correctly)
