#Requires -Version 5.1
#Requires -RunAsAdministrator

<#
.SYNOPSIS
Build a Windows 11 installation ISO with the VirtIO drivers used by OpenBSD vmd.

.DESCRIPTION
New-VmdWindowsIso.ps1 copies an original Windows 11 ISO, injects signed
virtio-win drivers into its offline Windows images, and rebuilds a dual
BIOS/UEFI bootable ISO.

The boot images receive the boot-critical vioscsi and viostor drivers plus
vioinput, so the next vmd absolute-input device can work in Windows Setup.

The install images receive vioscsi, viostor, NetKVM, viorng and vioinput, plus
optional viosnd when found.  The upstream virtio-win suite did not contain a
Windows virtio-snd driver when this script was written; viosnd discovery is
included so a future signed package is picked up without a script change.

Run this script from an elevated Windows PowerShell 5.1 prompt.  It requires
the Deployment Tools component of the Windows ADK for oscdimg.exe.  Supply an
official signed virtio-win ISO; the script deliberately does not use DISM's
ForceUnsigned option.

.PARAMETER WindowsIso
Path to an original Windows 11 x64 ISO.

.PARAMETER VirtioIso
Path to a virtio-win ISO containing w11\amd64 driver directories.

.PARAMETER OutputIso
Path of the modified ISO to create.  Existing files are refused unless -Force
is specified.

.PARAMETER InstallIndex
One or more install.wim/install.esd indexes to service.  The default is every
index.  Unselected editions remain on the output media but do not receive the
VirtIO drivers.  Use Get-WindowsImage -ImagePath <path> to inspect image
names/indexes.

.PARAMETER WorkDirectory
An unused directory for extracted media and WIM mounts.  By default a unique
directory is made below the current user's temporary directory.

.PARAMETER OscdimgPath
Explicit path to oscdimg.exe.  The script otherwise searches PATH and the
normal Windows ADK Deployment Tools locations.

.PARAMETER VolumeLabel
Optional output UDF volume label.  The source ISO label is preserved by
default.

.PARAMETER SkipWinRE
Do not inject the boot-critical drivers into each selected edition's embedded
Windows Recovery Environment.  Servicing WinRE is enabled by default so the
recovery environment can see a vmd virtio-blk target.

.PARAMETER KeepWorkDirectory
Keep extracted media after a successful build.  Failed builds are always kept
for diagnosis.

.PARAMETER Force
Allow replacement of an existing OutputIso.  Source ISO files are never
modified.

.EXAMPLE
PowerShell.exe -ExecutionPolicy Bypass -File .\New-VmdWindowsIso.ps1 `
    -WindowsIso C:\ISO\Win11.iso `
    -VirtioIso C:\ISO\virtio-win.iso `
    -OutputIso C:\ISO\Win11-vmd.iso

.EXAMPLE
.\New-VmdWindowsIso.ps1 -WindowsIso C:\ISO\Win11.iso `
    -VirtioIso C:\ISO\virtio-win.iso `
    -OutputIso C:\ISO\Win11-Pro-vmd.iso -InstallIndex 6 `
    -KeepWorkDirectory
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $WindowsIso,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $VirtioIso,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $OutputIso,

    [int[]] $InstallIndex,
    [string] $WorkDirectory,
    [string] $OscdimgPath,
    [string] $VolumeLabel,
    [switch] $SkipWinRE,
    [switch] $KeepWorkDirectory,
    [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Step {
    param([string] $Message)
    Write-Host "`n==> $Message" -ForegroundColor Cyan
}

function Resolve-ExistingFile {
    param([string] $Path, [string] $Description)

    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.PSIsContainer) {
        throw "$Description is a directory: $Path"
    }
    return $item.FullName
}

