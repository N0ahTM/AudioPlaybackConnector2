# Dependency evaluation probes

Historical isolated measurements, not the product acceptance record. Product JSON, CLI11 and spdlog integrations now exist; current ownership/testing contracts are in [INVARIANTS](../../docs/INVARIANTS.md). MSBuild builds the product; CMake only builds these probes.

## Run

PowerShell 7, CMake 4.2+, Visual Studio 2026/v145 including ARM64 tools:

```powershell
pwsh -NoProfile -File benchmarks/dependencies/run.ps1
```

Use a fresh `-OutputRoot` per measurement. Outputs default outside the repo under LocalAppData; dependency caches are separate. Each process has a 15-second watchdog. C++26, Release, `/W4 /WX`, no PCH; x64 executes locally, ARM64 only cross-compiles on x64. No publishing/Actions.

Manifest pins CLI11 2.6.2, nlohmann/json 3.12.0 port 2 and spdlog 1.17.0 against vcpkg baseline `cb2981c4e03d421fa03b9bb5044cd1986180e7e4`. spdlog defaults are disabled and std formatting used. Third-party includes stay in implementation files. Single-unit `spdlog_header_only`, `SPDLOG_WCHAR_FILENAMES` and `SPDLOG_DISABLE_DEFAULT_LOGGER` preserve native UTF-16 paths without mismatching the narrow-filename compiled library ABI or adding global state.

## Measurements and limits

`measurements.json`: target build time, EXE/ZIP size, output/exit, license SHA256. ZIPs include full restored copyright text. Times include MSBuild/compile/link, exclude restore, and are single observations. Empty baseline is not the previous product; probe ZIP deltas are not MSIX deltas.

- CLI11: Unicode, exclusivity and seven native/product parser comparisons. Native acceptance differs for `--id -- -device`, `--id=device`, `--id -device`, empty values and error codes/text. Product normalization/golden tests preserve the original contract.
- JSON: Unicode round trip, malformed input and types. Product schema 2 validates whole input; no old-format migration.
- spdlog: rotation, bounded overflow, private pool, blocked sink, Unicode filenames and UTF-8 readback. A blocked raw pool destructor waits; the owned preallocated Windows-work cleanup bounds caller wait while retaining pool/records/sink. Twenty x64 release-probe iterations verified timeout, eventual delivery and cleanup. This alone is not product shutdown proof.
- Product LoggerTests now cover real exclusive-oplock blocked rotation and owner destruction/release. Five native crash child paths verify actual dumps and emergency records after Logger destruction. See INVARIANTS for boundaries.

## Initial local result, 2026-09-19

MSVC 19.51.36248.0, CMake 4.2.3, SDK 10.0.26100.0; predates expanded probes/header-only integration:

| Probe | x64 build ms | ARM64 build ms | x64 EXE bytes | ARM64 EXE bytes |
|---|---:|---:|---:|---:|
| baseline | 2148 | 1981 | 12288 | 13824 |
| CLI11 | 4196 | 3791 | 381440 | 386048 |
| JSON | 5108 | 5165 | 107520 | 116736 |
| spdlog | 4234 | 4192 | 391168 | 427008 |

All x64 processes succeeded; ARM64 not executed. spdlog: 248 overruns and shutdown waiting for blocked sink. Initial TEMP output triggered MSB8029 (now LocalAppData); no compiler warnings.

## Product comparison and remaining acceptance

CLI integration matched 12,000 argument combinations against `f96349c`, seed 919054: acceptance, command/target/flags, UTF-16, diagnostics and exit codes. Historical parser existed only in a temporary comparison executable; product/tests link current CoreRuntime. x64/ARM64 builds passed without warnings. Measured MSIX sizes: 2,467,279 / 2,525,907 bytes, with exact notices including CLI11 BSD text. These are historical absolute sizes, not isolated dependency deltas.

Still require baseline-to-product comparisons of production logic, build time and package size; golden CLI/Windows quoting/help coverage; complete codec validation; logger shutdown/error/drop/crash contracts; both architectures, supported hardware runs, license/vulnerability/SBOM and final plan acceptance. Codec size includes stricter validation, not just parser replacement. Do not treat probe results as those gates.

License origins: [CLI11](https://github.com/CLIUtils/CLI11), [JSON](https://github.com/nlohmann/json), [spdlog](https://github.com/gabime/spdlog). Full restored licenses accompany probe packages; summaries do not replace them.
