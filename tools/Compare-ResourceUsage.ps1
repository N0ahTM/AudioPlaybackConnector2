#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string]$InputDirectory,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$Scenario,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$BaselineVariant,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$CandidateVariant,

    [Parameter()]
    [ValidateRange(2, 10000)]
    [int]$MinimumPairs = 20,

    [Parameter()]
    [ValidateRange(100, 50000)]
    [int]$BootstrapIterations = 10000,

    [Parameter()]
    [ValidateRange(0, 60000)]
    [double]$MaximumScheduleLatenessMilliseconds = 500,

    [Parameter()]
    [ValidateRange(0, [int]::MaxValue)]
    [int]$RandomSeed = 1729,

    [Parameter()]
    [string]$OutputDirectory,

    [Parameter()]
    [switch]$AllowDirtyHarness,

    [Parameter()]
    [switch]$AllowSameExecutable,

    [Parameter()]
    [switch]$PassThru
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-Percentile {
    param(
        [Parameter(Mandatory)]
        [double[]]$SortedValues,

        [Parameter(Mandatory)]
        [ValidateRange(0, 100)]
        [double]$Percentile
    )

    if ($SortedValues.Count -eq 0) {
        return $null
    }
    if ($SortedValues.Count -eq 1) {
        return $SortedValues[0]
    }

    $position = ($Percentile / 100.0) * ($SortedValues.Count - 1)
    $lowerIndex = [int][Math]::Floor($position)
    $upperIndex = [int][Math]::Ceiling($position)
    if ($lowerIndex -eq $upperIndex) {
        return $SortedValues[$lowerIndex]
    }

    $fraction = $position - $lowerIndex
    return ($SortedValues[$lowerIndex] * (1.0 - $fraction)) +
        ($SortedValues[$upperIndex] * $fraction)
}

function Get-Median {
    param(
        [Parameter(Mandatory)]
        [double[]]$Values
    )

    return Get-Percentile -SortedValues ([double[]]@($Values | Sort-Object)) -Percentile 50
}

function Get-BootstrapMedianInterval {
    param(
        [Parameter(Mandatory)]
        [double[]]$Values,

        [Parameter(Mandatory)]
        [int]$Iterations,

        [Parameter(Mandatory)]
        [int]$Seed
    )

    $random = [Random]::new($Seed)
    $medians = [double[]]::new($Iterations)
    $sample = [double[]]::new($Values.Count)
    for ($iteration = 0; $iteration -lt $Iterations; $iteration++) {
        for ($index = 0; $index -lt $Values.Count; $index++) {
            $sample[$index] = $Values[$random.Next($Values.Count)]
        }
        $medians[$iteration] = Get-Median -Values $sample
    }

    [Array]::Sort($medians)
    return [ordered]@{
        Lower95 = Get-Percentile -SortedValues $medians -Percentile 2.5
        Upper95 = Get-Percentile -SortedValues $medians -Percentile 97.5
    }
}

function Get-PropertyPathValue {
    param(
        [Parameter(Mandatory)]
        [object]$InputObject,

        [Parameter(Mandatory)]
        [string]$Path
    )

    $value = $InputObject
    foreach ($segment in $Path.Split('.')) {
        $property = $value.PSObject.Properties[$segment]
        if ($null -eq $property) {
            throw "Required property '$Path' is missing."
        }
        $value = $property.Value
    }
    if ($null -eq $value) {
        throw "Required property '$Path' is null."
    }
    return [double]$value
}

function Assert-Equal {
    param(
        [Parameter(Mandatory)]
        [AllowNull()]
        [object]$Left,

        [Parameter(Mandatory)]
        [AllowNull()]
        [object]$Right,

        [Parameter(Mandatory)]
        [string]$Label
    )

    if ($Left -ne $Right) {
        throw "$Label differs: '$Left' versus '$Right'."
    }
}

