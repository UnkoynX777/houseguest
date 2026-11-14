param(
    [string] $MessageFile,
    [string] $Range
)
$ErrorActionPreference = 'Stop'

$attribution = @(
    '(?im)^\s*co-authored-by:.*(claude|anthropic|openai|copilot|codex|gemini|chatgpt|\bgpt\b|\bai\b)',
    '(?im)^\s*co-authored-by:.*noreply@anthropic\.com',
    '(?i)generated (with|by) .*(claude|anthropic|openai|copilot|codex|gemini|chatgpt|\bai\b)',
    '(?i)claude\.com/claude-code',
    [regex]::Escape([char]::ConvertFromUtf32(0x1F916))
)
$aiIdentity = '(?i)(claude|anthropic|openai|copilot|codex|gemini|chatgpt)'

function Test-Text([string] $label, [string] $text) {
    foreach ($pattern in $attribution) {
        if ($text -match $pattern) { return "${label}: automatic AI attribution found (/$pattern/)" }
    }
}

$problems = New-Object System.Collections.Generic.List[string]

if ($MessageFile) {
    $message = [System.IO.File]::ReadAllText($MessageFile)
    $problem = Test-Text 'commit message' $message
    if ($problem) { $problems.Add($problem) }
    $name = (git config user.name)
    $email = (git config user.email)
    if ("$name <$email>" -match $aiIdentity) { $problems.Add("configured identity looks like an AI tool: $name <$email>") }
    if ($email -notmatch '^\d+\+[A-Za-z0-9-]+@users\.noreply\.github\.com$') {
        $problems.Add("user.email is not a GitHub noreply address with the numeric id: $email")
    }
}

if ($Range) {
    foreach ($line in (git log --format='%H%x1f%an%x1f%ae%x1f%cn%x1f%ce' $Range)) {
        $f = $line -split "`u{1f}"
        if (("$($f[1]) $($f[2]) $($f[3]) $($f[4])") -match $aiIdentity) { $problems.Add("AI identity in author or committer of $($f[0])") }
        $problem = Test-Text $f[0] (git log -1 --format=%B $f[0] | Out-String)
        if ($problem) { $problems.Add($problem) }
    }
}

if ($problems.Count) { $problems | ForEach-Object { Write-Error $_ -ErrorAction Continue }; exit 1 }
