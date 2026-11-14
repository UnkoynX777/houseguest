param(
    [int] $Seconds = 30,
    [string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\fuzz')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$runtime = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$env:PATH = (Join-Path $runtime.FullName 'bin\Hostx64\x64') + ';' + $env:PATH

$failed = @()
foreach ($target in 'policy', 'glob', 'state', 'ipc', 'audit') {
    $exe = Join-Path $Build "fuzz\fuzz_$target.exe"
    $work = Join-Path $env:TEMP "houseguest-fuzz-$target"
    $artifacts = Join-Path $env:TEMP "houseguest-fuzz-$target-crashes"
    Remove-Item $work, $artifacts -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $work, $artifacts | Out-Null
    $seeds = Join-Path $root "fuzz\corpus\$target"
    Write-Host "== fuzz_$target ($Seconds s)"
    & $exe "-max_total_time=$Seconds" '-timeout=20' '-rss_limit_mb=2048' "-artifact_prefix=$artifacts\" '-print_final_stats=1' $work $seeds 2>&1 |
        Select-String -Pattern 'Done|stat::|ERROR|SUMMARY|crash|slow-unit' | ForEach-Object { $_.Line }
    if ($LASTEXITCODE -ne 0) { $failed += $target }
}
if ($failed.Count) { Write-Host "Fuzzing found problems in: $($failed -join ', '). Inputs are in $env:TEMP\houseguest-fuzz-*-crashes."; exit 1 }
Write-Host 'Fuzzing finished without findings.'
