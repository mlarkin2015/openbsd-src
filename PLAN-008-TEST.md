# PLAN-008-TEST: Testing and Verification

## Review Findings (2026-08 source audit) — CORRECTIONS TO THIS PLAN

1. Existing regress assets to build on (not mentioned in the plan):
   `regress/sys/arch/amd64/vmm/` (kernel vcpu tests via /dev/vmm),
   `regress/sys/arch/amd64/vmmcall/`, `vmcall/`, `seves_mmio/`,
   `regress/usr.sbin/vmd/config/` (parser tests) and `diskfmt/`.
   Hyper-V CPUID/MSR tests belong in the kernel-side vmm regress dir where they
   can use /dev/vmm directly — no SSH-into-guest needed.
2. The shell-test sketches assume SSH into running guests and external tools
   (`acpidump`, `tpm2-tools`, `cpuid`) that don't exist on a bare OpenBSD host;
   either vendor minimal tools, run validation in a helper guest, or check tables
   directly by reading them out of guest memory via /dev/mem of a test process.
3. ACPI validation is better done offline: dump tables from a booted Linux test
   VM once, then diff against golden files in regress — deterministic and CI-safe.
4. Windows end-to-end tests (§8.6) must be marked as requiring licensed media and
   skipped by default (`regress` runs on machines without ISOs).
5. Add early unit-style tests for pure components as they land: fw_cfg file-dir
   contents, SMBIOS table checksums, NVRAM store read/write/compact, TPM PCR
   extend math — all testable without booting a VM.

## Goal

Create comprehensive test infrastructure to verify Windows VM compatibility and correctness of all implemented features.

## Current State

- `regress/` directory exists with regression tests for lib/, sbin/, usr.bin/, usr.sbin/
- Existing focused vmd tests under `regress/usr.sbin/vmd/` cover configuration,
  disk formats, LAPIC ICR/reset behavior, IOAPIC logical/lowest-priority
  routing, x86 MMIO decoding, and non-PAE/PAE32 guest page-table translation.
- Existing vmm tests under `regress/sys/` remain limited.
- Manual SMP validation boots OpenBSD GENERIC.MP guests with two, four and
  eight vCPUs.  Four/eight-vCPU fsck/mountroot testing exposed and validated
  the fix for a lost `VMM_IOC_INTR` assertion at `VMM_IOC_RUN` entry.  Longer
  stress and an Intel VMX field run remain manual test items.  FreeBSD
  15.1-p3 now completes a two-vCPU x2APIC/x2AVIC boot, login and clean
  shutdown after correcting the LAPIC version and unaccelerated-EOI backing
  state.  It also runs with eight vCPUs under varied guest-to-host iperf3
  loads, reaching an observed peak of 27.2 Gbit/s.  Four-vCPU Linux and
  OpenBSD smoke tests pass with the same kernel, and an eight-vCPU Linux guest
  passed package-update stress and reached 17.7 Gbit/s guest-to-host.  A
  four-vCPU-configured OpenBSD VM running a GENERIC uniprocessor kernel no
  longer spins its firmware APs.  OpenBSD SMP reboot, Ubuntu 24 SMP reboot and
  Ubuntu halt/poweroff pass after VM-wide stop/reset coordination.  Repeated
  reset loops, pause/unpause and Intel VMX remain manual items.
- OpenBSD 7.9/i386 now boots with an IOAPIC/APIC clock, mounts root, obtains
  DHCP, passes SSH/ping checks and powers off cleanly.  Gentoo/i686 6.12 boots
  its Btrfs root with virtio MSI-X after a `pci=nomsi` control run isolated and
  validated the xAPIC logical-MSI fix.  CentOS 7/i386 detects its virtio disk
  but remains a legacy PCI INTx/ACPI `_PRT` routing test case.
- NetBSD 11/amd64 boots with one and two vCPUs.  Its modern virtio-net driver
  obtains DHCP, transmits and receives valid Ethernet frames, advances its
  MSI-X queue interrupt counter without vioif errors, passes a bridged gateway
  ping and powers off cleanly.  This covers independent VirtIO 1.x split-ring
  mappings and queue reinitialization after a device reset.
