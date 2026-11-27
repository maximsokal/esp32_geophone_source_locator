param(
    [Parameter(Mandatory = $true)]
    [int]$DiskNumber,

    [string]$ImagePath = "",
    [string]$ImageUrl = "https://files.beagle.cc/file/beagleboard-public-2021/images/pocketbeagle2-debian-13.3-base-v6.12-arm64-2026-02-12-8gb.img.xz",
    [string]$ExpectedSha256 = "84966ED33217A3DB803A049B16F357575F062A1C591ECA88115E4AB5CA4F3BBD"
)

$ErrorActionPreference = "Stop"

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw "Administrator privileges are required to write raw blocks to PhysicalDrive$DiskNumber. Open an elevated PowerShell or run VS Code as Administrator."
}

$disk = Get-Disk -Number $DiskNumber
if ($disk.BusType -ne "USB" -and $disk.BusType -ne "SD") {
    throw "Safety abort: Disk $DiskNumber has BusType=$($disk.BusType) ($($disk.FriendlyName)). Only USB/SD removable media is allowed."
}

$workDir = Join-Path $env:TEMP "pb2_flash_cache"
New-Item -ItemType Directory -Force $workDir | Out-Null

if ([string]::IsNullOrWhiteSpace($ImagePath)) {
    $xzPath = Join-Path $workDir "pb2-debian-13.3.img.xz"
    $rawPath = Join-Path $workDir "pb2-debian-13.3.img"
    if (-not (Test-Path $xzPath)) {
        Write-Host "[pb2] Downloading official Debian 13.3 image..."
        & curl.exe -L --progress-bar -o $xzPath $ImageUrl
        if ($LASTEXITCODE -ne 0) { throw "Download failed" }
    }
    $actualSha = (Get-FileHash $xzPath -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actualSha -ne $ExpectedSha256.ToUpperInvariant()) {
        throw "SHA-256 mismatch! Expected $ExpectedSha256, got $actualSha"
    }
    Write-Host "[pb2] SHA-256 verified: $actualSha"
    if (-not (Test-Path $rawPath)) {
        Write-Host "[pb2] Decompressing .img.xz via WSL2 xz..."
        $xzWsl = ($xzPath -replace '^([A-Za-z]):\\', { "/mnt/$($args[0].Groups[1].Value.ToLower())/" }) -replace '\\', '/'
        & wsl -d Ubuntu -- bash -lc "xz -dk -f '$xzWsl'"
        if ($LASTEXITCODE -ne 0) { throw "xz decompression failed" }
    }
    $ImagePath = $rawPath
}

if (-not (Test-Path $ImagePath)) {
    throw "Image file not found: $ImagePath"
}

Write-Host "[pb2] Target: Disk $($disk.Number) - $($disk.FriendlyName) ($([math]::Round($disk.Size/1GB,2)) GB)"

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public class Pb2RawDisk {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern SafeFileHandle CreateFile(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(
        SafeFileHandle hDevice, uint dwIoControlCode,
        IntPtr lpInBuffer, uint nInBufferSize,
        IntPtr lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);

    public static SafeFileHandle LockAndDismountVolume(string letter) {
        var h = CreateFile(@"\\.\" + letter + ":", 0xC0000000u, 3u, IntPtr.Zero, 3u, 0u, IntPtr.Zero);
        if (h.IsInvalid) return null;
        uint dummy;
        DeviceIoControl(h, 0x00090018u, IntPtr.Zero, 0, IntPtr.Zero, 0, out dummy, IntPtr.Zero);
        DeviceIoControl(h, 0x00090020u, IntPtr.Zero, 0, IntPtr.Zero, 0, out dummy, IntPtr.Zero);
        return h;
    }

    public static FileStream OpenWrite(int diskNum) {
        var h = CreateFile(@"\\.\PhysicalDrive" + diskNum, 0xC0000000u, 3u, IntPtr.Zero, 3u, 0x80000000u, IntPtr.Zero);
        if (h.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        return new FileStream(h, FileAccess.ReadWrite);
    }
}
"@

$volHandles = @()
Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter } | ForEach-Object {
    $vh = [Pb2RawDisk]::LockAndDismountVolume($_.DriveLetter)
    if ($vh) { $volHandles += $vh }
}

$fsIn = [System.IO.File]::OpenRead($ImagePath)
$fsOut = [Pb2RawDisk]::OpenWrite($DiskNumber)
try {
    $buf = New-Object byte[] (4 * 1024 * 1024)
    $total = $fsIn.Length
    $written = 0L
    while (($read = $fsIn.Read($buf, 0, $buf.Length)) -gt 0) {
        $toWrite = $read
        $rem = $toWrite % 512
        if ($rem -ne 0) {
            $pad = 512 - $rem
            [Array]::Clear($buf, $toWrite, $pad)
            $toWrite += $pad
        }
        $fsOut.Write($buf, 0, $toWrite)
        $written += $read
        $pct = [math]::Round(($written * 100.0) / $total, 1)
        Write-Progress -Activity "Flashing PocketBeagle 2 Debian 13.3 to Disk $DiskNumber" -Status "$pct% ($([math]::Round($written/1MB)) / $([math]::Round($total/1MB)) MB)" -PercentComplete $pct
    }
    $fsOut.Flush()
} finally {
    $fsOut.Dispose()
    $fsIn.Dispose()
    foreach ($vh in $volHandles) { $vh.Dispose() }
}

Update-HostStorageCache
Start-Sleep -Seconds 2
Get-Partition -DiskNumber $DiskNumber | Format-Table PartitionNumber,DriveLetter,Type,Size
Write-Host "[pb2] FLASH_COMPLETE"
