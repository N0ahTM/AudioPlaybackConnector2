param(
    [string]$Executable = '',
    [ValidateRange(1, 10000)]
    [int]$Iterations = 10,
    [uint32]$Seed = 1,
    [ValidateRange(1, 3600)]
    [int]$RunTimeoutSeconds = 60,
    [ValidateRange(1, 43200)]
    [int]$TotalTimeoutSeconds = 600,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $repository 'x64/Release/tests/AudioPlaybackConnector2.CoreTests.exe'
}
$binary = (Resolve-Path -LiteralPath $Executable).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path ([IO.Path]::GetTempPath()) ('apc-concurrency-' + [guid]::NewGuid().ToString('N'))
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$results = [System.Collections.Generic.List[object]]::new()
$total = [Diagnostics.Stopwatch]::StartNew()
Write-Host "Stress logs: $outputRoot"
for ($iteration = 0; $iteration -lt $Iterations; ++$iteration) {
    $remaining = [long]$TotalTimeoutSeconds * 1000 - $total.ElapsedMilliseconds
    if ($remaining -le 0) { throw "Global stress watchdog expired. Results: $outputRoot" }
    $currentSeed = [uint32](([uint64]$Seed + [uint64]$iteration) % 4294967296)
    $stdout = Join-Path $outputRoot "$currentSeed.stdout.log"
    $stderr = Join-Path $outputRoot "$currentSeed.stderr.log"
    Write-Host "CoreTests seed=$currentSeed ($($iteration + 1)/$Iterations)"
    $run = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $binary -ArgumentList @('--seed', [string]$currentSeed) `
        -WorkingDirectory $repository -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    try {
        $budget = [int][Math]::Min([long]$RunTimeoutSeconds * 1000, $remaining)
        $completed = $process.WaitForExit($budget)
        if (-not $completed) {
            $process.Kill($true)
            if (-not $process.WaitForExit(5000)) { throw "Timed-out test process $($process.Id) did not terminate." }
        } else {
            $process.WaitForExit()
        }
        $results.Add([pscustomobject]@{
            Seed = $currentSeed
            TimedOut = -not $completed
            ExitCode = $process.ExitCode
            ElapsedMilliseconds = $run.ElapsedMilliseconds
            Stdout = $stdout
            Stderr = $stderr
        })
        $results | ConvertTo-Json -AsArray | Set-Content -LiteralPath (Join-Path $outputRoot 'results.json') -Encoding utf8
        if (-not $completed -or $process.ExitCode -ne 0) {
            throw "CoreTests failed at seed $currentSeed. Replay: & '$binary' --seed $currentSeed. Logs: $outputRoot"
        }
    } finally {
        $process.Dispose()
    }
}
Write-Host "All $Iterations seeded suite-order stress runs passed. Results: $outputRoot"
