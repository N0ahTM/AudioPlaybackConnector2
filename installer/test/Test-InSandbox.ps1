#Requires -Version 5.1
# Host-side launcher: boots Windows Sandbox with the installer under test mapped in,
# waits for the in-sandbox verification to finish, tears the sandbox down and prints
# the result. Run from the repo root, e.g.:
#   .\installer\test\Test-InSandbox.ps1                                   # fresh install, newest setup
#   .\installer\test\Test-InSandbox.ps1 -SetupExe dist\installer\AudioPlaybackConnector2-WebSetup.exe
#   .\installer\test\Test-InSandbox.ps1 -PreInstallSetup dist\installer\AudioPlaybackConnector2-Setup-0.7.0.exe  # update path
#   .\installer\test\Test-InSandbox.ps1 -LaunchApp                        # also verify the app starts
[CmdletBinding()]
param(
    [string] $SetupExe,
    [string] $PreInstallSetup,
    [switch] $LaunchApp,
    [int] $TimeoutMinutes = 20,
    [string] $OutDir
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
Push-Location $repoRoot
try {
    $sandboxExe = Join-Path $env:SystemRoot 'System32\WindowsSandbox.exe'
    if (-not (Test-Path $sandboxExe)) {
        throw @"
Windows Sandbox not found. Enable it once (Windows Pro/Enterprise/Education, admin + reboot):
  Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All
or:  Optional Features -> Windows Sandbox. Not available on Windows Home.
"@
    }

    if (-not $SetupExe) {
        $SetupExe = Get-ChildItem 'dist\installer' -Filter 'AudioPlaybackConnector2-Setup-*.exe' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
        if (-not $SetupExe) {
            $SetupExe = Get-ChildItem 'dist\installer' -Filter 'AudioPlaybackConnector2-WebSetup.exe' -ErrorAction SilentlyContinue |
                Select-Object -First 1 -ExpandProperty FullName
        }
    }
    if (-not $SetupExe -or -not (Test-Path $SetupExe)) {
        throw 'No setup found. Build one first: .\installer\build-installer.ps1 -Mode Web'
    }
    $SetupExe = (Resolve-Path $SetupExe).Path
    $payloadDir = Split-Path $SetupExe -Parent
    $scriptsDir = Join-Path $repoRoot 'installer\test\sandbox'

    if (-not $OutDir) {
        $OutDir = Join-Path $repoRoot ("dist\installer\sandbox-out\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    }
    $null = New-Item -ItemType Directory -Path $OutDir -Force
    $OutDir = (Resolve-Path $OutDir).Path

    $preArgs = ''
    if ($PreInstallSetup) {
        if (-not (Test-Path $PreInstallSetup)) { throw "PreInstallSetup not found: $PreInstallSetup" }
        $PreInstallSetup = (Resolve-Path $PreInstallSetup).Path
        if ((Split-Path $PreInstallSetup -Parent) -ne $payloadDir) {
            # the sandbox only sees $payloadDir; copy the older setup next to the tested one
            Copy-Item $PreInstallSetup $payloadDir -Force
            $PreInstallSetup = Join-Path $payloadDir (Split-Path $PreInstallSetup -Leaf)
        }
        $preArgs = " -PreInstallSetup 'C:\apc2-test\payload\$(Split-Path $PreInstallSetup -Leaf)'"
    }
    $launchArgs = if ($LaunchApp) { ' -LaunchApp' } else { '' }

    $setupName = Split-Path $SetupExe -Leaf
    $logon = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\apc2-test\scripts\run-install-test.ps1" +
             " -SetupExe 'C:\apc2-test\payload\$setupName' -OutDir 'C:\apc2-test\out'$preArgs$launchArgs"

    $wsb = Join-Path $OutDir 'test.wsb'
    @"
<Configuration>
  <Networking>Enable</Networking>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <PrinterRedirection>Disable</PrinterRedirection>
  <ClipboardRedirection>Enable</ClipboardRedirection>
  <MemoryInMB>4096</MemoryInMB>
  <MappedFolders>
    <MappedFolder><HostFolder>$payloadDir</HostFolder><SandboxFolder>C:\apc2-test\payload</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$scriptsDir</HostFolder><SandboxFolder>C:\apc2-test\scripts</SandboxFolder><ReadOnly>true</ReadOnly></MappedFolder>
    <MappedFolder><HostFolder>$OutDir</HostFolder><SandboxFolder>C:\apc2-test\out</SandboxFolder><ReadOnly>false</ReadOnly></MappedFolder>
  </MappedFolders>
  <LogonCommand><Command>$logon</Command></LogonCommand>
</Configuration>
"@ | Set-Content $wsb -Encoding UTF8

    Write-Host "Setup:    $setupName"
    Write-Host "Out:      $OutDir"
    Write-Host "Starting Windows Sandbox (boot + install can take several minutes) ..."
    $sandbox = Start-Process -FilePath $sandboxExe -ArgumentList "`"$wsb`"" -PassThru

    $doneFile = Join-Path $OutDir 'DONE'
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    while (-not (Test-Path $doneFile)) {
        if ((Get-Date) -gt $deadline) {
            Write-Warning "Timeout after $TimeoutMinutes min - sandbox kept open for manual inspection."
            Write-Host "Partial results (if any): $OutDir"
            exit 2
        }
        if ($sandbox.HasExited) {
            Write-Warning 'Sandbox window was closed before the test finished.'
            exit 2
        }
        Start-Sleep -Seconds 5
    }

    Write-Host 'Test finished, closing sandbox ...'
    Stop-Process -Id $sandbox.Id -Force -ErrorAction SilentlyContinue

    $resultFile = Join-Path $OutDir 'result.json'
    if (Test-Path $resultFile) {
        $result = Get-Content $resultFile -Raw | ConvertFrom-Json
        Write-Host ''
        Write-Host ('================  ' + $(if ($result.pass) { 'PASS' } else { 'FAIL' }) + '  ================')
        $result | ConvertTo-Json -Depth 4 | Write-Host
    } else {
        Write-Warning 'No result.json - see sandbox-transcript.txt'
    }
    Write-Host "Artifacts: $OutDir"
    if (-not $result.pass) { exit 1 }
} finally {
    Pop-Location
}
