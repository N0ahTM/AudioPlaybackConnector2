param(
    [ValidateRange(5, 300)]
    [int]$IdleSampleSeconds = 30,

    [ValidateRange(3, 100)]
    [int]$PickerIterations = 20,

    [ValidateRange(1, 50)]
    [int]$SettingsIterations = 10,

    [ValidateRange(5, 120)]
    [int]$VisibleUiSampleSeconds = 15
)

$ErrorActionPreference = 'Stop'
$packageName = 'N0ahTM.AudioPlaybackConnector2'
$applicationUserModelId = 'N0ahTM.AudioPlaybackConnector2_f4eqcdwtr7cg6!App'
$megabyte = 1MB

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class PerformanceBaselineNativeWindow
{
    public delegate bool EnumWindowsCallback(IntPtr windowHandle, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    public struct Rectangle
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr windowHandle, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr windowHandle);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr windowHandle, out Rectangle rectangle);

    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr windowHandle, uint message, IntPtr wordParameter, IntPtr longParameter);
}
'@

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)

    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    if ($sorted.Count -eq 1) { return [math]::Round($sorted[0], 2) }

    $position = ($sorted.Count - 1) * $Percentile
    $lowerIndex = [math]::Floor($position)
    $upperIndex = [math]::Ceiling($position)
    $fraction = $position - $lowerIndex
    $value = $sorted[$lowerIndex] + (($sorted[$upperIndex] - $sorted[$lowerIndex]) * $fraction)
    return [math]::Round($value, 2)
}

function Get-Distribution {
    param([double[]]$Values)

    return [ordered]@{
        count = $Values.Count
        minimum = [math]::Round(($Values | Measure-Object -Minimum).Minimum, 2)
        median = Get-Percentile -Values $Values -Percentile 0.50
        percentile95 = Get-Percentile -Values $Values -Percentile 0.95
        maximum = [math]::Round(($Values | Measure-Object -Maximum).Maximum, 2)
        average = [math]::Round(($Values | Measure-Object -Average).Average, 2)
    }
}

function Invoke-ControlCommand {
    param([string[]]$Arguments)

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $output = & $script:controlExecutable @Arguments 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    $stopwatch.Stop()
    if ($exitCode -ne 0) {
        throw "Control command '$($Arguments -join ' ')' failed with exit code $exitCode`: $output"
    }

    return [ordered]@{
        elapsedMilliseconds = $stopwatch.Elapsed.TotalMilliseconds
        output = $output.Trim()
    }
}

function Get-ApplicationProcess {
    $process = Get-Process -Name 'AudioPlaybackConnector2' -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending |
        Select-Object -First 1
    if (-not $process) { throw 'AudioPlaybackConnector2 is not running.' }
    return $process
}

