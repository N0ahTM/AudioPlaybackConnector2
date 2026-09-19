param(
    [ValidateSet('x64', 'ARM64')][string[]]$Architecture = @('x64', 'ARM64'),
    [string]$OutputRoot = (Join-Path $env:LOCALAPPDATA ('AudioPlaybackConnector2/DependencyProbes/' + [guid]::NewGuid().ToString('N'))),
    [string]$InstalledRoot = (Join-Path $env:TEMP 'apc-dependency-probes/vcpkg_installed'),
    [string]$VcpkgExe
)
$ErrorActionPreference = 'Stop'
if (-not $VcpkgExe) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $VcpkgExe = Join-Path $installation 'VC/vcpkg/vcpkg.exe'
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
$InstalledRoot = [System.IO.Path]::GetFullPath($InstalledRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw "Use a fresh output directory for cold-build measurements: $OutputRoot" }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
$measurements = [System.Collections.Generic.List[object]]::new()
$ports = @{ cli11 = 'cli11'; json = 'nlohmann-json'; spdlog = 'spdlog' }
foreach ($platform in $Architecture) {
    $triplet = $platform.ToLowerInvariant() + '-windows-static-md'
    & $VcpkgExe install "--triplet=$triplet" "--x-manifest-root=$PSScriptRoot" "--x-install-root=$InstalledRoot" --disable-metrics
    if ($LASTEXITCODE -ne 0) { throw "Restore failed for $triplet" }
    $prefix = Join-Path $InstalledRoot $triplet
    $build = Join-Path $OutputRoot "build-$platform"
    & cmake -S $PSScriptRoot -B $build -G 'Visual Studio 18 2026' -A $platform -T v145 "-DCMAKE_PREFIX_PATH=$prefix"
    if ($LASTEXITCODE -ne 0) { throw "Configure failed for $platform" }
    foreach ($probe in @('baseline', 'cli11', 'json', 'spdlog')) {
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        & cmake --build $build --config Release --target "probe_$probe" -- /nologo /v:minimal
        $timer.Stop()
        if ($LASTEXITCODE -ne 0) { throw "Build failed for $platform/$probe" }
        $exe = Join-Path $build "Release/probe_$probe.exe"
        $package = Join-Path $OutputRoot "package-$platform-$probe"
        New-Item -ItemType Directory -Path $package | Out-Null
        Copy-Item -LiteralPath $exe -Destination $package
        $licenseHash = $null
        if ($ports.ContainsKey($probe)) {
            $license = Join-Path $prefix "share/$($ports[$probe])/copyright"
            Copy-Item -LiteralPath $license -Destination (Join-Path $package 'THIRD_PARTY_LICENSE.txt')
            $licenseHash = (Get-FileHash -LiteralPath $license -Algorithm SHA256).Hash
        }
        $zip = Join-Path $OutputRoot "$platform-$probe.zip"
        Compress-Archive -Path (Join-Path $package '*') -DestinationPath $zip
        $native = $platform -eq 'x64' -or [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64'
        $exitCode = $null
        $stdout = ''
        $stderr = ''
        $timedOut = $false
        if ($native) {
            $start = [System.Diagnostics.ProcessStartInfo]::new($exe)
            $start.UseShellExecute = $false
            $start.CreateNoWindow = $true
            $start.RedirectStandardOutput = $true
            $start.RedirectStandardError = $true
            $start.ArgumentList.Add((Join-Path $OutputRoot "run-$platform-$probe"))
            $process = [System.Diagnostics.Process]::Start($start)
            $outTask = $process.StandardOutput.ReadToEndAsync()
            $errTask = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(15000)) {
                $process.Kill($true)
                $process.WaitForExit()
                $timedOut = $true
            }
            $stdout = $outTask.GetAwaiter().GetResult()
            $stderr = $errTask.GetAwaiter().GetResult()
            $exitCode = $process.ExitCode
            $process.Dispose()
        }
        $measurements.Add([pscustomobject]@{
            Architecture = $platform; Probe = $probe; BuildMilliseconds = $timer.ElapsedMilliseconds
            ExecutableBytes = (Get-Item -LiteralPath $exe).Length; ProbeZipBytes = (Get-Item -LiteralPath $zip).Length
            Executed = $native; TimedOut = $timedOut; ExitCode = $exitCode; Stdout = $stdout; Stderr = $stderr; LicenseSha256 = $licenseHash
        })
        $measurements | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputRoot 'measurements.json')
        if ($timedOut) { throw "Probe watchdog expired: $platform/$probe" }
        if ($native -and $exitCode -ne 0) { throw "Probe failed: $platform/$probe, exit $exitCode; $stderr" }
    }
}
Write-Output "Dependency probe measurements: $OutputRoot/measurements.json"
