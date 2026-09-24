# Run in an elevated, disposable x64 Windows test account after installing a branch package.
# https://learn.microsoft.com/windows-hardware/drivers/devtest/application-verifier-testing-applications
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageFullName,
    [string]$Debugger = "${env:ProgramFiles(x86)}/Windows Kits/10/Debuggers/x64/cdb.exe",
    [ValidateRange(1, 100)][int]$Iterations = 3,
    [ValidateRange(15, 600)][int]$RunTimeoutSeconds = 90,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'Application Verifier package runs require x64 Windows and PowerShell 7.'
}
$debuggerPath = (Resolve-Path -LiteralPath $Debugger).Path
$verifier = (Get-Command appverif.exe -ErrorAction Stop).Source
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Application Verifier changes per-image diagnostics; use an elevated disposable test session.'
}
$packages = @(Get-AppxPackage | Where-Object PackageFullName -CEQ $PackageFullName)
if ($packages.Count -ne 1 -or $packages[0].Architecture -ne 'X64') {
    throw 'Specify exactly one installed x64 test package by its full name.'
}
$package = $packages[0]
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $repoRoot 'scripts/release/ReleaseVersion.psm1') -Force
$tags = @(git -C $repoRoot tag --list 'v*')
if ($LASTEXITCODE -ne 0) { throw 'Cannot read release tags; fetch tags before testing a branch package.' }
$releaseVersions = @(
    foreach ($tag in $tags) {
        if ($tag -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+$') { continue }
        $version = Resolve-ReleaseVersion -Tag $tag
        [version]::Parse($version.PackageVersion)
    }
)
if ($releaseVersions.Count -eq 0) { throw 'No release tags found; fetch tags before testing a branch package.' }
$latestReleaseVersion = $releaseVersions | Sort-Object -Descending | Select-Object -First 1
if ([version]$package.Version -le $latestReleaseVersion) {
    throw "Package version $($package.Version) is not newer than the latest tagged release $latestReleaseVersion."
}
$binary = Join-Path $package.InstallLocation 'AudioPlaybackConnector2.exe'
$cli = Join-Path $package.InstallLocation 'AudioPlaybackConnector2.Control/AudioPlaybackConnector2.Control.exe'
if (-not (Test-Path -LiteralPath $binary -PathType Leaf) -or -not (Test-Path -LiteralPath $cli -PathType Leaf)) {
    throw 'The installed package does not contain the expected app and CLI.'
}
$imageName = [IO.Path]::GetFileName($binary)
$processName = [IO.Path]::GetFileNameWithoutExtension($binary)
if (Get-Process -Name $processName -ErrorAction SilentlyContinue) {
    throw 'Close existing app instances before testing; the runner never takes over an existing process.'
}
$ifeo = "HKLM:/SOFTWARE/Microsoft/Windows NT/CurrentVersion/Image File Execution Options/$imageName"
if (Test-Path -LiteralPath $ifeo) {
    throw 'Existing per-image debugging settings must be preserved. Use a clean test account/machine.'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('apc-appverifier-' + [guid]::NewGuid().ToString('N'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'OutputDirectory must be a new directory.' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$logRoot = Join-Path $env:USERPROFILE 'AppVerifierLogs'
$existingLogs = @{}
foreach ($file in @(Get-ChildItem -LiteralPath $logRoot -File -ErrorAction SilentlyContinue)) {
    $existingLogs[$file.FullName] = $file.LastWriteTimeUtc
}

# WM_CLOSE uses the normal host teardown, including settings flush and window revocation.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class AppVerifierWindowClose {
    private delegate bool EnumProc(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool PostMessage(IntPtr window, uint message, IntPtr w, IntPtr l);
    public static bool Request(uint target) {
        bool posted = false;
        EnumWindows((window, data) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == target) posted |= PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return posted;
    }
}
"@

$enabled = $false
$passed = $false
$debugProcess = $null
$appProcess = $null
try {
    # Basics includes Heaps, Handles, Locks and thread-pool checks; keep full-page heap.
    $enabled = $true # Even a partially failed setup must be cleaned up.
    & $verifier -verify $imageName *> (Join-Path $output 'enable.log')
    if ($LASTEXITCODE -ne 0) { throw 'Application Verifier setup failed.' }
    & $verifier -query '*' -for $imageName *> (Join-Path $output 'settings.log')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read back Application Verifier settings.' }
    $settings = Get-Content -LiteralPath (Join-Path $output 'settings.log') -Raw
    foreach ($layer in @('Heaps', 'Handles', 'Locks')) {
        if ($settings -notmatch "(?i)\b$layer\b") { throw "Verifier layer missing: $layer" }
    }
    for ($iteration = 1; $iteration -le $Iterations; ++$iteration) {
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $debugLog = Join-Path $output "$iteration.debugger.log"
        # The initial command installs stop handlers, confirms verifier and continues.
        # Subsequent breaks/AVs fail immediately;
        # other unhandled stops hit the watchdog instead of hanging the runner.
        $commands = 'sxe -c ".echo APC_VERIFIER_FAILURE; !avrf; .lastevent; kv; q" av; sxe -c ".echo APC_VERIFIER_FAILURE; !avrf; .lastevent; kv; q" 80000003; lm m verifier; g'
        $debugProcess = Start-Process -FilePath $debuggerPath -WindowStyle Hidden -PassThru `
            -ArgumentList @('-o', '-G', '-c', ('"' + $commands.Replace('"', '\"') + '"'), ('"' + $binary + '"')) `
            -RedirectStandardOutput $debugLog -RedirectStandardError (Join-Path $output "$iteration.debugger.stderr.log")
        $appProcess = $null
        while ($clock.Elapsed.TotalSeconds -lt $RunTimeoutSeconds -and -not $debugProcess.HasExited) {
            $candidate = Get-Process -Name $processName -ErrorAction SilentlyContinue | Where-Object Path -EQ $binary | Select-Object -First 1
            if ($candidate) {
                $native = Get-CimInstance Win32_Process -Filter "ProcessId=$($candidate.Id)"
                if ($native.ParentProcessId -ne $debugProcess.Id) { throw 'App process is not owned by this debugger run.' }
                $appProcess = $candidate
                break
            }
            Start-Sleep -Milliseconds 100
        }
        if (-not $appProcess) { throw "Iteration ${iteration}: packaged app did not start under the debugger." }
        # Keep the process handle, so an exited process ID cannot be mistaken for a new process.
        $null = $appProcess.Handle
        foreach ($command in @('status', 'show', 'settings', 'status')) {
            $remaining = [int]($RunTimeoutSeconds * 1000 - $clock.ElapsedMilliseconds)
            if ($remaining -le 0 -or $appProcess.HasExited) { throw "Iteration ${iteration}: app watchdog expired or app exited." }
            $commandLog = Join-Path $output "$iteration.$command.$($clock.ElapsedMilliseconds).log"
            $client = Start-Process -FilePath $cli -ArgumentList $command -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $commandLog -RedirectStandardError "$commandLog.stderr"
            try {
                if (-not $client.WaitForExit([Math]::Min(15000, $remaining))) {
                    $client.Kill($true)
                    if (-not $client.WaitForExit(5000)) { throw 'CLI watchdog could not terminate its child.' }
                    throw "CLI $command timed out."
                }
                if ($client.ExitCode -ne 0) { throw "CLI $command failed with exit code $($client.ExitCode)." }
            } finally { $client.Dispose() }
        }
        if (-not [AppVerifierWindowClose]::Request([uint32]$appProcess.Id)) { throw 'No application window accepted shutdown.' }
        $remaining = [Math]::Max(1, [int]($RunTimeoutSeconds * 1000 - $clock.ElapsedMilliseconds))
        if (-not $appProcess.WaitForExit($remaining) -or $appProcess.ExitCode -ne 0) { throw 'App shutdown failed or timed out.' }
        if (-not $debugProcess.WaitForExit(5000) -or $debugProcess.ExitCode -ne 0) { throw 'Debugger did not finish successfully.' }
        $diagnostics = Get-Content -LiteralPath $debugLog -Raw
        if ($diagnostics -match 'APC_VERIFIER_FAILURE|VERIFIER STOP|second chance' -or $diagnostics -notmatch '(?im)^[0-9a-f`]+\s+[0-9a-f`]+\s+verifier\b') {
            throw 'Debugger reported a failure or did not confirm the verifier module.'
        }
        $appProcess.Dispose(); $appProcess = $null
        $debugProcess.Dispose(); $debugProcess = $null
        Write-Host "AppVerifier packaged UI iteration $iteration/$Iterations passed."
    }
    # AppVerifier creates logs for stops. Even a non-breaking stop needs review.
    $newLogs = @(Get-ChildItem -LiteralPath $logRoot -Filter 'AudioPlaybackConnector2*' -File -ErrorAction SilentlyContinue | Where-Object {
        -not $existingLogs.ContainsKey($_.FullName) -or $existingLogs[$_.FullName] -ne $_.LastWriteTimeUtc
    })
    if ($newLogs.Count) { throw 'Application Verifier generated stop logs; inspect the failure diagnostics.' }
    $passed = $true
} finally {
    $cleanupFailed = $false
    foreach ($child in @($debugProcess, $appProcess)) {
        if ($null -eq $child) { continue }
        try {
            if (-not $child.HasExited) {
                $child.Kill($true)
                if (-not $child.WaitForExit(5000)) { throw 'Test child did not terminate.' }
            }
        } catch {
            $passed = $false
            $cleanupFailed = $true
            Write-Warning "Test child cleanup: $_"
        } finally { $child.Dispose() }
    }
    if ($enabled) {
        & $verifier -delete settings -for $imageName *> (Join-Path $output 'cleanup.log')
        if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $ifeo)) {
            $passed = $false
            $cleanupFailed = $true
            Write-Warning "Verifier cleanup failed. Inspect $ifeo before running this app again."
        }
    }
    # Preserve only this run's changed verifier logs, and only on failure.
    if (-not $passed) {
        foreach ($file in @(Get-ChildItem -LiteralPath $logRoot -Filter 'AudioPlaybackConnector2*' -File -ErrorAction SilentlyContinue)) {
            if (-not $existingLogs.ContainsKey($file.FullName) -or $existingLogs[$file.FullName] -ne $file.LastWriteTimeUtc) {
                Copy-Item -LiteralPath $file.FullName -Destination $output
            }
        }
        Write-Host "Failed AppVerifier diagnostics: $output"
    } else {
        # This new directory contains only files created by this run; no recursive deletion.
        Get-ChildItem -LiteralPath $output -File | Remove-Item
        Remove-Item -LiteralPath $output
    }
    if ($cleanupFailed) { throw "AppVerifier cleanup incomplete. Diagnostics: $output" }
}
