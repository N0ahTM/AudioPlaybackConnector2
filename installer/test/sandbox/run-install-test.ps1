#Requires -Version 5.1
# Runs INSIDE Windows Sandbox (as WDAGUtilityAccount) via the .wsb LogonCommand.
# Silently installs the staged setup, verifies the result and writes all
# artifacts into the shared out folder. Ends by writing a DONE sentinel file
# so the host-side Test-InSandbox.ps1 can tear the sandbox down.
param(
    [string] $SetupExe,        # e.g. C:\apc2-test\payload\AudioPlaybackConnector2-WebSetup.exe
    [string] $PreInstallSetup, # optional older setup for update/downgrade scenarios
    [string] $OutDir,          # e.g. C:\apc2-test\out
    [switch] $LaunchApp
)

$ErrorActionPreference = 'Continue' # keep going so partial evidence survives

function Resolve-TestFolder([string] $name) {
    foreach ($candidate in @("C:\apc2-test\$name",
                             (Join-Path $env:USERPROFILE "Desktop\$name"))) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }
    throw "Mapped folder '$name' not found (neither C:\apc2-test nor Desktop)."
}

try {
    $payload = Resolve-TestFolder 'payload'
    if (-not $OutDir) { $OutDir = Resolve-TestFolder 'out' }
    if (-not $SetupExe) {
        $SetupExe = Get-ChildItem $payload -Filter 'AudioPlaybackConnector2-*.exe' |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not (Test-Path $SetupExe)) { throw "Setup not found: $SetupExe" }

    Start-Transcript -Path (Join-Path $OutDir 'sandbox-transcript.txt') -Force | Out-Null
    Write-Host "Setup under test: $SetupExe"

    $result = [ordered]@{
        setupExe           = Split-Path $SetupExe -Leaf
        preInstall         = $null
        installedBefore    = $null
        setupExitCode      = $null
        installedAfter     = $null
        certInTrustedPeople = $null
        appLaunched        = $null
        pass               = $false
        notes              = @()
    }

    function Get-InstalledVersion {
        $p = Get-AppxPackage -Name 'N0ahTM.AudioPlaybackConnector2' -ErrorAction SilentlyContinue
        if ($p) { return [string]$p.Version }
        return $null
    }

    function Invoke-SetupSilent([string] $exe, [string] $logName) {
        $args = @('/VERYSILENT', '/NORESTART', '/SUPPRESSMSGBOXES', '/NOCANCEL',
                  '/MERGETASKS=!launch', "/LOG=$(Join-Path $OutDir $logName)")
        Write-Host "Running: $exe $($args -join ' ')"
        $proc = Start-Process -FilePath $exe -ArgumentList $args -Wait -PassThru
        Write-Host "Exit code: $($proc.ExitCode)"
        return $proc.ExitCode
    }

    if ($PreInstallSetup) {
        Write-Host "--- Pre-install (older version): $PreInstallSetup"
        $preExe = Join-Path $payload (Split-Path $PreInstallSetup -Leaf)
        if (-not (Test-Path $preExe)) { $preExe = $PreInstallSetup }
        $result.preInstall = [ordered]@{
            exe = Split-Path $preExe -Leaf
            exitCode = (Invoke-SetupSilent $preExe 'setup-preinstall.log')
        }
        $result.installedBefore = Get-InstalledVersion
        Write-Host "Installed after pre-install: $($result.installedBefore)"
    }

    $result.setupExitCode = Invoke-SetupSilent $SetupExe 'setup.log'
    $result.installedAfter = Get-InstalledVersion
    Write-Host "Installed after setup: $($result.installedAfter)"

    $certs = @(Get-ChildItem 'Cert:\CurrentUser\TrustedPeople' -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -match 'AudioPlaybackConnector2' })
    $result.certInTrustedPeople = $certs.Count -gt 0
    Write-Host "Cert in CurrentUser\TrustedPeople: $($result.certInTrustedPeople) ($($certs.Count) match(es))"

    $installLog = Join-Path $env:LOCALAPPDATA 'AudioPlaybackConnector2\install.log'
    if (Test-Path $installLog) {
        Copy-Item $installLog (Join-Path $OutDir 'install.log') -Force
    } else {
        $result.notes += 'install.log missing (install-app.ps1 transcript)'
    }

    if ($LaunchApp -and $result.installedAfter) {
        $pkg = Get-AppxPackage -Name 'N0ahTM.AudioPlaybackConnector2'
        $family = $pkg.PackageFamilyName
        Write-Host "Launching shell:AppsFolder\$family!App"
        Start-Process "shell:AppsFolder\$family!App"
        Start-Sleep -Seconds 8
        $appProc = Get-Process -Name 'AudioPlaybackConnector2' -ErrorAction SilentlyContinue
        $result.appLaunched = [bool]$appProc
        Write-Host "App process running: $($result.appLaunched)"
        if ($appProc) { $appProc | Stop-Process -Force -ErrorAction SilentlyContinue }
    }

    $result.pass = ($result.setupExitCode -eq 0) -and
                   [bool]$result.installedAfter -and
                   $result.certInTrustedPeople -and
                   (-not $LaunchApp -or $result.appLaunched)
    $result | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'result.json') -Encoding UTF8
    Write-Host ("RESULT: " + $(if ($result.pass) { 'PASS' } else { 'FAIL' }))
} catch {
    $_ | Out-String | Write-Host
    @{ pass = $false; error = "$_" } | ConvertTo-Json |
        Set-Content (Join-Path $OutDir 'result.json') -Encoding UTF8
} finally {
    try { Stop-Transcript | Out-Null } catch {}
    Set-Content (Join-Path $OutDir 'DONE') -Value (Get-Date -Format o) -Encoding ASCII
}
