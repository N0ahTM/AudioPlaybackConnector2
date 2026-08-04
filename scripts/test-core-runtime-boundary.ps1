param(
    [Parameter(Mandatory = $true)]
    [string]$VerifierPath
)

$ErrorActionPreference = 'Stop'
$resolvedVerifier = (Resolve-Path -LiteralPath $VerifierPath).Path
$testDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("apc-boundary-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testDirectory | Out-Null

try {
    $cleanProject = Join-Path $testDirectory 'Clean.vcxproj'
    $uiProject = Join-Path $testDirectory 'Ui.vcxproj'
    $rootProject = Join-Path $testDirectory 'Root.vcxproj'
    $importProject = Join-Path $testDirectory 'ImportRoot.vcxproj'
    $uiProps = Join-Path $testDirectory 'Shared.props'
    $nestedRootProject = Join-Path $testDirectory 'NestedRoot.vcxproj'
    $nestedUiDirectory = Join-Path $testDirectory 'Ui'
    $nestedUiProject = Join-Path $nestedUiDirectory 'NestedUi.vcxproj'
    $nestedUiProps = Join-Path $nestedUiDirectory 'NestedShared.props'
    $sourceRootProject = Join-Path $testDirectory 'SourceRoot.vcxproj'
    $sourceProjectDirectory = Join-Path $testDirectory 'Source'
    $sourceProject = Join-Path $sourceProjectDirectory 'Source.vcxproj'
    $sourceProps = Join-Path $sourceProjectDirectory 'Source.props'
    $forbiddenSource = Join-Path $sourceProjectDirectory 'ui.cpp'
    $wildcardDirectory = Join-Path $testDirectory 'Wildcard'
    $wildcardProject = Join-Path $wildcardDirectory 'Wildcard.vcxproj'
    New-Item -ItemType Directory -Path $nestedUiDirectory | Out-Null
    New-Item -ItemType Directory -Path $sourceProjectDirectory | Out-Null
    New-Item -ItemType Directory -Path $wildcardDirectory | Out-Null
    Set-Content -LiteralPath $cleanProject -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003" />
'@
    Set-Content -LiteralPath $uiProject -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup><UseWinUI>true</UseWinUI></PropertyGroup>
</Project>
'@
    Set-Content -LiteralPath $rootProject -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup><ProjectReference Include="Ui.vcxproj" /></ItemGroup>
</Project>
'@
    Set-Content -LiteralPath $uiProps -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup><UseWinUI>true</UseWinUI></PropertyGroup>
</Project>
'@
    Set-Content -LiteralPath $importProject -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="Shared.props" />
</Project>
'@
    Set-Content -LiteralPath $nestedRootProject -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup><ProjectReference Include="Ui\NestedUi.vcxproj" /></ItemGroup>
</Project>
'@
    Set-Content -LiteralPath $nestedUiProject -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="$(MSBuildProjectDirectory)\NestedShared.props" />
</Project>
'@
    Set-Content -LiteralPath $nestedUiProps -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup><UseWinUI>true</UseWinUI></PropertyGroup>
</Project>
'@
    Set-Content -LiteralPath $sourceRootProject -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup><ProjectReference Include="Source\Source.vcxproj" /></ItemGroup>
</Project>
'@
    Set-Content -LiteralPath $sourceProject -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="$(MSBuildProjectDirectory)\Source.props" />
</Project>
'@
    Set-Content -LiteralPath $sourceProps -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup><ClCompile Include="$(MSBuildProjectDirectory)\ui.cpp" /></ItemGroup>
</Project>
'@
    Set-Content -LiteralPath $forbiddenSource -Encoding UTF8 -Value @'
#include <winrt/Microsoft.UI.Xaml.h>
'@
    Set-Content -LiteralPath $wildcardProject -Encoding UTF8 -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="$(MSBuildThisFileDirectory)*.props" />
</Project>
'@

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $cleanProject -ComponentName BoundarySelfTest *> $null
    if ($LASTEXITCODE -ne 0) {
        throw 'Boundary verifier rejected a clean project.'
    }

    $previousErrorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $rootProject -ComponentName BoundarySelfTest 2> $null 1> $null
    $negativeExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeExitCode -eq 0) {
        throw 'Boundary verifier accepted a transitive WinUI ProjectReference.'
    }

    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $importProject -ComponentName BoundarySelfTest 2> $null 1> $null
    $negativeImportExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeImportExitCode -eq 0) {
        throw 'Boundary verifier accepted a WinUI dependency from a local MSBuild import.'
    }

    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $nestedRootProject -ComponentName BoundarySelfTest 2> $null 1> $null
    $negativeNestedExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeNestedExitCode -eq 0) {
        throw 'Boundary verifier used the wrong MSBuildProjectDirectory for a transitive project.'
    }

    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $sourceRootProject -ComponentName BoundarySelfTest 2> $null 1> $null
    $negativeSourceExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeSourceExitCode -eq 0) {
        throw 'Boundary verifier skipped a variable-based source from a first-party import.'
    }

    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $wildcardProject -ComponentName BoundarySelfTest 2> $null 1> $null
    $negativeWildcardExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeWildcardExitCode -eq 0) {
        throw 'Boundary verifier accepted an unverifiable first-party wildcard import.'
    }
} finally {
    Remove-Item -LiteralPath $testDirectory -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'Boundary verifier self-test passed.'