- The NetBSD 11/i386 installer reaches sysinst with one and two vCPUs after
  correcting protected-mode REP OUTS segment setup and page translation.  A
  two-vCPU run also passed DHCP, MSI-X network interrupts, bridged ICMP and a
  clean S5 shutdown.
- NetBSD 11/i386 probes and reads a virtio-scsi CD-ROM without corruption.
  Full 341,522,432-byte ISO reads using both 2 KiB and 64 KiB request sizes
  matched the host SHA-256
  `ab4de566f077ab6e780b24bc399898e9616ed2b8acb2e75e26acb13eacb669f3`.
  NetBSD CD-ROM operation was also independently confirmed.
- No Windows-specific tests
- No automated VM boot testing (all manual)
- No guest-side test infrastructure

## What to Build

### 8.1 Regression Test Infrastructure

**What**: Extend the existing regress framework to test vmm/vmd functionality.

**Test structure**:
```
regress/sbin/vmd/
├── Makefile               — test runner
├── test_ovmf_boot         — OVMF boot test
├── test_ovmf_nvram        — NVRAM variable persistence test
├── test_hyperv_cpuid      — Hyper-V CPUID leaf test
├── test_hyperv_msr        — Hyper-V MSR test
├── test_acpi_tables       — ACPI table validation test
├── test_virtio_devices    — VirtIO device enumeration test
├── test_tpm_pcr           — TPM PCR test
├── test_balloon           — Memory balloon test
├── test_migration         — Live migration test
└── Makefile.inc            — shared definitions
```

**Test format**: Shell scripts that:
1. Create a VM configuration
2. Start the VM
3. Send commands to vmd via `vmctl`
4. Check for expected output in logs
5. Stop the VM
6. Assert expected results

**Example test**:
```sh
#!/bin/sh
# test_ovmf_boot — Test OVMF firmware boot

. $TESTLIB

# Setup
vmctl start -c vm.conf test_ovmf
sleep 5

# Check that OVMF boot message is in log
if vmctl info test_ovmf | grep -q "OVMF"; then
    echo "PASS: OVMF boot detected"
else
    echo "FAIL: OVMF boot not detected"
    exit 1
fi

# Teardown
vmctl stop -f test_ovmf
```

**Files to create:**
- `regress/sbin/vmd/Makefile` — test runner
- `regress/sbin/vmd/test_ovmf_boot` — OVMF boot test
- `regress/sbin/vmd/test_ovmf_nvram` — NVRAM test
- `regress/sbin/vmd/test_hyperv_cpuid` — Hyper-V CPUID test
- `regress/sbin/vmd/test_hyperv_msr` — Hyper-V MSR test
- `regress/sbin/vmd/test_acpi_tables` — ACPI table test
- `regress/sbin/vmd/test_virtio_devices` — VirtIO device test
- `regress/sbin/vmd/test_tpm_pcr` — TPM PCR test
- `regress/sbin/vmd/test_balloon` — Balloon test
- `regress/sbin/vmd/test_migration` — Migration test

### 8.2 ACPI Table Validation

**What**: Validate ACPI tables for correctness and Windows compatibility.

**Tool**: Use `acpidt` from Linux or the `acpi` tool from FreeBSD/OpenBSD to check tables.

**Test**:
1. Start a Linux VM (easier to debug)
2. Extract ACPI tables from the guest (`acpidump`)
3. Compare with expected tables
4. Validate with `acpi-diff` or `iasl` (Intel ACPI compiler)

**Implementation**:
```sh
#!/bin/sh
# test_acpi_tables — Validate ACPI tables

. $TESTLIB

# Start Linux VM
vmctl start -c vm.conf test_acpi_linux
sleep 10

# Extract ACPI tables from guest
ssh root@localhost "acpidump > /tmp/acpidump.dat"

# Validate tables
iasl -d /tmp/acpidump.dat
if [ $? -eq 0 ]; then
    echo "PASS: ACPI tables compile without errors"
else
    echo "FAIL: ACPI tables have errors"
    exit 1
fi

# Check for required tables
for table in RSDP XSDT MADT FADT HPET MCFG; do
    if strings /tmp/acpidump.dat | grep -q "$table"; then
        echo "PASS: $table table present"
    else
        echo "FAIL: $table table missing"
        exit 1
    fi
done

# Teardown
vmctl stop -f test_acpi_linux
```