function Assert-RunIntegrity {
    param(
        [Parameter(Mandatory)]
        [object]$Run,

        [Parameter(Mandatory)]
        [string]$Path
    )

    if ([int]$Run.SchemaVersion -lt 2) {
        throw "'$Path' uses unsupported schema version '$($Run.SchemaVersion)'."
    }
    if ([string]::IsNullOrWhiteSpace([string]$Run.Run.PairId)) {
        throw "'$Path' has no Run.PairId."
    }
    if ([string]::IsNullOrWhiteSpace([string]$Run.Run.Variant)) {
        throw "'$Path' has no Run.Variant."
    }
    if ([string]$Run.Package.ExecutableSha256 -notmatch '^[A-Fa-f0-9]{64}$') {
        throw "'$Path' has no valid executable SHA-256."
    }
    if ([string]$Run.Package.ManifestSha256 -notmatch '^[A-Fa-f0-9]{64}$') {
        throw "'$Path' has no valid package-manifest SHA-256."
    }
    if (-not $AllowDirtyHarness -and [bool]$Run.Source.GitWorkingTreeDirty) {
        throw "'$Path' was captured with a dirty benchmark-harness worktree."
    }

    $before = $Run.Host.EnvironmentBeforeLaunch
    $start = $Run.Host.EnvironmentBeforeMeasurement
    $after = $Run.Host.EnvironmentAfterMeasurement
    Assert-Equal $before.UserNotificationState.Code $start.UserNotificationState.Code "$Path QUNS before/start"
    Assert-Equal $before.UserNotificationState.Code $after.UserNotificationState.Code "$Path QUNS before/after"
    Assert-Equal $before.Power.ActiveSchemeGuid $start.Power.ActiveSchemeGuid "$Path power scheme before/start"
    Assert-Equal $before.Power.ActiveSchemeGuid $after.Power.ActiveSchemeGuid "$Path power scheme before/after"
    Assert-Equal $before.Power.ACLineStatus $start.Power.ACLineStatus "$Path AC source before/start"
    Assert-Equal $before.Power.ACLineStatus $after.Power.ACLineStatus "$Path AC source before/after"

    if ([string]$Run.Run.RequiredUserNotificationState -ne "Any" -and
        [string]$Run.Run.RequiredUserNotificationState -ne [string]$before.UserNotificationState.Name) {
        throw "'$Path' did not capture its required user-notification state."
    }
    if ([bool]$Run.Run.RequiredEnergySaverOff -and
        ([bool]$before.Power.EnergySaverEnabled -or
            [bool]$start.Power.EnergySaverEnabled -or
            [bool]$after.Power.EnergySaverEnabled)) {
        throw "'$Path' required Energy Saver off but captured it enabled."
    }

    $requiredAdaptiveResidency = if ($null -ne $Run.Run.PSObject.Properties['RequiredAdaptiveResidency']) {
        [string]$Run.Run.RequiredAdaptiveResidency
    }
    else {
        "Any"
    }
    if ($requiredAdaptiveResidency -ne "Any") {
        $adaptiveStart = $Run.Host.AdaptiveResourcesBeforeMeasurement
        $adaptiveAfter = $Run.Host.AdaptiveResourcesAfterMeasurement
        if ($null -eq $adaptiveStart -or $null -eq $adaptiveAfter) {
            throw "'$Path' required adaptive residency but has no adaptive-resource snapshots."
        }
        if (-not [bool]$adaptiveStart.evaluated -or -not [bool]$adaptiveAfter.evaluated) {
            throw "'$Path' captured adaptive resources before policy evaluation."
        }
        if ([string]$adaptiveStart.residency -ne $requiredAdaptiveResidency -or
            [string]$adaptiveAfter.residency -ne $requiredAdaptiveResidency) {
            throw "'$Path' did not remain in its required adaptive residency."
        }
        if ($requiredAdaptiveResidency -eq "Hot" -and
            (-not [bool]$adaptiveStart.uiResourcesLoaded -or
                -not [bool]$adaptiveStart.uiResourcesInitialized -or
                -not [bool]$adaptiveAfter.uiResourcesLoaded -or
                -not [bool]$adaptiveAfter.uiResourcesInitialized)) {
            throw "'$Path' recorded Hot residency without initialized, loaded UI resources."
        }
        if ($requiredAdaptiveResidency -eq "Cold" -and
            ([bool]$adaptiveStart.uiResourcesLoaded -or [bool]$adaptiveAfter.uiResourcesLoaded)) {
            throw "'$Path' recorded Cold residency with loaded UI resources."
        }
    }

    if ([double]$Run.Summary.ScheduleLatenessMilliseconds.Maximum -gt $MaximumScheduleLatenessMilliseconds) {
        throw "'$Path' exceeded the maximum schedule lateness: " +
            "$($Run.Summary.ScheduleLatenessMilliseconds.Maximum) ms."
    }

    $durationMilliseconds = [double]$Run.Run.RequestedDurationSeconds * 1000.0
    $intervalMilliseconds = [double]$Run.Run.SampleIntervalMilliseconds
    $expectedSamples = [int][Math]::Ceiling($durationMilliseconds / $intervalMilliseconds) + 1
    if ([int]$Run.Run.SampleCount -ne $expectedSamples) {
        throw "'$Path' has $($Run.Run.SampleCount) samples; expected $expectedSamples."
    }
}

