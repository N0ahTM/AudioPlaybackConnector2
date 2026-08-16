#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$PackageName = "N0ahTM.AudioPlaybackConnector2",

    [Parameter()]
    [ValidateNotNullOrEmpty()]
    [string]$ApplicationId = "App",

    [Parameter()]
    [ValidatePattern('^\d+\.\d+\.\d+\.\d+$')]
    [string]$ExpectedPackageVersion,

    [Parameter()]
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$ExpectedExecutableSha256,

    [Parameter()]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$Variant,

    [Parameter()]
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$PairId,

    [Parameter()]
    [ValidateRange(1, 60)]
    [int]$StartupTimeoutSeconds = 30,

    [Parameter()]
    [ValidateRange(1, 30)]
    [int]$OpenTimeoutSeconds = 10,

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
    [switch]$RequireEnergySaverOff,

    [Parameter()]
    [string]$OutputDirectory,

    [Parameter()]
    [switch]$LeaveRunning,

    [Parameter()]
    [switch]$PassThru
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not ("ApcPickerBenchmark.NativeMethods" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

namespace ApcPickerBenchmark {
    public struct SystemPowerStatus {
        public byte ACLineStatus;
        public byte BatteryFlag;
        public byte BatteryLifePercent;
        public byte SystemStatusFlag;
        public uint BatteryLifeTime;
        public uint BatteryFullLifeTime;
    }

    public static class NativeMethods {
        [DllImport("shell32.dll")]
        public static extern int SHQueryUserNotificationState(out int state);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetSystemPowerStatus(out SystemPowerStatus status);
    }
}
"@
}

function Get-EnvironmentSnapshot {
    $notificationNames = @{
        1 = "NotPresent"
        2 = "Busy"
        3 = "RunningD3dFullScreen"
        4 = "PresentationMode"
        5 = "AcceptsNotifications"
        6 = "QuietTime"
        7 = "App"
    }
    [int]$notificationCode = 0
    $hr = [ApcPickerBenchmark.NativeMethods]::SHQueryUserNotificationState([ref]$notificationCode)
    if ($hr -lt 0) {
        [System.Runtime.InteropServices.Marshal]::ThrowExceptionForHR($hr)
    }
    $powerStatus = [ApcPickerBenchmark.SystemPowerStatus]::new()
    if (-not [ApcPickerBenchmark.NativeMethods]::GetSystemPowerStatus([ref]$powerStatus)) {
        throw [System.ComponentModel.Win32Exception]::new(
            [System.Runtime.InteropServices.Marshal]::GetLastWin32Error(),
            "GetSystemPowerStatus failed."
        )
    }
    $schemeText = (& powercfg.exe /getactivescheme 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $schemeText -notmatch '([0-9a-fA-F-]{36})') {
        throw "Unable to determine the active power scheme."
    }
    return [pscustomobject][ordered]@{
        UserNotificationState = [pscustomobject][ordered]@{
            Code = $notificationCode
            Name = if ($notificationNames.ContainsKey($notificationCode)) {
                $notificationNames[$notificationCode]
            }
            else {
                "Unknown"
            }
        }
        ActivePowerSchemeGuid = $Matches[1].ToLowerInvariant()
        ACLineStatus = [int]$powerStatus.ACLineStatus
        EnergySaverEnabled = $powerStatus.SystemStatusFlag -ne 0
    }
}

function Assert-EnvironmentSnapshot {
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
        throw "User notification state at $Phase is '$($Snapshot.UserNotificationState.Name)', expected '$ExpectedUserNotificationState'."
    }
    if ($RequireEnergySaverOff -and $Snapshot.EnergySaverEnabled) {
        throw "Energy Saver is enabled at $Phase."
    }
    if ($null -ne $Baseline) {
        if ($Snapshot.UserNotificationState.Code -ne $Baseline.UserNotificationState.Code) {
            throw "User notification state changed before $Phase."
        }
        if ($Snapshot.ActivePowerSchemeGuid -ne $Baseline.ActivePowerSchemeGuid) {
            throw "The active power scheme changed before $Phase."
        }
        if ($Snapshot.ACLineStatus -ne $Baseline.ACLineStatus) {
            throw "The AC power source changed before $Phase."
        }
        if ($Snapshot.EnergySaverEnabled -ne $Baseline.EnergySaverEnabled) {
            throw "Energy Saver state changed before $Phase."
        }
    }
}

