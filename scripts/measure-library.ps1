# Measures how fast the application library answers on this machine: cold scan, refresh when nothing changed,
# steady-state request latency and the time to the first catalog after a restart. It runs a private service
# instance (build with -Instances) and only reads the machine. Needs an elevated shell.
param(
    [string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build/work'),
    [int] $Samples = 15,
    [string] $Output = ''
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandArgumentPassing = 'Standard'
$root = 'C:\HgMeasure'; $bin = "$root\bin"; $data = "$root\data"
$env:HOUSEGUEST_INSTANCE = 'hgmeasure'
$env:HOUSEGUEST_PIN = '482913'
$cli = "$bin\houseguest-cli.exe"
$body = '{"profile_id":"builtin-guest"}'

function Call($name, $arguments = '{}') { & $cli call $name $arguments 2>&1 | Out-String | ConvertFrom-Json }
function Seconds($block) { $watch = [Diagnostics.Stopwatch]::StartNew(); & $block | Out-Null; $watch.Elapsed.TotalSeconds }
function WaitIdle { for ($i = 0; $i -lt 400; $i++) { $v = Call 'apps.library' $body; if (-not $v.scanning) { return $v }; Start-Sleep -Milliseconds 100 } throw 'scan did not finish' }
function Start-Instance { Start-Process "$bin\houseguest-service.exe" -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden }

$service = $null
try {
    Get-Process houseguest-service -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $bin | Out-Null
    Copy-Item "$Build\service\houseguest-service.exe", "$Build\service\houseguest-cli.exe" $bin

    $service = Start-Instance
    Start-Sleep -Seconds 3
    & $cli setup | Out-Null
    $result = [ordered]@{}

    $overhead = (1..$Samples | ForEach-Object { Seconds { Call 'status' } } | Measure-Object -Average).Average * 1000
    $result.cli_overhead_ms = [math]::Round($overhead, 1)

    $result.cold_scan_s = [math]::Round((Seconds { Call 'apps.library' $body; WaitIdle }), 2)
    $library = Call 'apps.library' $body
    $result.entries = @($library.apps).Count

    $result.cold_scan_service_ms = (Call 'apps.sources').scan_ms
    $result.refresh_nothing_changed_s = [math]::Round((Seconds { Call 'apps.refresh'; Start-Sleep -Milliseconds 200; WaitIdle }), 2)
    $result.refresh_nothing_changed_service_ms = (Call 'apps.sources').scan_ms

    $latency = 1..$Samples | ForEach-Object { Seconds { Call 'apps.library' $body } }
    $result.library_request_ms = [math]::Round(($latency | Measure-Object -Average).Average * 1000 - $overhead, 1)

    $diagnostics = Call 'diagnostics'
    $result.apps_library = @($diagnostics.commands | Where-Object { $_.cmd -eq 'apps.library' })[0]
    $result.library_cache = $diagnostics.caches.library

    $service.Refresh()
    $result.service_working_set_mb = [math]::Round($service.WorkingSet64 / 1MB, 1)
    $result.service_private_mb = [math]::Round($service.PrivateMemorySize64 / 1MB, 1)

    Stop-Process $service -Force; $service.WaitForExit()
    $restart = [Diagnostics.Stopwatch]::StartNew()
    $service = Start-Instance
    do { Start-Sleep -Milliseconds 50; $again = try { Call 'apps.library' $body } catch { $null } } until ($again -and @($again.apps).Count -gt 0)
    $result.restart_to_first_catalog_s = [math]::Round($restart.Elapsed.TotalSeconds, 2)
    $result.restart_entries = @($again.apps).Count
    $null = WaitIdle
    $result.restart_refresh_done_s = [math]::Round($restart.Elapsed.TotalSeconds, 2)

    $json = $result | ConvertTo-Json
    $json
    if ($Output) { Set-Content $Output $json }
}
finally {
    Get-Process houseguest-service -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
}
