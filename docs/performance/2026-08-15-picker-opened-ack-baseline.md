# First picker `Flyout.Opened` acknowledgement benchmark — 2026-08-15

This benchmark compares the instrumented `develop` baseline with the cumulative
single-process optimization branch at the first picker open of a fresh packaged
process. The endpoint is the matching WinUI `Flyout.Opened` callback. It is not a
claim about compositor presentation or pixels reaching the display.

## Protocol

- 20 paired Release/x64 runs per variant, ordered AB/BA alternately.
- Every run installs the signed variant, launches a fresh process, waits for the
  control server, records presentation generation zero, and invokes `show --json`.
- The command returns only after the matching `Flyout.Opened` handler advances the
  monotone presentation generation. A follow-up status query verifies the advance.
- Every accepted run remained in QUNS `NotPresent`, on the same AC power scheme,
  and outside Energy Saver from pre-launch through acknowledgement.
- The metric begins immediately before control-client process creation, so it
  intentionally includes control-client startup and IPC.
- No outlier was removed. Both variants produced occasional 400–620 ms values.

The two variants carry the same small measurement boundary:

- baseline: develop plus instrumentation commit `1bce780`, package `0.7.65005.0`
- candidate: application commit `b78acda`, packaging commit `7926a9b`, package
  `0.7.65006.0`

Both bundles were signed by certificate thumbprint
`CFF0DF8009F8FF2F844F9B733571C1A1E577FB9C`.

## Result

| Metric | Baseline | Candidate | Paired candidate − baseline |
|---|---:|---:|---:|
| Median acknowledgement | 154.910 ms | 146.913 ms | -4.331 ms |
| MAD | 26.982 ms | 11.101 ms | 27.550 ms |
| Runs above 400 ms | 7 / 20 | 3 / 20 | — |

The deterministic 10,000-iteration bootstrap 95% interval for the paired median
is **[-152.500, +16.338] ms**. It crosses zero, so this series does not establish a
picker-open latency improvement or regression. The point estimate is about 4.3 ms
faster, but the correct conclusion is that first-open acknowledgement remained
within the noisy host distribution while the previously recorded memory and
thread reductions were retained.

Startup-to-control-ready medians were effectively identical (890.193 ms baseline,
890.260 ms candidate), which provides a useful sanity check but is not a complete
startup-performance measurement.

## Provenance

- raw runs: `artifacts/perf/picker-open-ack-20x/` (ignored by Git)
- comparison artifact:
  `artifacts/perf/comparisons/20260816T021243.434Z-picker-open-ack-baseline-vs-candidate.json`
- comparison SHA-256:
  `90F70FAC646703219C662FF2F5565FC0F9BEA04EA8B238489092CCED5BD20355`
- baseline bundle SHA-256:
  `077A9484F5FFCDE2D24CA89A167539F525DC60AE4AC37A397FC7D40DD87ADF5A`
- candidate bundle SHA-256:
  `30569E6C70D216A2E2BE9FB4B0ED5DEC100AEE3875BC5F92FFC47673D00D865D`

For screen-presented latency, collect ETW/PresentMon traces with a deterministic
visual trigger. This control-to-framework metric must not be relabeled as that
stronger measurement.
