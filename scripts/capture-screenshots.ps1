# Runs the service in console mode and the devtools host, then drives the real UI.
# Needs an elevated shell and a devtools build (cmake -DHOUSEGUEST_DEVTOOLS=ON). Creates and removes one local account.
param([string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\devtools'))
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$data = Join-Path $env:TEMP 'houseguest-ui-data'
$guest = 'hgdemo' + (Get-Random -Maximum 999)
$password = [Guid]::NewGuid().ToString('N') + 'Aa1!'
$service = $null
$ui = $null

try {
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item "$env:LOCALAPPDATA\Houseguest" -Recurse -Force -ErrorAction SilentlyContinue
    net user $guest $password /add /y | Out-Null
    $service = Start-Process (Join-Path $Build 'service\houseguest-service.exe') -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden
    if (-not (Test-Path (Join-Path $Build 'service\houseguest-service.exe'))) { throw 'Build the service in the devtools tree first.' }
    Remove-Item (Join-Path $Build 'host\ui') -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $root 'ui\dist') (Join-Path $Build 'host\ui') -Recurse
    $ui = Start-Process (Join-Path $Build 'host\houseguest.exe') -PassThru
    Start-Sleep -Seconds 4
    $signals = Join-Path $env:TEMP 'houseguest-ui-signals'
    Remove-Item $signals -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $signals | Out-Null
    $env:GUEST_USER = $guest
    $env:SIGNAL_DIR = $signals
    $blocked = (Get-Command node).Source
    $credential = New-Object System.Management.Automation.PSCredential($guest, (ConvertTo-SecureString $password -AsPlainText -Force))
    $driver = Start-Process node -ArgumentList ('"' + (Join-Path $PSScriptRoot 'capture-screenshots.mjs') + '"') -PassThru -NoNewWindow
    while (-not $driver.HasExited) {
        if (Test-Path (Join-Path $signals 'launch')) {
            Remove-Item (Join-Path $signals 'launch')
            Start-Process $blocked -ArgumentList '-e', 'setTimeout(()=>{},30000)' -Credential $credential -WindowStyle Hidden | Out-Null
            New-Item (Join-Path $signals 'launched') | Out-Null
        }
        Start-Sleep -Milliseconds 200
    }
    $driver.WaitForExit()
    if ($driver.ExitCode -ne 0) { exit $driver.ExitCode }
}
finally {
    if ($ui -and -not $ui.HasExited) { Stop-Process -Id $ui.Id -Force }
    if ($service -and -not $service.HasExited) { Stop-Process -Id $service.Id -Force }
    net user $guest /delete 2>&1 | Out-Null
    Remove-Item $data -Recurse -Force -ErrorAction SilentlyContinue
}
