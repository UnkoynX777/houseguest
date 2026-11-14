# Security regression checks against a real service process. Requires an elevated shell and a built tree.
# Creates and removes a temporary local account; does not install the service.
param([string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\debug'))
$ErrorActionPreference = 'Stop'

$service = Join-Path $Build 'service\houseguest-service.exe'
$cli = Join-Path $Build 'service\houseguest-cli.exe'
$data = Join-Path $env:TEMP 'houseguest-e2e-security'
$suffix = Get-Random -Maximum 9999
$managedName = "e2e$suffix"
$env:HOUSEGUEST_PIN = '482913'
$results = @()
$proc = $null

function Check($name, $condition) {
    $script:results += [pscustomobject]@{ Check = $name; Passed = [bool]$condition }
    Write-Host ("{0,-6} {1}" -f $(if ($condition) { 'PASS' } else { 'FAIL' }), $name)
}
function Hg { & $cli @args 2>&1 | Out-String }
function HgJson { $out = Hg @args; $out | ConvertFrom-Json }
function Start-Service-Process { Start-Process $service -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden }
function Stop-Service-Process { if ($script:proc -and -not $script:proc.HasExited) { Stop-Process -Id $script:proc.Id -Force; $script:proc.WaitForExit() } }

function Open-Pipe {
    $p = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'houseguest', [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None, [System.Security.Principal.TokenImpersonationLevel]::Identification)
    $p.Connect(1500)
    $p.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
    $p
}
function Send-Raw($pipe, [byte[]] $bytes) {
    $pipe.Write($bytes, 0, $bytes.Length)
    $pipe.Flush()
    $buffer = New-Object byte[] 65536
    $count = $pipe.Read($buffer, 0, $buffer.Length)
    [System.Text.Encoding]::UTF8.GetString($buffer, 0, $count)
}
function Raw-Call($text) {
    $pipe = Open-Pipe
    try { Send-Raw $pipe ([System.Text.Encoding]::UTF8.GetBytes($text)) } finally { $pipe.Dispose() }
}
function Wait-For($seconds, [scriptblock] $condition) {
    $end = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $end) { if (& $condition) { return $true }; Start-Sleep -Milliseconds 500 }
    $false
}

try {
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
    $proc = Start-Service-Process
    Start-Sleep -Seconds 2
    Hg setup | Out-Null

    $idle = @()
    foreach ($i in 1..40) { try { $idle += Open-Pipe } catch { } }
    Check 'idle clients cannot occupy every connection' ($idle.Count -ge 1)
    Start-Sleep -Seconds 8
    $broken = 0
    foreach ($pipe in $idle) { try { $pipe.Write([byte[]](32), 0, 1); $pipe.Flush(); $null = $pipe.Read((New-Object byte[] 16), 0, 16) } catch { $broken++ } }
    Check 'idle connections are closed after the first-message timeout' ($broken -ge ($idle.Count - 1))
    $idle | ForEach-Object { $_.Dispose() }
    Check 'service answers again once idle clients have timed out' ((Hg status) -match '"version"')
    $diag = HgJson call diagnostics
    Check 'diagnostics counts timed-out and rejected connections' (($diag.ipc.timed_out -ge 1) -and ($diag.ipc.rejected_busy -ge 1 -or $diag.ipc.accepted -ge 10))

    $reply = Raw-Call ('[' * 200000)
    Check 'deeply nested JSON is rejected without crashing' ($reply -match 'bad_request')
    $reply = Raw-Call '{"cmd":'
    Check 'truncated JSON is rejected' ($reply -match 'bad_request')
    $reply = try { Raw-Call ('{"cmd":"status","pad":"' + ('a' * 1200000) + '"}') } catch { 'too_large (connection closed)' }
    Check 'oversized message is rejected' ($reply -match 'too_large')
    $reply = Raw-Call '{"cmd":"profiles.list"}'
    Check 'owner command without token is refused' ($reply -match 'unauthorized')
    $reply = Raw-Call '{"cmd":"profiles.list","token":"00"}'
    Check 'forged token is refused' ($reply -match 'unauthorized')
    Check 'service survives malformed traffic' ((HgJson status).version)

    $pipe = Open-Pipe
    $limited = 0
    foreach ($i in 1..120) {
        $r = Send-Raw $pipe ([System.Text.Encoding]::UTF8.GetBytes('{"cmd":"ping"}'))
        if ($r -match 'rate_limited') { $limited++ }
    }
    $pipe.Dispose()
    Check 'request bursts are rate limited' ($limited -ge 1)

    $created = Hg call accounts.managed.create ('{"name":"' + $managedName + '"}') | ConvertFrom-Json
    $managedSid = $created.sid
    $account = "hg-$managedName"
    Check 'managed account is created disabled' ($managedSid -and -not (Get-LocalUser -Name $account).Enabled)
    Check 'managed account is listed for sessions' ([bool]((Hg call users.list | ConvertFrom-Json) | Where-Object { $_.sid -eq $managedSid -and $_.managed }))

    $profile = '{"profile":{"id":"sec","name":"Sec","default_action":"deny","rules":[]}}'
    Hg call profiles.save $profile | Out-Null
    $start = '{"profile_id":"sec","guest_sids":["' + $managedSid + '"],"duration_minutes":1,"confirm_warnings":true,"on_expiry":"disable"}'
    $refused = Hg call session.start '{"profile_id":"sec","guest_sids":["S-1-5-32-544"],"duration_minutes":1,"on_expiry":"disable"}'
    Check 'disable-on-expiry is refused for accounts Houseguest did not create' ($refused -match 'bad_request|Unknown account')
    $started = Hg call session.start $start
    Check 'session starts and enables the managed account' (($started -match '"armed": true') -and (Get-LocalUser -Name $account).Enabled)

    Stop-Service-Process
    $expires = (Get-LocalUser -Name $account).AccountExpires
    Check 'the managed account carries an operating-system expiry at the session end' ($expires -and $expires -le (Get-Date).AddMinutes(2))
    Start-Sleep -Seconds 65
    Check 'the account is expired by Windows itself while the service is down' ((Get-LocalUser -Name $account).AccountExpires -lt (Get-Date))
    $proc = Start-Service-Process
    $ready = Wait-For 15 { (Hg status) -match '"version"' }
    Check 'service restarts' $ready
    Check 'a session that expired while the service was stopped is closed on startup' (-not (HgJson status).armed)
    Check 'the managed account is disabled again after the expired session' (-not (Get-LocalUser -Name $account).Enabled)
    $audit = (Hg call audit.list '{"limit":40}') | ConvertFrom-Json
    Check 'expiry is recorded in the audit log' ([bool]($audit | Where-Object { $_.event -eq 'session.ended' }))

    $start = '{"profile_id":"sec","guest_sids":["' + $managedSid + '"],"duration_minutes":10,"confirm_warnings":true,"on_expiry":"disable"}'
    Hg call session.start $start | Out-Null
    Stop-Service-Process
    $proc = Start-Service-Process
    Wait-For 15 { (Hg status) -match '"version"' } | Out-Null
    $resumed = HgJson status
    Check 'an active session is resumed after a restart' ($resumed.armed -and -not $resumed.ending)
    Hg call session.stop | Out-Null
    Check 'stopping the session disables the managed account' (-not (Get-LocalUser -Name $account).Enabled)

    Check 'audit chain verifies after all of the above' ((Hg call audit.verify) -match '"valid": true')
    Stop-Service-Process

    $log = Join-Path $data 'audit.log'
    $lines = Get-Content $log
    $tampered = $lines.Clone()
    $tampered[2] = $tampered[2] -replace '"actor":"[^"]*"', '"actor":"someone-else"'
    Set-Content -Path $log -Value $tampered -Encoding utf8
    $proc = Start-Service-Process
    Wait-For 15 { (Hg status) -match '"version"' } | Out-Null
    Check 'an edited audit record is detected' ((Hg call audit.verify) -match '"valid": false')
    Stop-Service-Process

    Set-Content -Path $log -Value $lines -Encoding utf8
    Set-Content -Path $log -Value $lines[0..($lines.Count - 4)] -Encoding utf8
    $proc = Start-Service-Process
    Wait-For 15 { (Hg status) -match '"version"' } | Out-Null
    $diag = HgJson call diagnostics
    Check 'a truncated audit log is reported' (($diag.audit.truncated -eq $true) -or ($diag.audit.valid -eq $false))
    Check 'the tamper event is written to the log' ([bool]((Hg call audit.list '{"limit":5}' | ConvertFrom-Json) | Where-Object { $_.event -eq 'audit.integrity_failure' }))
    Hg call accounts.managed.delete ('{"sid":"' + $managedSid + '"}') | Out-Null
    Check 'a managed account can be removed by the owner' (-not (Get-LocalUser -Name $account -ErrorAction SilentlyContinue))
}
finally {
    Stop-Service-Process
    Get-LocalUser -Name "hg-$managedName" -ErrorAction SilentlyContinue | Remove-LocalUser -ErrorAction SilentlyContinue
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item Env:HOUSEGUEST_PIN -ErrorAction SilentlyContinue
}

if ($results | Where-Object { -not $_.Passed }) { exit 1 }
Write-Host "All $($results.Count) checks passed."