**Files to create:**
- `regress/sbin/vmd/test_acpi_tables` — ACPI validation test
- `regress/sbin/vmd/acpi_check.sh` — helper script for ACPI validation

### 8.3 Guest-Side Testing

**What**: Run tests inside a guest VM to verify Windows compatibility.

**Linux guest tests** (easier to automate):
1. **VirtIO device enumeration**:
   ```bash
   # lspci should show VirtIO devices
   lspci | grep -i virtio
   # Should show: network, block, rng, vmmci, balloon
   
   # dmesg should show VirtIO drivers loaded
   dmesg | grep virtio
   ```

2. **Hyper-V CPUID detection**:
   ```bash
   # CPUID leaf 0x40000000 should return Hyper-V signature
   cpuid -l 0x40000000
   # Should show: vendor "Microsoft" or "Msft"
   ```

3. **ACPI validation**:
   ```bash
   # Check ACPI tables
   acpidump
   # Check for required tables
   cat /sys/firmware/acpi/tables/*
   ```

4. **TPM access**:
   ```bash
   # Check TPM device
   ls /dev/tpm0
   # Try to read TPM info
   tpm2_getcap capabilities | grep -i tpm
   ```

5. **Clock accuracy**:
   ```bash
   # Compare guest clock with host clock
   ntpdate -q ntp.ubuntu.com
   # Should show offset < 10ms
   ```

**Windows guest tests** (manual, via RDP):
1. **Device Manager**: Check for unrecognized devices
2. **Windows Performance Monitor**: Check clock accuracy
3. **TPM Management**: Verify TPM is present and working
4. **Hyper-V Settings**: Verify Hyper-V features are enabled
5. **BitLocker**: Test BitLocker encryption/decryption
6. **Windows Update**: Verify updates can be installed

**Files to create:**
- `regress/guest/linux/virtio_test.sh` — Linux guest VirtIO test
- `regress/guest/linux/hyperv_test.sh` — Linux guest Hyper-V test
- `regress/guest/linux/acpi_test.sh` — Linux guest ACPI test
- `regress/guest/linux/tpm_test.sh` — Linux guest TPM test
- `regress/guest/linux/clock_test.sh` — Linux guest clock test

### 8.4 Hyper-V CPUID/MSR Validation

**What**: Verify that Hyper-V CPUID leaves and MSRs are correctly implemented.

**Test**:
```sh
#!/bin/sh
# test_hyperv_cpuid — Test Hyper-V CPUID leaves

. $TESTLIB

# Start Linux VM
vmctl start -c vm.conf test_hyperv
sleep 5

# SSH into guest and check CPUID
ssh root@localhost 'cpuid -l 0x40000000' | grep -q 'Msft'
if [ $? -eq 0 ]; then
    echo "PASS: CPUID 0x40000000 returns Hyper-V signature"
else
    echo "FAIL: CPUID 0x40000000 does not return Hyper-V signature"
    exit 1
fi

# Check CPUID 0x40000001 (Interface)
ssh root@localhost 'cpuid -l 0x40000001' | grep -q 'WN01'
if [ $? -eq 0 ]; then
    echo "PASS: CPUID 0x40000001 returns Windows compatibility"
else
    echo "FAIL: CPUID 0x40000001 does not return Windows compatibility"
    exit 1
fi

# Teardown
vmctl stop -f test_hyperv
```

**Files to create:**
- `regress/sbin/vmd/test_hyperv_cpuid` — Hyper-V CPUID test
- `regress/sbin/vmd/test_hyperv_msr` — Hyper-V MSR test

### 8.5 TPM PCR Test

**What**: Verify TPM PCR functionality.

**Test**:
```sh
#!/bin/sh
# test_tpm_pcr — Test TPM PCR operations

. $TESTLIB

# Start Linux VM
vmctl start -c vm.conf test_tpm
sleep 5

# SSH into guest
ssh root@localhost 'tpm2_pcrread' | grep -q 'sha256:'
if [ $? -eq 0 ]; then
    echo "PASS: TPM PCR read works"
else
    echo "FAIL: TPM PCR read failed"
    exit 1
fi

# Extend a PCR
ssh root@localhost 'echo "test data" | tpm2_pcrextend sha256:0'
if [ $? -eq 0 ]; then
    echo "PASS: PCR extend works"
else
    echo "FAIL: PCR extend failed"
    exit 1
fi

# Teardown
vmctl stop -f test_tpm
```

