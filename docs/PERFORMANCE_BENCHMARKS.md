# Performance benchmarks

`tools/Measure-ResourceUsage.ps1` records a repeatable resource baseline for the
installed, packaged application. It launches the package through its application
user model ID, waits for a configurable warm-up period, samples the exact packaged
executable, and normally stops only the process it launched.

Build and install the Release MSIX before collecting results. Close an already
running instance so startup and warm-up measurements remain comparable. Then run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Measure-ResourceUsage.ps1 `
    -ExpectedPackageVersion 0.7.0.0 `
    -Scenario idle-baseline `
    -WarmupSeconds 10 `
    -DurationSeconds 30 `
    -SampleIntervalMilliseconds 1000
```

Results are written atomically as JSON and CSV below `artifacts/perf/`. That
directory is ignored by Git. The JSON includes package, executable, process, OS,
PowerShell, and Git metadata so measurements from different builds or hosts are
not accidentally compared as equivalent.

Use `-AttachToExisting` only for scenarios that cannot begin with a clean process.
An attached process is never stopped by the harness. Use `-LeaveRunning` when a
newly launched process must remain alive after a run.

## Metrics

- `WorkingSetBytes`: all physical pages currently resident for the process.
- `PrivateWorkingSetBytes`: resident pages unique to the process.
- `SharedWorkingSetEstimateBytes`: working set minus private working set; this is
  an estimate, not memory that can be attributed exclusively to the application.
- `PrivateCommitBytes`: committed private virtual memory.
- `CpuDeltaMilliseconds`: process CPU time consumed since the preceding sample.
- `CpuPercentOneCore`: interval CPU time relative to one logical processor.
- `CpuPercentMachine`: interval CPU time normalized across all logical processors.
- `ThreadCount` and `HandleCount`: point-in-time process resource counts.

The first scheduled sample is taken immediately after warm-up and the final sample
at the requested duration. Percentiles use linear interpolation over sorted sample
values. Prefer private working set and private commit when judging application
cost; the total working set includes shareable framework and system image pages.

## Comparison protocol

For an A/B comparison, keep the installed package architecture, Windows build,
power mode, foreground state, warm-up, duration, and sample interval identical.
Run each short scenario at least 20 times, alternate A and B runs where practical,
and compare distributions rather than a single minimum. Useful scenarios include
idle, first picker open, repeated picker open, settings open/close, update due/not
due, suspend/resume, and command-line control stress.

This harness intentionally does not create artificial memory pressure, manipulate
the process working set, or run destructive low-memory experiments on the host.
Perform pressure and long-running fault-injection scenarios only in a disposable
test VM with separately reviewed tooling.