function Get-UniqueValue {
    param(
        [Parameter(Mandatory)]
        [object[]]$Runs,

        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string]$Label
    )

    $values = @($Runs | ForEach-Object {
            $value = $_
            foreach ($segment in $Path.Split('.')) {
                $value = $value.PSObject.Properties[$segment].Value
            }
            [string]$value
        } | Sort-Object -Unique)
    if ($values.Count -ne 1) {
        throw "$Label must be identical across runs; found: $($values -join ', ')."
    }
    return $values[0]
}

function Format-InvariantNumber {
    param(
        [Parameter(Mandatory)]
        [double]$Value
    )

    return $Value.ToString("0.###", [Globalization.CultureInfo]::InvariantCulture)
}

$inputRoot = [System.IO.Path]::GetFullPath($InputDirectory)
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
    $OutputDirectory = Join-Path $repositoryRoot "artifacts\perf\comparisons"
}
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)

$selectedRuns = [System.Collections.Generic.List[object]]::new()
foreach ($file in Get-ChildItem -LiteralPath $inputRoot -Filter '*.json' -File -Recurse) {
    if ($file.Length -gt 16MB) {
        throw "Benchmark artifact '$($file.FullName)' exceeds the 16 MiB input limit."
    }
    try {
        $run = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    }
    catch {
        throw "Could not parse '$($file.FullName)': $($_.Exception.Message)"
    }

    if ($null -eq $run.PSObject.Properties['Run']) {
        throw "'$($file.FullName)' is not a resource-benchmark artifact."
    }
    if ([string]$run.Run.Scenario -ne $Scenario) {
        continue
    }
    if ([string]$run.Run.Variant -ne $BaselineVariant -and
        [string]$run.Run.Variant -ne $CandidateVariant) {
        continue
    }

    Assert-RunIntegrity -Run $run -Path $file.FullName
    $run | Add-Member -NotePropertyName ArtifactPath -NotePropertyValue $file.FullName
    $selectedRuns.Add($run)
}

if ($selectedRuns.Count -eq 0) {
    throw "No matching benchmark runs were found in '$inputRoot'."
}

$baselineRuns = @($selectedRuns | Where-Object { $_.Run.Variant -eq $BaselineVariant })
$candidateRuns = @($selectedRuns | Where-Object { $_.Run.Variant -eq $CandidateVariant })
if ($baselineRuns.Count -eq 0 -or $candidateRuns.Count -eq 0) {
    throw "Both variants must have matching runs. Found baseline=$($baselineRuns.Count), candidate=$($candidateRuns.Count)."
}

