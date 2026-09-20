param(
    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$DisablePrecompiledHeaders
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
Push-Location $repository
try {
    foreach ($project in @('CoreRuntime', 'CoreTests', 'Control')) {
        & pwsh -NoProfile -File "$PSScriptRoot/verify-core-runtime-boundary.ps1" -ProjectPath "AudioPlaybackConnector2.$project/AudioPlaybackConnector2.$project.vcxproj"
        if ($LASTEXITCODE -ne 0) { throw "Headless boundary verification failed for $project." }
    }
    $diagnostics = Join-Path ([IO.Path]::GetTempPath()) ('apc-headless-' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($diagnostics) | Out-Null
    $restoreLog = Join-Path $diagnostics 'restore.binlog'
    & msbuild AudioPlaybackConnector2.Headless.slnf "/bl:$restoreLog;ProjectImports=None" /t:Restore /p:RestorePackagesConfig=true "/p:Configuration=$Configuration" "/p:Platform=$Platform" /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Headless package restore failed. Private diagnostic: $restoreLog" }
    Remove-Item -LiteralPath $restoreLog -Force
    $buildLog = Join-Path $diagnostics 'build.binlog'
    $arguments = @(
        'AudioPlaybackConnector2.Headless.slnf', "/bl:$buildLog;ProjectImports=None", '/t:Build', '/m', '/nologo', '/v:minimal',
        "/p:Configuration=$Configuration", "/p:Platform=$Platform", '/p:PreferredToolArchitecture=x64'
    )
    if ($DisablePrecompiledHeaders) { $arguments += '/p:ApcDisablePrecompiledHeaders=true' }
    & msbuild @arguments
    if ($LASTEXITCODE -ne 0) { throw "Headless build failed. Private diagnostic: $buildLog" }
    Remove-Item -LiteralPath $buildLog -Force
    Remove-Item -LiteralPath $diagnostics

    $nativeArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    if ($Platform -eq 'ARM64' -and $nativeArchitecture -ne 'Arm64') {
        Write-Host 'ARM64 cross-build complete; execute the tests on ARM64 Windows.'
    } else {
        & "./$Platform/$Configuration/tests/AudioPlaybackConnector2.CoreTests.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
    }
} finally {
    Pop-Location
}
