param([string]$MSBuildPath = 'msbuild')

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$targets = [Security.SecurityElement]::Escape((Join-Path $repository 'Directory.Build.targets'))
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('apc-build-version-' + [guid]::NewGuid().ToString('N') + '.proj')
try {
    [IO.File]::WriteAllText($fixture, "<Project><Import Project=`"$targets`" /></Project>")
    $cases = @(
        @{ Arguments = @('/p:ReleaseTag=v1.2.3'); Expected = '1.2.3.0' },
        @{ Arguments = @('/p:ReleaseTag=v65535.65535.65535'); Expected = '65535.65535.65535.0' },
        @{ Arguments = @('/p:ReleaseTag=v1.2.3', '/p:PackageVersion=1.2.3.0'); Expected = '1.2.3.0' },
        @{ Arguments = @('/p:ReleaseTag=v1.2.3', '/p:PackageVersion=1.2.4.0'); Failure = 'PackageVersion disagrees' },
        @{ Arguments = @('/p:ReleaseTag=v65536.0.0'); Failure = 'exceeds 65535' },
        @{ Arguments = @('/p:ReleaseTag=v01.2.3'); Failure = 'Release tag must be canonical' }
    )
    foreach ($case in $cases) {
        $arguments = @($fixture, '/t:ResolveBuildVersion', '/nologo', '/v:quiet', '/getProperty:PackageVersion') + $case.Arguments
        $output = (& $MSBuildPath @arguments 2>&1) -join "`n"
        $code = $LASTEXITCODE
        if ($case.ContainsKey('Expected')) {
            if ($code -ne 0 -or $output.Trim() -ne $case.Expected) { throw "Version evaluation failed: $output" }
        } elseif ($code -eq 0 -or $output -notlike "*$($case.Failure)*") {
            throw "Invalid build version did not fail for its intended reason: $output"
        }
    }
} finally { Remove-Item -LiteralPath $fixture -Force -ErrorAction SilentlyContinue }
Write-Host 'MSBuild tag derivation and inconsistent-version rejection passed.'
