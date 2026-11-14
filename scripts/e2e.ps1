# Local end-to-end check of enforcement. Requires an elevated shell and a built tree.
# Creates a temporary local account and removes it afterwards. Does not install the service.
param([string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\debug'))
$ErrorActionPreference = 'Stop'

$service = Join-Path $Build 'service\houseguest-service.exe'
$cli = Join-Path $Build 'service\houseguest-cli.exe'
$data = Join-Path $env:TEMP 'houseguest-e2e'
$guestName = 'hgguest' + (Get-Random -Maximum 9999)
$guestPassword = [Guid]::NewGuid().ToString('N') + 'Aa1!'
$env:HOUSEGUEST_PIN = '482913'
$results = @()
$proc = $null

function Check($name, $condition) {
    $script:results += [pscustomobject]@{ Check = $name; Passed = [bool]$condition }
    Write-Host ("{0,-6} {1}" -f $(if ($condition) { 'PASS' } else { 'FAIL' }), $name)
}
function Hg { & $cli @args 2>&1 | Out-String }
function HgJson { $out = Hg @args; $out | ConvertFrom-Json }

try {
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
    $proc = Start-Process $service -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2

    $status = HgJson status
    Check 'service answers status' ($status.version -and -not $status.configured)
    Check 'supervisor is running' ($status.enforcement -eq 'supervised')

    Hg setup | Out-Null
    Check 'setup configures the owner' ((HgJson status).configured)

    $bad = Hg call profiles.list
    $env:HOUSEGUEST_PIN = '000000'
    $wrong = Hg call profiles.list
    $env:HOUSEGUEST_PIN = '482913'
    Check 'wrong PIN is rejected' ($wrong -match 'invalid_pin')

    net user $guestName $guestPassword /add /y | Out-Null
    $users = (Hg call users.list) | ConvertFrom-Json
    $guestSid = ($users | Where-Object name -eq $guestName).sid
    Check 'guest account is listed' ([bool]$guestSid)

    $profile = @{ profile = @{
        id = 'e2e'; name = 'E2E'; default_action = 'deny'
        rules = @(@{ id = 'ping'; action = 'allow'; priority = 0; match = @{ type = 'path'; value = '%SystemRoot%\System32\ping.exe' } })
    } } | ConvertTo-Json -Depth 6 -Compress
    $saved = Hg call profiles.save $profile
    Check 'profile is saved' ($saved -match '"id": "e2e"')

    $start = @{ profile_id = 'e2e'; guest_sids = @($guestSid); duration_minutes = 5; confirm_warnings = $true } | ConvertTo-Json -Compress
    $started = Hg call session.start $start
    Check 'session starts' ($started -match '"armed": true')

    $credential = New-Object System.Management.Automation.PSCredential($guestName, (ConvertTo-SecureString $guestPassword -AsPlainText -Force))
    $allowed = Start-Process "$env:SystemRoot\System32\ping.exe" -ArgumentList '-n', '30', '127.0.0.1' -Credential $credential -PassThru -WindowStyle Hidden
    $denied = Start-Process "$env:SystemRoot\System32\choice.exe" -ArgumentList '/T', '30', '/D', 'y' -Credential $credential -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 4
    $allowed.Refresh(); $denied.Refresh()
    Check 'allowed application keeps running' (-not $allowed.HasExited)
    Check 'denied application is terminated' ($denied.HasExited)

    $audit = (Hg call audit.list '{"limit":20}') | ConvertFrom-Json
    Check 'denial is recorded in the audit log' ([bool]($audit | Where-Object { $_.event -eq 'app.blocked' -and $_.detail.path -like '*choice.exe' }))
    Check 'audit hash chain verifies' ((Hg call audit.verify) -match '"valid": true')

    $burst = 1..12 | ForEach-Object {
        Start-Process "$env:SystemRoot\System32\choice.exe" -ArgumentList '/T', '30', '/D', 'y' -Credential $credential -PassThru -WindowStyle Hidden
    }
    Start-Sleep -Seconds 8
    $survivors = @($burst | ForEach-Object { $_.Refresh(); $_ } | Where-Object { -not $_.HasExited })
    Check 'twelve simultaneous denied launches are all terminated' ($survivors.Count -eq 0)

    $diag = (Hg call diagnostics) | ConvertFrom-Json
    Check 'diagnostics reports event-based monitoring' ($diag.supervisor.mode -eq 'supervised')
    Check 'capability probe reports a ready system' ($diag.capabilities.ready)
    Check 'stop latency was measured' ($diag.supervisor.latency_ms.samples -ge 1)
    Write-Host ("       stop latency over {0} samples: average {1:N0} ms, p95 {2:N0} ms, max {3:N0} ms" -f $diag.supervisor.latency_ms.samples, $diag.supervisor.latency_ms.average, $diag.supervisor.latency_ms.p95, $diag.supervisor.latency_ms.max)

    logman stop HouseguestProcessTrace -ets | Out-Null
    Start-Sleep -Seconds 8
    Check 'losing the event session switches to polling' ((HgJson status).enforcement -eq 'polling')
    $late = Start-Process "$env:SystemRoot\System32\choice.exe" -ArgumentList '/T', '30', '/D', 'y' -Credential $credential -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 4
    $late.Refresh()
    Check 'polling mode still terminates denied launches' ($late.HasExited)
    $recovered = $false
    for ($i = 0; $i -lt 30 -and -not $recovered; $i++) { Start-Sleep -Seconds 2; $recovered = ((HgJson status).enforcement -eq 'supervised') }
    Check 'the event session is restored automatically' $recovered

    Hg call session.stop | Out-Null
    Check 'session stops' (-not (HgJson status).armed)
    if (-not $allowed.HasExited) { Stop-Process -Id $allowed.Id -Force -ErrorAction SilentlyContinue }
}
finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    net user $guestName /delete 2>&1 | Out-Null
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item Env:HOUSEGUEST_PIN -ErrorAction SilentlyContinue
}

if ($results | Where-Object { -not $_.Passed }) { exit 1 }
Write-Host "All $($results.Count) checks passed."

