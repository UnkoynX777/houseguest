param(
    [string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release'),
    [int] $Seconds = 60
)
$ErrorActionPreference = 'Stop'
$service = Join-Path $Build 'service\houseguest-service.exe'
$cli = Join-Path $Build 'service\houseguest-cli.exe'
$host_ = Join-Path $Build 'host\houseguest.exe'
$data = Join-Path $env:TEMP 'houseguest-measure'
$env:HOUSEGUEST_PIN = '482913'
$guest = 'hgmeasure' + (Get-Random -Maximum 999)
$password = [Guid]::NewGuid().ToString('N') + 'Aa1!'

function Sample([System.Diagnostics.Process] $process, [int] $seconds) {
    $process.Refresh()
    $start = $process.TotalProcessorTime
    Start-Sleep -Seconds $seconds
    $process.Refresh()
    $cpu = ($process.TotalProcessorTime - $start).TotalSeconds / $seconds / [Environment]::ProcessorCount * 100
    [pscustomobject]@{ WorkingSetMB = [math]::Round($process.WorkingSet64 / 1MB, 1); PrivateMB = [math]::Round($process.PrivateMemorySize64 / 1MB, 1); CpuPercent = [math]::Round($cpu, 3) }
}

$proc = $null
$ui = $null
try {
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
    $proc = Start-Process $service -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 3
    $idle = Sample $proc $Seconds
    & $cli setup | Out-Null
    net user $guest $password /add /y | Out-Null
    $sid = ((& $cli call users.list | ConvertFrom-Json) | Where-Object name -eq $guest).sid
    $profileId = (& $cli call profiles.list | ConvertFrom-Json)[0].id
    $start = @{ profile_id = $profileId; guest_sids = @($sid); duration_minutes = 30; confirm_warnings = $true } | ConvertTo-Json -Compress
    & $cli call session.start $start | Out-Null
    $active = Sample $proc $Seconds

    Write-Host "Service, no session:     $($idle.WorkingSetMB) MB working set, $($idle.PrivateMB) MB private, $($idle.CpuPercent) % CPU"
    Write-Host "Service, active session: $($active.WorkingSetMB) MB working set, $($active.PrivateMB) MB private, $($active.CpuPercent) % CPU"

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $ui = Start-Process $host_ -PassThru
    while (-not $ui.MainWindowHandle -or $ui.MainWindowHandle -eq 0) { Start-Sleep -Milliseconds 20; $ui.Refresh() }
    Write-Host "Interface window visible after $($timer.ElapsedMilliseconds) ms"
    Start-Sleep -Seconds 6
    $children = Get-CimInstance Win32_Process | Where-Object { $_.ParentProcessId -eq $ui.Id -or $_.Name -eq 'msedgewebview2.exe' -and $_.CommandLine -like '*Houseguest*' }
    $webview = ($children | Measure-Object -Property WorkingSetSize -Sum).Sum / 1MB
    $ui.Refresh()
    Write-Host ("Interface host: {0:N1} MB working set; WebView2 processes started by it: {1:N1} MB" -f ($ui.WorkingSet64 / 1MB), $webview)
}
finally {
    if ($ui -and -not $ui.HasExited) { $ui.Kill() }
    if ($proc -and -not $proc.HasExited) { $proc.Kill() }
    net user $guest /delete 2>&1 | Out-Null
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
}