$baselineByPair = @{}
foreach ($run in $baselineRuns) {
    $pairId = [string]$run.Run.PairId
    if ($baselineByPair.ContainsKey($pairId)) {
        throw "Baseline pair '$pairId' appears more than once."
    }
    $baselineByPair[$pairId] = $run
}
$candidateByPair = @{}
foreach ($run in $candidateRuns) {
    $pairId = [string]$run.Run.PairId
    if ($candidateByPair.ContainsKey($pairId)) {
        throw "Candidate pair '$pairId' appears more than once."
    }
    $candidateByPair[$pairId] = $run
}

$pairIds = @($baselineByPair.Keys | Sort-Object)
if ($pairIds.Count -lt $MinimumPairs) {
    throw "Found $($pairIds.Count) baseline pairs; at least $MinimumPairs are required."
}
if ($pairIds.Count -gt 1000) {
    throw "At most 1000 paired runs can be compared at once."
}
if ($candidateByPair.Count -ne $pairIds.Count) {
    throw "Variant pair counts differ: baseline=$($pairIds.Count), candidate=$($candidateByPair.Count)."
}
foreach ($pairId in $pairIds) {
    if (-not $candidateByPair.ContainsKey($pairId)) {
        throw "Candidate run for pair '$pairId' is missing."
    }
}

$allRuns = @($baselineRuns) + @($candidateRuns)
$harnessCommit = Get-UniqueValue $allRuns 'Source.GitCommit' 'Harness Git commit'
$baselineExecutableHash = Get-UniqueValue $baselineRuns 'Package.ExecutableSha256' 'Baseline executable hash'
$candidateExecutableHash = Get-UniqueValue $candidateRuns 'Package.ExecutableSha256' 'Candidate executable hash'
if (-not $AllowSameExecutable -and $baselineExecutableHash -eq $candidateExecutableHash) {
    throw "Baseline and candidate executable hashes are identical."
}

$baselineManifestHash = Get-UniqueValue $baselineRuns 'Package.ManifestSha256' 'Baseline manifest hash'
$candidateManifestHash = Get-UniqueValue $candidateRuns 'Package.ManifestSha256' 'Candidate manifest hash'
$baselinePackageVersion = Get-UniqueValue $baselineRuns 'Package.Version' 'Baseline package version'
$candidatePackageVersion = Get-UniqueValue $candidateRuns 'Package.Version' 'Candidate package version'

$metrics = @(
    [ordered]@{ Name = 'PrivateWorkingSetAverageBytes'; Path = 'Summary.PrivateWorkingSetBytes.Average'; Unit = 'bytes' },
    [ordered]@{ Name = 'PrivateCommitAverageBytes'; Path = 'Summary.PrivateCommitBytes.Average'; Unit = 'bytes' },
    [ordered]@{ Name = 'CpuAveragePercentOneCore'; Path = 'Summary.CpuAveragePercentOneCore'; Unit = 'percent' },
    [ordered]@{ Name = 'ThreadCountP50'; Path = 'Summary.ThreadCount.P50'; Unit = 'count' },
    [ordered]@{ Name = 'HandleCountP50'; Path = 'Summary.HandleCount.P50'; Unit = 'count' }
)

