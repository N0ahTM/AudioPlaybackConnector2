#Requires -Version 5.1

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-SyntheticEnvironment {
    param(
        [Parameter()]
        [int]$NotificationState = 5
    )

    return [ordered]@{
        UserNotificationState = [ordered]@{
            Code = $NotificationState
            Name = if ($NotificationState -eq 5) { "AcceptsNotifications" } else { "RunningD3dFullScreen" }
        }
        Power = [ordered]@{
            ActiveSchemeGuid = "00000000-0000-0000-0000-000000000001"
            ACLineStatus = 1
            EnergySaverEnabled = $false
        }
    }
}

function Get-SyntheticRun {
    param(
        [Parameter(Mandatory)]
        [string]$PairId,

        [Parameter(Mandatory)]
        [string]$Variant,

        [Parameter(Mandatory)]
        [string]$ExecutableHash,

        [Parameter(Mandatory)]
        [double]$PrivateWorkingSet,

        [Parameter()]
        [int]$NotificationState = 5,

        [Parameter()]
        [bool]$Dirty = $false,

        [Parameter()]
        [string]$AdaptiveResidency = "Hot",

        [Parameter()]
        [bool]$UiResourcesLoaded = $true
    )

    $environment = Get-SyntheticEnvironment -NotificationState $NotificationState
    return [ordered]@{
        SchemaVersion = 2
        Run = [ordered]@{
            Scenario = "synthetic-hot"
            Variant = $Variant
            PairId = $PairId
            WarmupSeconds = 35
            RequestedDurationSeconds = 2
            ActualDurationSeconds = 2.01
            SampleIntervalMilliseconds = 1000
            SampleCount = 3
            RequiredUserNotificationState = if ($NotificationState -eq 5) {
                "AcceptsNotifications"
            }
            else {
                "RunningD3dFullScreen"
            }
            RequiredAdaptiveResidency = $AdaptiveResidency
            RequiredEnergySaverOff = $true
        }
        Package = [ordered]@{
            Name = "N0ahTM.AudioPlaybackConnector2"
            FamilyName = "N0ahTM.AudioPlaybackConnector2_test"
            Version = if ($Variant -eq "baseline") { "0.7.1.0" } else { "0.7.2.0" }
            Publisher = "CN=AudioPlaybackConnector2"
            Architecture = "X64"
            ExecutableSha256 = $ExecutableHash
            ManifestSha256 = if ($Variant -eq "baseline") { "C" * 64 } else { "D" * 64 }
        }
        Host = [ordered]@{
            MachineName = "synthetic-host"
            OperatingSystemBuildNumber = "26200"
            LogicalProcessorCount = 16
            TotalVisibleMemoryBytes = 32GB
            EnvironmentBeforeLaunch = $environment
            EnvironmentBeforeMeasurement = $environment
            EnvironmentAfterMeasurement = $environment
            AdaptiveResourcesBeforeMeasurement = [ordered]@{
                evaluated = $true
                residency = $AdaptiveResidency
                uiResourcesLoaded = $UiResourcesLoaded
                uiResourcesInitialized = $UiResourcesLoaded
            }
            AdaptiveResourcesAfterMeasurement = [ordered]@{
                evaluated = $true
                residency = $AdaptiveResidency
                uiResourcesLoaded = $UiResourcesLoaded
                uiResourcesInitialized = $UiResourcesLoaded
            }
        }
        Source = [ordered]@{
            GitCommit = "0123456789abcdef0123456789abcdef01234567"
            GitWorkingTreeDirty = $Dirty
        }
        Summary = [ordered]@{
            PrivateWorkingSetBytes = [ordered]@{ Average = $PrivateWorkingSet }
            PrivateCommitBytes = [ordered]@{ Average = $PrivateWorkingSet + 50 }
            CpuAveragePercentOneCore = $PrivateWorkingSet / 100
            ThreadCount = [ordered]@{ P50 = 50 }
            HandleCount = [ordered]@{ P50 = 900 }
            ScheduleLatenessMilliseconds = [ordered]@{ Maximum = 10 }
        }
    }
}

