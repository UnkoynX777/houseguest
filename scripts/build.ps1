param(
    [ValidateSet('Debug', 'Release')] [string] $Config = 'Debug',
    [switch] $RunTests,
    [switch] $Devtools,
    [switch] $Sanitize,
    [switch] $Fuzz,
    [switch] $Instances
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC build tools not found.' }

$cmakeBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = "$cmakeBin\CMake\bin;$cmakeBin\Ninja;$env:PATH"

$name = if ($Instances) { 'work' } elseif ($Fuzz) { 'fuzz' } elseif ($Sanitize) { 'asan' } elseif ($Devtools) { 'devtools' } else { $Config.ToLower() }
$build = Join-Path $root "build\$name"
$flags = ''
if ($Devtools) { $flags += ' -DHOUSEGUEST_DEVTOOLS=ON' }
if ($Sanitize) { $flags += ' -DHOUSEGUEST_SANITIZE=ON' }
if ($Fuzz) { $flags += ' -DHOUSEGUEST_FUZZ=ON' }
if ($Instances) { $flags += ' -DHOUSEGUEST_INSTANCES=ON' }
$script = "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && " +
    "cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config$flags && " +
    "cmake --build `"$build`""
if ($RunTests) { $script += " && ctest --test-dir `"$build`" --output-on-failure" }
cmd /c $script
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
