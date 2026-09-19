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
        -ProjectPath $cleanProject *> $null
    if ($LASTEXITCODE -ne 0) {
        throw 'Boundary verifier rejected a clean project.'
    }

    $previousErrorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $rootProject 2> $null 1> $null
    $negativeExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeExitCode -eq 0) {
        throw 'Boundary verifier accepted a transitive WinUI ProjectReference.'
    }

    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $importProject 2> $null 1> $null
    $negativeImportExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeImportExitCode -eq 0) {
        throw 'Boundary verifier accepted a WinUI dependency from a local MSBuild import.'
    }

    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $nestedRootProject 2> $null 1> $null
    $negativeNestedExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeNestedExitCode -eq 0) {
        throw 'Boundary verifier used the wrong MSBuildProjectDirectory for a transitive project.'
    }

    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $sourceRootProject 2> $null 1> $null
    $negativeSourceExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeSourceExitCode -eq 0) {
        throw 'Boundary verifier skipped a variable-based source from a first-party import.'
    }

    $ErrorActionPreference = 'Continue'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier `
        -ProjectPath $wildcardProject 2> $null 1> $null
    $negativeWildcardExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeWildcardExitCode -eq 0) {
        throw 'Boundary verifier accepted an unverifiable first-party wildcard import.'
    }

    $includeProject = Join-Path $testDirectory 'IncludeContext.vcxproj'
    $nativeHeaders = Join-Path $testDirectory 'NativeHeaders'
    $guiHeaders = Join-Path $testDirectory 'AudioPlaybackConnector2/include'
    New-Item -ItemType Directory -Path $nativeHeaders, $guiHeaders -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $testDirectory 'include-context.cpp') -Value '#include <pch.h>'
    Set-Content -LiteralPath (Join-Path $nativeHeaders 'pch.h') -Value '#include <cstdint>'
    Set-Content -LiteralPath (Join-Path $guiHeaders 'pch.h') -Value '#include <winrt/Microsoft.UI.Xaml.h>'
    Set-Content -LiteralPath $includeProject -Value @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup><ClCompile>
    <AdditionalIncludeDirectories>$(ProjectDir)NativeHeaders;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
  </ClCompile></ItemDefinitionGroup>
  <ItemGroup><ClCompile Include="$(MSBuildProjectDirectory)\include-context.cpp" /></ItemGroup>
</Project>
'@
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier -ProjectPath $includeProject *> $null
    if ($LASTEXITCODE -ne 0) {
        throw 'Boundary verifier ignored the declaring project include path or misresolved an absolute source path.'
    }
    Set-Content -LiteralPath (Join-Path $nativeHeaders 'pch.h') -Value '#include <winrt/Microsoft.UI.Xaml.h>'
    $ErrorActionPreference = 'Continue'
    $includeFailure = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $resolvedVerifier -ProjectPath $includeProject 2>&1
    $negativeIncludeExitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorPreference
    if ($negativeIncludeExitCode -eq 0 -or ($includeFailure -join ' ') -notmatch 'forbidden\s+XAML\s+dependency') {
        throw "Boundary verifier failed to report a forbidden include (exit $negativeIncludeExitCode): $($includeFailure -join ' ')"
    }
} finally {
    $resolvedTestDirectory = [System.IO.Path]::GetFullPath($testDirectory)
    $temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\')
    if ([System.IO.Path]::GetDirectoryName($resolvedTestDirectory) -ne $temporaryRoot -or
        [System.IO.Path]::GetFileName($resolvedTestDirectory) -notlike 'apc-boundary-*') {
        throw 'Refusing to remove a test directory outside its temporary root.'
    }
    Remove-Item -LiteralPath $testDirectory -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'Boundary verifier self-test passed.'
exit 0
