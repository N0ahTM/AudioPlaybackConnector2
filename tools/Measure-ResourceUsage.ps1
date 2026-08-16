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
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$Variant,

    [Parameter()]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$PairId,

    [Parameter()]
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')]
    [string]$ExpectedPackageVersion,

    [Parameter()]
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedExecutableSha256,

    [Parameter()]
    [ValidateSet(
        "Any",
        "NotPresent",
        "Busy",
        "RunningD3dFullScreen",
        "PresentationMode",
        "AcceptsNotifications",
        "QuietTime",
        "App"
    )]
    [string]$ExpectedUserNotificationState = "Any",

    [Parameter()]
    [ValidateSet("Any", "Cold", "Warm", "Hot")]
    [string]$ExpectedAdaptiveResidency = "Any",

    [Parameter()]
    [switch]$RequireEnergySaverOff,

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

function Get-AdaptiveResourceStatus {
    param(
        [Parameter(Mandatory)]
        [string]$ControlExecutablePath,

        [Parameter()]
        [ValidateRange(1, 30)]
        [int]$TimeoutSeconds = 5
    )

    $standardOutputPath = [System.IO.Path]::GetTempFileName()
    $standardErrorPath = [System.IO.Path]::GetTempFileName()
    try {
        $probe = Start-Process `
            -FilePath $ControlExecutablePath `
            -ArgumentList @("status", "--json") `
            -RedirectStandardOutput $standardOutputPath `
            -RedirectStandardError $standardErrorPath `
            -WindowStyle Hidden `
            -PassThru
        # Windows PowerShell 5.1 does not reliably populate ExitCode unless the
        # native process handle is materialized before waiting.
        [void]$probe.Handle
        if (-not $probe.WaitForExit($TimeoutSeconds * 1000)) {
            $probe.Kill()
            $probe.WaitForExit()
            throw "The adaptive-resource status probe timed out after $TimeoutSeconds seconds."
        }
        $probe.WaitForExit()
        if ($probe.ExitCode -ne 0) {
            [string]$errorText = Get-Content -LiteralPath $standardErrorPath -Raw -ErrorAction SilentlyContinue
            $errorText = ([string]$errorText).Trim()
            throw "The adaptive-resource status probe exited with code $($probe.ExitCode): $errorText"
        }

        $json = Get-Content -LiteralPath $standardOutputPath -Raw
        $status = $json | ConvertFrom-Json
        if ($null -eq $status.PSObject.Properties["adaptiveResources"]) {
            throw "The installed application does not expose adaptive-resource status."
        }
        return $status.adaptiveResources
    }
    finally {
        Remove-Item -LiteralPath $standardOutputPath, $standardErrorPath -Force -ErrorAction SilentlyContinue
    }
}

function Assert-AdaptiveResourceStatus {
    param(
        [Parameter(Mandatory)]
        [object]$Status,

        [Parameter(Mandatory)]
        [ValidateSet("Cold", "Warm", "Hot")]
        [string]$ExpectedResidency,

        [Parameter(Mandatory)]
        [string]$Phase
    )

    if (-not [bool]$Status.evaluated) {
        throw "Adaptive resources have not been evaluated at $Phase."
    }
    if ([string]$Status.residency -ne $ExpectedResidency) {
        throw "Adaptive residency at $Phase is '$($Status.residency)', expected '$ExpectedResidency'."
    }
    if ($ExpectedResidency -eq "Hot" -and
        (-not [bool]$Status.uiResourcesLoaded -or -not [bool]$Status.uiResourcesInitialized)) {
        throw "Hot adaptive residency at $Phase does not have initialized, loaded UI resources."
    }
    if ($ExpectedResidency -eq "Cold" -and [bool]$Status.uiResourcesLoaded) {
        throw "Cold adaptive residency at $Phase still has loaded UI resources."
    }
}
$ErrorActionPreference = "Stop"

