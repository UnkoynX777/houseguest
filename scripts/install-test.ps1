# Installs the MSI on this machine, checks service permissions as a standard user, then uninstalls.
# Requires an elevated shell. Creates and removes one local account. Modifies the machine: use a VM when in doubt.
param([Parameter(Mandatory)] [string] $Msi)
$ErrorActionPreference = 'Stop'
$results = @()
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
    Check 'service answers over the pipe' ($status.version -eq '0.1.0')

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
}
finally {
    $remove = Start-Process msiexec.exe -ArgumentList '/x', "`"$Msi`"", '/qn', '/l*v', "`"$log.uninstall`"" -Wait -PassThru
    Check 'msi uninstalls without errors' ($remove.ExitCode -eq 0)
    Check 'service is removed' (-not (Get-Service Houseguest -ErrorAction SilentlyContinue))
    Check 'install folder is removed' (-not (Test-Path $install))
    net user $guest /delete 2>&1 | Out-Null
}
if ($results | Where-Object { -not $_.Passed }) { exit 1 }
Write-Host "All $($results.Count) checks passed."
