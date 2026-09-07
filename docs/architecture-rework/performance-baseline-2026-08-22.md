# Performance Baseline — 2026-08-22

This is the before-rework snapshot for the current x64 release. It is a reproducible local baseline, not a claim about every computer. Later measurements must use the same scenario, sampling rules, and release configuration before they are compared with these values.

## Result summary

| Scenario | Working set median | Private commit median | CPU average | Threads median | Handles median |
|---|---:|---:|---:|---:|---:|
| Idle, Warm residency | 93.40 MiB | 76.61 MiB | 0.02% | 50 | 922 |
| Picker visible, Hot residency | 99.25 MiB | 81.77 MiB | 0.01% | 48 | 962 |
| Settings visible | 128.40 MiB | 100.16 MiB | 0.02% | 54 | 1,429 |

CPU is normalized to the whole 17-logical-processor machine. A value of 100% would mean all logical processors are fully occupied.

| User-visible operation | Runs | First run | Median | 95th percentile | Maximum |
|---|---:|---:|---:|---:|---:|
| Start process until named-pipe control is ready | 1 | 957.56 ms | — | — | — |
| Show picker until XAML `Opened` acknowledgement | 20 | 73.50 ms | 32.68 ms | 42.65 ms | 73.50 ms |
| Create settings until fully initialized and revealed | 10 | 71.09 ms | 56.02 ms | 69.48 ms | 71.09 ms |

The measured CLI latencies intentionally include control-process startup, named-pipe transport, dispatch, and the application acknowledgement. They are therefore conservative end-to-end figures for CLI users, not isolated rendering microbenchmarks.

## Resource distributions

Each resource scenario sampled the same application process once per second. Idle used 30 samples; visible UI scenarios used 15 samples each.

| Metric | Idle | Picker visible | Settings visible |
|---|---:|---:|---:|
| Working set average | 93.24 MiB | 99.32 MiB | 128.33 MiB |
| Working set p95 | 93.50 MiB | 99.53 MiB | 128.44 MiB |
| Private commit average | 76.60 MiB | 81.78 MiB | 99.92 MiB |
| Private commit p95 | 76.75 MiB | 81.84 MiB | 100.25 MiB |
| Whole-machine CPU median | 0.00% | 0.00% | 0.00% |
| Whole-machine CPU p95 | 0.05% | 0.05% | 0.09% |
| Handles p95 | 933 | 965 | 1,432 |
| Threads p95 | 52 | 50 | 54 |

Relative to the idle median, a visible picker added approximately 5.85 MiB working set and 5.16 MiB private commit. A visible settings window added approximately 35.00 MiB working set and 23.55 MiB private commit. These deltas are more useful for architecture comparisons than treating all process memory as feature-specific.

## What “ready” means

The picker command is acknowledged only after `TrayController` receives the flyout `Opened` event and increments `devicePickerOpenedGeneration`. The control request waits for that generation change. It does not merely measure how quickly the command was queued.

The settings window is created and activated off-screen. `RootGrid_Loaded` localizes and initializes the content, selects and renders the initial page, applies layout and theme, then calls `RevealAtTarget`. `SettingsWindowPresenter::Show` returns successfully only after activation has completed without initialization failure. Consequently, the named-pipe command duration measures the first visible window and complete initial content as one readiness boundary; there is no earlier intentionally blank visible phase to measure separately.

## Measurement identity

- Source commit: `d2a4587831e8d7155a2fe8b331a38f07d0b4c428`
- Source subject: `Unify x64 and ARM64 release pipeline`
- Configuration: `Release`, native `x64`, installed from the generated x64/ARM64 MSIX bundle
- Package: `N0ahTM.AudioPlaybackConnector2_0.8.1.0_x64__f4eqcdwtr7cg6`
- Bundle SHA-256: `7714E2AA9F07DCF386F338745A11C86C83EE5F9D4E3FF6C3C384A7877B05FE65`
- Installed application SHA-256: `3509E8C0B58ABBB9EE484CEA3D9EE4AD19CDE02EC790B8839FCB0F9BF7839F20`
- Installed control CLI SHA-256: `AED3B50BD6CB135357D58B7D5B6305A92AA093E0AAD78FF8AFEE4B110FF35E48`
- Build validation: solution Release rebuild completed with 0 warnings and 0 errors; all x64 core tests passed

## Test environment and state

- Windows 11 Pro, version `10.0.26200`, build `26200`
- Intel Core Ultra 9 185H, 17 logical processors
- 31.42 GiB physical memory
- Active Windows power scheme: Performance
- Battery: connected/charging state, 100%
- Connected playback devices during capture: 0
- Energy saver: off
- User activity state: available
- Reported memory pressure: high
- Resource state before UI interaction: Warm; picker resources not loaded or initialized
- Resource state after UI interaction: Hot; picker resources loaded and initialized

The high-memory-pressure state is important. Adaptive resource management deliberately kept the picker at Warm residency before interaction, so this capture exercises the resource policy that is explicitly retained by the rework.

## Procedure

The script [measure-performance-baseline.ps1](measure-performance-baseline.ps1) performs the following sequence:

1. Stop the existing app process and launch the installed package.
2. Measure time until `status --json` succeeds over the named pipe.
3. Wait 15 seconds, record adaptive state, then sample idle resources.
4. Open and close the picker 20 times. Each opening waits for the actual `Opened` acknowledgement.
5. Keep the picker visible and sample resources.
6. Create and close the settings window 10 times. Each iteration creates a fresh window; the CLI acknowledgement is the full-ready boundary.
7. Keep settings visible and sample resources.
8. Close settings, record final adaptive state, and leave the tray app running.

## Comparison rules for the rework

- Compare Release x64 builds on the same machine, power scheme, display configuration, and resource-pressure/residency state.
- Preserve the exact feature state: no connected devices, no settings edits, no active connect operation/Connecting animation, and no unrelated foreground workload.
- Use at least 20 latency runs for a formal before/after decision. The 10-run settings result here is a snapshot; rerun both versions with 20 or more iterations before claiming a regression or improvement.
- Report median and p95, not only the best run or arithmetic mean.
- Report working set and private commit separately. Neither is a substitute for the other.
- Treat startup-to-control-ready as a distinct metric from tray visibility and device-enumeration completion.
- Investigate sustained regressions larger than 10% or 10 MiB. Do not optimize against sub-millisecond or single-sample noise.
- Keep adaptive resource management enabled. A benchmark that disables the policy does not represent the shipping product.

## Known limits

- This capture does not include an active Bluetooth connection, reconnect traffic, toast creation, suspend/resume, an update check, or Stream Deck traffic. Those require separate scenario baselines if the corresponding module is rewritten.
- CPU sampling at one-second intervals describes steady-state cost, not short UI spikes. Use ETW/WPA when a latency regression must be attributed to a particular call path.
- Startup was captured once. It is an orientation value only; use repeated cold and warm launches for a release-performance claim.
- Working set is total resident process memory; private commit is committed private virtual memory. This script does not currently query private working set.
