[CmdletBinding()]
param(
    [string]$RepositoryRoot = '.'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$markdownFiles = @(
    git -C $root ls-files '*.md'
    git -C $root ls-files --others --exclude-standard '*.md'
) | Sort-Object -Unique
if ($LASTEXITCODE -ne 0) { throw 'Unable to enumerate tracked Markdown files.' }

$failures = [System.Collections.Generic.List[string]]::new()
foreach ($relativeFile in $markdownFiles) {
    $path = Join-Path $root $relativeFile
    $content = Get-Content -LiteralPath $path -Raw -Encoding utf8
    foreach ($match in [regex]::Matches($content, '(?m)(?!!)\[[^\]]+\]\(([^)]+)\)')) {
        $target = $match.Groups[1].Value.Trim()
        if ($target.StartsWith('<') -and $target.EndsWith('>')) {
            $target = $target.Substring(1, $target.Length - 2)
        }
        if ($target -match '^(?i:https?://|mailto:|#)') { continue }

        $target = $target.Split('#')[0].Split('?')[0]
        if ([string]::IsNullOrWhiteSpace($target)) { continue }
        $target = [Uri]::UnescapeDataString($target)
        $resolvedTarget = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $path) $target))
        if (-not $resolvedTarget.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $resolvedTarget)) {
            $line = 1 + ($content.Substring(0, $match.Index) -split "`n").Count - 1
            $failures.Add("$relativeFile`:$line has missing local target '$target'.")
        }
    }
}

if ($failures.Count -gt 0) { throw ($failures -join [Environment]::NewLine) }
Write-Host "Validated local links in $($markdownFiles.Count) tracked Markdown files."