function Initialize-NativeResourceProbe {
    if ("ApcResourceBenchmark.NativeMethods" -as [type]) {
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace ApcResourceBenchmark {
    [StructLayout(LayoutKind.Sequential)]
    public struct SystemPowerStatus {
        public byte ACLineStatus;
        public byte BatteryFlag;
        public byte BatteryLifePercent;
        public byte SystemStatusFlag;
        public int BatteryLifeTime;
        public int BatteryFullLifeTime;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ProcessMemoryCountersEx2 {
        public uint cb;
        public uint PageFaultCount;
        public UIntPtr PeakWorkingSetSize;
        public UIntPtr WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage;
        public UIntPtr QuotaPagedPoolUsage;
        public UIntPtr QuotaPeakNonPagedPoolUsage;
        public UIntPtr QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage;
        public UIntPtr PeakPagefileUsage;
        public UIntPtr PrivateUsage;
        public UIntPtr PrivateWorkingSetSize;
        public UIntPtr SharedCommitUsage;
    }

    public static class NativeMethods {
        [DllImport("shell32.dll")]
        public static extern int SHQueryUserNotificationState(out int state);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetSystemPowerStatus(out SystemPowerStatus status);

        [DllImport("psapi.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetProcessMemoryInfo(
            IntPtr process,
            out ProcessMemoryCountersEx2 counters,
            uint size
        );

        [DllImport("powrprof.dll")]
        private static extern uint PowerGetActiveScheme(IntPtr userRootPowerKey, out IntPtr activePolicyGuid);

        [DllImport("kernel32.dll")]
        private static extern IntPtr LocalFree(IntPtr memory);

        public static Guid GetActivePowerSchemeGuid() {
            IntPtr policyGuid;
            uint result = PowerGetActiveScheme(IntPtr.Zero, out policyGuid);
            if (result != 0) {
                throw new System.ComponentModel.Win32Exception((int)result, "PowerGetActiveScheme failed.");
            }

            try {
                return (Guid)Marshal.PtrToStructure(policyGuid, typeof(Guid));
            }
            finally {
                LocalFree(policyGuid);
            }
        }
    }
}
'@
}

function Get-UserNotificationStateName {
    param(
        [Parameter(Mandatory)]
        [int]$State
    )

    switch ($State) {
        1 { return "NotPresent" }
        2 { return "Busy" }
        3 { return "RunningD3dFullScreen" }
        4 { return "PresentationMode" }
        5 { return "AcceptsNotifications" }
        6 { return "QuietTime" }
        7 { return "App" }
        default { return "Unknown" }
    }
}

function Get-ResourceEnvironmentSnapshot {
    Initialize-NativeResourceProbe

    $notificationState = 0
    $notificationResult = [ApcResourceBenchmark.NativeMethods]::SHQueryUserNotificationState(
        [ref]$notificationState
    )
    if ($notificationResult -lt 0) {
        [System.Runtime.InteropServices.Marshal]::ThrowExceptionForHR($notificationResult)
    }

    $powerStatus = [ApcResourceBenchmark.SystemPowerStatus]::new()
    if (-not [ApcResourceBenchmark.NativeMethods]::GetSystemPowerStatus([ref]$powerStatus)) {
        throw [System.ComponentModel.Win32Exception]::new(
            [System.Runtime.InteropServices.Marshal]::GetLastWin32Error(),
            "GetSystemPowerStatus failed."
        )
    }

    return [pscustomobject][ordered]@{
        CapturedUtc = [DateTime]::UtcNow.ToString("o")
        UserNotificationState = [ordered]@{
            Code = $notificationState
            Name = Get-UserNotificationStateName -State $notificationState
        }
        Power = [ordered]@{
            ActiveSchemeGuid = [ApcResourceBenchmark.NativeMethods]::GetActivePowerSchemeGuid().ToString("D")
            ACLineStatus = [int]$powerStatus.ACLineStatus
            BatteryFlag = [int]$powerStatus.BatteryFlag
            BatteryLifePercent = [int]$powerStatus.BatteryLifePercent
            EnergySaverEnabled = $powerStatus.SystemStatusFlag -ne 0
            BatteryLifeTimeSeconds = [int]$powerStatus.BatteryLifeTime
            BatteryFullLifeTimeSeconds = [int]$powerStatus.BatteryFullLifeTime
        }
    }
}

function Assert-ResourceEnvironment {
    param(
        [Parameter(Mandatory)]
        [object]$Snapshot,

        [Parameter(Mandatory)]
        [string]$Phase,

        [Parameter()]
        [AllowNull()]
        [object]$Baseline
    )

    if ($ExpectedUserNotificationState -ne "Any" -and
        $Snapshot.UserNotificationState.Name -ne $ExpectedUserNotificationState) {
        throw "User-notification state at $Phase was '$($Snapshot.UserNotificationState.Name)' " +
            "($($Snapshot.UserNotificationState.Code)); expected '$ExpectedUserNotificationState'."
    }

    if ($RequireEnergySaverOff -and $Snapshot.Power.EnergySaverEnabled) {
        throw "Energy Saver was enabled at $Phase. This run is not comparable."
    }

    if ($null -ne $Baseline) {
        if ($Snapshot.UserNotificationState.Code -ne $Baseline.UserNotificationState.Code) {
            throw "User-notification state changed between pre-launch and $Phase. This run is not comparable."
        }
        if ($Snapshot.Power.ActiveSchemeGuid -ne $Baseline.Power.ActiveSchemeGuid) {
            throw "The active power scheme changed between pre-launch and $Phase. This run is not comparable."
        }
        if ($Snapshot.Power.ACLineStatus -ne $Baseline.Power.ACLineStatus) {
            throw "The AC power source changed between pre-launch and $Phase. This run is not comparable."
        }
    }
}

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

function Get-ProcessMemorySnapshot {
    param(
        [Parameter(Mandatory)]
        [IntPtr]$Handle
    )

    $counters = [ApcResourceBenchmark.ProcessMemoryCountersEx2]::new()
    $counters.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($counters)
    if (-not [ApcResourceBenchmark.NativeMethods]::GetProcessMemoryInfo(
            $Handle,
            [ref]$counters,
            $counters.cb
        )) {
        throw [System.ComponentModel.Win32Exception]::new(
            [System.Runtime.InteropServices.Marshal]::GetLastWin32Error(),
            "GetProcessMemoryInfo failed."
        )
    }

    return [pscustomobject][ordered]@{
        WorkingSetBytes = [long]$counters.WorkingSetSize.ToUInt64()
        PrivateWorkingSetBytes = [long]$counters.PrivateWorkingSetSize.ToUInt64()
        PrivateCommitBytes = [long]$counters.PrivateUsage.ToUInt64()
    }
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

$executableSha256 = (Get-FileHash -LiteralPath $installedExecutablePath -Algorithm SHA256).Hash.ToUpperInvariant()
if ($ExpectedExecutableSha256 -and $executableSha256 -ne $ExpectedExecutableSha256.ToUpperInvariant()) {
    throw "Installed executable SHA-256 is '$executableSha256', expected '$($ExpectedExecutableSha256.ToUpperInvariant())'."
}

$installedManifestPath = Join-Path $package.InstallLocation "AppxManifest.xml"
$manifestSha256 = if (Test-Path -LiteralPath $installedManifestPath -PathType Leaf) {
    (Get-FileHash -LiteralPath $installedManifestPath -Algorithm SHA256).Hash.ToUpperInvariant()
}
else {
    $null
}

$environmentBeforeLaunch = Get-ResourceEnvironmentSnapshot
Assert-ResourceEnvironment -Snapshot $environmentBeforeLaunch -Phase "pre-launch"

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
    $targetHandle = $targetProcess.Handle

    if ($WarmupSeconds -gt 0) {
        Start-Sleep -Milliseconds ([int][Math]::Ceiling($WarmupSeconds * 1000.0))
    }

    $targetProcess.Refresh()
    if ($targetProcess.HasExited) {
        throw "The target process exited during warm-up."
    }

    $environmentBeforeMeasurement = Get-ResourceEnvironmentSnapshot
    Assert-ResourceEnvironment `
        -Snapshot $environmentBeforeMeasurement `
        -Phase "measurement start" `
        -Baseline $environmentBeforeLaunch

    $adaptiveResourcesBeforeMeasurement = $null
    $adaptiveResourcesAfterMeasurement = $null
    if ($ExpectedAdaptiveResidency -ne "Any") {
        $controlExecutablePath = Join-Path `
            $package.InstallLocation `
            "AudioPlaybackConnector2.Control\AudioPlaybackConnector2.Control.exe"
        if (-not (Test-Path -LiteralPath $controlExecutablePath -PathType Leaf)) {
            throw "The installed control executable was not found at '$controlExecutablePath'."
        }
        $adaptiveResourcesBeforeMeasurement = Get-AdaptiveResourceStatus `
            -ControlExecutablePath $controlExecutablePath
        Assert-AdaptiveResourceStatus `
            -Status $adaptiveResourcesBeforeMeasurement `
            -ExpectedResidency $ExpectedAdaptiveResidency `
            -Phase "measurement start"
    }

    $logicalProcessorCount = [Environment]::ProcessorCount
    $operatingSystem = Get-CimInstance Win32_OperatingSystem
    $measurementStartedUtc = [DateTime]::UtcNow
    $measurementStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $previousCpuMilliseconds = $targetProcess.TotalProcessorTime.TotalMilliseconds
    $measurementInitialCpuMilliseconds = $previousCpuMilliseconds
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
        $memory = Get-ProcessMemorySnapshot -Handle $targetHandle
        $sharedWorkingSetEstimateBytes =
            [Math]::Max(0, $memory.WorkingSetBytes - $memory.PrivateWorkingSetBytes)

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
                ScheduledMilliseconds = [Math]::Round($targetMilliseconds, 3)
                ScheduleLatenessMilliseconds = [Math]::Round(
                    [Math]::Max(0.0, $elapsedMilliseconds - $targetMilliseconds),
                    3
                )
                WorkingSetBytes = $memory.WorkingSetBytes
                PrivateWorkingSetBytes = $memory.PrivateWorkingSetBytes
                SharedWorkingSetEstimateBytes = $sharedWorkingSetEstimateBytes
                PrivateCommitBytes = $memory.PrivateCommitBytes
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
    $cpuTotalDeltaMilliseconds = $previousCpuMilliseconds - $measurementInitialCpuMilliseconds
    $completedUtc = [DateTime]::UtcNow
    $environmentAfterMeasurement = Get-ResourceEnvironmentSnapshot
    Assert-ResourceEnvironment `
        -Snapshot $environmentAfterMeasurement `
        -Phase "measurement completion" `
        -Baseline $environmentBeforeLaunch

    if ($ExpectedAdaptiveResidency -ne "Any") {
        $adaptiveResourcesAfterMeasurement = Get-AdaptiveResourceStatus `
            -ControlExecutablePath $controlExecutablePath
        Assert-AdaptiveResourceStatus `
            -Status $adaptiveResourcesAfterMeasurement `
            -ExpectedResidency $ExpectedAdaptiveResidency `
            -Phase "measurement completion"
    }

    $completedExecutableSha256 =
        (Get-FileHash -LiteralPath $installedExecutablePath -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($completedExecutableSha256 -ne $executableSha256) {
        throw "The installed executable changed during measurement. The run is not comparable."
    }

    $completedManifestSha256 = if (Test-Path -LiteralPath $installedManifestPath -PathType Leaf) {
        (Get-FileHash -LiteralPath $installedManifestPath -Algorithm SHA256).Hash.ToUpperInvariant()
    }
    else {
        $null
    }
    if ($completedManifestSha256 -ne $manifestSha256) {
        throw "The installed package manifest changed during measurement. The run is not comparable."
    }

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
        ScheduleLatenessMilliseconds = Get-Distribution -Values @($samples.ScheduleLatenessMilliseconds)
        CpuTotalDeltaMilliseconds = [Math]::Round($cpuTotalDeltaMilliseconds, 3)
        CpuAveragePercentOneCore = if ($measurementStopwatch.Elapsed.TotalMilliseconds -gt 0) {
            [Math]::Round(
                ($cpuTotalDeltaMilliseconds / $measurementStopwatch.Elapsed.TotalMilliseconds) * 100.0,
                6
            )
        }
        else {
            $null
        }
        CpuAveragePercentMachine = if ($measurementStopwatch.Elapsed.TotalMilliseconds -gt 0) {
            [Math]::Round(
                ($cpuTotalDeltaMilliseconds / $measurementStopwatch.Elapsed.TotalMilliseconds) * 100.0 /
                    $logicalProcessorCount,
                6
            )
        }
        else {
            $null
        }
    }

    $runId = $runStartedUtc.ToString("yyyyMMdd'T'HHmmss.fff'Z'")
    $safeScenario = ($Scenario -replace '[^A-Za-z0-9._-]', '-') -replace '-+', '-'
    $safeScenario = $safeScenario.Trim('-')
    if ([string]::IsNullOrWhiteSpace($safeScenario)) {
        $safeScenario = "scenario"
    }

    $result = [pscustomobject][ordered]@{
        SchemaVersion = 2
        Run = [ordered]@{
            Id = $runId
            Scenario = $Scenario
            Variant = if ($Variant) { $Variant } else { $null }
            PairId = if ($PairId) { $PairId } else { $null }
            StartedUtc = $runStartedUtc.ToString("o")
            MeasurementStartedUtc = $measurementStartedUtc.ToString("o")
            CompletedUtc = $completedUtc.ToString("o")
            WarmupSeconds = $WarmupSeconds
            RequestedDurationSeconds = $DurationSeconds
            ActualDurationSeconds = [Math]::Round($measurementStopwatch.Elapsed.TotalSeconds, 6)
            SampleIntervalMilliseconds = $SampleIntervalMilliseconds
            SampleCount = $samples.Count
            AttachedToExistingProcess = -not $launchedByHarness
            RequiredUserNotificationState = $ExpectedUserNotificationState
            RequiredAdaptiveResidency = $ExpectedAdaptiveResidency
            RequiredEnergySaverOff = [bool]$RequireEnergySaverOff
        }
        Package = [ordered]@{
            Name = $package.Name
            FullName = $package.PackageFullName
            FamilyName = $package.PackageFamilyName
            Version = $packageVersion
            Publisher = $package.Publisher
            Architecture = $package.Architecture.ToString()
            Status = $package.Status.ToString()
            ApplicationId = $ApplicationId
            InstallLocation = $package.InstallLocation
            ExecutablePath = $installedExecutablePath
            ExecutableFileVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($installedExecutablePath).FileVersion
            ExecutableSha256 = $executableSha256
            ManifestPath = if ($manifestSha256) { $installedManifestPath } else { $null }
            ManifestSha256 = $manifestSha256
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
            EnvironmentBeforeLaunch = $environmentBeforeLaunch
            EnvironmentBeforeMeasurement = $environmentBeforeMeasurement
            EnvironmentAfterMeasurement = $environmentAfterMeasurement
            AdaptiveResourcesBeforeMeasurement = $adaptiveResourcesBeforeMeasurement
            AdaptiveResourcesAfterMeasurement = $adaptiveResourcesAfterMeasurement
        }
        Source = [ordered]@{
            Role = "BenchmarkHarnessOnly"
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
