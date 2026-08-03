param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath
)

$ErrorActionPreference = 'Stop'
$resolvedProject = (Resolve-Path -LiteralPath $ProjectPath).Path
$projectDirectory = Split-Path -Parent $resolvedProject
$repositoryDirectory = Split-Path -Parent $projectDirectory
$projectText = Get-Content -LiteralPath $resolvedProject -Raw

$forbiddenProjectPatterns = @(
    '<UseWinUI>\s*true\s*</UseWinUI>',
    '<ApplicationDefinition\b',
    '<Page\b',
    '<Midl\b'
)

$failures = [System.Collections.Generic.List[string]]::new()
foreach ($pattern in $forbiddenProjectPatterns) {
    if ($projectText -match $pattern) {
        $failures.Add("project imports or declares forbidden UI surface: $pattern")
    }
}

$packagesPath = Join-Path $projectDirectory 'packages.config'
if (Test-Path -LiteralPath $packagesPath -PathType Leaf) {
    $packagesText = Get-Content -LiteralPath $packagesPath -Raw
    if ($packagesText -match '(?i)(Microsoft\.WindowsAppSDK\.WinUI|Microsoft\.UI\.Xaml)') {
        $failures.Add('packages.config references a forbidden WinUI/XAML package')
    }
}

[xml]$project = $projectText
$namespace = [System.Xml.XmlNamespaceManager]::new($project.NameTable)
$namespace.AddNamespace('msb', 'http://schemas.microsoft.com/developer/msbuild/2003')
$dependencyNodes = $project.SelectNodes('//msb:Import | //msb:Reference | //msb:PackageReference', $namespace)
foreach ($dependencyNode in $dependencyNodes) {
    $dependency = $dependencyNode.GetAttribute('Include')
    if ([string]::IsNullOrWhiteSpace($dependency)) {
        $dependency = $dependencyNode.GetAttribute('Project')
    }
    if ($dependency -match '(?i)(Microsoft\.WindowsAppSDK\.WinUI|Microsoft\.UI\.Xaml)') {
        $failures.Add("project resolves a forbidden UI dependency: $dependency")
    }
}
$nodes = $project.SelectNodes('//msb:ClCompile | //msb:ClInclude', $namespace)
$forbiddenSourcePattern = '(?i)(Microsoft::UI|winrt/Microsoft\.UI|Windows::UI::Xaml|winrt/Windows\.UI\.Xaml|\.xaml\.h|microsoft\.ui\.xaml\.window\.h)'
$includePattern = '#\s*include\s*[<"]([^>"]+)[>"]'
$searchRoots = @(
    (Join-Path $projectDirectory 'include'),
    (Join-Path $repositoryDirectory 'AudioPlaybackConnector2\include'),
    (Join-Path $repositoryDirectory 'AudioPlaybackConnector2\src'),
    (Join-Path $repositoryDirectory 'AudioPlaybackConnector2\res')
)
$pending = [System.Collections.Generic.Queue[string]]::new()
$visited = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

foreach ($node in $nodes) {
    $include = $node.GetAttribute('Include')
    if ([string]::IsNullOrWhiteSpace($include) -or $include.Contains('$(') -or $include.Contains('*')) {
        continue
    }

    $candidate = [System.IO.Path]::GetFullPath((Join-Path $projectDirectory $include))
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        $failures.Add("declared boundary file does not exist: $include")
        continue
    }

    $pending.Enqueue($candidate)
}

while ($pending.Count -ne 0) {
    $candidate = $pending.Dequeue()
    if (-not $visited.Add($candidate)) {
        continue
    }

    $content = Get-Content -LiteralPath $candidate -Raw
    if ($content -match $forbiddenSourcePattern) {
        $relativePath = [System.IO.Path]::GetRelativePath($repositoryDirectory, $candidate)
        $failures.Add("forbidden XAML dependency in $relativePath")
    }

    foreach ($match in [regex]::Matches($content, $includePattern)) {
        $includeName = $match.Groups[1].Value.Replace('/', '\')
        $includeCandidates = @((Join-Path (Split-Path -Parent $candidate) $includeName))
        $includeCandidates += $searchRoots | ForEach-Object { Join-Path $_ $includeName }
        foreach ($includeCandidate in $includeCandidates) {
            if (Test-Path -LiteralPath $includeCandidate -PathType Leaf) {
                $pending.Enqueue([System.IO.Path]::GetFullPath($includeCandidate))
                break
            }
        }
    }
}

if ($failures.Count -ne 0) {
    $failures | Sort-Object -Unique | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Host "CoreRuntime boundary verified: no WinUI/XAML dependencies."
