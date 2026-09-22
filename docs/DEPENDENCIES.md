# Dependency acceptance record

Durable before/after evidence for the CLI11, nlohmann/json and spdlog adoptions. The temporary probe
workspace (`benchmarks/dependencies`) was removed after this record was captured; rebuild probes from
the measurements here if a new evaluation is ever needed.

## Decisions and pins

| Area | Library | Pinned version | Contract kept in product code |
| --- | --- | --- | --- |
| CLI parsing | CLI11 | 2.6.2 | Value/escape normalization, UTF-16, ordered validation, diagnostic text and exit codes |
| Settings codec | nlohmann/json | 3.12.0 (port 2) | Whole-input schema-2 validation, typed limits, no legacy migration |
| Logging | spdlog | 1.17.0 | Single worker in a private pool, wchar filenames, default logger disabled, bounded shutdown |

Restore pins live in `vcpkg.json`/`vcpkg-configuration.json` against vcpkg baseline
`cb2981c4e03d421fa03b9bb5044cd1986180e7e4`. License and notice gates: `config/dependency-licenses.json`,
`scripts/verify-dependency-licenses.py`, and the SBOM gate (`scripts/test-sbom.py`).

## Production logic before/after

Reproducible without checkout via `py scripts/measure-source.py --baseline f5404c597080f8e2d892e6b9787fee38bbb17113 --revision HEAD`;
the recorded report is [source-measurements.json](source-measurements.json).

| Metric | Baseline f5404c5 (v0.9.1+23) | Current 68a2596 | Delta |
| --- | ---: | ---: | ---: |
| Files | 136 | 128 | −8 |
| Physical lines | 26,239 | 22,657 | −3,582 |
| Nonblank lines | 23,297 | 20,103 | −3,194 |
| Bytes | 1,140,260 | 1,004,830 | −135,430 |

Line counts measure physical source volume, not behavioral complexity; the adopted libraries also carry
new validation that the old code lacked.

### Attribution of the delta

The total delta spans the whole branch, not just the library adoptions. Measured per commit with the same
script (product sources only, no tests):

| Change | Commit | Lines | Files |
| --- | --- | ---: | ---: |
| nlohmann/json codec (stricter validation included) | `a150a05` | +7 | +2 |
| CLI11 parser (product grammar/diagnostics stay) | `a4b10da` | −55 | 0 |
| spdlog logging (includes the new crash/emergency path) | `b176b12` | +100 | +1 |
| In-app updater removed | `ff28e54` | −1,222 | −8 |
| Architecture simplification (bridge, forwarding layers, dead code) | rest of branch | ≈ −2,412 | — |
| **Total** | f5404c5 → 68a2596 | **−3,582** | −8 |

The libraries are line-neutral on purpose: their value is replacing hand-rolled correctness-critical
infrastructure (parsing edge cases, Unicode, async log queueing) with maintained upstream code, while
product policy, validation and shutdown rules deliberately remain first-party. The size reduction comes
from the updater removal and the architecture simplification.

## Isolated probe measurements (2026-09-19)

MSVC 19.51.36248.0, CMake 4.2.3, SDK 10.0.26100.0, C++26 draft, Release, `/W4 /WX`, no PCH.
Single observations; times include compile+link, exclude restore. ARM64 was cross-compiled, not executed.

| Probe | x64 build ms | ARM64 build ms | x64 EXE bytes | ARM64 EXE bytes |
| --- | ---: | ---: | ---: | ---: |
| baseline (empty) | 2,148 | 1,981 | 12,288 | 13,824 |
| CLI11 | 4,196 | 3,791 | 381,440 | 386,048 |
| JSON | 5,108 | 5,165 | 107,520 | 116,736 |
| spdlog | 4,234 | 4,192 | 391,168 | 427,008 |

Probe deltas are isolated per-library costs, not product MSIX deltas.

## Package size

- Before: public v0.9.1 bundle `AudioPlaybackConnector2_0.9.1.0_x64_ARM64.msixbundle` = 4,212,381 bytes
  (GitHub release asset, both architectures in one bundle, pre-rewrite).
- After: per-architecture MSIX 2,467,279 bytes (x64) / 2,525,907 bytes (ARM64), measured from the
  branch packaging verifier with exact notices including the CLI11 BSD text.
- The bundle aggregates and compresses both architectures, so bundle-vs-single-MSIX is indicative, not exact.

## Behavior evidence

- CLI: 12,000 argument combinations compared against the historical parser at f96349c, seed 919054 —
  acceptance, command/target/flags, UTF-16, diagnostics and exit codes. Product golden tests pin the contract.
- JSON: Unicode round trips, malformed input and type rejection through the schema-2 codec tests.
- spdlog: rotation, bounded overflow, private pool, blocked sink, Unicode filenames and UTF-8 readback;
  twenty x64 release-probe iterations verified timeout, eventual delivery and cleanup. Product LoggerTests
  cover real exclusive-oplock blocked rotation and owner destruction; five native crash child paths verify
  actual dumps and emergency records after Logger destruction. See [INVARIANTS](INVARIANTS.md) for the
  shutdown and ownership contracts.

## Limits

Single-observation timings; probe deltas are not product deltas; ARM64 probes were cross-compiled only;
no Store/hardware installation is implied by this record.
