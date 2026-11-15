# Drives the built interface in headless Chrome against a mock service and checks navigation behaviour: cached
# pages, preserved filters and scroll, windowed lists, generation-driven refresh and stored preferences.
# Pass -Shots <folder> to also save screenshots of the detail panel.
# Build the interface first (cd ui; npm run build). Needs Node 22 and Chrome; uses port 5599 and debugging port 9444.
param([string] $Shots = '')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$chrome = @("$env:ProgramFiles\Google\Chrome\Application\chrome.exe", "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $chrome) { throw 'Chrome was not found.' }
$profile = Join-Path ([IO.Path]::GetTempPath()) ('houseguest-ui-check-' + (Get-Random))
$server = Start-Process node -ArgumentList "`"$here\serve.mjs`"" -PassThru -WindowStyle Hidden
$browser = Start-Process $chrome -ArgumentList '--headless=new', '--remote-debugging-port=9444', "--user-data-dir=$profile", '--window-size=1440,900', 'http://127.0.0.1:5599/' -PassThru
try {
    Start-Sleep -Seconds 4
    node --test "$here\certificate.test.mjs"
    if ($LASTEXITCODE -ne 0) { throw 'The certificate name checks failed.' }
    node "$here\check.mjs"
    if ($LASTEXITCODE -ne 0) { throw 'The interface checks failed.' }
    node "$here\details.mjs"
    if ($LASTEXITCODE -ne 0) { throw 'The detail panel checks failed.' }
    node "$here\panel.mjs"
    if ($LASTEXITCODE -ne 0) { throw 'The panel and layer checks failed.' }
    if ($Shots) { node "$here\shots.mjs" $Shots }
}
finally {
    Get-CimInstance Win32_Process -Filter "Name='node.exe'" | Where-Object { $_.CommandLine -like '*serve.mjs*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { $_.CommandLine -like '*remote-debugging-port=9444*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
