# Adaptive cold-residency benchmark — 2026-08-15

## Scope

This benchmark compares the cumulative single-process optimization branch with
the `develop` baseline while Windows reports `Busy`. The baseline always keeps
the device-picker XAML tree preloaded (`Hot`); the candidate's adaptive policy
releases it (`Cold`). This is a policy-treatment comparison, not an attribution
of the result to one individual commit.

Both packages expose the same read-only residency status schema. The baseline
instrumentation adds one atomic diagnostic mirror and JSON serialization only;
it does not change picker residency or preload behavior.

## Protocol

- 20 paired runs, alternating AB/BA order.
- 15 seconds warm-up and 30 seconds measurement per run.
- 1-second fixed sample interval; 31 samples per run.
- `Busy` QUNS state and Energy Saver off required before launch, before
  measurement, and after measurement.
- Internal status required `Hot` plus loaded/initialized picker resources for
  every baseline run, and `Cold` plus unloaded picker resources for every
  candidate run.
- Package identity, x64 architecture, host, Windows build, power scheme, AC
  source, timing configuration, executable hashes, and sample completeness were
  validated by `Compare-ResourceUsage.ps1`.
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
| Harness source | `f9c187ff09444a954e7bc30d8f3d030b9d99f8fb` |
| Comparison artifact SHA-256 | `DC39554ABDEAD2C5B5B2C138C67E4B8C2132577D1C65D75F0EFD9AF01BC8FA1D` |

The signed bundles used the expected certificate thumbprint
`CFF0DF8009F8FF2F844F9B733571C1A1E577FB9C`.

## Results

Negative paired differences favor the candidate.

| Metric | Baseline median | Candidate median | Paired median difference | 95% bootstrap interval | Paired median % |
|---|---:|---:|---:|---:|---:|
| Private working set average | 48.15 MiB | 47.12 MiB | -0.98 MiB | [-1.04, -0.90] MiB | -2.04% |
| Private commit average | 66.32 MiB | 65.36 MiB | -0.97 MiB | [-1.02, -0.85] MiB | -1.47% |
| Thread count P50 | 27 | 24 | -3 | [-4, -2.5] | -11.11% |
| Handle count P50 | 723 | 730.5 | +8 | [+4.5, +9] | +1.11% |
| CPU average, one-core normalized | 0% | 0% | 0 percentage points | [0, 0] | not claimable |

Three baseline runs showed roughly 4 MiB lower private working set and commit
than its normal band. They passed all gates and remain in the paired sample; they
were not removed as outliers. The robust median and interval still favor the
candidate for private memory.

## Decision and limits

Keep the adaptive Cold residency: the private-memory and thread reductions are
stable across the gated paired distribution. The candidate also retains eight
more handles at the median, so the result is not an across-the-board resource
reduction and future lifecycle work should investigate that delta.

No idle-CPU claim is made because 30-second runs rounded the primary CPU medians
to zero. This test also does not establish `AcceptsNotifications`/Hot parity,
first-open latency, low-memory response latency, Bluetooth hardware behavior, or
long-duration energy use. Those require separate gated scenarios and ETW/UI
latency instrumentation.