function Close-SettingsWindow {
    param([uint32]$ProcessId)

    $closed = $false
    $callback = [PerformanceBaselineNativeWindow+EnumWindowsCallback]{
        param([IntPtr]$windowHandle, [IntPtr]$parameter)

        [uint32]$ownerProcessId = 0
        [void][PerformanceBaselineNativeWindow]::GetWindowThreadProcessId($windowHandle, [ref]$ownerProcessId)
        if ($ownerProcessId -ne $ProcessId -or
            -not [PerformanceBaselineNativeWindow]::IsWindowVisible($windowHandle)) {
            return $true
        }

        $rectangle = New-Object PerformanceBaselineNativeWindow+Rectangle
        if (-not [PerformanceBaselineNativeWindow]::GetWindowRect($windowHandle, [ref]$rectangle)) {
            return $true
        }

        if (($rectangle.Right - $rectangle.Left) -ge 300 -and ($rectangle.Bottom - $rectangle.Top) -ge 200) {
            [void][PerformanceBaselineNativeWindow]::PostMessage($windowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            $script:settingsWindowClosed = $true
        }
        return $true
    }

    $script:settingsWindowClosed = $false
    [void][PerformanceBaselineNativeWindow]::EnumWindows($callback, [IntPtr]::Zero)
    $closed = $script:settingsWindowClosed
    Remove-Variable -Name settingsWindowClosed -Scope Script -ErrorAction SilentlyContinue
    return $closed
}

function Measure-ProcessResources {
    param([int]$Seconds)

    $logicalProcessorCount = [Environment]::ProcessorCount
    $samples = [System.Collections.Generic.List[object]]::new()
    $process = Get-ApplicationProcess
    $previousProcessorTime = $process.TotalProcessorTime
    $previousTimestamp = [datetime]::UtcNow

    for ($sampleIndex = 0; $sampleIndex -lt $Seconds; $sampleIndex++) {
        Start-Sleep -Seconds 1
        $process = Get-Process -Id $process.Id
        $timestamp = [datetime]::UtcNow
        $processorTime = $process.TotalProcessorTime
        $wallSeconds = ($timestamp - $previousTimestamp).TotalSeconds
        $processorSeconds = ($processorTime - $previousProcessorTime).TotalSeconds
        $machineCpuPercent = if ($wallSeconds -gt 0) {
            100 * $processorSeconds / ($wallSeconds * $logicalProcessorCount)
        } else { 0 }

        $samples.Add([ordered]@{
            workingSetMegabytes = $process.WorkingSet64 / $megabyte
            privateCommitMegabytes = $process.PrivateMemorySize64 / $megabyte
            machineCpuPercent = $machineCpuPercent
            threadCount = $process.Threads.Count
            handleCount = $process.HandleCount
        })
        $previousTimestamp = $timestamp
        $previousProcessorTime = $processorTime
    }

    return [ordered]@{
        durationSeconds = $Seconds
        workingSetMegabytes = Get-Distribution -Values @($samples.workingSetMegabytes)
        privateCommitMegabytes = Get-Distribution -Values @($samples.privateCommitMegabytes)
        machineCpuPercent = Get-Distribution -Values @($samples.machineCpuPercent)
        threadCount = Get-Distribution -Values @($samples.threadCount)
        handleCount = Get-Distribution -Values @($samples.handleCount)
    }
}

$package = Get-AppxPackage -Name $packageName | Select-Object -First 1
if (-not $package) { throw "Package '$packageName' is not installed." }
$controlExecutable = Join-Path $package.InstallLocation 'AudioPlaybackConnector2.Control\AudioPlaybackConnector2.Control.exe'
$applicationExecutable = Join-Path $package.InstallLocation 'AudioPlaybackConnector2.exe'

Get-Process -Name 'AudioPlaybackConnector2' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$startupStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
Start-Process -FilePath 'explorer.exe' -ArgumentList "shell:AppsFolder\$applicationUserModelId" -WindowStyle Hidden
$startupStatus = $null
for ($attempt = 0; $attempt -lt 80; $attempt++) {
    Start-Sleep -Milliseconds 50
    try {
        $candidate = Invoke-ControlCommand -Arguments @('status', '--json')
        $startupStatus = $candidate.output | ConvertFrom-Json
        if ($startupStatus.running) { break }
    } catch {
        $startupStatus = $null
    }
}
$startupStopwatch.Stop()
if (-not $startupStatus) { throw 'The application did not become ready for CLI commands.' }
$startupReadyMilliseconds = $startupStopwatch.Elapsed.TotalMilliseconds

Start-Sleep -Seconds 15
$statusBeforeMeasurements = (Invoke-ControlCommand -Arguments @('status', '--json')).output | ConvertFrom-Json
$idleResources = Measure-ProcessResources -Seconds $IdleSampleSeconds

$pickerLatencies = [System.Collections.Generic.List[double]]::new()
for ($iteration = 0; $iteration -lt $PickerIterations; $iteration++) {
    $openResult = Invoke-ControlCommand -Arguments @('show')
    $pickerLatencies.Add($openResult.elapsedMilliseconds)
    Start-Sleep -Milliseconds 200
    [void](Invoke-ControlCommand -Arguments @('show'))
    Start-Sleep -Milliseconds 250
}
[void](Invoke-ControlCommand -Arguments @('show'))
Start-Sleep -Seconds 1
$pickerVisibleResources = Measure-ProcessResources -Seconds $VisibleUiSampleSeconds
[void](Invoke-ControlCommand -Arguments @('show'))

$settingsLatencies = [System.Collections.Generic.List[double]]::new()
$applicationProcess = Get-ApplicationProcess
for ($iteration = 0; $iteration -lt $SettingsIterations; $iteration++) {
    $settingsResult = Invoke-ControlCommand -Arguments @('settings')
    $settingsLatencies.Add($settingsResult.elapsedMilliseconds)
    Start-Sleep -Milliseconds 300
    if (-not (Close-SettingsWindow -ProcessId $applicationProcess.Id)) {
        throw "Could not find the visible settings window in iteration $iteration."
    }
    Start-Sleep -Milliseconds 500
}
[void](Invoke-ControlCommand -Arguments @('settings'))
Start-Sleep -Seconds 1
$settingsVisibleResources = Measure-ProcessResources -Seconds $VisibleUiSampleSeconds
if (-not (Close-SettingsWindow -ProcessId $applicationProcess.Id)) {
    throw 'Could not close the settings window after resource sampling.'
}

$statusAfterMeasurements = (Invoke-ControlCommand -Arguments @('status', '--json')).output | ConvertFrom-Json
$operatingSystem = Get-CimInstance Win32_OperatingSystem
$processor = Get-CimInstance Win32_Processor | Select-Object -First 1
$computer = Get-CimInstance Win32_ComputerSystem
$applicationHash = Get-FileHash -Algorithm SHA256 -LiteralPath $applicationExecutable
$controlHash = Get-FileHash -Algorithm SHA256 -LiteralPath $controlExecutable

$result = [ordered]@{
    capturedAt = [datetimeoffset]::Now.ToString('o')
    package = [ordered]@{
        name = $package.Name
        version = $package.Version.ToString()
        architecture = $package.Architecture.ToString()
        packageFullName = $package.PackageFullName
        applicationSha256 = $applicationHash.Hash
        controlSha256 = $controlHash.Hash
    }
    environment = [ordered]@{
        operatingSystemCaption = $operatingSystem.Caption
        operatingSystemVersion = $operatingSystem.Version
        operatingSystemBuild = $operatingSystem.BuildNumber
        processor = $processor.Name.Trim()
        logicalProcessorCount = $computer.NumberOfLogicalProcessors
        physicalMemoryGigabytes = [math]::Round($computer.TotalPhysicalMemory / 1GB, 2)
    }
    startupToControlReadyMilliseconds = [math]::Round($startupReadyMilliseconds, 2)
    statusBeforeMeasurements = $statusBeforeMeasurements
    idleResources = $idleResources
    pickerOpenMilliseconds = Get-Distribution -Values $pickerLatencies.ToArray()
    pickerOpenRawMilliseconds = @($pickerLatencies | ForEach-Object { [math]::Round($_, 2) })
    pickerVisibleResources = $pickerVisibleResources
    settingsFullyReadyMilliseconds = Get-Distribution -Values $settingsLatencies.ToArray()
    settingsFullyReadyRawMilliseconds = @($settingsLatencies | ForEach-Object { [math]::Round($_, 2) })
    settingsVisibleResources = $settingsVisibleResources
    statusAfterMeasurements = $statusAfterMeasurements
}

$result | ConvertTo-Json -Depth 12
