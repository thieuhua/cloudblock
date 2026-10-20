# Clone/verify pinned WNBD public headers. Does NOT install WNBD binaries or driver.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$pin='20e029d04df111a1a4ef12e98130a73491dee6d4'
$dest=Join-Path $repo 'third_party\wnbd'
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git not found in PATH' }
if (-not (Test-Path -LiteralPath $dest)) {
    New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force | Out-Null
    & git clone --filter=blob:none https://github.com/cloudbase/wnbd.git $dest
    if ($LASTEXITCODE -ne 0) { throw 'WNBD source clone failed' }
}
if (-not (Test-Path (Join-Path $dest '.git'))) {
    throw "Existing folder $dest is not a WNBD git checkout; refusing overwrite"
}
$remote=(& git -C $dest remote get-url origin).Trim()
if ($remote -notin @('https://github.com/cloudbase/wnbd.git',
                      'https://github.com/cloudbase/wnbd')) {
    throw "Unexpected WNBD remote: $remote"
}
$dirty = & git -C $dest status --porcelain
if ($dirty) { throw 'WNBD dependency directory is dirty; refusing checkout' }
& git -C $dest fetch --no-tags origin $pin
if ($LASTEXITCODE -ne 0) { throw 'WNBD pinned commit fetch failed' }
& git -C $dest checkout --detach $pin
if ($LASTEXITCODE -ne 0) { throw 'WNBD checkout failed' }
$head=(& git -C $dest rev-parse HEAD).Trim()
if ($head -ne $pin) { throw "Wrong WNBD revision: $head" }
$header=Join-Path $dest 'include\wnbd.h'
if (-not (Test-Path -LiteralPath $header)) { throw "Missing WNBD SDK header: $header" }
Write-Host "VERIFIED: WNBD SDK at $pin"
Write-Host "Include path: $(Join-Path $dest 'include')"
Write-Host 'No kernel driver or DLL installed.'
