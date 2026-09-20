# Contributing

## Prerequisites

Visual Studio 2026 18.6+ with MSVC 14.51/v145, Desktop development with C++, VS vcpkg integration and Windows SDK 10.0.26100.0+. WinUI builds additionally need Windows application development. Runtime: Windows 10 2004+ (Windows 11 recommended). Use a VS developer shell with PowerShell 7.

## Building

Headless restore/build/test:

```powershell
pwsh ./scripts/build-headless.ps1 -Platform x64
```

CoreRuntime concurrency/lifetime analysis:

```powershell
msbuild AudioPlaybackConnector2.CoreRuntime/AudioPlaybackConnector2.CoreRuntime.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:RunCodeAnalysis=true
```

Seeded suite-order stress with per-run and global watchdog:

```powershell
pwsh ./scripts/test/run-concurrency-stress.ps1 -Iterations 10 -Seed 42
```

Full solution restore:

```powershell
msbuild AudioPlaybackConnector2.slnx -t:restore -p:RestorePackagesConfig=true
```

Unpackaged development build:

```powershell
msbuild AudioPlaybackConnector2/AudioPlaybackConnector2.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:PreferredToolArchitecture=x64
```

Full native/package verification when required:

```powershell
foreach ($architecture in @('x64', 'ARM64')) {
    msbuild AudioPlaybackConnector2.slnx /p:Configuration=Release /p:Platform=$architecture /p:AppxBundle=Never /p:AppxBundlePlatforms=$architecture /p:AppxPackageSigningEnabled=false /t:Rebuild
}
```

Headless validates CoreRuntime/CoreTests/Control boundaries, restores/builds only those projects and runs tests without WinUI/XAML. `-Platform ARM64` cross-builds; run tests on ARM64 hardware. `-DisablePrecompiledHeaders` checks direct includes. Root vcpkg manifest restores automatically; architecture-specific install roots/triplets prevent cross-removal. No global `vcpkg integrate install` or Classic Mode needed.

Builds use C++26 draft `/std:c++latest` (`stdcpplatest`), not a fixed `/std:c++26` switch. Directory.Build.props/targets enforce common rules and MSVC 14.51+; do not silently downgrade projects.

Version comes from nearest reachable release tag (`git fetch --tags` if missing), or `/p:ReleaseTag=vX.Y.Z` for candidates/source archives. EXE/manifest derive from it; leave the manifest template unchanged.

`config/Concurrency.ruleset` treats MSVC findings as errors; PCH uses ordinary WIL defaults. CoreRuntime analysis does not replace full-app analysis or runtime race tests. Runner supports `--list`, `--suite SettingsStore`, `--seed 42`; seeds shuffle suites, not every thread interleaving. Stress logs retain seed, replay command, output and timing; deterministic scenarios remain necessary.

Failed headless restore/builds retain an MSBuild binlog at the reported private temporary path; successful logs are deleted. Imported project text is excluded (`ProjectImports=None`), but logs can still contain paths, environment values and command arguments. Inspect before sharing. The unsigned CI build uploads its failed binlog only when a manual **Build** run enables `upload_failure_binlog`, with one-day retention; routine runs do not upload it. Signing/publishing steps deliberately do not capture binlogs because certificate credentials reach MSBuild. Never include diagnostics in release assets.

## Build and launch notes

