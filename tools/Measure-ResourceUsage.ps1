#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$PackageName = "N0ahTM.AudioPlaybackConnector2",

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$ApplicationId = "App",

    [Parameter()]
    [ValidatePattern('^[^\\/:*?"<>|]+\.exe$')]
    [string]$ExecutableName = "AudioPlaybackConnector2.exe",

    [Parameter()]
    [ValidateRange(0, 3600)]
    [double]$WarmupSeconds = 10,

    [Parameter()]
    [ValidateRange(1, 86400)]
    [double]$DurationSeconds = 30,

    [Parameter()]
    [ValidateRange(100, 60000)]
    [int]$SampleIntervalMilliseconds = 1000,

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [ValidateLength(1, 80)]
    [string]$Scenario = "idle",

    [Parameter()]
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')]
    [string]$ExpectedPackageVersion,

    [Parameter()]
    [ValidateRange(1, 300)]
    [int]$ProcessStartupTimeoutSeconds = 30,

    [Parameter()]
    [string]$OutputDirectory,

    [Parameter()]
    [switch]$AttachToExisting,

    [Parameter()]
    [switch]$LeaveRunning,

    [Parameter()]
    [switch]$PassThru
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Test-PathEqual {
    param(
        [Parameter(Mandatory)]
        [string]$Left,

        [Parameter(Mandatory)]
        [string]$Right
    )

    return [string]::Equals(
        [System.IO.Path]::GetFullPath($Left).TrimEnd('\'),
        [System.IO.Path]::GetFullPath($Right).TrimEnd('\'),
        [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-PackagedProcess {
    param(
        [Parameter(Mandatory)]
        [string]$ImageName,

        [Parameter(Mandatory)]
        [string]$ExpectedExecutablePath
    )

    $escapedImageName = $ImageName.Replace("'", "''")
    $candidates = @(Get-CimInstance Win32_Process -Filter "Name = '$escapedImageName'")
    return @($candidates | Where-Object {
            $_.ExecutablePath -and (Test-PathEqual -Left $_.ExecutablePath -Right $ExpectedExecutablePath)
        })
}

function Get-PrivateWorkingSet {
    param(
        [Parameter(Mandatory)]
        [int]$Id
    )

    $perf = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -Filter "IDProcess = $Id"
    if ($null -eq $perf) {
        return $null
    }

    return [long]$perf.WorkingSetPrivate
}

function Get-Percentile {
    param(
        [Parameter(Mandatory)]
        [object[]]$SortedValues,

        [Parameter(Mandatory)]
        [ValidateRange(0, 100)]
        [double]$Percentile
    )

    if ($SortedValues.Count -eq 0) {
        return $null
    }

    if ($SortedValues.Count -eq 1) {
        return [double]$SortedValues[0]
    }

    $position = ($Percentile / 100.0) * ($SortedValues.Count - 1)
    $lowerIndex = [int][Math]::Floor($position)
    $upperIndex = [int][Math]::Ceiling($position)
    if ($lowerIndex -eq $upperIndex) {
        return [double]$SortedValues[$lowerIndex]
    }

    $fraction = $position - $lowerIndex
    return ([double]$SortedValues[$lowerIndex] * (1.0 - $fraction)) +
        ([double]$SortedValues[$upperIndex] * $fraction)
}

function Get-Distribution {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [object[]]$Values
    )

    $numbers = @($Values | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ } | Sort-Object)
    if ($numbers.Count -eq 0) {
        return $null
    }

    $average = ($numbers | Measure-Object -Average).Average
    return [ordered]@{
        Minimum = $numbers[0]
        Average = [double]$average
        P50 = Get-Percentile -SortedValues $numbers -Percentile 50
        P95 = Get-Percentile -SortedValues $numbers -Percentile 95
        P99 = Get-Percentile -SortedValues $numbers -Percentile 99
        Maximum = $numbers[$numbers.Count - 1]
    }
}

function Get-GitState {
    param(
        [Parameter(Mandatory)]
        [string]$RepositoryRoot
    )

    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        return [ordered]@{ Commit = $null; Dirty = $null }
    }

    $commit = (& git -C $RepositoryRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) {
        return [ordered]@{ Commit = $null; Dirty = $null }
    }

    $status = @(& git -C $RepositoryRoot status --porcelain 2>$null)
    return [ordered]@{
        Commit = [string]$commit
        Dirty = $status.Count -gt 0
    }
}

function ConvertTo-MiB {
    param([AllowNull()][object]$Bytes)

    if ($null -eq $Bytes) {
        return $null
    }

    return [Math]::Round(([double]$Bytes / 1MB), 3)
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot "artifacts\perf"
}

$packageCandidates = @(Get-AppxPackage -Name $PackageName | Where-Object { $_.Name -eq $PackageName })
if ($packageCandidates.Count -eq 0) {
    throw "Package '$PackageName' is not installed for the current user. Install the Release MSIX before measuring."
}

$package = $packageCandidates | Sort-Object Version -Descending | Select-Object -First 1
$packageVersion = $package.Version.ToString()
if ($ExpectedPackageVersion -and $packageVersion -ne $ExpectedPackageVersion) {
    throw "Installed package version is '$packageVersion', expected '$ExpectedPackageVersion'."
}

if ([string]::IsNullOrWhiteSpace($package.InstallLocation)) {
    throw "Package '$PackageName' has no installed location. Remove the stale or externally registered development package and install the Release MSIX before measuring."
}

$installedExecutablePath = Join-Path $package.InstallLocation $ExecutableName
if (-not (Test-Path -LiteralPath $installedExecutablePath -PathType Leaf)) {
    throw "The packaged executable was not found at '$installedExecutablePath'."
}

$existingProcesses = @(Get-PackagedProcess -ImageName $ExecutableName -ExpectedExecutablePath $installedExecutablePath)
if ($existingProcesses.Count -gt 1) {
    throw "Found $($existingProcesses.Count) matching packaged processes. Stop them before collecting a reproducible benchmark."
}

if ($existingProcesses.Count -eq 1 -and -not $AttachToExisting) {
    throw "The packaged application is already running (PID $($existingProcesses[0].ProcessId)). Stop it or use -AttachToExisting."
}

$launchedByHarness = $false
$targetProcess = $null
$targetStartTimeUtc = $null
$runStartedUtc = [DateTime]::UtcNow

try {
    if ($existingProcesses.Count -eq 1) {
        $targetProcess = Get-Process -Id ([int]$existingProcesses[0].ProcessId)
    }
    else {
        $applicationUserModelId = "$($package.PackageFamilyName)!$ApplicationId"
        $explorerPath = Join-Path $env:WINDIR "explorer.exe"
        Start-Process -FilePath $explorerPath -ArgumentList "shell:AppsFolder\$applicationUserModelId"
        $launchedByHarness = $true

        $startupDeadline = [DateTime]::UtcNow.AddSeconds($ProcessStartupTimeoutSeconds)
        do {
            Start-Sleep -Milliseconds 100
            $startedProcesses = @(Get-PackagedProcess -ImageName $ExecutableName -ExpectedExecutablePath $installedExecutablePath)
            if ($startedProcesses.Count -gt 0) {
                if ($startedProcesses.Count -gt 1) {
                    throw "More than one packaged process appeared during startup."
                }

                $targetProcess = Get-Process -Id ([int]$startedProcesses[0].ProcessId)
                break
            }
        } while ([DateTime]::UtcNow -lt $startupDeadline)

        if ($null -eq $targetProcess) {
            throw "The packaged application did not start within $ProcessStartupTimeoutSeconds seconds."
        }
    }

    $targetStartTimeUtc = $targetProcess.StartTime.ToUniversalTime()
    $targetId = $targetProcess.Id

    if ($WarmupSeconds -gt 0) {
        Start-Sleep -Milliseconds ([int][Math]::Ceiling($WarmupSeconds * 1000.0))
    }

    $targetProcess.Refresh()
    if ($targetProcess.HasExited) {
        throw "The target process exited during warm-up."
    }

    $logicalProcessorCount = [Environment]::ProcessorCount
    $operatingSystem = Get-CimInstance Win32_OperatingSystem
    $measurementStartedUtc = [DateTime]::UtcNow
    $measurementStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $previousCpuMilliseconds = $targetProcess.TotalProcessorTime.TotalMilliseconds
    $previousElapsedMilliseconds = 0.0
    $samples = [System.Collections.Generic.List[object]]::new()

    $durationMilliseconds = $DurationSeconds * 1000.0
    $sampleSchedule = [System.Collections.Generic.List[double]]::new()
    $scheduledMilliseconds = 0.0
    while ($scheduledMilliseconds -lt $durationMilliseconds) {
        $sampleSchedule.Add($scheduledMilliseconds)
        $scheduledMilliseconds += $SampleIntervalMilliseconds
    }
    if ($sampleSchedule.Count -eq 0 -or $sampleSchedule[$sampleSchedule.Count - 1] -ne $durationMilliseconds) {
        $sampleSchedule.Add($durationMilliseconds)
    }

    for ($sampleIndex = 0; $sampleIndex -lt $sampleSchedule.Count; $sampleIndex++) {
        $targetMilliseconds = $sampleSchedule[$sampleIndex]
        $remainingMilliseconds = $targetMilliseconds - $measurementStopwatch.Elapsed.TotalMilliseconds
        if ($remainingMilliseconds -gt 0) {
            Start-Sleep -Milliseconds ([int][Math]::Ceiling($remainingMilliseconds))
        }

        $targetProcess.Refresh()
        if ($targetProcess.HasExited) {
            throw "The target process exited while collecting sample $sampleIndex."
        }

        $sampledUtc = [DateTime]::UtcNow
        $elapsedMilliseconds = $measurementStopwatch.Elapsed.TotalMilliseconds
        $cpuTotalMilliseconds = $targetProcess.TotalProcessorTime.TotalMilliseconds
        $cpuDeltaMilliseconds = $cpuTotalMilliseconds - $previousCpuMilliseconds
        $elapsedDeltaMilliseconds = $elapsedMilliseconds - $previousElapsedMilliseconds
        $privateWorkingSetBytes = Get-PrivateWorkingSet -Id $targetId
        $workingSetBytes = [long]$targetProcess.WorkingSet64
        $sharedWorkingSetEstimateBytes = if ($null -eq $privateWorkingSetBytes) {
            $null
        }
        else {
            [Math]::Max(0, $workingSetBytes - $privateWorkingSetBytes)
        }

        $cpuPercentOneCore = if ($elapsedDeltaMilliseconds -gt 0) {
            ($cpuDeltaMilliseconds / $elapsedDeltaMilliseconds) * 100.0
        }
        else {
            $null
        }
        $cpuPercentMachine = if ($null -ne $cpuPercentOneCore) {
            $cpuPercentOneCore / $logicalProcessorCount
        }
        else {
            $null
        }

        $samples.Add([pscustomobject][ordered]@{
                SampleIndex = $sampleIndex
                TimestampUtc = $sampledUtc.ToString("o")
                ElapsedMilliseconds = [Math]::Round($elapsedMilliseconds, 3)
                WorkingSetBytes = $workingSetBytes
                PrivateWorkingSetBytes = $privateWorkingSetBytes
                SharedWorkingSetEstimateBytes = $sharedWorkingSetEstimateBytes
                PrivateCommitBytes = [long]$targetProcess.PrivateMemorySize64
                ThreadCount = $targetProcess.Threads.Count
                HandleCount = $targetProcess.HandleCount
                CpuTotalMilliseconds = [Math]::Round($cpuTotalMilliseconds, 3)
                CpuDeltaMilliseconds = [Math]::Round($cpuDeltaMilliseconds, 3)
                CpuPercentOneCore = [Math]::Round($cpuPercentOneCore, 6)
                CpuPercentMachine = [Math]::Round($cpuPercentMachine, 6)
            })

        $previousCpuMilliseconds = $cpuTotalMilliseconds
        $previousElapsedMilliseconds = $elapsedMilliseconds
    }

    $measurementStopwatch.Stop()
    $completedUtc = [DateTime]::UtcNow
    $gitMetadata = Get-GitState -RepositoryRoot $repositoryRoot
    $summary = [ordered]@{
        WorkingSetBytes = Get-Distribution -Values @($samples.WorkingSetBytes)
        PrivateWorkingSetBytes = Get-Distribution -Values @($samples.PrivateWorkingSetBytes)
        PrivateCommitBytes = Get-Distribution -Values @($samples.PrivateCommitBytes)
        ThreadCount = Get-Distribution -Values @($samples.ThreadCount)
        HandleCount = Get-Distribution -Values @($samples.HandleCount)
        CpuDeltaMilliseconds = Get-Distribution -Values @($samples.CpuDeltaMilliseconds)
        CpuPercentOneCore = Get-Distribution -Values @($samples.CpuPercentOneCore)
        CpuPercentMachine = Get-Distribution -Values @($samples.CpuPercentMachine)
    }

    $runId = $runStartedUtc.ToString("yyyyMMdd'T'HHmmss.fff'Z'")
    $safeScenario = ($Scenario -replace '[^A-Za-z0-9._-]', '-') -replace '-+', '-'
    $safeScenario = $safeScenario.Trim('-')
    if ([string]::IsNullOrWhiteSpace($safeScenario)) {
        $safeScenario = "scenario"
    }

    $result = [pscustomobject][ordered]@{
        SchemaVersion = 1
        Run = [ordered]@{
            Id = $runId
            Scenario = $Scenario
            StartedUtc = $runStartedUtc.ToString("o")
            MeasurementStartedUtc = $measurementStartedUtc.ToString("o")
            CompletedUtc = $completedUtc.ToString("o")
            WarmupSeconds = $WarmupSeconds
            RequestedDurationSeconds = $DurationSeconds
            ActualDurationSeconds = [Math]::Round($measurementStopwatch.Elapsed.TotalSeconds, 6)
            SampleIntervalMilliseconds = $SampleIntervalMilliseconds
            SampleCount = $samples.Count
            AttachedToExistingProcess = -not $launchedByHarness
        }
        Package = [ordered]@{
            Name = $package.Name
            FamilyName = $package.PackageFamilyName
            Version = $packageVersion
            ApplicationId = $ApplicationId
            InstallLocation = $package.InstallLocation
            ExecutablePath = $installedExecutablePath
            ExecutableFileVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($installedExecutablePath).FileVersion
        }
        Process = [ordered]@{
            Id = $targetId
            StartTimeUtc = $targetStartTimeUtc.ToString("o")
        }
        Host = [ordered]@{
            MachineName = [Environment]::MachineName
            OperatingSystemCaption = $operatingSystem.Caption
            OperatingSystemVersion = $operatingSystem.Version
            OperatingSystemBuildNumber = $operatingSystem.BuildNumber
            LogicalProcessorCount = $logicalProcessorCount
            TotalVisibleMemoryBytes = [long]$operatingSystem.TotalVisibleMemorySize * 1KB
            PowerShellVersion = $PSVersionTable.PSVersion.ToString()
        }
        Source = [ordered]@{
            RepositoryRoot = $repositoryRoot
            GitCommit = $gitMetadata.Commit
            GitWorkingTreeDirty = $gitMetadata.Dirty
        }
        Summary = $summary
        Samples = @($samples)
    }

    $outputDirectoryFull = [System.IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Path $outputDirectoryFull -Force | Out-Null
    $fileStem = "$runId-$safeScenario"
    $jsonPath = Join-Path $outputDirectoryFull "$fileStem.json"
    $csvPath = Join-Path $outputDirectoryFull "$fileStem.csv"
    $temporaryJsonPath = "$jsonPath.tmp"
    $temporaryCsvPath = "$csvPath.tmp"

    try {
        $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporaryJsonPath -Encoding UTF8
        $samples | Select-Object *,
            @{ Name = "WorkingSetMiB"; Expression = { ConvertTo-MiB $_.WorkingSetBytes } },
            @{ Name = "PrivateWorkingSetMiB"; Expression = { ConvertTo-MiB $_.PrivateWorkingSetBytes } },
            @{ Name = "PrivateCommitMiB"; Expression = { ConvertTo-MiB $_.PrivateCommitBytes } } |
            Export-Csv -LiteralPath $temporaryCsvPath -NoTypeInformation -Encoding UTF8
        Move-Item -LiteralPath $temporaryJsonPath -Destination $jsonPath -Force
        Move-Item -LiteralPath $temporaryCsvPath -Destination $csvPath -Force
    }
    finally {
        Remove-Item -LiteralPath $temporaryJsonPath, $temporaryCsvPath -Force -ErrorAction SilentlyContinue
    }

    Write-Information "Resource benchmark completed." -InformationAction Continue
    Write-Information "JSON: $jsonPath" -InformationAction Continue
    Write-Information "CSV:  $csvPath" -InformationAction Continue
    Write-Information ("Private working set average: {0:N2} MiB" -f (ConvertTo-MiB $summary.PrivateWorkingSetBytes.Average)) -InformationAction Continue
    Write-Information ("Private commit average:      {0:N2} MiB" -f (ConvertTo-MiB $summary.PrivateCommitBytes.Average)) -InformationAction Continue

    if ($PassThru) {
        $result
    }
}
finally {
    if ($launchedByHarness -and -not $LeaveRunning -and $null -ne $targetProcess) {
        try {
            $processToStop = Get-Process -Id $targetProcess.Id -ErrorAction Stop
            $sameStart = [Math]::Abs(
                ($processToStop.StartTime.ToUniversalTime() - $targetStartTimeUtc).TotalMilliseconds
            ) -lt 1000.0
            $sameExecutable = Test-PathEqual -Left $processToStop.Path -Right $installedExecutablePath
            if ($sameStart -and $sameExecutable) {
                Stop-Process -Id $processToStop.Id -Force -ErrorAction Stop
                [void]$processToStop.WaitForExit(5000)

                $cleanupDeadline = [DateTime]::UtcNow.AddSeconds(5)
                do {
                    $remainingProcesses = @(
                        Get-PackagedProcess -ImageName $ExecutableName -ExpectedExecutablePath $installedExecutablePath
                    )
                    if ($remainingProcesses.Count -eq 0) {
                        break
                    }
                    Start-Sleep -Milliseconds 100
                } while ([DateTime]::UtcNow -lt $cleanupDeadline)

                if ($remainingProcesses.Count -ne 0) {
                    Write-Warning "The packaged process is still visible after the cleanup timeout."
                }
            }
            else {
                Write-Warning "The benchmark process identity changed before cleanup; refusing to stop PID $($processToStop.Id)."
            }
        }
        catch {
            Write-Warning "Could not stop the process launched by the benchmark harness: $($_.Exception.Message)"
        }
    }
}