function Resolve-OutputPath {
    param([string] $Path)

    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Find-Oscdimg {
    param([string] $RequestedPath)

    if ($RequestedPath) {
        return Resolve-ExistingFile $RequestedPath 'oscdimg.exe'
    }

    $command = Get-Command oscdimg.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Path
    }

    $candidates = @()
    if (${env:ProgramFiles(x86)}) {
        $candidates += Join-Path ${env:ProgramFiles(x86)} `
            'Windows Kits\10\Assessment and Deployment Kit\Deployment Tools\amd64\Oscdimg\oscdimg.exe'
        $candidates += Join-Path ${env:ProgramFiles(x86)} `
            'Windows Kits\10\Assessment and Deployment Kit\Deployment Tools\x86\Oscdimg\oscdimg.exe'
    }
    if ($env:ProgramFiles) {
        $candidates += Join-Path $env:ProgramFiles `
            'Windows Kits\10\Assessment and Deployment Kit\Deployment Tools\amd64\Oscdimg\oscdimg.exe'
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Get-Item -LiteralPath $candidate).FullName
        }
    }

    throw 'oscdimg.exe was not found. Install the Windows ADK Deployment Tools component or pass -OscdimgPath.'
}

function Get-ObjectProperty {
    param([object] $Object, [string] $Name)

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Get-WindowsImageVersion {
    param([object] $Image)

    $value = Get-ObjectProperty $Image 'Version'
    if ($null -ne $value) {
        try {
            return [Version]$value
        }
        catch {
            throw "Unrecognized Windows image version '$value'."
        }
    }

    $major = Get-ObjectProperty $Image 'MajorVersion'
    $minor = Get-ObjectProperty $Image 'MinorVersion'
    $build = Get-ObjectProperty $Image 'Build'
    $revision = Get-ObjectProperty $Image 'SPBuild'
    if ($null -eq $major -or $null -eq $minor -or $null -eq $build) {
        throw "DISM did not report a version for image index $($Image.ImageIndex)."
    }
    if ($null -eq $revision) {
        $revision = 0
    }
    return [Version]::new([int]$major, [int]$minor, [int]$build,
        [int]$revision)
}

function Assert-Windows11X64Images {
    param([object[]] $Images, [string] $ImagePath)

    $maximumBuild = 0
    foreach ($image in $Images) {
        # The list form of Get-WindowsImage returns only summary fields.
        # Query each index for architecture and version metadata.
        $details = Get-WindowsImage -ImagePath $ImagePath `
            -Index $image.ImageIndex
        $architecture = [string](Get-ObjectProperty $details 'Architecture')
        if ($architecture.ToLowerInvariant() -notin @('x64', 'amd64', '9')) {
            throw "$([IO.Path]::GetFileName($ImagePath)) index $($image.ImageIndex) is '$architecture', not x64."
        }

        $version = Get-WindowsImageVersion $details
        if ($version.Major -ne 10 -or $version.Build -lt 22000) {
            throw "$([IO.Path]::GetFileName($ImagePath)) index $($image.ImageIndex) is Windows $version, not Windows 11."
        }
        $maximumBuild = [Math]::Max($maximumBuild, $version.Build)
    }
    return $maximumBuild
}

function Publish-Iso {
    param(
        [string] $TemporaryPath,
        [string] $DestinationPath,
        [switch] $AllowReplace
    )

    if (-not (Test-Path -LiteralPath $DestinationPath)) {
        Move-Item -LiteralPath $TemporaryPath -Destination $DestinationPath
        return
    }
    if (-not $AllowReplace) {
        throw "OutputIso appeared during the build; use -Force to replace it: $DestinationPath"
    }

    # Keep the previous good image recoverable until the completed temporary
    # image has taken its place.  All paths are in one directory, so both
    # renames stay on the same filesystem.
    $backupPath = "$DestinationPath.$([Guid]::NewGuid().ToString('N')).backup"
    $published = $false
    Move-Item -LiteralPath $DestinationPath -Destination $backupPath
    try {
        Move-Item -LiteralPath $TemporaryPath -Destination $DestinationPath
        $published = $true
    }
    finally {
        if (-not $published -and
            -not (Test-Path -LiteralPath $DestinationPath) -and
            (Test-Path -LiteralPath $backupPath)) {
            Move-Item -LiteralPath $backupPath -Destination $DestinationPath
        }
    }

    try {
        Remove-Item -LiteralPath $backupPath -Force
    }
    catch {
        Write-Warning "The old output ISO remains at $backupPath"
    }
}

