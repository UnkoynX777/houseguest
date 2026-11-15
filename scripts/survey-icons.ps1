# Lists where the picture of every library entry on this machine comes from, and compares it with what asking the
# shell for the executable alone would have given. It runs a private service instance (build with -Instances) to
# read the library, then the icon renderer from the host tests over what the service reported. Read-only.
param(
    [string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build/work'),
    [string] $Output = (Join-Path ([IO.Path]::GetTempPath()) 'houseguest-icon-survey')
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandArgumentPassing = 'Standard'
$root = 'C:\HgSurvey'; $bin = "$root\bin"; $data = "$root\data"
$env:HOUSEGUEST_INSTANCE = 'hgsurvey'
$env:HOUSEGUEST_PIN = '482913'
$cli = "$bin\houseguest-cli.exe"
$body = '{"profile_id":"builtin-guest"}'

function Call($name, $arguments = '{}') { & $cli call $name $arguments 2>&1 | Out-String | ConvertFrom-Json }

$service = $null
try {
    Get-Process houseguest-service -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
    Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory $bin | Out-Null
    Copy-Item "$Build\service\houseguest-service.exe", "$Build\service\houseguest-cli.exe" $bin
    $service = Start-Process "$bin\houseguest-service.exe" -ArgumentList '--console', '--data-dir', $data -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 3
    & $cli setup | Out-Null
    for ($i = 0; $i -lt 600; $i++) { $v = Call 'apps.library' $body; if (-not $v.scanning -and @($v.apps).Count) { break }; Start-Sleep -Milliseconds 200 }
    New-Item -ItemType Directory $Output -Force | Out-Null
    $file = Join-Path $Output 'library.json'
    (& $cli call 'apps.library' $body 2>&1 | Out-String) | Set-Content $file -Encoding utf8
    $env:HG_SURVEY = $file
    $env:HG_SURVEY_OUT = Join-Path $Output 'png'
    Remove-Item Env:HOUSEGUEST_INSTANCE, Env:HOUSEGUEST_PIN
    & (Join-Path $Build 'host/houseguest_host_tests.exe') '[.survey]' | Where-Object { $_ -match '^(old|entries)' }
} finally {
    if ($service) { Stop-Process $service -Force -ErrorAction SilentlyContinue }
}