function Invoke-ControlJson {
    param(
        [Parameter(Mandatory)]
        [string]$ControlExecutablePath,

        [Parameter(Mandatory)]
        [string[]]$Arguments,

        [Parameter(Mandatory)]
        [int]$TimeoutSeconds
    )

    $standardOutputPath = [System.IO.Path]::GetTempFileName()
    $standardErrorPath = [System.IO.Path]::GetTempFileName()
    try {
        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        $probe = Start-Process `
            -FilePath $ControlExecutablePath `
            -ArgumentList $Arguments `
            -RedirectStandardOutput $standardOutputPath `
            -RedirectStandardError $standardErrorPath `
            -WindowStyle Hidden `
            -PassThru
        [void]$probe.Handle
        if (-not $probe.WaitForExit($TimeoutSeconds * 1000)) {
            $probe.Kill()
            $probe.WaitForExit()
            throw "Control command '$($Arguments -join ' ')' timed out after $TimeoutSeconds seconds."
        }
        $probe.WaitForExit()
        $stopwatch.Stop()
        if ($probe.ExitCode -ne 0) {
            [string]$errorText = Get-Content -LiteralPath $standardErrorPath -Raw -ErrorAction SilentlyContinue
            throw "Control command '$($Arguments -join ' ')' exited with code $($probe.ExitCode): $($errorText.Trim())"
        }
        $json = Get-Content -LiteralPath $standardOutputPath -Raw | ConvertFrom-Json
        return [pscustomobject][ordered]@{
            Json = $json
            ElapsedMilliseconds = $stopwatch.Elapsed.TotalMilliseconds
        }
    }
    finally {
        Remove-Item -LiteralPath $standardOutputPath, $standardErrorPath -Force -ErrorAction SilentlyContinue
    }
}

function Get-PackagedProcess {
    param(
        [Parameter(Mandatory)]
        [string]$ExecutablePath
    )

    $imageName = [System.IO.Path]::GetFileName($ExecutablePath).Replace("'", "''")
    return @(Get-CimInstance Win32_Process -Filter "Name = '$imageName'" | Where-Object {
            $_.ExecutablePath -and
            [string]::Equals(
                [System.IO.Path]::GetFullPath($_.ExecutablePath),
                [System.IO.Path]::GetFullPath($ExecutablePath),
                [System.StringComparison]::OrdinalIgnoreCase
            )
        })
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot "artifacts\perf\picker-open-ack"
}

$packageCandidates = @(Get-AppxPackage -Name $PackageName | Where-Object Name -eq $PackageName)
if ($packageCandidates.Count -eq 0) {
    throw "Package '$PackageName' is not installed for the current user."
}
$package = $packageCandidates | Sort-Object Version -Descending | Select-Object -First 1
$packageVersion = $package.Version.ToString()
if ($ExpectedPackageVersion -and $packageVersion -ne $ExpectedPackageVersion) {
    throw "Installed package version is '$packageVersion', expected '$ExpectedPackageVersion'."
}

$appExecutablePath = Join-Path $package.InstallLocation "AudioPlaybackConnector2.exe"
$controlExecutablePath = Join-Path `
    $package.InstallLocation `
    "AudioPlaybackConnector2.Control\AudioPlaybackConnector2.Control.exe"
foreach ($path in @($appExecutablePath, $controlExecutablePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required packaged executable was not found at '$path'."
    }
}

$executableSha256 = (Get-FileHash -LiteralPath $appExecutablePath -Algorithm SHA256).Hash.ToUpperInvariant()
if ($ExpectedExecutableSha256 -and $executableSha256 -ne $ExpectedExecutableSha256.ToUpperInvariant()) {
    throw "Installed executable SHA-256 is '$executableSha256', expected '$($ExpectedExecutableSha256.ToUpperInvariant())'."
}

$existingProcesses = @(Get-PackagedProcess -ExecutablePath $appExecutablePath)
if ($existingProcesses.Count -ne 0) {
    throw "The packaged application is already running. Stop it before measuring first-open acknowledgement."
}