function Write-SyntheticRun {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [object]$Run
    )

    $Run | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Invoke-ExpectedFailure {
    param(
        [Parameter(Mandatory)]
        [scriptblock]$Action,

        [Parameter(Mandatory)]
        [string]$MessagePattern
    )

    try {
        & $Action
    }
    catch {
        if ($_.Exception.Message -match $MessagePattern) {
            return
        }
        throw
    }
    throw "Expected failure matching '$MessagePattern' did not occur."
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$compareScript = Join-Path $repositoryRoot "tools\Compare-ResourceUsage.ps1"
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("apc2-perf-tools-" + [Guid]::NewGuid().ToString("N"))
$runsRoot = Join-Path $testRoot "runs"
$outputRoot = Join-Path $testRoot "output"

try {
    New-Item -ItemType Directory -Path $runsRoot, $outputRoot -Force | Out-Null
    $baselineHash = "A" * 64
    $candidateHash = "B" * 64
    Write-SyntheticRun (Join-Path $runsRoot "baseline-r01.json") `
        (Get-SyntheticRun "r01" "baseline" $baselineHash 100)
    Write-SyntheticRun (Join-Path $runsRoot "candidate-r01.json") `
        (Get-SyntheticRun "r01" "candidate" $candidateHash 90)
    Write-SyntheticRun (Join-Path $runsRoot "baseline-r02.json") `
        (Get-SyntheticRun "r02" "baseline" $baselineHash 110)
    Write-SyntheticRun (Join-Path $runsRoot "candidate-r02.json") `
        (Get-SyntheticRun "r02" "candidate" $candidateHash 95)

    $comparison = & $compareScript `
        -InputDirectory $runsRoot `
        -Scenario synthetic-hot `
        -BaselineVariant baseline `
        -CandidateVariant candidate `
        -MinimumPairs 2 `
        -BootstrapIterations 200 `
        -OutputDirectory $outputRoot `
        -PassThru
    if ($comparison.PairCount -ne 2) {
        throw "Expected two paired runs."
    }
    $workingSet = $comparison.Metrics | Where-Object Name -eq "PrivateWorkingSetAverageBytes"
    if ($workingSet.PairedDifferenceMedian -ne -12.5) {
        throw "Unexpected paired working-set median: $($workingSet.PairedDifferenceMedian)."
    }
    if ($workingSet.PairedDifferenceBootstrap95.Lower95 -gt -10 -or
        $workingSet.PairedDifferenceBootstrap95.Upper95 -lt -15) {
        throw "Bootstrap interval did not cover both synthetic paired differences."
    }

    Write-SyntheticRun (Join-Path $runsRoot "candidate-r01.json") `
        (Get-SyntheticRun "r01" "candidate" $candidateHash 90 -AdaptiveResidency Cold -UiResourcesLoaded $false)
    Write-SyntheticRun (Join-Path $runsRoot "candidate-r02.json") `
        (Get-SyntheticRun "r02" "candidate" $candidateHash 95 -AdaptiveResidency Cold -UiResourcesLoaded $false)
    $differentResidencies = & $compareScript `
        -InputDirectory $runsRoot `
        -Scenario synthetic-hot `
        -BaselineVariant baseline `
        -CandidateVariant candidate `
        -MinimumPairs 2 `
        -BootstrapIterations 100 `
        -OutputDirectory $outputRoot `
        -PassThru
    if ($differentResidencies.Baseline.RequiredAdaptiveResidency -ne "Hot" -or
        $differentResidencies.Candidate.RequiredAdaptiveResidency -ne "Cold") {
        throw "Variant-specific adaptive-residency requirements were not preserved."
    }

    Write-SyntheticRun (Join-Path $runsRoot "candidate-r01.json") `
        (Get-SyntheticRun "r01" "candidate" $candidateHash 90)

    Write-SyntheticRun (Join-Path $runsRoot "candidate-r02.json") `
        (Get-SyntheticRun "r02" "candidate" $candidateHash 95 -UiResourcesLoaded $false)
    Invoke-ExpectedFailure {
        & $compareScript `
            -InputDirectory $runsRoot `
            -Scenario synthetic-hot `
            -BaselineVariant baseline `
            -CandidateVariant candidate `
            -MinimumPairs 2 `
            -BootstrapIterations 100 `
            -OutputDirectory $outputRoot
    } "Hot residency without initialized, loaded UI resources"

    Write-SyntheticRun (Join-Path $runsRoot "candidate-r02.json") `
        (Get-SyntheticRun "r02" "candidate" $baselineHash 95)
    Invoke-ExpectedFailure {
        & $compareScript `
            -InputDirectory $runsRoot `
            -Scenario synthetic-hot `
            -BaselineVariant baseline `
            -CandidateVariant candidate `
            -MinimumPairs 2 `
            -BootstrapIterations 100 `
            -OutputDirectory $outputRoot
    } "Candidate executable hash must be identical across runs"

    Write-SyntheticRun (Join-Path $runsRoot "candidate-r02.json") `
        (Get-SyntheticRun "r02" "candidate" $candidateHash 95 -NotificationState 3)
    Invoke-ExpectedFailure {
        & $compareScript `
            -InputDirectory $runsRoot `
            -Scenario synthetic-hot `
            -BaselineVariant baseline `
            -CandidateVariant candidate `
            -MinimumPairs 2 `
            -BootstrapIterations 100 `
            -OutputDirectory $outputRoot
    } "UserNotificationState.Code differs"

    Write-SyntheticRun (Join-Path $runsRoot "candidate-r01.json") `
        (Get-SyntheticRun "r01" "candidate" $baselineHash 90)
    Write-SyntheticRun (Join-Path $runsRoot "candidate-r02.json") `
        (Get-SyntheticRun "r02" "candidate" $baselineHash 95)
    Invoke-ExpectedFailure {
        & $compareScript `
            -InputDirectory $runsRoot `
            -Scenario synthetic-hot `
            -BaselineVariant baseline `
            -CandidateVariant candidate `
            -MinimumPairs 2 `
            -BootstrapIterations 100 `
            -OutputDirectory $outputRoot
    } "executable hashes are identical"

    Write-SyntheticRun (Join-Path $runsRoot "candidate-r01.json") `
        (Get-SyntheticRun "r01" "candidate" $candidateHash 90)
    Write-SyntheticRun (Join-Path $runsRoot "candidate-r02.json") `
        (Get-SyntheticRun "r02" "candidate" $candidateHash 95)
    Write-SyntheticRun (Join-Path $runsRoot "baseline-r01.json") `
        (Get-SyntheticRun "r01" "baseline" $baselineHash 100 -Dirty $true)
    Invoke-ExpectedFailure {
        & $compareScript `
            -InputDirectory $runsRoot `
            -Scenario synthetic-hot `
            -BaselineVariant baseline `
            -CandidateVariant candidate `
            -MinimumPairs 2 `
            -BootstrapIterations 100 `
            -OutputDirectory $outputRoot
    } "dirty benchmark-harness worktree"

    Write-Information "Performance tool tests passed" -InformationAction Continue
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
        $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if (-not $resolvedTestRoot.StartsWith($resolvedTempRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove test directory outside the temporary root: $resolvedTestRoot"
        }
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