For DEP0840 naming WinAppRuntime.Main.2/Singleton, install the [Windows App SDK runtime](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/downloads#windows-app-sdk-20) and [Singleton](https://apps.microsoft.com/detail/9p5z076k079h).

Local MSIX uses a developer certificate. If signing thumbprint is unavailable, select/create a certificate in Package.appxmanifest > Packaging and update PackageCertificateThumbprint in the packaging project. Export/trust that certificate for another test machine; the public release certificate is unnecessary for local builds.

## Local checks

Choose checks using the matrix; these are the source/runner commands:

```powershell
$files = git ls-files '*.cpp' '*.h' '*.hpp' '*.cxx' '*.cc' '*.ixx'
clang-format --dry-run --Werror $files

$files = git ls-files 'AudioPlaybackConnector2/src/*.cpp' 'AudioPlaybackConnector2/ui/*.cpp'
cppcheck `
  --enable=warning,performance,portability `
  --std=c++20 `
  --language=c++ `
  --platform=win64 `
  --inline-suppr `
  -IAudioPlaybackConnector2/include `
  -IAudioPlaybackConnector2/res `
  -IAudioPlaybackConnector2/ui `
  -IAudioPlaybackConnector2/ui/App `
  -IAudioPlaybackConnector2/ui/MainWindow `
  -IAudioPlaybackConnector2/ui/DevicePickerView `
  -IAudioPlaybackConnector2/ui/SettingsWindow `
  --error-exitcode=1 `
  --template=gcc `
  $files

.\x64\Release\tests\AudioPlaybackConnector2.CoreTests.exe

pwsh ./scripts/validate-localizations.ps1
pwsh ./scripts/validate-markdown-links.ps1
pwsh ./scripts/update-cli-reference.ps1 -Check
```

For the packaged UI, use a disposable x64 Windows test account with a branch package installed, Application Verifier, the x64 Windows SDK debugger (`cdb.exe`), and an elevated PowerShell 7 session:

```powershell
./scripts/test/run-appverifier.ps1 -PackageFullName '<installed test package full name>' -Iterations 3
```

This runner enables [AppVerifier Basics](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/application-verifier-testing-applications) (including full-page heap, handles and locks), launches the installed executable under CDB, exercises CLI status/picker/settings, and closes the app through WM_CLOSE. Each iteration has a watchdog. Existing app processes/debugger settings and published 0.9.1 packages are rejected; settings created by the runner are removed in `finally`. Failure logs stay in the reported temporary directory; successful diagnostics are removed. Bluetooth, resume, Explorer restart and interactive toast activation still require the separate hardware/UI acceptance matrix. After interrupting the PowerShell process itself, inspect the image's AppVerifier settings before another launch.

Runner isolates LOCALAPPDATA per process and retains real logger output there. Tests inject platform boundaries, not link-time replacements. Pipe fixtures admit their own process; Release keeps production trust checks, including real-pipe rejection of unpackaged peers. Debug additionally allows exact executable-file matches; Release does not inherit that allowance.

## How to Contribute

Branch from main, make a cohesive change, run relevant checks and open a PR against main. Report versions/reproduction/redacted diagnostics with the [bug template](.github/ISSUE_TEMPLATE/bug_report.md). Discuss early ideas in [Discussions](https://github.com/N0ahTM/AudioPlaybackConnector2/discussions); use the [feature template](.github/ISSUE_TEMPLATE/feature_request.md) for concrete proposals.

| Change | Required checks |
| --- | --- |
| Markdown | Links and documented commands; no unrelated product rebuild |
| Existing translation | Localization validator, placeholders and affected UI; see [Translating](docs/TRANSLATING.md) |
| Policy/core | Formatting, affected CoreTests, x64 CoreRuntime |
| Threading/device/settings/control | Full CoreTests, x64 Release, static analysis |
| Windows/architecture boundary | Above plus ARM64 compile |
| Concurrency | Owner race/shutdown tests, seeded stress/watchdog, callback/wait-under-lock audit |
| Security boundary | Relevant negative tests and CodeQL |
| Package/version/release | Complete x64/ARM64 package and release validation; distribution changes also require shared EXE hashes, separate channels, feed/signature/monotonicity and installation checks |

CI/final acceptance remain required. Cross-compilation is not ARM64 execution. Repeat passed checks only for relevant changes/failures/unresolved concerns. Small docs/translation changes need no unrelated package rebuild.

## Code Style

- `/W4 /WX`; include declarations directly, with PCH only a cache.
- English localization resources are canonical; preserve placeholders/fallback. See [Translating](docs/TRANSLATING.md).
- Explicit mutable/async owners and cancellation; no references beyond lock/snapshot lifetime or unchanged global context forwarded through layers. See [Architecture](docs/ARCHITECTURE.md) and [Invariants](docs/INVARIANTS.md).
- Prefer supported standard facilities; abstractions require a real responsibility, invariant, lifetime or platform/test boundary.
- Comments explain why/ownership/invariants. Keep three-line C++ section banners consistent (Types, Constructors, Public Interface, Helpers, Member Variables; domain sections where useful):

```cpp
/*------------------------------------------------------------------------------------------------------------*/
/*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/
```

User-visible changes go under Unreleased; release preparation creates dated sections. See [Releasing](docs/RELEASING.md).

## License

Contributions use the [MIT License](LICENSE).

## Dependency and supply-chain checks

**Licenses:** after restore, `python scripts/verify-dependency-licenses.py --triplet x64-windows-static-md` (or `arm64-windows-static-md`). Reviewed `config/dependency-licenses.json` covers manifest pins, restored product/build dependencies, normalized copyright hashes and full notices. New dependencies/features/versions/license text require review, not blind hash regeneration. Offline checks: `python scripts/test-dependency-licenses.py`. vcpkg coverage does not replace NuGet notices, vulnerability review or SBOM.

**SBOM:** `pwsh scripts/release/generate-sbom.ps1 -DropPath <staging-directory> -Tag vX.Y.Z` after assembling a fresh drop. Python 3.9+ on PATH and both architecture restores required; `-Triplets x64-windows-static-md` only for an explicitly x64-only local drop. Wrapper verifies config/sbom-tool.json pin, includes actual NuGet/vcpkg metadata, creates SPDX 2.2 and `_manifest/AudioPlaybackConnector2_SBOM.zip`. `python scripts/release/verify-sbom.py --drop <staging-directory>` verifies complete file/hash/external-reference coverage offline.

**Attestations:** pinned release Action attests SBOM and provenance. Before promotion, scripts/release/verify-build-attestations.ps1 requires local coverage, provenance for every staged file and package/feed SBOM attestations bound to repo/tag commit/reusable workflow. Promotion verifier checks archive asset hashes. scripts/test-build-attestations.ps1 and scripts/test-release-promotion.ps1 mock GitHub; they do not verify real signed attestations. Local SBOM generation neither attests nor publishes.

**Updates:** monthly Dependabot for Actions, three NuGet manifests and vcpkg registry; at most one version-update PR per ecosystem, no automerge. Review overrides/licenses/notices after baseline updates. Read-only Dependency Review rejects reported vulnerabilities in all PR dependency scopes; explicit license inventory handles missing Microsoft license metadata. GitHub graph omits vcpkg advisories; SHA-pinned Actions lack SemVer advisory alerts.

**Vulnerabilities:** after x64 restore, `python scripts/check-dependency-vulnerabilities.py --report <report.json>` validates licenses, resolves the three product libraries' upstream commits from vcpkg SPDX and queries OSV for those and exact project NuGet versions. Only public identifiers go to GitHub/OSV. Query failures/incomplete responses/any advisory fail; timestamped JSON records queries/results. No match proves neither coverage nor absence. Build-helper ports and pinned Actions need upstream review. Offline rejection tests: `python scripts/test-dependency-vulnerabilities.py`.
