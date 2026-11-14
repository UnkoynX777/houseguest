# Isolated end-to-end check of continuity and scoped dependencies. Requires an elevated shell and a build made with
# -DHOUSEGUEST_INSTANCES=ON. It runs a private service instance beside any installed one, in audit-only mode,
# so nothing is ended: it watches what the policy would block. Files live under C:\HgTest and are removed afterwards.
param([string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build/work'))
$ErrorActionPreference = 'Stop'
$PSNativeCommandArgumentPassing = 'Standard'
$root = 'C:\HgTest'; $bin = "$root\bin"; $apps = "$root\apps"; $data = "$root\data"
$env:HOUSEGUEST_INSTANCE = 'hgtest'
$env:HOUSEGUEST_PIN = '482913'
$results = @()
function Check($name, $ok) { $script:results += [pscustomobject]@{ Check = $name; Passed = [bool]$ok }; Write-Host ('{0,-5} {1}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name) }
function Hg { & "$bin\houseguest-cli.exe" @args 2>&1 | Out-String }
function HgJson { Hg @args | ConvertFrom-Json }
function Events($event) { (HgJson call audit.list '{"limit":1000}') | Where-Object { $_.event -eq $event } }
function Sha($p) { (Get-FileHash $p -Algorithm SHA256).Hash.ToLower() }

$service = $null; $hostProc = $null
try {
    Get-Process houseguest-service -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $bin, $apps | Out-Null
    icacls $root /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' | Out-Null
    Copy-Item "$Build\service\houseguest-service.exe", "$Build\service\houseguest-cli.exe", "$Build\host\houseguest.exe" $bin
    Copy-Item "$Build\host\ui" "$bin\ui" -Recurse -ErrorAction SilentlyContinue
    Copy-Item C:\Windows\System32\cmd.exe "$apps\launcher.exe"
    Copy-Item C:\Windows\System32\PING.EXE "$apps\child.exe"
    Copy-Item C:\Windows\System32\PING.EXE "$apps\renamed.exe"
    Copy-Item C:\Windows\System32\PING.EXE "$apps\other.exe"
    [IO.File]::AppendAllText("$apps\other.exe", 'x')
    icacls $root /setowner '*S-1-5-32-544' /T /C | Out-Null

    $service = Start-Process "$bin\houseguest-service.exe" -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 3
    Check 'isolated service answers' ((HgJson status).version)
    Check 'supervisor runs on ETW events' ((HgJson status).enforcement -eq 'supervised')
    Hg setup | Out-Null

    $me = (HgJson call users.list) | Where-Object { $_.owner }
    $app = @{
        id = 'app-test'; name = 'Test launcher'; mode = 'approved'; unknown = 'review'; priority = 50
        components = @(
            @{ id = 'main'; role = 'main'; path = "$apps\launcher.exe"; sha256 = (Sha "$apps\launcher.exe") },
            @{ id = 'dep'; role = 'helper'; scope = 'descendant'; path = "$apps\child.exe"; sha256 = (Sha "$apps\child.exe") }
        )
    }
    $saved = HgJson call apps.save (@{ profile_id = 'builtin-restricted'; app = $app } | ConvertTo-Json -Depth 6 -Compress)
    Check 'application with a pinned dependency is saved' ($saved.app.id -eq 'app-test')

    $preview = HgJson call policy.preview (@{ profile_id = 'builtin-restricted'; guest_sids = @($me.sid) } | ConvertTo-Json -Compress)
    Check 'preview reports continuity facts' ($preview.continuity.install_trusted -eq $true)

    $start = @{ profile_id = 'builtin-restricted'; guest_sids = @($me.sid); duration_minutes = 20; audit_only = $true; confirm_warnings = $true } | ConvertTo-Json -Compress
    $session = Hg call session.start $start
    Check 'audit-only session starts for the owner account' ($session -match '"armed": true')
    Start-Sleep -Seconds 4

    $before = @(Events 'app.would_block').Count
    Write-Host "baseline would_block events after the initial sweep: $before"

    $hostProc = Start-Process "$bin\houseguest.exe" -PassThru
    Start-Sleep -Seconds 12
    $children = Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'msedgewebview2.exe' -and $_.CommandLine -like '*Houseguest*' }
    Write-Host ("Houseguest WebView2 processes running: {0}" -f @($children).Count)
    Check 'the interface started its WebView2 processes' (@($children).Count -ge 3)

    $chain = "$apps\child.exe -n 3 127.0.0.1 >nul & $apps\other.exe -n 3 127.0.0.1 >nul & $apps\renamed.exe -n 3 127.0.0.1 >nul"
    Start-Process "$apps\launcher.exe" -ArgumentList '/c', $chain -Wait -WindowStyle Hidden
    Start-Process "$apps\child.exe" -ArgumentList '-n', '3', '127.0.0.1' -Wait -WindowStyle Hidden
    Start-Sleep -Seconds 4

    $blocked = Events 'app.would_block'
    $paths = @($blocked | ForEach-Object { $_.detail.path.ToLower() })
    $mine = @($blocked | Where-Object { $_.detail.path -like "$apps*" })
    $names = @($mine | ForEach-Object { (Split-Path $_.detail.path -Leaf) })
    Write-Host ("would_block under test folder: {0}" -f ($names -join ', '))

    Write-Host ($preview.continuity | ConvertTo-Json -Compress)
    $blocked | Where-Object { $_.detail.path -like '*webview*' -or $_.detail.path -like "$apps*" } | ForEach-Object { Write-Host ("  blocked: {0} | parent={1} | reason={2} note={3}" -f $_.detail.path, $_.detail.parent, $_.detail.reason, $_.detail.note) }
    Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" | ForEach-Object { $p = Get-CimInstance Win32_Process -Filter "ProcessId=$($_.ParentProcessId)"; Write-Host ("  webview pid={0} parent={1} ({2})" -f $_.ProcessId, $_.ParentProcessId, $p.Name) }
    Check 'Houseguest host is not blocked' (-not ($paths | Where-Object { $_ -like "$($bin.ToLower())*" }))
    Check 'WebView2 processes are not blocked' (-not ($paths | Where-Object { $_ -like '*msedgewebview2*' }))
    Check 'explorer, svchost, conhost, RuntimeBroker are not blocked' (-not ($paths | Where-Object { $_ -match '\\(explorer|svchost|conhost|runtimebroker|sihost|taskhostw|ctfmon|dllhost)\.exe$' }))
    Check 'the selected executable runs' (-not ($names -contains 'launcher.exe'))
    Check 'an approved dependency runs below its application' ($mine.Count -ge 1 -and -not ($mine | Where-Object { $_.detail.path -like '*child.exe' -and $_.detail.parent -like '*launcher.exe' }))
    Check 'an unapproved program started by the application is blocked' ($names -contains 'other.exe')
    $other = $mine | Where-Object { $_.detail.path -like '*other.exe' } | Select-Object -First 1
    Check 'the block names the application and recommends review' ($other.detail.related_app -eq 'app-test' -and $other.detail.recommendation -eq 'approve_dependency')
    Check 'a renamed copy with the same hash is blocked' ($names -contains 'renamed.exe')
    Check 'the approved dependency started on its own is blocked' (($mine | Where-Object { $_.detail.path -like '*child.exe' -and $_.detail.parent -notlike '*launcher.exe' }).Count -ge 1)

    $reviews = HgJson call reviews.list
    Check 'unknown dependency is queued for review' (@($reviews | Where-Object { $_.path -like '*other.exe' }).Count -ge 1)
    $explain = HgJson call apps.explain (@{ profile_id = 'builtin-restricted'; path = "$apps\launcher.exe" } | ConvertTo-Json -Compress)
    Check 'diagnosis is built from real data' ($explain.decision.action -eq 'allow' -and $explain.summary.approved -ge 1 -and $explain.summary.pending -ge 1)

    $diag = HgJson call diagnostics
    Write-Host ("lineage records: {0}; assessments avg {1:N0} us p95 {2:N0} us; spared {3}" -f $diag.supervisor.lineage_records, $diag.supervisor.assessment_us.average, $diag.supervisor.assessment_us.p95, $diag.supervisor.protection_spared)
    Check 'lineage tracked the verified Houseguest tree' ($diag.supervisor.lineage_records -ge 1)

    $seq = (HgJson call audit.list '{"limit":1}')[0].seq
    $service.Kill(); $service.WaitForExit()
    Start-Sleep 2
    $service = Start-Process "$bin\houseguest-service.exe" -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden
    Start-Sleep 10
    Check 'the session resumes after a service restart' ((HgJson status).armed)
    $stored = (HgJson call profiles.list) | Where-Object { $_.id -eq 'builtin-restricted' }
    Check 'the saved application and its dependency survive a restart' (($stored.apps | Where-Object { $_.id -eq 'app-test' }).components.Count -eq 2)
    $later = @(Events 'app.would_block' | Where-Object { $_.seq -gt $seq })
    Write-Host ("would_block events after restart: {0}" -f $later.Count)
    $laterPaths = @($later | ForEach-Object { $_.detail.path.ToLower() })
    Check 'a restart does not block the running interface or WebView2' (-not ($laterPaths | Where-Object { $_ -like "$($bin.ToLower())*" -or $_ -like '*msedgewebview2*' }))
    Check 'a restart does not block the approved dependency started before it' (-not ($laterPaths | Where-Object { $_ -like '*launcher.exe' }))

    $stop = Hg call session.stop
    Check 'the owner can end the session' ((HgJson status).armed -eq $false)
    $seq2 = (HgJson call audit.list '{"limit":1}')[0].seq
    Start-Sleep 3
    Start-Process "$apps\other.exe" -ArgumentList '-n', '2', '127.0.0.1' -Wait -WindowStyle Hidden
    Check 'nothing is evaluated once the session has ended' (-not (@(Events 'app.would_block' | Where-Object { $_.seq -gt $seq2 }).Count))
}
finally {
    if ($hostProc -and -not $hostProc.HasExited) { $hostProc.Kill() }
    Get-Process msedgewebview2 -ErrorAction SilentlyContinue | Where-Object { $_.Id -and (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)").CommandLine -like '*HgTest*' } | Stop-Process -Force -ErrorAction SilentlyContinue
    if ($service -and -not $service.HasExited) { $service.Kill() }
    Start-Sleep 1
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
    $results | Format-Table -AutoSize
    if ($results | Where-Object { -not $_.Passed }) { exit 1 }
}
