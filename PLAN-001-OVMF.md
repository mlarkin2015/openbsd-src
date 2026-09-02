# PLAN-001-OVMF: UEFI Firmware Support

## Scope and decisions

The first UEFI milestone is an X64 OVMF firmware image that builds on
OpenBSD and reaches the OVMF boot manager under vmm/vmd.  It is deliberately
small in scope:

- SeaBIOS remains the default for existing VMs.
- UEFI will be selected explicitly with `firmware uefi`; `firmware bios`
  selects SeaBIOS.
- No CSM is included.  Legacy guests should use SeaBIOS.
- The initial image has serial output, SMM disabled, and Secure Boot disabled.
- The build produces combined and split CODE/VARS 4 MB flash images.
- A VM without an `efivars` path will use an ephemeral copy of the template
  variable store.  A configured path will be private to, and persistent for,
  that VM.

Secure Boot and enrolled Microsoft keys are later milestones.  The split
flash layout is retained so they can be added without changing the VM-facing
storage design.

## Firmware build milestone

The port overlay is in `ports/sysutils/firmware/vmm`.  It extends the existing
`sysutils/firmware/vmm` port so one ports-framework build produces:

- `firmware/vmm-bios` -- the existing SeaBIOS image;
- `firmware/vmm-uefi` -- combined OVMF VARS+CODE image;
- `firmware/vmm-uefi-code` -- immutable OVMF code flash;
- `firmware/vmm-uefi-vars` -- pristine variable-store template; and
- corresponding license files.

The build uses EDK2 `edk2-stable202605`, LLVM 22, NASM, IASL, Python, GNU
make, and the ports `x86_64-elf` toolchain already needed by SeaBIOS.  All
EDK2 submodules used by the release are pinned in `DIST_TUPLE`; the build does
not fetch from the network after the ports distfiles have been populated.

The OpenBSD build adaptations are:

- include `<uuid.h>` when building `GenFv`;
- disable OpenBSD retguard for freestanding firmware objects;
- permit EDK2's executable/read-only linker layout with LLVM's linker; and
- provide stable tool prefixes rather than relying on Linux tool names.

## OVMF platform adaptations for vmd

Generic OVMF recognizes vmd's PCI host bridge device ID `0x0666` and uses
vmd's actual platform resources:

- PCI I/O: `0x1000` through `0xffff`;
- 32-bit PCI MMIO: `0xf0000000` through `0xffbfffff`;
- no 64-bit PCI BAR aperture yet;
- PM1A control at I/O port `0xb008`; and
- PCI interrupt routing already assigned by vmd.

vmd's E820 map is intentionally not changed.  It accurately describes the
reserved conventional-memory hole below 1 MB and a separate RAM extent above
1 MB.  Generic OVMF's QEMU path assumes that the first base-zero E820 RAM
record also describes all low memory.  The vmd platform path instead scans
usable E820 records for the highest RAM end below 4 GB.  This keeps the guest
memory map truthful while giving OVMF the value it needs for PEI memory
detection.

vmd does not currently emulate a free-running ACPI PM timer, so this initial
OVMF build uses EDK2's CPU/APIC timer library.  That is sufficient for firmware
boot.  A stable TSC-based firmware timer or ACPI PM timer emulation should be
considered before treating UEFI runtime services as complete.

OVMF is a flash image, not an EFI executable loaded by vmd.  Reset begins at
the x86 reset vector at `0xfffffff0`, and OVMF performs its own mode
transitions.  The existing flat-16 firmware entry path is therefore the right
starting point; no PE/COFF loader or synthetic EFI memory map is required in
vmd.

## Validated so far

- EDK2 BaseTools build and its host tests run on OpenBSD.
- A 4 MB X64 DEBUG image reaches DXE and the OVMF boot manager under vmd.
- OVMF discovers vmd's PCI devices and assigns BARs within the advertised
  apertures.
- A clean ports build produces and packages both SeaBIOS and the combined and
  split OVMF images in one invocation.
- The packaged RELEASE image exposes an interactive COM1 console: a diskless
  smoke test reaches the expected no-bootable-device prompt, accepts keyboard
  input, and opens the OVMF Boot Manager menu.

The firmware-only smoke test does not establish guest boot support yet.
Generic OVMF currently sees no ACPI tables because vmd's fw_cfg directory does
not publish them in the format OVMF consumes.

## Remaining runtime integration

1. Add `firmware bios|uefi` to `vm.conf` and the equivalent explicit vmctl
   selection.  Keep BIOS as the compatibility default.
2. Teach vmd to load the combined image for ephemeral operation and the split
   CODE/VARS images for persistent operation.  Firmware code must be
   guest-read-only; the variable flash must implement flash write/erase
   semantics rather than ordinary writable RAM.
3. Add `efivars path` to per-VM configuration.  Create a private copy from
   `vmm-uefi-vars` when the file does not yet exist, validate its size/header,
   and persist changes safely.  Manual VMs and configured VMs without this
   option get an in-memory copy that is discarded at shutdown.
4. Publish vmd's existing ACPI tables through fw_cfg in OVMF's table-loader
   format, or add an equally well-defined vmd-specific handoff.  Do not let
   OVMF invent QEMU PIIX/Q35 tables for the vmd platform.
5. Boot an EFI-capable OpenBSD or Linux disk, then exercise installation,
   reboot, shutdown, SMP, MSI-X devices, and persistent boot variables.
6. Add regression coverage for firmware selection, invalid variable stores,
   ephemeral versus persistent variables, flash bounds, and ACPI handoff.
7. Only after the non-SMM path is stable, evaluate Secure Boot, authenticated
   variable updates, key enrollment, and TPM-backed measured boot.

## Deferred items and constraints

- CSM/legacy Option ROM support is deferred; SeaBIOS serves that use case.
- vmm does not emulate SMM/SMRAM, so an SMM-required OVMF build is unsupported.
- SMBIOS is not currently generated by vmd.  It is useful for Windows but is
  not required to prove the initial UEFI boot path.
- The present vmd firmware loader limit already accommodates the 4 MB combined
  image.  Split pflash support will require explicit memory-map and access
  handling rather than treating both files as one ordinary BIOS blob.