$startedProcess = $null
$measurement = $null
try {
    $environmentBeforeLaunch = Get-EnvironmentSnapshot
    Assert-EnvironmentSnapshot -Snapshot $environmentBeforeLaunch -Phase "launch"
    $launchStartedUtc = [DateTime]::UtcNow
    $launchStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $applicationUserModelId = "$($package.PackageFamilyName)!$ApplicationId"
    Start-Process `
        -FilePath (Join-Path $env:WINDIR "explorer.exe") `
        -ArgumentList "shell:AppsFolder\$applicationUserModelId"

    $startupDeadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    $initialStatus = $null
    do {
        Start-Sleep -Milliseconds 50
        $processes = @(Get-PackagedProcess -ExecutablePath $appExecutablePath)
        if ($processes.Count -gt 1) {
            throw "More than one packaged process appeared during startup."
        }
        if ($processes.Count -eq 1) {
            $startedProcess = Get-Process -Id ([int]$processes[0].ProcessId)
            try {
                $initialStatus = Invoke-ControlJson `
                    -ControlExecutablePath $controlExecutablePath `
                    -Arguments @("status", "--json") `
                    -TimeoutSeconds 2
            }
            catch {
                $initialStatus = $null
            }
        }
    } while ($null -eq $initialStatus -and [DateTime]::UtcNow -lt $startupDeadline)

    if ($null -eq $initialStatus -or $null -eq $startedProcess) {
        throw "The packaged application did not become control-ready within $StartupTimeoutSeconds seconds."
    }
    $launchStopwatch.Stop()

    $generationProperty = $initialStatus.Json.PSObject.Properties["devicePickerOpenedGeneration"]
    if ($null -eq $generationProperty) {
        throw "The installed application does not expose devicePickerOpenedGeneration."
    }
    $initialGeneration = [uint64]$generationProperty.Value
    $environmentBeforeOpen = Get-EnvironmentSnapshot
    Assert-EnvironmentSnapshot `
        -Snapshot $environmentBeforeOpen `
        -Phase "picker open" `
        -Baseline $environmentBeforeLaunch

    $showResult = Invoke-ControlJson `
        -ControlExecutablePath $controlExecutablePath `
        -Arguments @("show", "--json") `
        -TimeoutSeconds $OpenTimeoutSeconds
    $finalStatus = Invoke-ControlJson `
        -ControlExecutablePath $controlExecutablePath `
        -Arguments @("status", "--json") `
        -TimeoutSeconds 5
    $finalGeneration = [uint64]$finalStatus.Json.devicePickerOpenedGeneration
    if ($finalGeneration -le $initialGeneration) {
        throw "The show command returned before a new Flyout.Opened acknowledgement was observable."
    }
    $environmentAfterOpen = Get-EnvironmentSnapshot
    Assert-EnvironmentSnapshot `
        -Snapshot $environmentAfterOpen `
        -Phase "picker-open acknowledgement" `
        -Baseline $environmentBeforeLaunch

    $gitCommit = (& git -C $repositoryRoot rev-parse HEAD 2>$null)
    $gitDirty = @(& git -C $repositoryRoot status --porcelain 2>$null).Count -gt 0
    $measurement = [ordered]@{
        SchemaVersion = 1
        Metric = "ControlToFlyoutOpenedAck"
        Run = [ordered]@{
            Variant = $Variant
            PairId = $PairId
            StartedUtc = $launchStartedUtc.ToString("o")
            ProcessId = $startedProcess.Id
            StartupToControlReadyMilliseconds = $launchStopwatch.Elapsed.TotalMilliseconds
            ControlToFlyoutOpenedAckMilliseconds = $showResult.ElapsedMilliseconds
            InitialPickerOpenedGeneration = $initialGeneration
            FinalPickerOpenedGeneration = $finalGeneration
        }
        Package = [ordered]@{
            Name = $package.Name
            FamilyName = $package.PackageFamilyName
            Version = $packageVersion
            Publisher = $package.Publisher
            Architecture = $package.Architecture.ToString()
            ExecutableSha256 = $executableSha256
        }
        Host = [ordered]@{
            EnvironmentBeforeLaunch = $environmentBeforeLaunch
            EnvironmentBeforeOpen = $environmentBeforeOpen
            EnvironmentAfterOpen = $environmentAfterOpen
        }
        Source = [ordered]@{
            GitCommit = [string]$gitCommit
            GitWorkingTreeDirty = $gitDirty
        }
        Semantics = [ordered]@{
            Start = "Control client process creation immediately before the show command"
            Stop = "Control response after the matching WinUI Flyout.Opened callback"
            Excludes = @("screen presentation", "compositor latency", "display scanout")
        }
    }

    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $timestamp = [DateTime]::UtcNow.ToString("yyyyMMddTHHmmss.fffZ")
    $variantLabel = if ($Variant) { $Variant } else { "unlabeled" }
    $pairLabel = if ($PairId) { $PairId } else { "unpaired" }
    $outputPath = Join-Path $OutputDirectory "$timestamp-picker-open-ack-$variantLabel-$pairLabel.json"
    $measurement | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputPath -Encoding UTF8
    Write-Host ("Picker Opened acknowledgement: {0:N3} ms -> {1}" -f `
            $measurement.Run.ControlToFlyoutOpenedAckMilliseconds, $outputPath)
    if ($PassThru) {
        [pscustomobject]$measurement
    }
}
finally {
    if (-not $LeaveRunning -and $null -ne $startedProcess) {
        Stop-Process -Id $startedProcess.Id -Force -ErrorAction SilentlyContinue
        $startedProcess.WaitForExit(5000) | Out-Null
    }
}
