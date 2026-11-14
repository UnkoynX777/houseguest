param([string] $Build = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Native {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
    [DllImport("shell32.dll")] public static extern int Shell_NotifyIconGetRect(ref NOTIFYICONIDENTIFIER id, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct NOTIFYICONIDENTIFIER { public int cbSize; public IntPtr hWnd; public uint uID; public Guid guidItem; }
    public static bool HasTrayIcon(IntPtr window) {
        var id = new NOTIFYICONIDENTIFIER { cbSize = Marshal.SizeOf(typeof(NOTIFYICONIDENTIFIER)), hWnd = window, uID = 1 };
        RECT r;
        return Shell_NotifyIconGetRect(ref id, out r) >= 0;
    }
}
'@

$exe = Join-Path $Build 'host\houseguest.exe'
if (-not (Test-Path $exe)) { throw "Build the host first: $exe" }
$settingsDir = Join-Path $env:LOCALAPPDATA 'Houseguest'
$settingsFile = Join-Path $settingsDir 'ui.json'
$backup = $null
if (Test-Path $settingsFile) { $backup = Get-Content $settingsFile -Raw }
$results = New-Object System.Collections.Generic.List[object]
function Check([string] $name, [bool] $ok) {
    $results.Add([pscustomobject]@{ Name = $name; Ok = $ok })
    Write-Host ("{0}  {1}" -f ($(if ($ok) { 'PASS' } else { 'FAIL' }), $name))
}
function Wait-Window([int] $seconds = 10) {
    for ($i = 0; $i -lt $seconds * 10; $i++) {
        $h = [Native]::FindWindow('HouseguestWindow', [NullString]::Value)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 100
    }
    return [IntPtr]::Zero
}
function Rect([IntPtr] $h) { $r = New-Object Native+RECT; [void][Native]::GetWindowRect($h, [ref]$r); $r }
function Inside-AnyScreen($r) {
    foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
        $w = $s.WorkingArea
        if ($r.Left -ge $w.Left - 16 -and $r.Top -ge $w.Top - 16 -and $r.Right -le $w.Right + 16 -and $r.Bottom -le $w.Bottom + 16) { return $true }
    }
    return $false
}
function Tray-Icon-Present([IntPtr] $window) { [Native]::HasTrayIcon($window) }

$process = $null
try {
    Remove-Item $settingsDir\ui.json -Force -ErrorAction SilentlyContinue
    $process = Start-Process $exe -PassThru
    $h = Wait-Window
    Check "window is created ($($h.ToInt64()))" ($h.ToInt64() -ne 0)
    Start-Sleep -Seconds 2
    $r = Rect $h
    Check 'default window fits inside a work area' (Inside-AnyScreen $r)
    $screen = [System.Windows.Forms.Screen]::FromHandle($h).WorkingArea
    Check 'default window is centered' ([math]::Abs((($r.Left + $r.Right) / 2) - ($screen.Left + $screen.Right) / 2) -lt 24)
    Check 'tray icon is present' (Tray-Icon-Present $h)

    [void][Native]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Seconds 1
    Check 'closing hides the window and keeps the process' ((-not $process.HasExited) -and (-not [Native]::IsWindowVisible($h)))
    $second = Start-Process $exe -PassThru
    Start-Sleep -Seconds 2
    Check 'a second launch restores the hidden window' ([Native]::IsWindowVisible($h))
    Check 'a second launch does not leave a second process' ($second.HasExited)
    $process.Kill(); $process.WaitForExit()

    Set-Content $settingsFile '{"minimize_to_tray":true,"theme":"dark","language":"en","placement":{"x":9000,"y":7000,"width":5000,"height":4000,"maximized":false}}'
    $process = Start-Process $exe -PassThru
    $h = Wait-Window
    Start-Sleep -Seconds 2
    Check 'saved placement outside every monitor is repositioned' (Inside-AnyScreen (Rect $h))
    $process.Kill(); $process.WaitForExit()

    Set-Content $settingsFile '{"minimize_to_tray":true,"theme":"system","placement":{"x":100,"y":100,"width":1000,"height":700,"maximized":false}}'
    $process = Start-Process $exe -ArgumentList '--tray' -PassThru
    $h = Wait-Window
    Start-Sleep -Seconds 2
    Check 'started with --tray the window stays hidden' ((-not [Native]::IsWindowVisible($h)) -and (-not $process.HasExited))
    Check 'tray icon exists while hidden' (Tray-Icon-Present $h)
}
finally {
    if ($process -and -not $process.HasExited) { $process.Kill() }
    if ($null -ne $backup) { Set-Content $settingsFile $backup } else { Remove-Item $settingsFile -Force -ErrorAction SilentlyContinue }
}
$failed = @($results | Where-Object { -not $_.Ok })
if ($failed.Count) { Write-Host "$($failed.Count) check(s) failed."; exit 1 }
Write-Host "All $($results.Count) host checks passed."
