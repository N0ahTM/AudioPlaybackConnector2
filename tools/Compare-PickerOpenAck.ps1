#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$InputDirectory,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$BaselineVariant,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$CandidateVariant,

    [Parameter()]
    [ValidateRange(2, 1000)]
    [int]$MinimumPairs = 20,

    [Parameter()]
    [ValidateRange(100, 1000000)]
    [int]$BootstrapIterations = 10000,

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
        [double]$Percentile
    )

    if ($SortedValues.Count -eq 0) { return $null }
    if ($SortedValues.Count -eq 1) { return $SortedValues[0] }
    $position = ($Percentile / 100.0) * ($SortedValues.Count - 1)
    $lower = [int][Math]::Floor($position)
    $upper = [int][Math]::Ceiling($position)
    if ($lower -eq $upper) { return $SortedValues[$lower] }
    $fraction = $position - $lower
    return ($SortedValues[$lower] * (1.0 - $fraction)) + ($SortedValues[$upper] * $fraction)
}

function Get-Median {
    param([Parameter(Mandatory)][double[]]$Values)
    return Get-Percentile -SortedValues ([double[]]@($Values | Sort-Object)) -Percentile 50
}

function Get-MedianAbsoluteDeviation {
    param([Parameter(Mandatory)][double[]]$Values)
    $median = Get-Median $Values
    return Get-Median ([double[]]@($Values | ForEach-Object { [Math]::Abs($_ - $median) }))
}

function Assert-Equal {
    param($Left, $Right, [string]$Label)
    if ($Left -ne $Right) {
        throw "$Label differs: '$Left' versus '$Right'."
    }
}

function Assert-StableEnvironment {
    param([Parameter(Mandatory)][object]$Run, [Parameter(Mandatory)][string]$Path)
    $before = $Run.Host.EnvironmentBeforeLaunch
    foreach ($phaseName in @("EnvironmentBeforeOpen", "EnvironmentAfterOpen")) {
        $phase = $Run.Host.$phaseName
        Assert-Equal $before.UserNotificationState.Code $phase.UserNotificationState.Code "$Path QUNS"
        Assert-Equal $before.ActivePowerSchemeGuid $phase.ActivePowerSchemeGuid "$Path power scheme"
        Assert-Equal $before.ACLineStatus $phase.ACLineStatus "$Path AC source"
        Assert-Equal $before.EnergySaverEnabled $phase.EnergySaverEnabled "$Path Energy Saver"
    }
}

$resolvedInput = (Resolve-Path -LiteralPath $InputDirectory).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $resolvedInput "comparison"
}

$runs = [System.Collections.Generic.List[object]]::new()
foreach ($file in Get-ChildItem -LiteralPath $resolvedInput -File -Filter "*.json") {
    $run = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    if ($run.Metric -ne "ControlToFlyoutOpenedAck") { continue }
    if ($run.SchemaVersion -ne 1) { throw "Unsupported schema in '$($file.FullName)'." }
    if ($run.Run.Variant -notin @($BaselineVariant, $CandidateVariant)) { continue }
    if ([string]::IsNullOrWhiteSpace([string]$run.Run.PairId)) {
        throw "Missing PairId in '$($file.FullName)'."
    }
    if (-not $AllowDirtyHarness -and [bool]$run.Source.GitWorkingTreeDirty) {
        throw "Dirty harness worktree in '$($file.FullName)'."
    }
    $value = [double]$run.Run.ControlToFlyoutOpenedAckMilliseconds
    if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -le 0) {
        throw "Invalid acknowledgement latency in '$($file.FullName)'."
    }
    if ([uint64]$run.Run.FinalPickerOpenedGeneration -le [uint64]$run.Run.InitialPickerOpenedGeneration) {
        throw "Picker generation did not advance in '$($file.FullName)'."
    }
    Assert-StableEnvironment -Run $run -Path $file.FullName
    $runs.Add([pscustomobject]@{ File = $file.FullName; Data = $run; Value = $value })
}