function Mount-IsoReadOnly {
    param([string] $Path)

    $mountedByUs = $false
    $image = Get-DiskImage -ImagePath $Path -ErrorAction SilentlyContinue
    try {
        if ($null -eq $image -or -not $image.Attached) {
            $image = Mount-DiskImage -ImagePath $Path -StorageType ISO `
                -Access ReadOnly -PassThru
            $mountedByUs = $true
        }

        $volumes = @($image | Get-Volume | Where-Object {
            $null -ne $_.DriveLetter
        })
        if ($volumes.Count -ne 1) {
            throw "Expected one mounted volume with a drive letter for $Path; found $($volumes.Count)."
        }

        return [PSCustomObject]@{
            Image       = $image
            Root        = "$($volumes[0].DriveLetter):\"
            Label       = $volumes[0].FileSystemLabel
            MountedByUs = $mountedByUs
        }
    }
    catch {
        if ($mountedByUs) {
            Dismount-DiskImage -ImagePath $Path -ErrorAction SilentlyContinue
        }
        throw
    }
}

function Find-VirtioInf {
    param(
        [string] $Root,
        [string[]] $Families,
        [string[]] $InfNames
    )

    foreach ($family in $Families) {
        foreach ($infName in $InfNames) {
            foreach ($osDirectory in @('w11', 'Win11')) {
                foreach ($archDirectory in @('amd64', 'x64')) {
                    $candidate = Join-Path $Root `
                        "$family\$osDirectory\$archDirectory\$infName"
                    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                        return (Get-Item -LiteralPath $candidate).FullName
                    }
                }
            }
        }
    }

    # Accommodate repacked virtio-win media while refusing drivers for a
    # different Windows release or architecture.
    $matches = @()
    foreach ($infName in $InfNames) {
        $matches += @(Get-ChildItem -LiteralPath $Root -Filter $infName `
            -File -Recurse -ErrorAction SilentlyContinue | Where-Object {
                $path = $_.FullName.ToLowerInvariant()
                ($path -match '\\(w11|win11)\\(amd64|x64)\\') -or
                ($path -match '\\(amd64|x64)\\(w11|win11)\\')
            })
    }
    $matches = @($matches | Sort-Object FullName -Unique)
    if ($matches.Count -gt 1) {
        throw "Multiple matching driver packages were found: $($matches.FullName -join ', ')"
    }
    if ($matches.Count -eq 1) {
        return $matches[0].FullName
    }
    return $null
}

function Invoke-Robocopy {
    param([string] $Source, [string] $Destination)

    & robocopy.exe $Source $Destination /E /COPY:DAT /DCOPY:DAT /R:2 /W:1
    $result = $LASTEXITCODE
    if ($result -gt 7) {
        throw "robocopy failed with exit code $result"
    }
}

function Service-WindowsImage {
    param(
        [string] $ImagePath,
        [int] $Index,
        [string] $MountPath,
        [object[]] $Drivers,
        [switch] $ServiceRecovery,
        [string] $RecoveryMountPath,
        [object[]] $RecoveryDrivers
    )

    $image = Get-WindowsImage -ImagePath $ImagePath -Index $Index
    Write-Step "Servicing $([IO.Path]::GetFileName($ImagePath)) index $Index ($($image.ImageName))"

    $mountAttempted = $false
    try {
        $mountAttempted = $true
        Mount-WindowsImage -ImagePath $ImagePath -Index $Index `
            -Path $MountPath -CheckIntegrity | Out-Null

        foreach ($driver in $Drivers) {
            Write-Host "  + $($driver.Name): $($driver.Path)"
            Add-WindowsDriver -Path $MountPath -Driver $driver.Path | Out-Null
        }

        if ($ServiceRecovery) {
            if (-not $RecoveryMountPath) {
                throw 'RecoveryMountPath is required when servicing WinRE.'
            }
            $winreWim = Join-Path $MountPath `
                'Windows\System32\Recovery\winre.wim'
            if (Test-Path -LiteralPath $winreWim -PathType Leaf) {
                $winreItem = Get-Item -LiteralPath $winreWim -Force
                $winreAttributes = $winreItem.Attributes
                try {
                    $winreItem.IsReadOnly = $false
                    $recoveryImages = @(Get-WindowsImage `
                        -ImagePath $winreWim)
                    foreach ($recoveryImage in $recoveryImages) {
                        Service-WindowsImage -ImagePath $winreWim `
                            -Index $recoveryImage.ImageIndex `
                            -MountPath $RecoveryMountPath `
                            -Drivers $RecoveryDrivers
                    }
                }
                finally {
                    if (Test-Path -LiteralPath $winreWim -PathType Leaf) {
                        [IO.File]::SetAttributes($winreWim,
                            $winreAttributes)
                    }
                }
            }
            else {
                Write-Warning "No embedded winre.wim found in install image index $Index."
            }
        }

        Dismount-WindowsImage -Path $MountPath -Save -CheckIntegrity | Out-Null
        $mountAttempted = $false
    }
    finally {
        if ($mountAttempted) {
            Write-Warning "Discarding the incomplete mount at $MountPath"
            Dismount-WindowsImage -Path $MountPath -Discard `
                -ErrorAction SilentlyContinue | Out-Null
        }
    }
}

