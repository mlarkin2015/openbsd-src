# Windows installation media for vmd

`New-VmdWindowsIso.ps1` builds a Windows 10, Windows 11, or Windows Server x64
installation ISO containing the signed VirtIO drivers needed by the current
vmd device model, including the absolute-input device.

## Requirements

- A Windows technician machine at the same or a newer build than the source
  media.  Older DISM versions may reject newer images.
- An elevated, 64-bit Windows PowerShell 5.1 prompt.
- A local NTFS work volume with ample free space.  Allow roughly three times
  the source ISO size when servicing every edition.
- The Windows ADK **Deployment Tools** component, which supplies
  `oscdimg.exe`.
- An original Windows x64 ISO and the official **stable**
  `virtio-win.iso`.  The GitHub repository contains its packaging sources;
  use the published binary ISO linked below, not a source archive.

Official downloads and command documentation:

- [Windows ADK](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install)
- [Stable virtio-win ISO](https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso)
- [DISM offline driver servicing](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/add-and-remove-drivers-to-an-offline-windows-image?view=windows-11)
- [`oscdimg` options](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/oscdimg-command-line-options?view=windows-11)

## Usage

From an elevated PowerShell prompt:

```powershell
PowerShell.exe -ExecutionPolicy Bypass -File .\New-VmdWindowsIso.ps1 `
    -WindowsIso C:\ISO\Win11.iso `
    -VirtioIso C:\ISO\virtio-win.iso `
    -OutputIso C:\ISO\Win11-vmd.iso
```

By default every Windows edition in the source media is serviced.  To reduce
build time and output size, select one or more indexes explicitly:

```powershell
.\New-VmdWindowsIso.ps1 `
    -WindowsIso C:\ISO\Win11.iso `
    -VirtioIso C:\ISO\virtio-win.iso `
    -OutputIso C:\ISO\Win11-Pro-vmd.iso `
    -InstallIndex 6
```

The script prints the available image names and indexes before servicing them.
Unselected editions remain on the ISO without VirtIO drivers and should not be
installed on vmd.  An `install.esd` is converted to `install.wim` while
preserving all editions, so the output can be larger than the source ISO.

The script normally detects the virtio-win OS directory from the selected
install images.  Client builds select `w10` or `w11`; supported Server builds
select `2k16`, `2k19`, `2k22`, or `2k25`.  To override detection for custom or
unusual media, specify the directory explicitly:

```powershell
.\New-VmdWindowsIso.ps1 `
    -WindowsIso C:\ISO\Win10.iso `
    -VirtioIso C:\ISO\virtio-win.iso `
    -VirtioDriverOs w10 `
    -OutputIso C:\ISO\Win10-vmd.iso
```

## Driver policy

| Driver | `boot.wim` / WinRE | `install.wim` | vmd device |
|---|---:|---:|---|
| `vioscsi` | yes | yes | installer CD-ROM (`1af4:1048`) |
| `viostor` | yes | yes | target disk (`1af4:1042`) |
| `vioinput` | yes | yes | absolute pointer (`1af4:1052`) |
| `NetKVM` | no | yes | network (`1af4:1041`) |
| `viorng` | no | yes | random source (`1af4:1044`) |
| `viosnd` | no | if found | possible future sound device |

The five current drivers are required and must all come from the selected OS
directory and `amd64`/`x64` architecture.  Mixing driver families is unsafe:
for example, current `w11` NetKVM fails to load on Windows 10 with Code 39
because it imports an unavailable entry point.  Broad recursive injection and
`ForceUnsigned` are deliberately avoided.  `viosnd` is optional because
upstream virtio-win does not currently ship a Windows virtio-snd driver; the
script will pick up a future signed package if one appears under the expected
layout.

The private OpenBSD vmm control device (`0b5d:0777`) has no Windows driver and
will remain unidentified.  Firmware ramfb is not a virtio-gpu PCI device, so a
virtio-gpu driver is not injected.

The `vioinput` driver has been runtime-tested with vmd's absolute tablet in an
installed Windows 10 guest.  Pointer motion, buttons, dragging and wheel input
work through the local RFB display without relative-pointer capture or warping.
NetKVM from `w10\amd64` has also been runtime-tested in that guest; it attaches
and provides working networking.

## Output and cleanup

The source ISOs are mounted read-only and never modified.  Input and output
SHA-256 hashes are printed.  The script builds to a temporary file and does
not replace an existing output ISO until the new image has completed and been
hashed.  Successful builds remove their generated work directory unless
`-KeepWorkDirectory` is given; failed builds retain it for inspection.
Existing output ISOs are preserved unless `-Force` is explicitly specified.
