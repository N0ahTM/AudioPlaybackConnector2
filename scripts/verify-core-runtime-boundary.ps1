param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectPath
)

$ErrorActionPreference = 'Stop'
$resolvedProject = (Resolve-Path -LiteralPath $ProjectPath).Path
$projectDirectory = Split-Path -Parent $resolvedProject
$parentDirectory = Split-Path -Parent $projectDirectory
$repositoryDirectory = if (Test-Path -LiteralPath (Join-Path $parentDirectory '.git')) {
    $parentDirectory
} else {
    $projectDirectory
}
$forbiddenProjectPatterns = @(
    '<UseWinUI>\s*true\s*</UseWinUI>'
)

$failures = [System.Collections.Generic.List[string]]::new()
$msbuildQueue = [System.Collections.Generic.Queue[string]]::new()
$msbuildQueue.Enqueue("$resolvedProject|$projectDirectory")
foreach ($sharedName in @('Directory.Build.props', 'Directory.Build.targets')) {
    $sharedPath = Join-Path $repositoryDirectory $sharedName
    if (Test-Path -LiteralPath $sharedPath -PathType Leaf) {
        $msbuildQueue.Enqueue("$sharedPath|$projectDirectory")
    }
}
$visitedMsbuildFiles = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$declaredFiles = [System.Collections.Generic.List[object]]::new()
$packagesDirectory = Join-Path $repositoryDirectory 'packages'

