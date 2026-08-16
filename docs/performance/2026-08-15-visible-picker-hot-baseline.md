# Visible picker Hot-residency benchmark — 2026-08-15

## Scope

This benchmark compares the cumulative single-process optimization branch with
the instrumented `develop` baseline while the device picker is visible. Both
variants are internally gated as `Hot` with initialized and loaded picker
resources. Windows reports `Busy` throughout the scenario.

The picker is opened through the packaged `apc2ctl show` control path before the
harness attaches. This measures steady visible-picker resource use; it does not
measure the time from command invocation to first presentation.

## Protocol

- 20 paired runs, alternating AB/BA order.
- Fresh package process and picker-open operation for every run.
- 5 seconds harness warm-up after the picker is already visible, followed by a
  30-second measurement.
- 1-second fixed sample interval; 31 samples per run.
- `Busy` QUNS state, Energy Saver off, and internal `Hot` residency required at
  all harness checkpoints.
- Loaded and initialized picker resources required immediately before and after
  sampling for both variants.
- Package identity, architecture, host, Windows build, power state, timing,
  executable hashes, and sample completeness validated by the strict comparator.
- Paired medians use 10,000 deterministic bootstrap iterations (seed 1729).

## Provenance

| Item | Value |
|---|---|
| Baseline source | `develop` `7b186629fc30ed2200d493df0c43c338c7648e15` plus diagnostic-only commits through `413fa5474e32cb0542da69ebf51b3a43eab16bd5` |
| Baseline package | `0.7.65004.0`, executable SHA-256 `212E7652A5B464C704EC8B0471E8850BA4089BDDCB10006858001849A950B834` |
| Baseline bundle SHA-256 | `5B765CFB4F57EB79281BE47EAD762E764A19319AAD446181AFCEEC8FFE1BA492` |
| Candidate source/package | `a875d29c8bc9cbbcc4bb77f7795b7b101fe37760`, `0.7.65003.0` |
| Candidate executable SHA-256 | `F1F1C941F544D815417ADA732394BC08E741D7BBF628706FCDD9C15CBD452DD7` |
| Candidate bundle SHA-256 | `554574D6299392C2EC915474DA10BFF9B739347D5F9958C6A9E14C4565D1D432` |
| Harness source | `71d2e541e6f7491ae8fa31b77224ca8da33d4993` |
| Comparison artifact SHA-256 | `8466074EEEF202245C58BCF08ECBD5FAF0922DDA6321695943B1BE61B0FE5D35` |

Both signed bundles use certificate thumbprint
`CFF0DF8009F8FF2F844F9B733571C1A1E577FB9C`.

## Results

Negative paired differences favor the candidate.

| Metric | Baseline median | Candidate median | Paired median difference | 95% bootstrap interval | Paired median % |
|---|---:|---:|---:|---:|---:|
| Private working set average | 56.89 MiB | 55.98 MiB | -0.73 MiB | [-1.11, -0.52] MiB | -1.29% |
| Private commit average | 75.24 MiB | 74.50 MiB | -0.70 MiB | [-0.97, -0.36] MiB | -0.93% |
| Thread count P50 | 29.5 | 26 | -3 | [-3.5, -2] | -10.00% |
| Handle count P50 | 783 | 785 | +2.5 | [0, +5] | +0.32% |
| CPU average, one-core normalized | 0% | 0% | 0 percentage points | [0, 0] | not claimable |

## Decision and limits

Keep the adaptive picker implementation: it does not trade the Cold-state memory
gain for a visible-Hot memory regression. In this scenario the candidate also
uses less private memory and fewer threads. The small positive handle difference
has a bootstrap interval touching zero and is not claimed as a stable regression.

No CPU or UI-latency claim is made. Separate measurement is still required for
first-open presentation latency, repeated open/close latency, normal desktop Hot
idle with `AcceptsNotifications`, Bluetooth hardware transitions, and long-run
energy behavior.