$windowsIsoPath = Resolve-ExistingFile $WindowsIso 'Windows ISO'
$virtioIsoPath = Resolve-ExistingFile $VirtioIso 'virtio-win ISO'
$outputIsoPath = Resolve-OutputPath $OutputIso

if (-not [Environment]::Is64BitProcess) {
    throw 'Run this script from 64-bit Windows PowerShell.'
}
$oscdimg = Find-Oscdimg $OscdimgPath

if ([IO.Path]::GetExtension($outputIsoPath) -ine '.iso') {
    throw "OutputIso must have an .iso extension: $outputIsoPath"
}
if ($outputIsoPath -ieq $windowsIsoPath -or
    $outputIsoPath -ieq $virtioIsoPath) {
    throw 'OutputIso must not overwrite either source ISO.'
}

if (-not $WorkDirectory) {
    $WorkDirectory = Join-Path ([IO.Path]::GetTempPath()) `
        "vmd-winiso-$([Guid]::NewGuid().ToString('N'))"
}
$workPath = Resolve-OutputPath $WorkDirectory
if (Test-Path -LiteralPath $workPath) {
    throw "WorkDirectory must not already exist: $workPath"
}
if ($workPath.IndexOfAny([char[]]'#,') -ne -1) {
    throw "WorkDirectory must not contain '#' or ',' because oscdimg uses them as boot-entry delimiters: $workPath"
}
$workPrefix = $workPath.TrimEnd('\') + '\'
if ($outputIsoPath -ieq $workPath -or
    $outputIsoPath.StartsWith($workPrefix,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputIso must be outside WorkDirectory because successful builds remove the work tree by default.'
}
$workRoot = [IO.Path]::GetPathRoot($workPath)
if ($workRoot -notmatch '^[A-Za-z]:\\$') {
    throw "WorkDirectory must be on a local NTFS volume: $workPath"
}
$workVolume = Get-Volume -DriveLetter ($workRoot.Substring(0, 1))
if ($workVolume.FileSystem -ine 'NTFS') {
    throw "WorkDirectory must be on NTFS; $workRoot is $($workVolume.FileSystem)."
}

$outputParent = Split-Path -Parent $outputIsoPath
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    New-Item -ItemType Directory -Path $outputParent | Out-Null
}
if (Test-Path -LiteralPath $outputIsoPath) {
    if (-not (Test-Path -LiteralPath $outputIsoPath -PathType Leaf)) {
        throw "OutputIso exists but is not a file: $outputIsoPath"
    }
    if (-not $Force) {
        throw "OutputIso already exists; use -Force to replace it: $outputIsoPath"
    }
}
$temporaryIsoPath = Join-Path $outputParent `
    ".$([IO.Path]::GetFileNameWithoutExtension($outputIsoPath)).$([Guid]::NewGuid().ToString('N')).partial.iso"

$driverSpecs = @(
    [PSCustomObject]@{
        Name = 'vioscsi'; Families = @('vioscsi');
        InfNames = @('vioscsi.inf'); Required = $true;
        Boot = $true; Install = $true
    },
    [PSCustomObject]@{
        Name = 'viostor'; Families = @('viostor');
        InfNames = @('viostor.inf'); Required = $true;
        Boot = $true; Install = $true
    },
    [PSCustomObject]@{
        Name = 'NetKVM'; Families = @('NetKVM');
        InfNames = @('netkvm.inf'); Required = $true;
        Boot = $false; Install = $true
    },
    [PSCustomObject]@{
        Name = 'viorng'; Families = @('viorng');
        InfNames = @('viorng.inf'); Required = $true;
        Boot = $false; Install = $true
    },
    [PSCustomObject]@{
        Name = 'vioinput'; Families = @('vioinput');
        InfNames = @('vioinput.inf'); Required = $true;
        Boot = $true; Install = $true
    },
    [PSCustomObject]@{
        Name = 'viosnd'; Families = @('viosnd', 'vioaudio');
        InfNames = @('viosnd.inf', 'vioaudio.inf'); Required = $false;
        Boot = $false; Install = $true
    }
)

$windowsMount = $null
$virtioMount = $null
$buildSucceeded = $false
$workCreated = $false
$isoPublished = $false

try {
    Write-Step 'Hashing source media'
    $windowsHash = (Get-FileHash -LiteralPath $windowsIsoPath `
        -Algorithm SHA256).Hash
    $virtioHash = (Get-FileHash -LiteralPath $virtioIsoPath `
        -Algorithm SHA256).Hash
    Write-Host "  Windows ISO SHA256:   $windowsHash"
    Write-Host "  virtio-win SHA256:    $virtioHash"

    Write-Step 'Creating work directories'
    New-Item -ItemType Directory -Path $workPath | Out-Null
    $workCreated = $true
    $mediaPath = Join-Path $workPath 'media'
    $mountPath = Join-Path $workPath 'mount'
    $recoveryMountPath = Join-Path $workPath 'recovery-mount'
    New-Item -ItemType Directory -Path $mediaPath | Out-Null
    New-Item -ItemType Directory -Path $mountPath | Out-Null
    New-Item -ItemType Directory -Path $recoveryMountPath | Out-Null

    Write-Step 'Mounting source ISOs read-only'
    $windowsMount = Mount-IsoReadOnly $windowsIsoPath
    $virtioMount = Mount-IsoReadOnly $virtioIsoPath

    Write-Step 'Resolving Windows 11 amd64 VirtIO drivers'
    $drivers = @()
    $missingRequired = @()
    foreach ($spec in $driverSpecs) {
        $inf = Find-VirtioInf -Root $virtioMount.Root `
            -Families $spec.Families -InfNames $spec.InfNames
        if ($null -eq $inf) {
            if ($spec.Required) {
                $missingRequired += $spec.Name
                Write-Host "  ! $($spec.Name): REQUIRED, not found" `
                    -ForegroundColor Red
            }
            else {
                Write-Host "  - $($spec.Name): not present; skipped"
            }
            continue
        }
        Write-Host "  + $($spec.Name): $inf"
        $drivers += [PSCustomObject]@{
            Name = $spec.Name
            Path = $inf
            Boot = $spec.Boot
            Install = $spec.Install
        }
    }
    if ($missingRequired.Count -ne 0) {
        throw "The virtio-win ISO lacks required w11\amd64 drivers: $($missingRequired -join ', ')"
    }

    Write-Step 'Copying the Windows installation media'
    Invoke-Robocopy -Source $windowsMount.Root -Destination $mediaPath
    & attrib.exe -R (Join-Path $mediaPath '*') /S /D
    if ($LASTEXITCODE -ne 0) {
        throw "attrib.exe failed with exit code $LASTEXITCODE"
    }

    $bootWim = Join-Path $mediaPath 'sources\boot.wim'
    if (-not (Test-Path -LiteralPath $bootWim -PathType Leaf)) {
        throw "The Windows ISO does not contain sources\boot.wim."
    }
    (Get-Item -LiteralPath $bootWim).IsReadOnly = $false

    $bootDrivers = @($drivers | Where-Object { $_.Boot })
    $bootImages = @(Get-WindowsImage -ImagePath $bootWim)
    $targetBuild = Assert-Windows11X64Images -Images $bootImages `
        -ImagePath $bootWim
    foreach ($image in $bootImages) {
        Service-WindowsImage -ImagePath $bootWim `
            -Index $image.ImageIndex -MountPath $mountPath `
            -Drivers $bootDrivers
    }

    $installWim = Join-Path $mediaPath 'sources\install.wim'
    $installEsd = Join-Path $mediaPath 'sources\install.esd'
    $installSwm = Join-Path $mediaPath 'sources\install.swm'
    $hasInstallWim = Test-Path -LiteralPath $installWim -PathType Leaf
    $hasInstallEsd = Test-Path -LiteralPath $installEsd -PathType Leaf
    $hasInstallSwm = Test-Path -LiteralPath $installSwm -PathType Leaf
    $payloadCount = @(@($hasInstallWim, $hasInstallEsd,
        $hasInstallSwm) | Where-Object { $_ }).Count
    if ($payloadCount -gt 1) {
        throw 'The Windows ISO contains multiple install payload formats (WIM, ESD, or SWM); refusing an ambiguous image.'
    }
    if ($hasInstallWim) {
        $installImagePath = $installWim
    }
    elseif ($hasInstallEsd) {
        $installImagePath = $installEsd
    }
    elseif ($hasInstallSwm) {
        throw 'Split install.swm media is not supported; use an ISO containing install.wim or install.esd.'
    }
    else {
        throw 'The Windows ISO contains neither sources\install.wim nor sources\install.esd.'
    }
    (Get-Item -LiteralPath $installImagePath).IsReadOnly = $false

    $availableInstallImages = @(Get-WindowsImage -ImagePath $installImagePath)
    $targetBuild = [Math]::Max($targetBuild,
        (Assert-Windows11X64Images -Images $availableInstallImages `
            -ImagePath $installImagePath))
    $hostBuild = [int](Get-ItemProperty -LiteralPath `
        'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber
    if ($hostBuild -lt $targetBuild) {
        Write-Warning "The technician OS build ($hostBuild) is older than the target image ($targetBuild). If DISM rejects the image, run this script on the same or a newer Windows 11 build."
    }
    Write-Step 'Available Windows install images'
    $availableInstallImages | Select-Object ImageIndex, ImageName | `
        Format-Table -AutoSize | Out-Host

    if ($null -eq $InstallIndex -or $InstallIndex.Count -eq 0) {
        $selectedIndexes = @($availableInstallImages.ImageIndex)
    }
    else {
        $selectedIndexes = @($InstallIndex | Sort-Object -Unique)
        foreach ($index in $selectedIndexes) {
            if ($index -notin $availableInstallImages.ImageIndex) {
                throw "InstallIndex $index does not exist in $([IO.Path]::GetFileName($installImagePath))."
            }
        }
    }

    if ([IO.Path]::GetExtension($installImagePath) -ieq '.esd') {
        Write-Step 'Converting install.esd to a serviceable install.wim'
        $destinationIndexBySource = @{}
        $destinationIndex = 0
        foreach ($sourceImage in $availableInstallImages) {
            $sourceIndex = [int]$sourceImage.ImageIndex
            Write-Host "  + exporting index $sourceIndex ($($sourceImage.ImageName))"
            $exportParameters = @{
                SourceImagePath = $installEsd
                SourceIndex = $sourceIndex
                DestinationImagePath = $installWim
                CheckIntegrity = $true
            }
            if (-not (Test-Path -LiteralPath $installWim)) {
                $exportParameters.CompressionType = 'Max'
            }
            Export-WindowsImage @exportParameters | Out-Null
            $destinationIndex++
            $destinationIndexBySource[$sourceIndex] = $destinationIndex
        }
        $exportedImages = @(Get-WindowsImage -ImagePath $installWim)
        if ($exportedImages.Count -ne $availableInstallImages.Count) {
            throw "Expected $($availableInstallImages.Count) exported install images; found $($exportedImages.Count)."
        }
        Remove-Item -LiteralPath $installEsd -Force
        $installImagePath = $installWim
        $selectedIndexes = @($selectedIndexes | ForEach-Object {
            $destinationIndexBySource[[int]$_]
        })
    }
    elseif ($selectedIndexes.Count -ne $availableInstallImages.Count) {
        Write-Warning 'Unselected install.wim editions will remain on the ISO without injected drivers.'
    }

    $installDrivers = @($drivers | Where-Object { $_.Install })
    foreach ($index in $selectedIndexes) {
        Service-WindowsImage -ImagePath $installImagePath `
            -Index $index -MountPath $mountPath -Drivers $installDrivers `
            -ServiceRecovery:(-not $SkipWinRE) `
            -RecoveryMountPath $recoveryMountPath `
            -RecoveryDrivers $bootDrivers
    }

    $biosBoot = Join-Path $mediaPath 'boot\etfsboot.com'
    $efiBoot = Join-Path $mediaPath 'efi\microsoft\boot\efisys.bin'
    if (-not (Test-Path -LiteralPath $biosBoot -PathType Leaf)) {
        throw "BIOS El Torito image not found: $biosBoot"
    }
    if (-not (Test-Path -LiteralPath $efiBoot -PathType Leaf)) {
        throw "UEFI El Torito image not found: $efiBoot"
    }

    if ([string]::IsNullOrWhiteSpace($VolumeLabel)) {
        $VolumeLabel = $windowsMount.Label
    }
    if ([string]::IsNullOrWhiteSpace($VolumeLabel)) {
        $VolumeLabel = 'VMD_WIN11'
    }
    $VolumeLabel = ($VolumeLabel -replace '[^A-Za-z0-9_]', '_')
    if ($VolumeLabel.Length -gt 32) {
        $VolumeLabel = $VolumeLabel.Substring(0, 32)
    }
    if ([string]::IsNullOrWhiteSpace($VolumeLabel)) {
        $VolumeLabel = 'VMD_WIN11'
    }

    $orderCandidates = @(
        'boot\bcd',
        'boot\boot.sdi',
        'boot\bootfix.bin',
        'boot\etfsboot.com',
        'efi\microsoft\boot\efisys.bin',
        'efi\boot\bootx64.efi',
        'bootmgr',
        'bootmgr.efi',
        'sources\boot.wim'
    )
    $orderEntries = @($orderCandidates | Where-Object {
        Test-Path -LiteralPath (Join-Path $mediaPath $_) -PathType Leaf
    })
    $orderPath = Join-Path $workPath 'boot-order.txt'
    [IO.File]::WriteAllLines($orderPath, $orderEntries,
        [Text.Encoding]::ASCII)

    Write-Step "Building dual BIOS/UEFI ISO: $outputIsoPath"
    $oscdimgArguments = @(
        '-m',
        '-o',
        '-h',
        '-u2',
        '-udfver102',
        "-l$VolumeLabel",
        "-yo$orderPath",
        "-bootdata:2#p0,e,b$biosBoot#pEF,e,b$efiBoot",
        $mediaPath,
        $temporaryIsoPath
    )
    & $oscdimg @oscdimgArguments
    if ($LASTEXITCODE -ne 0) {
        throw "oscdimg.exe failed with exit code $LASTEXITCODE"
    }

    $temporaryOutput = Get-Item -LiteralPath $temporaryIsoPath
    if ($temporaryOutput.Length -eq 0) {
        throw 'oscdimg.exe created an empty output file.'
    }
    $outputHash = (Get-FileHash -LiteralPath $temporaryIsoPath `
        -Algorithm SHA256).Hash
    Publish-Iso -TemporaryPath $temporaryIsoPath `
        -DestinationPath $outputIsoPath -AllowReplace:$Force
    $isoPublished = $true
    $buildSucceeded = $true
    $output = Get-Item -LiteralPath $outputIsoPath
    Write-Host "`nCreated $($output.FullName) ($([Math]::Round($output.Length / 1GB, 2)) GiB)" `
        -ForegroundColor Green
    Write-Host "Output ISO SHA256:      $outputHash" -ForegroundColor Green
}
finally {
    if ($null -ne $virtioMount -and $virtioMount.MountedByUs) {
        Dismount-DiskImage -ImagePath $virtioIsoPath `
            -ErrorAction SilentlyContinue
    }
    if ($null -ne $windowsMount -and $windowsMount.MountedByUs) {
        Dismount-DiskImage -ImagePath $windowsIsoPath `
            -ErrorAction SilentlyContinue
    }

    if (-not $isoPublished -and
        (Test-Path -LiteralPath $temporaryIsoPath)) {
        Remove-Item -LiteralPath $temporaryIsoPath -Force `
            -ErrorAction SilentlyContinue
    }

    if ($workCreated -and $buildSucceeded -and -not $KeepWorkDirectory) {
        try {
            Remove-Item -LiteralPath $workPath -Recurse -Force
        }
        catch {
            Write-Warning "The ISO was created, but the work directory could not be removed: $workPath"
        }
    }
    elseif ($workCreated) {
        Write-Host "Work directory retained at $workPath"
    }
}
