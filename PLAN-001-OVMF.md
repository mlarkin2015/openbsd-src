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
- vmd publishes its RSDP through fw_cfg and OVMF installs vmd's linked XSDT,
  FADT, MADT, DSDT, and FACS rather than QEMU-specific ACPI tables.
- An EFI-capable OpenBSD install image boots through autoconfiguration and
  reaches the installer prompt from a virtio block disk.
- OVMF boot variables written to a configured `efivars` file survive VM
  shutdown and a subsequent start.  VMs without the setting receive a fresh,
  ephemeral variable store.
- OVMF's EFI Shell `reset -s` enters vmd's ACPI S5 path and terminates the VM
  without leaving a spinning vmd process.
- OVMF's EFI Shell `reset -w` uses the conventional i8042 reset command and
  completes a full vmd guest-reboot cycle.  vmd also recognizes the PIIX reset
  control port used by OVMF's cold-reset fallback.
- OVMF may describe a virtio block boot option as `UEFI Misc Device`; this is
  generic firmware naming and does not mean that vmd exposed the disk with an
  incorrect PCI class.
- The configuration and complete vmd regression suites pass with the UEFI
  integration installed.

## Runtime integration milestone

1. `firmware bios|uefi` is implemented in `vm.conf`; `vmctl start -f` provides
   a one-boot selection.  BIOS remains the compatibility default.
2. vmd loads the split CODE/VARS images into the existing 4 MB firmware
   window.  The CODE extent is directly mapped as reserved firmware memory;
   the VARS extent is an MMIO-backed CFI flash device with byte-program and
   block-erase operations.
3. `efivars path` is implemented for configured VMs.  A missing file is
   created privately from `vmm-uefi-vars`; an existing file is size-checked,
   locked, and used directly as the flash backing store.  Manual VMs and
   configured VMs without `efivars` use an anonymous ephemeral copy.
4. OVMF's vmd platform path consumes vmd's fw_cfg RSDP and installs the linked
   ACPI tables in place.
5. Guest writes cannot overwrite vmd's internal PCI interrupt route after
   OVMF programs the guest-visible PCI Interrupt Line register.
6. The MMIO decoder supports the additional byte, immediate, arithmetic,
   zero-extension, and MOVS forms exercised by OVMF's flash and PCI paths.

## Next validation and follow-up

1. Exercise EFI SMP, MSI-X, and networking with an installed OpenBSD guest,
   followed by Linux and Windows installation media.  Confirm OpenBSD's
   `machine fw` flow as a guest-level check of the already-tested EFI reset
   service.
2. Add focused runtime coverage for malformed variable stores, ephemeral
   versus persistent variables, CFI flash bounds, and ACPI handoff.
3. Add a power-failure-safe persistence protocol if EFI variable durability
   across host crashes is required.  Flash updates reach the backing file
   immediately and are fsynced on orderly VM exit, but there is no journal or
   atomic recovery layer.
4. Only after the non-SMM path is stable, evaluate Secure Boot, authenticated
   variable updates, key enrollment, and TPM-backed measured boot.

## Deferred items and constraints

- CSM/legacy Option ROM support is deferred; SeaBIOS serves that use case.
- vmm does not emulate SMM/SMRAM, so an SMM-required OVMF build is unsupported.
- SMBIOS is not currently generated by vmd.  It is useful for Windows but is
  not required to prove the initial UEFI boot path.
- Firmware CODE is mapped as reserved guest memory rather than a separately
  protected ROM mapping.  OVMF's VARS region does have CFI flash programming
  and erase semantics, but crash-consistent variable-store recovery remains a
  future hardening item.
