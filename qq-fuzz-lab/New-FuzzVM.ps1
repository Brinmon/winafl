# New-FuzzVM.ps1 - create an unattended Windows fuzz VM in Hyper-V
# Run as ADMIN. You provide a Windows 10/11 eval ISO; the rest is automatic.
#   .\New-FuzzVM.ps1 -IsoPath E:\isos\Win11.iso -VmName fuzz-qq
# After install: run provision-vm.ps1 (see README) inside the VM.
param(
  [Parameter(Mandatory)][string]$IsoPath,
  [string]$VmName   = 'fuzz-qq',
  [string]$VmPath   = 'E:\HyperV\fuzz-qq',
  [int64]$MemGB     = 8,
  [int]$CpuCores    = 8,
  [int64]$DiskGB    = 60,
  [int]$Switch      = 1          # default virtual switch index; see Get-VMSwitch
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $IsoPath)) { throw "ISO not found: $IsoPath" }
if (-not (Test-Path $VmPath))  { New-Item -ItemType Directory -Path $VmPath -Force | Out-Null }

# pick a switch (Default Switch if present, else first external)
$sw = Get-VMSwitch | Where-Object { $_.Name -eq 'Default Switch' } | Select-Object -First 1
if (-not $sw) { $sw = (Get-VMSwitch)[0] }
if (-not $sw) { throw 'No virtual switch found. Create one first: New-VMSwitch -Name NAT -SwitchType Internal' }
Write-Host "Using switch: $($sw.Name)"

# generation 2 VM, SecureBoot off (eval ISOs), TPM off
$vm = New-VM -Name $VmName -MemoryStartupBytes ($MemGB*1GB) -Generation 2 -Path $VmPath -NoVHD
Set-VM -Name $VmName -ProcessorCount $CpuCores -AutomaticCheckpointsEnabled $false
Set-VMMemory -Name $VmName -DynamicMemoryEnabled $true -MinimumBytes 2GB -MaximumBytes ($MemGB*1GB)
Set-VMFirmware -Name $VmName -EnableSecureBoot Off
Set-VM -Name $VmName -CheckpointType Standard
Connect-VMNetworkAdapter -VMName $VmName -SwitchName $sw.Name

# attach virtual DVD with the ISO
Add-VMDvdDrive -VMName $VmName -Path $IsoPath
$dvd = Get-VMDvdDrive -VMName $VmName

# create the OS disk (VHDX on E:)
$vhd = Join-Path $VmPath "$VmName-disk.vhdx"
New-VHD -Path $vhd -SizeBytes ($DiskGB*1GB) -Dynamic | Out-Null
Add-VMHardDiskDrive -VMName $VmName -Path $vhd

# boot from DVD first (unattended install), then it flips to disk
$dvdDrive = Get-VMHardDiskDrive -VMName $VmName | Where-Object { $_.Path -eq $vhd }
Set-VMFirmware -VMName $VmName -FirstBootDevice $dvd

# copy autounattend.xml onto a second ISO so Windows setup picks it up automatically
$unattend = Join-Path $PSScriptRoot 'autounattend.xml'
if (-not (Test-Path $unattend)) { Write-Warning "autounattend.xml not found next to this script; unattended install may not trigger. See README." }

Write-Host @"

VM '$VmName' created.
  Memory: ${MemGB}GB (dynamic)   CPU: $CpuCores   Disk: ${DiskGB}GB ($vhd)
  Switch: $($sw.Name)   DVD: $($dvd.Path)

Next:
  1. Start-VM $VmName   (Windows install runs unattended from autounattend.xml)
  2. After first boot, run provision-vm.ps1 INSIDE the VM to install QQ + WinAFL.
"@
