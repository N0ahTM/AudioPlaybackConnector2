[CmdletBinding()]
param(
    [string]$MSBuildPath = 'msbuild',
    [string]$ToolPath = 'clang-tidy',
    [ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
    [string]$LogPath = (Join-Path ([IO.Path]::GetTempPath()) ('apc-clang-tidy-' + [guid]::NewGuid().ToString('N') + '.log'))
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tool = (Get-Command $ToolPath -ErrorAction Stop).Source
$version = (& $tool --version | Out-String)
if ($LASTEXITCODE -ne 0 -or $version -notmatch 'LLVM version 22\.1\.7(?:\s|$)') {
    throw 'Use LLVM 22.1.7, matching the CI analysis toolchain.'
}
$log = [IO.Path]::GetFullPath($LogPath)
New-Item -ItemType Directory -Path (Split-Path $log) -Force | Out-Null
$project = Join-Path $root 'AudioPlaybackConnector2.CoreRuntime/AudioPlaybackConnector2.CoreRuntime.vcxproj'
$config = Join-Path $root '.clang-tidy'
# The MSBuild task defaults to clang-analyzer-* when no check list is given; pin the
# configured set so the analysis matches .clang-tidy instead of generated-header noise.
# MSBuild command-line properties must escape the leading '-' (%2D) and ',' (%2C).
$checks = '%2D*%2Cbugprone-use-after-move%2Cbugprone-dangling-handle%2Cbugprone-infinite-loop' +
    '%2Cbugprone-suspicious-semicolon%2Cbugprone-suspicious-string-compare%2Cbugprone-undelegated-constructor' +
    '%2Cperformance-unnecessary-copy-initialization%2Cperformance-unnecessary-value-param' +
    '%2Creadability-redundant-smartptr-get'
# Visual Studio exports its actual compiler arguments and strips incompatible binary PCH usage.
# Restore/build CoreRuntime first so generated C++/WinRT headers are available.
& $MSBuildPath $project '/t:PrepareForBuild;ResolveReferences;ClangTidy' `
    /p:Configuration=Release "/p:Platform=$Platform" `
    "/p:ClangTidyToolPath=$(Split-Path $tool)" "/p:ClangTidyToolExe=$([IO.Path]::GetFileName($tool))" `
    "/p:ClangTidyToolExeAdditionalOptions=--config-file=$config --extra-arg=-Wno-nontrivial-memcall --extra-arg=-Wno-missing-field-initializers --extra-arg=-Wno-missing-designated-field-initializers" `
    "/p:ClangTidyChecks=$checks" `
    /p:MaxNumberOfProcesses=2 /v:minimal /nologo *> $log
$result = $LASTEXITCODE
Write-Output "clang-tidy exit=$result; log=$log"
if ($result -ne 0) {
    # Print the deduplicated findings; the runner-local log path is useless otherwise.
    $findings = Select-String -Path $log -Pattern 'error:' |
        ForEach-Object { ($_.Line -replace '\s*\[[^\]]*\.vcxproj\]\s*$', '') } | Sort-Object -Unique
    foreach ($finding in $findings) { Write-Output $finding }
    throw 'clang-tidy did not pass; inspect the printed findings and the local log.'
}
