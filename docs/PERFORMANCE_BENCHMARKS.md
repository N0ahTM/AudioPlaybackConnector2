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
    -ExpectedExecutableSha256 <64-character-installed-executable-hash> `
    -ExpectedUserNotificationState AcceptsNotifications `
    -ExpectedAdaptiveResidency Hot `
    -RequireEnergySaverOff `
    -Scenario available-hot-idle `
    -Variant candidate `
    -PairId r01 `
    -WarmupSeconds 35 `
    -DurationSeconds 120 `
    -SampleIntervalMilliseconds 1000
```

Results are written atomically as JSON and CSV below `artifacts/perf/`. That
directory is ignored by Git. The JSON includes the installed package full name,
publisher, architecture, executable and manifest SHA-256 hashes, process, OS,
PowerShell, and host-state metadata. `Source.GitCommit` identifies only the
worktree containing the benchmark harness; the installed executable hash is the
authoritative build identity.

Calculate the expected installed hash after installing each A/B variant:

```powershell
$package = Get-AppxPackage -Name N0ahTM.AudioPlaybackConnector2
$exe = Join-Path $package.InstallLocation AudioPlaybackConnector2.exe
(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
```

The harness checks the executable and package manifest again after sampling and
rejects a run if either changed. With `-ExpectedUserNotificationState`, it also
requires the same Windows notification state before launch, after warm-up, and
after measurement. Accepted names are `NotPresent`, `Busy`,
`RunningD3dFullScreen`, `PresentationMode`, `AcceptsNotifications`, `QuietTime`,
and `App`. Use `AcceptsNotifications` for normal desktop/hot measurements and the
matching fullscreen or presentation state for cold-policy measurements. Use
`-RequireEnergySaverOff` whenever Energy Saver is not the scenario under test.
Even when no notification state is requested, the observed state, active power
scheme, and AC/DC source must remain unchanged across those three checkpoints or
the run is rejected.

For adaptive-resource measurements, `-ExpectedAdaptiveResidency Cold|Warm|Hot`
queries the installed app immediately before and after sampling. The run is
rejected unless the internal policy remains in that residency. Hot additionally
requires initialized and loaded picker resources; Cold requires them to be
released. Use this gate for release claims instead of inferring residency from
QUNS and warm-up duration alone.

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
- `ScheduleLatenessMilliseconds`: how late the sample was relative to its fixed
  monotonic schedule. Large values invalidate high-frequency comparisons.
- `CpuTotalDeltaMilliseconds`, `CpuAveragePercentOneCore`, and
  `CpuAveragePercentMachine` in the JSON summary aggregate CPU over the complete
  measured duration instead of treating correlated interval percentages as
  independent observations.

The first scheduled sample is taken immediately after warm-up and the final sample
at the requested duration. Percentiles use linear interpolation over sorted sample
values. Private working set and private commit come directly from
`GetProcessMemoryInfo`; the harness does not perform a per-sample CIM query that
would distort the schedule. Prefer those private metrics when judging application
cost; the total working set includes shareable framework and system image pages.

## Comparison protocol

For an A/B comparison, keep the installed package architecture, Windows build,
power mode, notification state, foreground state, warm-up, duration, and sample
interval identical. Run each scenario at least 20 times per variant as alternating
AB/BA pairs, and reduce every run to one summary value before comparing paired
distributions. Samples within one run are time-correlated and are not independent
runs. Useful scenarios include idle, first picker open, repeated picker open,
settings open/close, update due/not due, suspend/resume, and command-line control
stress.

Give both members of an A/B pair the same `-PairId` and distinct stable
`-Variant` names. After collecting the series, run the strict paired comparison:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\Compare-ResourceUsage.ps1 `
    -InputDirectory .\artifacts\perf\available-hot `
    -Scenario available-hot-idle `
    -BaselineVariant develop `
    -CandidateVariant head `
    -MinimumPairs 20 `
    -BootstrapIterations 10000
```

The comparator rejects missing or duplicate pairs, dirty harness worktrees,
unstable executable hashes, mismatched package identity/architecture, differing
host or measurement configuration, changed QUNS/power state, incomplete sample
counts, and excessive schedule lateness. It reports paired median differences,
median absolute deviation, and a deterministic 95% bootstrap interval for the
paired median. Negative differences mean the candidate used fewer resources.
Adaptive-residency requirements must be stable within each variant, but may
differ between variants when the policy itself is the treatment under test. For
example, an always-preloaded baseline is gated as Hot while an adaptive candidate
is gated as Cold under the same fullscreen/presentation environment; both values
are retained in the comparison artifact and report.
Use `-AllowDirtyHarness` or `-AllowSameExecutable` only for explicit harness/control
experiments, not release claims.

The adaptive picker needs about 20 seconds to reach Hot after a healthy desktop
snapshot, so use at least 35 seconds of warm-up for
`AcceptsNotifications`/Hot steady state. Fullscreen or presentation pressure moves
to Cold after its grace period; use at least 15 seconds there. Do not mix those
states into a single idle average.

If the installed package is an app bundle, update it with the signed
`.msixbundle`; installing only its nested `.msix` cannot replace the bundle.

This harness intentionally does not create artificial memory pressure, manipulate
the process working set, or run destructive low-memory experiments on the host.
Perform pressure and long-running fault-injection scenarios only in a disposable
test VM with separately reviewed tooling.

## Recorded results

- [Adaptive Cold residency baseline — 2026-08-15](performance/2026-08-15-adaptive-cold-baseline.md)
- [Visible picker Hot-residency baseline — 2026-08-15](performance/2026-08-15-visible-picker-hot-baseline.md)
