#Requires -Version 5.1

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function New-SyntheticRun {
    param(
        [Parameter(Mandatory)][string]$PairId,
        [Parameter(Mandatory)][string]$Variant,
        [Parameter(Mandatory)][double]$Milliseconds
    )

    $environment = [ordered]@{
        UserNotificationState = [ordered]@{ Code = 5; Name = "AcceptsNotifications" }
        ActivePowerSchemeGuid = "00000000-0000-0000-0000-000000000001"
        ACLineStatus = 1
        EnergySaverEnabled = $false
    }
    return [ordered]@{
        SchemaVersion = 1
        Metric = "ControlToFlyoutOpenedAck"
        Run = [ordered]@{
            Variant = $Variant
            PairId = $PairId
            ControlToFlyoutOpenedAckMilliseconds = $Milliseconds
            InitialPickerOpenedGeneration = 0
            FinalPickerOpenedGeneration = 1
        }
        Package = [ordered]@{
            Name = "N0ahTM.AudioPlaybackConnector2"
            FamilyName = "N0ahTM.AudioPlaybackConnector2_test"
            Version = if ($Variant -eq "baseline") { "0.7.1.0" } else { "0.7.2.0" }
            Publisher = "CN=AudioPlaybackConnector2"
            Architecture = "X64"
            ExecutableSha256 = if ($Variant -eq "baseline") { "A" * 64 } else { "B" * 64 }
        }
        Host = [ordered]@{
            EnvironmentBeforeLaunch = $environment
            EnvironmentBeforeOpen = $environment
            EnvironmentAfterOpen = $environment
        }
        Source = [ordered]@{ GitWorkingTreeDirty = $false }
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$compareScript = Join-Path $repositoryRoot "tools\Compare-PickerOpenAck.ps1"
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("apc2-picker-perf-" + [Guid]::NewGuid().ToString("N"))
$runsRoot = Join-Path $testRoot "runs"
$outputRoot = Join-Path $testRoot "output"

try {
    New-Item -ItemType Directory -Path $runsRoot, $outputRoot -Force | Out-Null
    New-SyntheticRun r01 baseline 100 | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runsRoot "baseline-r01.json")
    New-SyntheticRun r01 candidate 90 | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runsRoot "candidate-r01.json")
    New-SyntheticRun r02 baseline 110 | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runsRoot "baseline-r02.json")
    New-SyntheticRun r02 candidate 95 | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runsRoot "candidate-r02.json")

    $result = & $compareScript `
        -InputDirectory $runsRoot `
        -BaselineVariant baseline `
        -CandidateVariant candidate `
        -MinimumPairs 2 `
        -BootstrapIterations 200 `
        -OutputDirectory $outputRoot `
        -PassThru
    if ($result.PairCount -ne 2 -or $result.PairedDifference.MedianMilliseconds -ne -12.5) {
        throw "Unexpected synthetic picker acknowledgement comparison."
    }

    $invalid = New-SyntheticRun r02 candidate 95
    $invalid.Run.FinalPickerOpenedGeneration = 0
    $invalid | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $runsRoot "candidate-r02.json")
    try {
        & $compareScript `
            -InputDirectory $runsRoot `
            -BaselineVariant baseline `
            -CandidateVariant candidate `
            -MinimumPairs 2 `
            -BootstrapIterations 100 `
            -OutputDirectory $outputRoot
    }
    catch {
        if ($_.Exception.Message -match "generation did not advance") {
            Write-Host "Picker performance tool tests passed"
            return
        }
        throw
    }
    throw "Expected a non-advancing picker generation to be rejected."
}
finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
