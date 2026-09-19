$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$validator = Join-Path $PSScriptRoot 'release/validate-release-metadata.ps1'
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$testDirectory = Join-Path $temporaryRoot ('apc-release-metadata-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDirectory | Out-Null
try {
    foreach ($file in @('CHANGELOG.md', 'Directory.Build.props', 'AudioPlaybackConnector2 (Package)/Package.appxmanifest', 'store/whats-new.json')) {
        $destination = Join-Path $testDirectory $file
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $repository $file) -Destination $destination
    }
    $notes = Get-Content -LiteralPath (Join-Path $testDirectory 'store/whats-new.json') -Raw | ConvertFrom-Json
    $version = $notes.version
    $changelogPath = Join-Path $testDirectory 'CHANGELOG.md'
    $lfText = [IO.File]::ReadAllText($changelogPath).Replace("`r`n", "`n")
    Push-Location $testDirectory
    try {
        foreach ($text in @($lfText, $lfText.Replace("`n", "`r`n"))) {
            [IO.File]::WriteAllText($changelogPath, $text)
            & pwsh -NoProfile -File $validator -Version $version *> $null
            if ($LASTEXITCODE -ne 0) { throw 'Valid metadata must pass with both LF and CRLF line endings.' }
        }
        $propsPath = Join-Path $testDirectory 'Directory.Build.props'
        [xml]$props = [IO.File]::ReadAllText($propsPath)
        $versionNode = $props.SelectSingleNode('/Project/PropertyGroup/PackageVersion')
        $versionNode.InnerText = '1.2.3.4'
        $props.Save($propsPath)
        & pwsh -NoProfile -File $validator -Version $version *> $null
        if ($LASTEXITCODE -eq 0) { throw 'Mismatched package version was accepted.' }
        $versionNode.InnerText = "$version.0"
        [void]$versionNode.ParentNode.AppendChild($versionNode.CloneNode($true))
        $props.Save($propsPath)
        & pwsh -NoProfile -File $validator -Version $version *> $null
        if ($LASTEXITCODE -eq 0) { throw 'Ambiguous package version declarations were accepted.' }
    } finally {
        Pop-Location
    }
} finally {
    $resolvedTestDirectory = [IO.Path]::GetFullPath($testDirectory)
    if ([IO.Path]::GetDirectoryName($resolvedTestDirectory) -ne $temporaryRoot -or
        [IO.Path]::GetFileName($resolvedTestDirectory) -notlike 'apc-release-metadata-*') {
        throw 'Refusing to remove a test directory outside its temporary root.'
    }
    Remove-Item -LiteralPath $resolvedTestDirectory -Recurse -Force
}
Write-Host 'Release metadata regression tests passed.'
exit 0
