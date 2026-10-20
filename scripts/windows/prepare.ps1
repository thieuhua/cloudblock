# Initialize ONLY the designated CloudBlock directories on host E:.
# Does not format, mount, install drivers, or modify existing data.
[CmdletBinding()]
param([switch]$CheckOnly)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$expectedSource = 'E:\thieuhua\cloudblock'
$actualSource = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\')
if (-not [string]::Equals($actualSource, $expectedSource,
                          [StringComparison]::OrdinalIgnoreCase)) {
    throw "Wrong checkout location: $actualSource. Required: $expectedSource"
}
$data = 'E:\CloudBlock'
$volume = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='E:'"
if ($null -eq $volume -or [int]$volume.DriveType -ne 3) {
    throw "E: must be a local fixed disk, not a network or virtual remote mount"
}
if ([uint64]$volume.FreeSpace -lt 9GB) {
    throw "E: must have at least 9 GiB free before CloudBlock setup"
}
$names = @('cache', 'disk', 'metadata', 'logs')
if ($CheckOnly) {
    Write-Host "CHECK ONLY - no files changed."
    Write-Host "Code: $actualSource"
    Write-Host "Data: $data"
    foreach ($name in $names) {
        $p = Join-Path $data $name
        Write-Host "${p} exists: $(Test-Path -LiteralPath $p -PathType Container)"
    }
    exit 0
}
if (-not (Test-Path -LiteralPath $data -PathType Container)) {
    New-Item -Path $data -ItemType Directory -ErrorAction Stop | Out-Null
}
foreach ($name in $names) {
    $p = Join-Path $data $name
    if (Test-Path -LiteralPath $p) {
        if (-not (Test-Path -LiteralPath $p -PathType Container)) {
            throw "Expected directory but found non-directory: $p"
        }
        if ((Get-Item -LiteralPath $p).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing reparse point storage directory: $p"
        }
    } else {
        New-Item -Path $p -ItemType Directory -ErrorAction Stop | Out-Null
    }
}
$manifestPath = Join-Path $data 'metadata\volume.json'
if (Test-Path -LiteralPath $manifestPath) {
    $existing = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($existing.format -ne 'cloudblock-local-v1' -or
        [uint64]$existing.logicalBytes -ne 68719476736 -or
        [uint64]$existing.chunkBytes -ne 16777216 -or
        [uint64]$existing.cacheBytes -ne 1073741824) {
        throw "Existing metadata is incompatible. Refusing to overwrite $manifestPath"
    }
    Write-Host "Existing CloudBlock metadata preserved."
} else {
    $meta = [ordered]@{
        format       = 'cloudblock-local-v1'
        volumeId     = [guid]::NewGuid().ToString()
        logicalBytes = [uint64]68719476736
        chunkBytes   = [uint64]16777216
        cacheBytes   = [uint64]1073741824
        diskId       = 'CloudBlockLocal'
        note         = 'Development volume; no crash-consistent journal yet'
    }
    $json = $meta | ConvertTo-Json -Depth 3
    # CreateNew semantics; will not overwrite any existing metadata.
    $f = [IO.File]::Open($manifestPath, [IO.FileMode]::CreateNew,
                         [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $dataBytes = [Text.Encoding]::UTF8.GetBytes($json)
        $f.Write($dataBytes, 0, $dataBytes.Length)
        $f.Flush($true)
    } finally {
        $f.Dispose()
    }
    Write-Host "Created metadata: $manifestPath"
}
Write-Host "READY: code=$actualSource; data=$data; E: free $([math]::Round([double]$volume.FreeSpace/1GB,1)) GiB"