function Test-IsWithinDirectory([string]$Path, [string]$Directory) {
    $root = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\') + '\'
    $candidate = [System.IO.Path]::GetFullPath($Path)
    return $candidate.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-IsWithinRepository([string]$Path) {
    return Test-IsWithinDirectory $Path $repositoryDirectory
}

function Resolve-LocalMsbuildImport(
    [string]$CurrentDirectory,
    [string]$EvaluationProjectDirectory,
    [string]$Import) {
    if ([string]::IsNullOrWhiteSpace($Import)) {
        return $null
    }
    if ($Import.Contains('*') -or $Import.Contains('?')) {
        throw "unverifiable wildcard MSBuild path: $Import"
    }
    $expanded = $Import.Replace('$(MSBuildProjectDirectory)', $EvaluationProjectDirectory)
    $expanded = $expanded.Replace('$(ProjectDir)', ($EvaluationProjectDirectory.TrimEnd('\') + '\'))
    $expanded = $expanded.Replace('$(MSBuildThisFileDirectory)', ($CurrentDirectory.TrimEnd('\') + '\'))
    if ($expanded.Contains('$(')) {
        if ($expanded -match '^\$\((VCTargetsPath|UserRootDir|MSBuildExtensionsPath|MSBuildToolsPath)\)[\\/]') {
            return $null
        }
        throw "unverifiable MSBuild import: $Import"
    }
    if ([System.IO.Path]::IsPathRooted($expanded)) {
        return [System.IO.Path]::GetFullPath($expanded)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $CurrentDirectory $expanded))
}

while ($msbuildQueue.Count -ne 0) {
    $queueEntry = $msbuildQueue.Dequeue().Split('|', 2)
    $currentProject = $queueEntry[0]
    $evaluationProjectDirectory = $queueEntry[1]
    if (-not $visitedMsbuildFiles.Add("$currentProject|$evaluationProjectDirectory")) {
        continue
    }
    $currentDirectory = Split-Path -Parent $currentProject
    $projectText = Get-Content -LiteralPath $currentProject -Raw
    foreach ($pattern in $forbiddenProjectPatterns) {
        if ($projectText -match $pattern) {
            $failures.Add("project imports or declares forbidden UI surface: $currentProject ($pattern)")
        }
    }

    $packagesPath = Join-Path $currentDirectory 'packages.config'
    if (Test-Path -LiteralPath $packagesPath -PathType Leaf) {
        $packagesText = Get-Content -LiteralPath $packagesPath -Raw
        if ($packagesText -match '(?i)(Microsoft\.WindowsAppSDK\.WinUI|Microsoft\.UI\.Xaml)') {
            $failures.Add("packages.config references a forbidden WinUI/XAML package: $packagesPath")
        }
    }

    [xml]$project = $projectText
    $namespace = [System.Xml.XmlNamespaceManager]::new($project.NameTable)
    $namespace.AddNamespace('msb', 'http://schemas.microsoft.com/developer/msbuild/2003')
    $forbiddenItemNodes = $project.SelectNodes(
        '//msb:ItemGroup/msb:ApplicationDefinition[@Include] | //msb:ItemGroup/msb:Page[@Include] | //msb:ItemGroup/msb:Midl[@Include]',
        $namespace)
    foreach ($forbiddenItemNode in $forbiddenItemNodes) {
        $failures.Add("project declares a forbidden UI item: $currentProject ($($forbiddenItemNode.LocalName))")
    }

    $dependencyNodes = $project.SelectNodes(
        '//msb:Import | //msb:ItemGroup/msb:Reference | //msb:ItemGroup/msb:PackageReference | //msb:ItemGroup/msb:ProjectReference',
        $namespace)
    foreach ($dependencyNode in $dependencyNodes) {
        $dependency = $dependencyNode.GetAttribute('Include')
        if ([string]::IsNullOrWhiteSpace($dependency)) {
            $dependency = $dependencyNode.GetAttribute('Project')
        }
        if ($dependency -match '(?i)(Microsoft\.WindowsAppSDK\.WinUI|Microsoft\.UI\.Xaml)') {
            $failures.Add("project resolves a forbidden UI dependency: $dependency")
        }
        if ($dependencyNode.LocalName -eq 'ProjectReference') {
            if ([string]::IsNullOrWhiteSpace($dependency)) {
                continue
            }
            if ($dependency.Contains('$(') -or $dependency.Contains('*')) {
                $failures.Add("project has an unverifiable project reference: $currentProject ($dependency)")
                continue
            }
            $referencedProject = [System.IO.Path]::GetFullPath((Join-Path $currentDirectory $dependency))
            if (-not (Test-Path -LiteralPath $referencedProject -PathType Leaf)) {
                $failures.Add("project reference does not exist: $referencedProject")
                continue
            }
            $msbuildQueue.Enqueue("$referencedProject|$(Split-Path -Parent $referencedProject)")
        } elseif ($dependencyNode.LocalName -eq 'Import') {
            try {
                $importedFile = Resolve-LocalMsbuildImport $currentDirectory $evaluationProjectDirectory $dependency
            } catch {
                $failures.Add("$currentProject ($($_.Exception.Message))")
                continue
            }
            if ($null -eq $importedFile -or -not (Test-IsWithinRepository $importedFile)) {
                continue
            }
            if (Test-IsWithinDirectory $importedFile $packagesDirectory) {
                continue
            }
            if (Test-Path -LiteralPath $importedFile -PathType Leaf) {
                $msbuildQueue.Enqueue("$importedFile|$evaluationProjectDirectory")
            }
        }
    }

    $nodes = $project.SelectNodes('//msb:ItemGroup/msb:ClCompile[@Include] | //msb:ItemGroup/msb:ClInclude[@Include]',
        $namespace)
    foreach ($node in $nodes) {
        # Resolve PCH and quoted headers in the declaring project's include context. In particular,
        # CoreTests compiles some shared sources with CoreRuntime's PCH, not the GUI project's PCH.
        $includeRoots = [System.Collections.Generic.List[string]]::new()
        $directoryNodes = @($node.SelectNodes('msb:AdditionalIncludeDirectories', $namespace))
        $directoryNodes += @($project.SelectNodes(
            '//msb:ItemDefinitionGroup/msb:ClCompile/msb:AdditionalIncludeDirectories', $namespace))
        foreach ($directoryNode in $directoryNodes) {
            foreach ($directory in $directoryNode.InnerText.Split(';')) {
                if ([string]::IsNullOrWhiteSpace($directory) -or $directory.Contains('%(')) { continue }
                try {
                    $resolvedDirectory = Resolve-LocalMsbuildImport $currentDirectory $evaluationProjectDirectory $directory
                } catch {
                    $failures.Add("unverifiable source include directory: $directory")
                    continue
                }
                if ($null -ne $resolvedDirectory -and (Test-IsWithinRepository $resolvedDirectory) -and
                    -not (Test-IsWithinDirectory $resolvedDirectory $packagesDirectory)) {
                    $includeRoots.Add($resolvedDirectory)
                }
            }
        }
        $declaredFiles.Add([pscustomobject]@{
            Directory = $currentDirectory
            EvaluationProjectDirectory = $evaluationProjectDirectory
            Include = $node.GetAttribute('Include')
            IncludeRoots = $includeRoots.ToArray()
        })
    }
}
$forbiddenSourcePattern = '(?i)(Microsoft::UI|winrt/Microsoft\.UI|Windows::UI::Xaml|winrt/Windows\.UI\.Xaml|\.xaml\.h|microsoft\.ui\.xaml\.window\.h)'
$includePattern = '#\s*include\s*[<"]([^>"]+)[>"]'
$searchRoots = @(
    (Join-Path $projectDirectory 'include'),
    (Join-Path $repositoryDirectory 'AudioPlaybackConnector2\include'),
    (Join-Path $repositoryDirectory 'AudioPlaybackConnector2\src'),
    (Join-Path $repositoryDirectory 'AudioPlaybackConnector2\res')
)
$pending = [System.Collections.Generic.Queue[object]]::new()
$visited = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

foreach ($declaredFile in $declaredFiles) {
    $include = $declaredFile.Include
    if ($include.Contains('*') -or $include.Contains('?')) {
        $failures.Add("declared boundary file uses an unverifiable wildcard: $include")
        continue
    }

    try {
        $candidate = Resolve-LocalMsbuildImport `
            $declaredFile.Directory $declaredFile.EvaluationProjectDirectory $include
    } catch {
        $failures.Add("declared boundary file is unverifiable: $include ($($_.Exception.Message))")
        continue
    }
    if ($null -eq $candidate -or -not (Test-IsWithinRepository $candidate)) {
        $failures.Add("declared boundary file resolves outside the repository: $include")
        continue
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        $failures.Add("declared boundary file does not exist: $include")
        continue
    }

    $pending.Enqueue([pscustomobject]@{ Path = $candidate; IncludeRoots = $declaredFile.IncludeRoots })
}

while ($pending.Count -ne 0) {
    $source = $pending.Dequeue()
    $candidate = $source.Path
    if (-not $visited.Add("$candidate|$($source.IncludeRoots -join ';')")) {
        continue
    }

    $content = Get-Content -LiteralPath $candidate -Raw
    if ($content -match $forbiddenSourcePattern) {
        $relativePath = $candidate.Substring($repositoryDirectory.TrimEnd('\').Length + 1)
        $failures.Add("forbidden XAML dependency in $relativePath")
    }

    foreach ($match in [regex]::Matches($content, $includePattern)) {
        $includeName = $match.Groups[1].Value.Replace('/', '\')
        $includeCandidates = @((Join-Path (Split-Path -Parent $candidate) $includeName))
        $includeCandidates += $source.IncludeRoots | ForEach-Object { Join-Path $_ $includeName }
        $includeCandidates += $searchRoots | ForEach-Object { Join-Path $_ $includeName }
        foreach ($includeCandidate in $includeCandidates) {
            if (Test-Path -LiteralPath $includeCandidate -PathType Leaf) {
                $pending.Enqueue([pscustomobject]@{
                    Path = [System.IO.Path]::GetFullPath($includeCandidate)
                    IncludeRoots = $source.IncludeRoots
                })
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
