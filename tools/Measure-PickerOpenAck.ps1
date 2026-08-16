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
    [string]$OutputDirectory,

    [Parameter()]
    [switch]$LeaveRunning,

    [Parameter()]
    [switch]$PassThru
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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
