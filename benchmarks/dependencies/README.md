# Dependency evaluation probes

These isolated programs are the first stage of the rewrite's dependency evaluation.
The JSON library is now used by the product settings codec and string loader; CLI11
and spdlog remain probe-only. Product builds
continue to use the existing MSBuild solution; CMake is used only for these probes.

Run locally with PowerShell 7, CMake 4.2 or newer and Visual Studio 2026 with v145,
including ARM64 tools:

```powershell
pwsh -NoProfile -File benchmarks/dependencies/run.ps1
```

The script restores a pinned vcpkg manifest, builds separate Release executables
for x64 and ARM64 with C++26, `/W4 /WX` and no precompiled headers, and executes
the supported architectures. On x64, ARM64 is only cross-compiled. Each process
has a 15-second watchdog. Output stays outside the repository. Use a fresh
`-OutputRoot` for every measurement; dependency downloads and binary packages are
cached separately. No Actions, releases or publishing are involved.

The manifest fixes CLI11 2.6.2, nlohmann/json 3.12.0 (port revision 2), and spdlog
1.17.0 against vcpkg baseline `cb2981c4e03d421fa03b9bb5044cd1986180e7e4`.
spdlog's optional default features are disabled; it uses the standard formatter.
Third-party headers are confined to the individual `.cpp` files.

## What is measured

`measurements.json` records elapsed target build time, executable and ZIP sizes,
process output and exit status, and SHA-256 of the restored license text. Each
probe ZIP includes its dependency's complete vcpkg copyright file. Build times
include MSBuild startup, compilation and linking, exclude dependency restore,
and are single observations, not statistically stable benchmarks. The empty
baseline helps show executable overhead but is not the existing implementation.
ZIP sizes are isolated probe artifacts, **not product MSIX size deltas**.

- CLI11: UTF-8 option values and rejection of mutually exclusive options.
- JSON: Unicode round trip, malformed input and explicit type inspection.
- spdlog: file rotation, bounded queue overflow accounting and a private pool
  with a deliberately blocked sink. No global logger registry is used.

The blocked-sink probe reports whether shutdown remained unfinished during a
short observation window. This is diagnostic evidence, not a bounded-shutdown
guarantee or an exhaustive scheduling test. The sink is released before joining.

## Initial local result, 2026-09-19

MSVC 19.51.36248.0, CMake 4.2.3, Windows SDK 10.0.26100.0:

| Probe | x64 build ms | ARM64 build ms | x64 EXE bytes | ARM64 EXE bytes |
|---|---:|---:|---:|---:|
| baseline | 2148 | 1981 | 12288 | 13824 |
| CLI11 | 4196 | 3791 | 381440 | 386048 |
| JSON | 5108 | 5165 | 107520 | 116736 |
| spdlog | 4234 | 4192 | 391168 | 427008 |

All x64 processes exited successfully; ARM64 was not executed. spdlog reported
248 queue overruns and shutdown waiting for the blocked sink. The first run was
under the Windows temporary directory and emitted MSBuild warning MSB8029 about
incremental builds there. The script now defaults to a dedicated LocalAppData
directory. No compiler warnings occurred.

## Adoption gates still open

- Compare adapters against the actual existing implementations, including net
  production logic removed and complete product package size. The settings codec
  replaces the old Store parser/writer and adds stricter current-format validation;
  its total source size must not be confused with parser boilerplate alone.
- CLI11: all golden stdout, stderr and exit-code cases, Windows UTF-16 argument
  handling, quoting, aliases and help formatting.
- JSON: product codec supports only schema 2, with complete validation and whole-input
  rejection. The user explicitly removed all historical-format migration. Product
  package size and supply-chain acceptance remain part of the final verification.
- spdlog: the product owner, bounded shutdown during stalled I/O, crash-path
  independence, error handling and drop reporting. The raw pool destructor
  waits for its worker and is insufficient for the required shutdown contract.
- Full product builds for both architectures, runtime checks where hardware is
  available, distribution notices, vulnerability review and reproducible SBOM.

License sources: [CLI11 BSD-3-Clause](https://github.com/CLIUtils/CLI11),
[nlohmann/json MIT](https://github.com/nlohmann/json), and
[spdlog MIT and bundled notices](https://github.com/gabime/spdlog). The exact
restored license files, rather than these abbreviated labels, accompany every
generated probe package.