$groups = $runs | Group-Object { "$($_.Data.Run.Variant)|$($_.Data.Run.PairId)" }
foreach ($group in $groups) {
    if ($group.Count -ne 1) { throw "Duplicate run '$($group.Name)'." }
}

$baselineRuns = @($runs | Where-Object { $_.Data.Run.Variant -eq $BaselineVariant })
$candidateRuns = @($runs | Where-Object { $_.Data.Run.Variant -eq $CandidateVariant })
if ($baselineRuns.Count -lt $MinimumPairs -or $candidateRuns.Count -lt $MinimumPairs) {
    throw "Found $($baselineRuns.Count) baseline and $($candidateRuns.Count) candidate runs; at least $MinimumPairs pairs are required."
}

foreach ($variantRuns in @($baselineRuns, $candidateRuns)) {
    $first = $variantRuns[0].Data
    foreach ($entry in $variantRuns) {
        Assert-Equal $first.Package.ExecutableSha256 $entry.Data.Package.ExecutableSha256 "Variant executable hash"
        Assert-Equal $first.Package.Version $entry.Data.Package.Version "Variant package version"
    }
}
if (-not $AllowSameExecutable) {
    Assert-Equal $false ($baselineRuns[0].Data.Package.ExecutableSha256 -eq $candidateRuns[0].Data.Package.ExecutableSha256) `
        "Baseline and candidate executable identity"
}

$baselineByPair = @{}
foreach ($entry in $baselineRuns) { $baselineByPair[[string]$entry.Data.Run.PairId] = $entry }
$candidateByPair = @{}
foreach ($entry in $candidateRuns) { $candidateByPair[[string]$entry.Data.Run.PairId] = $entry }
$pairIds = @($baselineByPair.Keys | Where-Object { $candidateByPair.ContainsKey($_) } | Sort-Object)
if ($pairIds.Count -lt $MinimumPairs) { throw "Only $($pairIds.Count) complete pairs were found." }
if ($pairIds.Count -ne $baselineRuns.Count -or $pairIds.Count -ne $candidateRuns.Count) {
    throw "Unpaired picker acknowledgement runs were found."
}

$differences = [System.Collections.Generic.List[double]]::new()
$pairs = [System.Collections.Generic.List[object]]::new()
foreach ($pairId in $pairIds) {
    $baseline = $baselineByPair[$pairId]
    $candidate = $candidateByPair[$pairId]
    foreach ($property in @("Name", "FamilyName", "Publisher", "Architecture")) {
        Assert-Equal $baseline.Data.Package.$property $candidate.Data.Package.$property "$pairId package $property"
    }
    $baselineEnvironment = $baseline.Data.Host.EnvironmentBeforeLaunch
    $candidateEnvironment = $candidate.Data.Host.EnvironmentBeforeLaunch
    Assert-Equal $baselineEnvironment.UserNotificationState.Code $candidateEnvironment.UserNotificationState.Code "$pairId QUNS"
    Assert-Equal $baselineEnvironment.ActivePowerSchemeGuid $candidateEnvironment.ActivePowerSchemeGuid "$pairId power scheme"
    Assert-Equal $baselineEnvironment.ACLineStatus $candidateEnvironment.ACLineStatus "$pairId AC source"
    Assert-Equal $baselineEnvironment.EnergySaverEnabled $candidateEnvironment.EnergySaverEnabled "$pairId Energy Saver"
    $difference = $candidate.Value - $baseline.Value
    $differences.Add($difference)
    $pairs.Add([ordered]@{
            PairId = $pairId
            BaselineMilliseconds = $baseline.Value
            CandidateMilliseconds = $candidate.Value
            DifferenceMilliseconds = $difference
        })
}

$differenceArray = [double[]]$differences.ToArray()
$random = [System.Random]::new(20260815)
$bootstrapMedians = [double[]]::new($BootstrapIterations)
for ($iteration = 0; $iteration -lt $BootstrapIterations; $iteration++) {
    $sample = [double[]]::new($differenceArray.Count)
    for ($index = 0; $index -lt $sample.Count; $index++) {
        $sample[$index] = $differenceArray[$random.Next($differenceArray.Count)]
    }
    $bootstrapMedians[$iteration] = Get-Median $sample
}
[Array]::Sort($bootstrapMedians)

$baselineValues = [double[]]@($baselineRuns | ForEach-Object Value)
$candidateValues = [double[]]@($candidateRuns | ForEach-Object Value)
$result = [ordered]@{
    SchemaVersion = 1
    Metric = "ControlToFlyoutOpenedAck"
    CreatedUtc = [DateTime]::UtcNow.ToString("o")
    PairCount = $pairs.Count
    Baseline = [ordered]@{
        Variant = $BaselineVariant
        Version = $baselineRuns[0].Data.Package.Version
        ExecutableSha256 = $baselineRuns[0].Data.Package.ExecutableSha256
        MedianMilliseconds = Get-Median $baselineValues
        MedianAbsoluteDeviationMilliseconds = Get-MedianAbsoluteDeviation $baselineValues
    }
    Candidate = [ordered]@{
        Variant = $CandidateVariant
        Version = $candidateRuns[0].Data.Package.Version
        ExecutableSha256 = $candidateRuns[0].Data.Package.ExecutableSha256
        MedianMilliseconds = Get-Median $candidateValues
        MedianAbsoluteDeviationMilliseconds = Get-MedianAbsoluteDeviation $candidateValues
    }
    PairedDifference = [ordered]@{
        MedianMilliseconds = Get-Median $differenceArray
        MedianAbsoluteDeviationMilliseconds = Get-MedianAbsoluteDeviation $differenceArray
        Bootstrap95 = [ordered]@{
            LowerMilliseconds = Get-Percentile $bootstrapMedians 2.5
            UpperMilliseconds = Get-Percentile $bootstrapMedians 97.5
            Iterations = $BootstrapIterations
            Seed = 20260815
        }
    }
    Pairs = $pairs
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$timestamp = [DateTime]::UtcNow.ToString("yyyyMMddTHHmmss.fffZ")
$baseName = "$timestamp-picker-open-ack-$BaselineVariant-vs-$CandidateVariant"
$jsonPath = Join-Path $OutputDirectory "$baseName.json"
$markdownPath = Join-Path $OutputDirectory "$baseName.md"
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
$markdown = @"
# Picker Opened acknowledgement comparison

- Pairs: $($result.PairCount)
- Baseline: ``$BaselineVariant`` $($result.Baseline.Version), median $([Math]::Round($result.Baseline.MedianMilliseconds, 3)) ms, MAD $([Math]::Round($result.Baseline.MedianAbsoluteDeviationMilliseconds, 3)) ms
- Candidate: ``$CandidateVariant`` $($result.Candidate.Version), median $([Math]::Round($result.Candidate.MedianMilliseconds, 3)) ms, MAD $([Math]::Round($result.Candidate.MedianAbsoluteDeviationMilliseconds, 3)) ms
- Paired candidate-minus-baseline median: $([Math]::Round($result.PairedDifference.MedianMilliseconds, 3)) ms
- Deterministic bootstrap 95% interval: [$([Math]::Round($result.PairedDifference.Bootstrap95.LowerMilliseconds, 3)), $([Math]::Round($result.PairedDifference.Bootstrap95.UpperMilliseconds, 3))] ms

This metric ends at the matching WinUI ``Flyout.Opened`` callback. It does not measure compositor presentation or display scanout.
"@
$markdown | Set-Content -LiteralPath $markdownPath -Encoding UTF8
Write-Host "Picker acknowledgement comparison completed."
Write-Host "JSON:     $jsonPath"
Write-Host "Markdown: $markdownPath"
if ($PassThru) { [pscustomobject]$result }
