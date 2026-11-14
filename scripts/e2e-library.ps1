# Isolated end-to-end check of the application library. Requires an elevated shell and a build made with
# -DHOUSEGUEST_INSTANCES=ON. It runs a private service instance beside any installed one and only reads the machine:
# nothing is ended or changed outside C:\HgLib, which is removed afterwards.
param([string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build/work'))
$ErrorActionPreference = 'Stop'
$PSNativeCommandArgumentPassing = 'Standard'
$root = 'C:\HgLib'; $bin = "$root\bin"; $portable = "$root\portable"; $data = "$root\data"
$env:HOUSEGUEST_INSTANCE = 'hglib'
$env:HOUSEGUEST_PIN = '482913'
$results = @()
function Check($name, $ok) { $script:results += [pscustomobject]@{ Check = $name; Passed = [bool]$ok }; Write-Host ('{0,-5} {1}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name) }
function Hg { & "$bin\houseguest-cli.exe" @args 2>&1 | Out-String }
function HgJson { Hg @args | ConvertFrom-Json }
function Json($value) { $value | ConvertTo-Json -Compress -Depth 5 }
function WaitScan { for ($i = 0; $i -lt 90; $i++) { $v = HgJson call apps.library (Json @{ profile_id = 'builtin-guest' }); if (-not $v.scanning) { return $v }; Start-Sleep 1 }; throw 'library scan did not finish' }

$service = $null
try {
    Get-Process houseguest-service -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $bin, "$portable\Alpha", "$portable\Beta" | Out-Null
    Copy-Item "$Build\service\houseguest-service.exe", "$Build\service\houseguest-cli.exe" $bin
    foreach ($name in 'Alpha\Alpha.exe', 'Alpha\unins000.exe', 'Beta\Beta.exe', 'Loose.exe') { Copy-Item C:\Windows\System32\find.exe "$portable\$name" }

    $service = Start-Process "$bin\houseguest-service.exe" -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 3
    Hg setup | Out-Null

    $first = HgJson call apps.library (Json @{ profile_id = 'builtin-guest' })
    Check 'the library answers immediately while it scans' ($null -ne $first.scanning)
    $library = WaitScan
    $apps = @($library.apps)
    Check 'the library finds installed programs' ($apps.Count -gt 20)

    $keys = $apps | ForEach-Object { $_.path.ToLower() }
    Check 'every executable is listed once' (($keys | Sort-Object -Unique).Count -eq $keys.Count)
    Check 'programs found by several sources are merged' (@($apps | Where-Object { $_.sources.Count -gt 1 }).Count -ge 1)
    Check 'tools and helpers are marked so they can be hidden' (@($apps | Where-Object { $_.role -ne 'application' }).Count -ge 1)

    $userPrograms = "$env:LOCALAPPDATA\Programs"
    $code = $apps | Where-Object { $_.path -like "$userPrograms\Microsoft VS Code\Code.exe" }
    if (Test-Path "$userPrograms\Microsoft VS Code\Code.exe") { Check 'Visual Studio Code installed for the user is listed' ($null -ne $code -and $code.role -eq 'application') }
    $perUser = @($apps | Where-Object { $_.path -like "$env:LOCALAPPDATA\*" -and $_.role -eq 'application' })
    Check 'programs installed for a single user are listed' ($perUser.Count -ge 1)
    $sources = HgJson call apps.sources
    Check 'every discovery source reports a status' (@($sources.sources | Where-Object { $_.scanned }).Count -ge 5)

    $diag = HgJson call apps.diagnose (Json @{ path = "$portable\Beta\Beta.exe" })
    Check 'diagnosis explains a program that is not listed' ($diag.verdict -eq 'not_detected')
    Check 'diagnosis recognises a missing file' ((HgJson call apps.diagnose (Json @{ path = "$portable\nope.exe" })).verdict -eq 'not_found')
    Check 'diagnosis recognises a folder' ((HgJson call apps.diagnose (Json @{ path = $portable })).verdict -eq 'folder')

    $scan = HgJson call apps.folder.scan (Json @{ path = $portable })
    $found = @($scan.apps | ForEach-Object { Split-Path $_.path -Leaf } | Sort-Object)
    Check 'a folder scan finds one program per sub folder and skips uninstallers' (($found -join ',') -eq 'Alpha.exe,Beta.exe,Loose.exe')

    Hg call apps.manual.add (Json @{ path = "$portable\Loose.exe" }) | Out-Null
    Hg call apps.portable.add (Json @{ path = $portable }) | Out-Null
    $library = WaitScan
    $loose = $library.apps | Where-Object { $_.path -like '*\portable\Loose.exe' }
    Check 'a program added by hand is listed as such' ($loose -and $loose.manual -and $loose.kind -eq 'manual')
    $alpha = $library.apps | Where-Object { $_.path -like '*\portable\Alpha\Alpha.exe' }
    Check 'a registered portable folder lists its programs as portable' ($alpha -and $alpha.kind -eq 'portable')
    Check 'the hand-added program is not duplicated by the folder' (@($library.apps | Where-Object { $_.path -like '*\portable\Loose.exe' }).Count -eq 1)
    Check 'diagnosis now finds the program' ((HgJson call apps.diagnose (Json @{ path = "$portable\Beta\Beta.exe" })).verdict -eq 'listed')

    $profile = (HgJson call profiles.list) | Where-Object { $_.id -eq 'builtin-guest' }
    $before = ($profile | ConvertTo-Json -Depth 8 -Compress).Length
    Hg call apps.refresh | Out-Null
    WaitScan | Out-Null
    $after = (((HgJson call profiles.list) | Where-Object { $_.id -eq 'builtin-guest' }) | ConvertTo-Json -Depth 8 -Compress).Length
    Check 'rediscovery leaves profiles and permissions untouched' ($before -eq $after)

    $service.Kill(); $service.WaitForExit(); Start-Sleep 2
    $service = Start-Process "$bin\houseguest-service.exe" -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden
    Start-Sleep 3
    $cached = HgJson call apps.library (Json @{ profile_id = 'builtin-guest' })
    Check 'the cached library is available right after a restart' (@($cached.apps).Count -ge $apps.Count)
    Check 'hand-added programs and folders survive a restart' ((HgJson call apps.sources).manual.Count -eq 1 -and (HgJson call apps.sources).portable_roots.Count -eq 1)

    Remove-Item "$portable\Loose.exe"
    Check 'a removed hand-added file is reported as missing' ((HgJson call apps.sources).manual[0].missing)
    Hg call apps.manual.remove (Json @{ path = "$portable\Loose.exe" }) | Out-Null
    Check 'removing a hand-added program forgets it' ((HgJson call apps.sources).manual.Count -eq 0)
}
finally {
    if ($service -and -not $service.HasExited) { $service.Kill() }
    Start-Sleep 1
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
    $results | Format-Table -AutoSize
    if ($results | Where-Object { -not $_.Passed }) { exit 1 }
}