**Files to create:**
- `regress/sbin/vmd/test_tpm_pcr` — TPM PCR test

### 8.6 Integration Test: Windows Boot

**What**: End-to-end test: boot Windows 10/11 from ISO with all features enabled.

**Test**:
```sh
#!/bin/sh
# test_windows_boot — End-to-end Windows boot test

. $TESTLIB

# Create VM config
cat > /tmp/test_win11.conf <<EOF
vm "win11_test" {
    firmware "ovmf"
    secure-boot on
    tpm on
    memory 4096
    cpus 2
    disk "hd0" "/tmp/win11_disk.vhdx"
    cdrom "/tmp/en_windows_11.iso"
}
EOF

# Start VM
vmctl start -c /tmp/test_win11.conf win11_test
sleep 30

# Check that VM is running
if vmctl info win11_test | grep -q "running"; then
    echo "PASS: Windows VM is running"
else
    echo "FAIL: Windows VM is not running"
    exit 1
fi

# Check that OVMF is used
if vmctl info win11_test | grep -q "OVMF"; then
    echo "PASS: OVMF firmware detected"
else
    echo "FAIL: OVMF firmware not detected"
    exit 1
fi

# Check TPM
if vmctl info win11_test | grep -q "tpm"; then
    echo "PASS: TPM enabled"
else
    echo "FAIL: TPM not enabled"
    exit 1
fi

# Teardown
vmctl stop -f win11_test
```

**Files to create:**
- `regress/sbin/vmd/test_windows_boot` — Windows boot test

### 8.7 Performance Benchmarks

**What**: Measure VM performance to ensure no regressions.

**Metrics**:
- Boot time (seconds)
- Interrupt latency (microseconds)
- Disk I/O throughput (MB/s)
- Network throughput (Mbps)
- Clock accuracy (milliseconds offset)

**Benchmarks**:
```sh
#!/bin/sh
# benchmark_boot_time — Measure VM boot time

. $TESTLIB

start=$(date +%s%N)
vmctl start -c vm.conf test_boot
end=$(date +%s%N)
boot_time=$(( (end - start) / 1000000 ))
echo "Boot time: ${boot_time}ms"
vmctl stop -f test_boot

# Expected: < 30 seconds for Linux, < 60 seconds for Windows
```

**Files to create:**
- `regress/sbin/vmd/benchmark_boot_time` — Boot time benchmark
- `regress/sbin/vmd/benchmark_disk_io` — Disk I/O benchmark
- `regress/sbin/vmd/benchmark_network` — Network throughput benchmark
- `regress/sbin/vmd/benchmark_clock` — Clock accuracy benchmark

### 8.8 Test VM Images

**What**: Provide pre-built test VM images for quick testing.

**Images**:
- `test-linux.qcow2` — Minimal Linux VM with VirtIO drivers
- `test-win10.qcow2` — Windows 10 VM with VirtIO drivers
- `test-win11.qcow2` — Windows 11 VM with VirtIO drivers

**Distribution**:
- Images are too large for source tree
- Distribute via separate download (e.g., OpenBSD FTP server)
- Include in release notes

**Files to create:**
- `regress/guest/images/README.md` — Image download instructions
- `regress/guest/images/setup.sh` — Script to download and setup test images

## Dependencies

- Linux guest VM for automated testing
- Windows guest VM for manual testing
- `acpidump`, `iasl` from Linux for ACPI validation
- `tpm2-tools` for TPM testing
- `ntpdate` for clock testing

## Risks

- **Test reliability**: VM tests are inherently non-deterministic. Use timeouts and retries.
- **Resource usage**: Running multiple VMs for testing requires significant RAM and CPU.
- **Windows licensing**: Windows VM images require valid licenses. Provide evaluation ISOs only.

## Implementation Order

1. Create regress directory structure and Makefiles
2. Implement ACPI table validation test
3. Implement Hyper-V CPUID/MSR test
4. Implement TPM PCR test
5. Implement VirtIO device test
6. Implement OVMF boot test
7. Implement Windows boot test
8. Implement performance benchmarks
9. Create test VM images
10. Run all tests and fix any failures
