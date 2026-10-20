# Build CloudBlock using CMake+Ninja without installing or configuring any drivers.
[CmdletBinding()]
param([switch]$WNBD, [switch]$SkipTests)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$expected='E:\thieuhua\cloudblock'
if (-not [string]::Equals($repo.TrimEnd('\'),$expected,[StringComparison]::OrdinalIgnoreCase)) {
    throw "Source must be at $expected; currently $repo"
}
foreach($cmd in 'cmake','ninja') {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { throw "$cmd not found in PATH" }
}
& (Join-Path $PSScriptRoot 'prepare.ps1')

$build=Join-Path $repo 'build\windows'
$argsList=@('-S',$repo,'-B',$build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release')
if ($WNBD) {
    $include=Join-Path $repo 'third_party\wnbd\include'
    if (-not (Test-Path (Join-Path $include 'wnbd.h'))) {
        throw 'WNBD SDK missing; run .\scripts\windows\fetch-wnbd-sdk.ps1 first'
    }
    $argsList+=@('-DCLOUDBLOCK_BUILD_WNBD=ON',"-DWNBD_INCLUDE_DIR=$include")
} else {
    $argsList+= '-DCLOUDBLOCK_BUILD_WNBD=OFF'
}
& cmake @argsList
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
& cmake --build $build --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
if (-not $SkipTests) {
    & ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed: $LASTEXITCODE" }
}
$binary=Join-Path $build 'cloudblock.exe'
if (-not (Test-Path -LiteralPath $binary)) { throw "Expected CLI not found: $binary" }
Write-Host 'Build complete.'
& $binary 'E:\CloudBlock' status
if ($LASTEXITCODE -ne 0) { throw 'CloudBlock CLI status failed' }
if ($WNBD) {
    $driver=Join-Path $build 'cloudblock-windows.exe'
    if (-not (Test-Path -LiteralPath $driver)) { throw 'Expected WNBD adapter not found' }
    Write-Host "WNBD adapter compiled: $driver (driver NOT installed / disk NOT mounted)"
}
