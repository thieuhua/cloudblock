[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Dll)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root='E:\thieuhua\cloudblock'
$exe=Join-Path $root 'build\windows\cloudblock-windows.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Build first: $exe" }
if (-not [IO.Path]::IsPathRooted($Dll)) { throw 'DLL path must be absolute' }
$dllFull=[IO.Path]::GetFullPath($Dll)
if (-not (Test-Path -LiteralPath $dllFull -PathType Leaf) -or
    [IO.Path]::GetFileName($dllFull) -ine 'libwnbd.dll') {
    throw 'Use a verified libwnbd.dll with an absolute path'
}
$service=Get-Service -Name 'wnbd' -ErrorAction SilentlyContinue
if ($null -eq $service -or $service.Status -ne 'Running') {
    throw 'WNBD driver service is not installed/running. Refusing to map virtual disk.'
}
Write-Host 'WNBD driver detected; starting only CloudBlockLocal user-mode adapter.'
Write-Host 'This does not format or assign any drive letter.'
& $exe --wnbd-dll $dllFull
exit $LASTEXITCODE