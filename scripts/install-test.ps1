# Installs the MSI on this machine, checks service permissions as a standard user, the uninstall guard, then uninstalls.
# Requires an elevated shell. Creates and removes one local account. Modifies the machine: use a VM when in doubt.
param([Parameter(Mandatory)] [string] $Msi)
$ErrorActionPreference = 'Stop'
$results = @()
$expectedVersion = if ($Msi -match 'Houseguest-(\d+\.\d+\.\d+)') { $Matches[1] } else { $null }
function Check($name, $condition) {
    $script:results += [pscustomobject]@{ Check = $name; Passed = [bool]$condition }
    Write-Host ("{0,-6} {1}" -f $(if ($condition) { 'PASS' } else { 'FAIL' }), $name)
}

$install = Join-Path $env:ProgramFiles 'Houseguest'
$cli = Join-Path $install 'houseguest-cli.exe'
$log = Join-Path $env:TEMP 'houseguest-install.log'
$guest = 'hginst' + (Get-Random -Maximum 999)
$password = [Guid]::NewGuid().ToString('N') + 'Aa1!'
$public = 'C:\Users\Public'

function AsGuest([string] $command) {
    $out = Join-Path $public "hg-$([Guid]::NewGuid().ToString('N')).txt"
    $credential = New-Object System.Management.Automation.PSCredential($guest, (ConvertTo-SecureString $password -AsPlainText -Force))
    Start-Process cmd.exe -ArgumentList '/c', "$command > `"$out`" 2>&1" -Credential $credential -WorkingDirectory $public -WindowStyle Hidden -Wait
    $text = if (Test-Path $out) { Get-Content $out -Raw } else { '' }
    Remove-Item $out -Force -ErrorAction SilentlyContinue
    $text
}

try {
    Remove-Item 'C:\ProgramData\Houseguest' -Recurse -Force -ErrorAction SilentlyContinue
    net user $guest $password /add /y | Out-Null
    $install_process = Start-Process msiexec.exe -ArgumentList '/i', "`"$Msi`"", '/qn', '/l*v', "`"$log`"" -Wait -PassThru
    Check 'msi installs without errors' ($install_process.ExitCode -eq 0)
    Check 'files are installed' ((Test-Path $cli) -and (Test-Path "$install\houseguest.exe") -and (Test-Path "$install\ui\index.html"))

    Start-Sleep -Seconds 3
    $service = Get-Service Houseguest -ErrorAction SilentlyContinue
    Check 'service is installed and running' ($service -and $service.Status -eq 'Running')
    Check 'service starts automatically' ((Get-CimInstance Win32_Service -Filter "Name='Houseguest'").StartMode -eq 'Auto')
    Check 'service runs as LocalSystem' ((Get-CimInstance Win32_Service -Filter "Name='Houseguest'").StartName -eq 'LocalSystem')

    $status = & $cli status | ConvertFrom-Json
    Check "service reports version $expectedVersion" ($status.version -eq $expectedVersion)

    Check 'standard user cannot stop the service' ((AsGuest 'sc.exe stop Houseguest') -match '5|denied|negado')
    Check 'service still running after the attempt' ((Get-Service Houseguest).Status -eq 'Running')
    Check 'standard user cannot claim ownership' ((AsGuest "set HOUSEGUEST_PIN=999999&& `"$cli`" setup") -match 'forbidden')
    Check 'ownership remains unclaimed' (-not ((& $cli status | ConvertFrom-Json).configured))

    $env:HOUSEGUEST_PIN = '482913'
    & $cli setup | Out-Null
    Check 'administrator can set up the owner' ((& $cli status | ConvertFrom-Json).configured)
    Check 'standard user cannot read the signed state file' ((AsGuest 'type C:\ProgramData\Houseguest\state.json') -match 'denied|negado')
    $auditRead = AsGuest 'type C:\ProgramData\Houseguest\audit.log'
    Check 'standard user cannot read the audit log' ($auditRead -match 'denied|negado')
    if ($auditRead -notmatch 'denied|negado') { Write-Host "  guest saw: $auditRead" }
    Check 'standard user cannot unlock with the right PIN' ((AsGuest "set HOUSEGUEST_PIN=482913&& `"$cli`" call profiles.list") -match 'forbidden')
    Check 'second setup is refused' ((& $cli setup 2>&1 | Out-String) -match 'conflict')
    Check 'administrator recovery works' ((& $cli recover 2>&1 | Out-String) -match 'reset_owner')
    Remove-Item Env:HOUSEGUEST_PIN -ErrorAction SilentlyContinue

    Stop-Service Houseguest
    Start-Service Houseguest
    Check 'administrator can restart the service' ((Get-Service Houseguest).Status -eq 'Running')
    Start-Sleep -Seconds 3

    $env:HOUSEGUEST_PIN = '482913'
    $guestSid = ((& $cli call users.list | ConvertFrom-Json) | Where-Object name -eq $guest).sid
    $profileId = (& $cli call profiles.list | ConvertFrom-Json)[0].id
    $start = @{ profile_id = $profileId; guest_sids = @($guestSid); duration_minutes = 5; audit_only = $true; confirm_warnings = $true } | ConvertTo-Json -Compress
    & $cli call session.start $start | Out-Null
    Check 'a guest session is active' ((& $cli status | ConvertFrom-Json).armed)
    $refused = Start-Process msiexec.exe -ArgumentList '/x', "`"$Msi`"", '/qn', '/l*v', "`"$log.refused`"" -Wait -PassThru
    Check 'uninstall is refused while a session is active' ($refused.ExitCode -ne 0)
    Check 'files and service remain after the refused uninstall' ((Test-Path $cli) -and (Get-Service Houseguest -ErrorAction SilentlyContinue))
    & $cli recover | Out-Null
    Check 'administrator recovery ends the session' (-not ((& $cli status | ConvertFrom-Json).armed))
    Remove-Item Env:HOUSEGUEST_PIN -ErrorAction SilentlyContinue
}
finally {
    $remove = Start-Process msiexec.exe -ArgumentList '/x', "`"$Msi`"", '/qn', '/l*v', "`"$log.uninstall`"" -Wait -PassThru
    Check 'msi uninstalls without errors' ($remove.ExitCode -eq 0)
    Check 'service is removed' (-not (Get-Service Houseguest -ErrorAction SilentlyContinue))
    Check 'install folder is removed' (-not (Test-Path $install))
    Remove-Item 'C:\ProgramData\Houseguest' -Recurse -Force -ErrorAction SilentlyContinue
    $reinstall = Start-Process msiexec.exe -ArgumentList '/i', "`"$Msi`"", '/qn' -Wait -PassThru
    if ($reinstall.ExitCode -eq 0) {
        Start-Sleep -Seconds 3
        $env:HOUSEGUEST_PIN = '482913'
        & $cli setup | Out-Null
        $guestSid = ((& $cli call users.list | ConvertFrom-Json) | Where-Object name -eq $guest).sid
        $profileId = (& $cli call profiles.list | ConvertFrom-Json)[0].id
        $start = @{ profile_id = $profileId; guest_sids = @($guestSid); duration_minutes = 5; audit_only = $true; confirm_warnings = $true } | ConvertTo-Json -Compress
        & $cli call session.start $start | Out-Null
        $forced = Start-Process msiexec.exe -ArgumentList '/x', "`"$Msi`"", '/qn', 'FORCE_UNINSTALL=1', '/l*v', "`"$log.forced`"" -Wait -PassThru
        Check 'an administrator can force the uninstall with FORCE_UNINSTALL=1' ($forced.ExitCode -eq 0 -and -not (Test-Path $install))
        Remove-Item Env:HOUSEGUEST_PIN -ErrorAction SilentlyContinue
    }
    Remove-Item 'C:\ProgramData\Houseguest' -Recurse -Force -ErrorAction SilentlyContinue
    net user $guest /delete 2>&1 | Out-Null
}
if ($results | Where-Object { -not $_.Passed }) { exit 1 }
Write-Host "All $($results.Count) checks passed."
