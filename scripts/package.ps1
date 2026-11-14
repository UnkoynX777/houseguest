# Builds the Release tree and produces the MSI, a portable zip and SHA-256 checksums in dist\.
param(
    [string] $Version = '0.2.0',
    [string[]] $ExtraFiles = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'dist'
$build = Join-Path $root 'build\release'

Push-Location (Join-Path $root 'ui')
try {
    npm ci --no-audit --no-fund
    npm run build
}
finally { Pop-Location }

& (Join-Path $PSScriptRoot 'build.ps1') -Config Release -RunTests

Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $out | Out-Null

$env:PATH += ";$env:USERPROFILE\.dotnet\tools"
$msi = Join-Path $out "Houseguest-$Version-x64.msi"
$uiDist = Join-Path $root 'ui\dist'
$source = Join-Path $root 'installer\Houseguest.wxs'
wix extension add -g WixToolset.Util.wixext/5.0.2 | Out-Null
wix build $source -arch x64 -ext WixToolset.Util.wixext -d Version=$Version -d BuildDir=$build -d RepoRoot=$root -d UiDist=$uiDist -o $msi
if ($LASTEXITCODE -ne 0) { throw 'wix build failed' }

$stage = Join-Path $out 'portable\Houseguest'
New-Item -ItemType Directory $stage -Force | Out-Null
Copy-Item "$build\service\houseguest-service.exe", "$build\service\houseguest-cli.exe", "$build\host\houseguest.exe", "$root\LICENSE" $stage
Copy-Item (Join-Path $root 'ui\dist') (Join-Path $stage 'ui') -Recurse
Compress-Archive -Path $stage -DestinationPath (Join-Path $out "Houseguest-$Version-x64-portable.zip")
Remove-Item (Join-Path $out 'portable') -Recurse -Force

foreach ($extra in $ExtraFiles) { Copy-Item $extra $out }
Remove-Item (Join-Path $out '*.wixpdb') -ErrorAction SilentlyContinue
$sums = Get-ChildItem $out -File | ForEach-Object {
    "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)"
}
[IO.File]::WriteAllText((Join-Path $out 'SHA256SUMS.txt'), (($sums -join "`n") + "`n"))
$sums