$pairRecords = [System.Collections.Generic.List[object]]::new()
foreach ($pairId in $pairIds) {
    $baseline = $baselineByPair[$pairId]
    $candidate = $candidateByPair[$pairId]

    foreach ($property in @(
            'Package.Name',
            'Package.FamilyName',
            'Package.Publisher',
            'Package.Architecture',
            'Run.WarmupSeconds',
            'Run.RequestedDurationSeconds',
            'Run.SampleIntervalMilliseconds',
            'Host.MachineName',
            'Host.OperatingSystemBuildNumber',
            'Host.LogicalProcessorCount',
            'Host.TotalVisibleMemoryBytes',
            'Host.EnvironmentBeforeLaunch.UserNotificationState.Code',
            'Host.EnvironmentBeforeLaunch.Power.ActiveSchemeGuid',
            'Host.EnvironmentBeforeLaunch.Power.ACLineStatus',
            'Host.EnvironmentBeforeLaunch.Power.EnergySaverEnabled'
        )) {
        $baselineValue = $baseline
        $candidateValue = $candidate
        foreach ($segment in $property.Split('.')) {
            $baselineValue = $baselineValue.PSObject.Properties[$segment].Value
            $candidateValue = $candidateValue.PSObject.Properties[$segment].Value
        }
        Assert-Equal $baselineValue $candidateValue "Pair '$pairId' $property"
    }

    $baselineAdaptiveResidency = if ($null -ne $baseline.Run.PSObject.Properties['RequiredAdaptiveResidency']) {
        [string]$baseline.Run.RequiredAdaptiveResidency
    }
    else {
        "Any"
    }
    $candidateAdaptiveResidency = if ($null -ne $candidate.Run.PSObject.Properties['RequiredAdaptiveResidency']) {
        [string]$candidate.Run.RequiredAdaptiveResidency
    }
    else {
        "Any"
    }
    Assert-Equal `
        $baselineAdaptiveResidency `
        $candidateAdaptiveResidency `
        "Pair '$pairId' required adaptive residency"

    $values = [ordered]@{}
    foreach ($metric in $metrics) {
        $baselineValue = Get-PropertyPathValue $baseline $metric.Path
        $candidateValue = Get-PropertyPathValue $candidate $metric.Path
        $values[$metric.Name] = [ordered]@{
            Baseline = $baselineValue
            Candidate = $candidateValue
            Difference = $candidateValue - $baselineValue
            PercentDifference = if ($baselineValue -ne 0) {
                (($candidateValue - $baselineValue) / $baselineValue) * 100.0
            }
            else {
                $null
            }
        }
    }

    $pairRecords.Add([pscustomobject][ordered]@{
            PairId = $pairId
            BaselineArtifact = $baseline.ArtifactPath
            CandidateArtifact = $candidate.ArtifactPath
            Values = $values
        })
}

$metricResults = [System.Collections.Generic.List[object]]::new()
for ($metricIndex = 0; $metricIndex -lt $metrics.Count; $metricIndex++) {
    $metric = $metrics[$metricIndex]
    $baselineValues = [double[]]@($pairRecords | ForEach-Object { $_.Values[$metric.Name].Baseline })
    $candidateValues = [double[]]@($pairRecords | ForEach-Object { $_.Values[$metric.Name].Candidate })
    $differences = [double[]]@($pairRecords | ForEach-Object { $_.Values[$metric.Name].Difference })
    $percentDifferences = [double[]]@($pairRecords | ForEach-Object {
            if ($null -ne $_.Values[$metric.Name].PercentDifference) {
                $_.Values[$metric.Name].PercentDifference
            }
        })
    $differenceMedian = Get-Median -Values $differences
    $absoluteDeviations = [double[]]@($differences | ForEach-Object { [Math]::Abs($_ - $differenceMedian) })
    $confidence = Get-BootstrapMedianInterval `
        -Values $differences `
        -Iterations $BootstrapIterations `
        -Seed ($RandomSeed + $metricIndex)

    $metricResults.Add([pscustomobject][ordered]@{
            Name = $metric.Name
            Unit = $metric.Unit
            BaselineMedian = Get-Median -Values $baselineValues
            CandidateMedian = Get-Median -Values $candidateValues
            PairedDifferenceMedian = $differenceMedian
            PairedDifferenceMedianAbsoluteDeviation = Get-Median -Values $absoluteDeviations
            PairedDifferenceBootstrap95 = $confidence
            PairedPercentDifferenceMedian = if ($percentDifferences.Count) {
                Get-Median -Values $percentDifferences
            }
            else {
                $null
            }
        })
}

