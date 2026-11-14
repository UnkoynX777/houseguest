# Fails if files that must never be published are tracked or staged, if likely
# credentials are present, or if the portable core includes Windows headers.
# Used by the pre-commit hook and CI. Does not rely on .gitignore.
param([switch] $All)
$ErrorActionPreference = 'Stop'
$root = (git rev-parse --show-toplevel).Trim()
Set-Location $root

$files = if ($All) { git ls-files } else { @(git diff --cached --name-only --diff-filter=ACMR) + @(git ls-files) | Sort-Object -Unique }
$problems = New-Object System.Collections.Generic.List[string]

$forbiddenPath = @(
    '(^|/)(CLAUDE|AGENTS|GEMINI|COPILOT)(\.local)?\.md$', '(^|/)\.(claude|cursor|codex|agents|aider|copilot|windsurf|grok)(/|$)',
    '(^|/)\.cursorrules$', '(^|/)\.cursorignore$', '(^|/)\.env(\..*)?$', '\.(pfx|p12|key|pem|snk)$',
    '(^|/)[^/]*\.private\.[^/]*$', '(^|/)private/', '(^|/)\.vs/', '(^|/)build/'
)
$secretContent = @(
    '-----BEGIN (RSA |EC |OPENSSH |)PRIVATE KEY-----', 'ghp_[A-Za-z0-9]{36}', 'github_pat_[A-Za-z0-9_]{50,}',
    'AKIA[0-9A-Z]{16}', 'sk-[A-Za-z0-9]{32,}', 'xox[baprs]-[A-Za-z0-9-]{10,}'
)
$thisScript = 'scripts/check-repo-hygiene.ps1'

foreach ($f in $files) {
    foreach ($p in $forbiddenPath) { if ($f -match $p) { $problems.Add("forbidden path: $f"); break } }
    if ($f -eq $thisScript -or -not (Test-Path -LiteralPath $f -PathType Leaf)) { continue }
    $item = Get-Item -LiteralPath $f
    if ($item.Length -gt 5MB) { continue }
    $text = [System.IO.File]::ReadAllText($item.FullName)
    foreach ($s in $secretContent) { if ($text -match $s) { $problems.Add("possible secret in ${f}: /$s/") } }
    if ($f -like 'core/*' -and $f -match '\.(hpp|cpp|h)$' -and $text -match '#\s*include\s*[<"](windows|winsock2|winnt)\.h[>"]') {
        $problems.Add("Windows header in portable core: $f")
    }
}

if ($problems.Count) { $problems | ForEach-Object { Write-Error $_ -ErrorAction Continue }; exit 1 }
Write-Host "Repository hygiene OK ($($files.Count) files checked)."