$createdUtc = [DateTime]::UtcNow
$result = [pscustomobject][ordered]@{
    SchemaVersion = 1
    CreatedUtc = $createdUtc.ToString('o')
    Scenario = $Scenario
    PairCount = $pairRecords.Count
    BootstrapIterations = $BootstrapIterations
    RandomSeed = $RandomSeed
    MaximumScheduleLatenessMilliseconds = $MaximumScheduleLatenessMilliseconds
    Harness = [ordered]@{
        GitCommit = $harnessCommit
        DirtyRunsAllowed = [bool]$AllowDirtyHarness
    }
    Baseline = [ordered]@{
        Variant = $BaselineVariant
        PackageVersion = $baselinePackageVersion
        ExecutableSha256 = $baselineExecutableHash
        ManifestSha256 = $baselineManifestHash
    }
    Candidate = [ordered]@{
        Variant = $CandidateVariant
        PackageVersion = $candidatePackageVersion
        ExecutableSha256 = $candidateExecutableHash
        ManifestSha256 = $candidateManifestHash
    }
    Metrics = @($metricResults)
    Pairs = @($pairRecords)
}

$safeScenario = ($Scenario -replace '[^A-Za-z0-9._-]', '-') -replace '-+', '-'
$safeScenario = $safeScenario.Trim('-')
if ([string]::IsNullOrWhiteSpace($safeScenario)) {
    $safeScenario = 'scenario'
}
$fileStem = "$($createdUtc.ToString("yyyyMMdd'T'HHmmss.fff'Z'"))-$safeScenario-$BaselineVariant-vs-$CandidateVariant"
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$jsonPath = Join-Path $outputRoot "$fileStem.json"
$markdownPath = Join-Path $outputRoot "$fileStem.md"
$temporaryJsonPath = "$jsonPath.tmp"
$temporaryMarkdownPath = "$markdownPath.tmp"

$markdown = [System.Collections.Generic.List[string]]::new()
$markdown.Add("# Resource comparison: $Scenario")
$markdown.Add("")
$markdown.Add("- Pairs: $($pairRecords.Count)")
$markdown.Add("- Baseline: ``$BaselineVariant`` ($baselinePackageVersion)")
$markdown.Add("- Candidate: ``$CandidateVariant`` ($candidatePackageVersion)")
$markdown.Add("- Bootstrap iterations: $BootstrapIterations (seed $RandomSeed)")
$markdown.Add("")
$markdown.Add("| Metric | Baseline median | Candidate median | Paired median difference | Median difference 95% bootstrap interval | Paired median % |")
$markdown.Add("|---|---:|---:|---:|---:|---:|")
foreach ($metric in $metricResults) {
    $percent = if ($null -ne $metric.PairedPercentDifferenceMedian) {
        Format-InvariantNumber $metric.PairedPercentDifferenceMedian
    }
    else {
        'n/a'
    }
    $markdown.Add(
        "| $($metric.Name) | $(Format-InvariantNumber $metric.BaselineMedian) | " +
        "$(Format-InvariantNumber $metric.CandidateMedian) | " +
        "$(Format-InvariantNumber $metric.PairedDifferenceMedian) | " +
        "[$(Format-InvariantNumber $metric.PairedDifferenceBootstrap95.Lower95), " +
        "$(Format-InvariantNumber $metric.PairedDifferenceBootstrap95.Upper95)] | $percent |"
    )
}
$markdown.Add("")
$markdown.Add("Negative differences mean the candidate used fewer resources than the baseline.")

try {
    $result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporaryJsonPath -Encoding UTF8
    $markdown | Set-Content -LiteralPath $temporaryMarkdownPath -Encoding UTF8
    Move-Item -LiteralPath $temporaryJsonPath -Destination $jsonPath -Force
    Move-Item -LiteralPath $temporaryMarkdownPath -Destination $markdownPath -Force
}
finally {
    Remove-Item -LiteralPath $temporaryJsonPath, $temporaryMarkdownPath -Force -ErrorAction SilentlyContinue
}

Write-Information "Resource comparison completed." -InformationAction Continue
Write-Information "JSON:     $jsonPath" -InformationAction Continue
Write-Information "Markdown: $markdownPath" -InformationAction Continue

if ($PassThru) {
    $result
}
