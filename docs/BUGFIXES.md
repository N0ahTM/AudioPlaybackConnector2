# Bug tracking – Unreleased

Technical working list, not a claim about defects in release 0.9.1 or fully verified historical fixes. IDs 01–50 correspond to the contributed collection; new findings get sequential IDs. Public-facing changes live in the [Changelog](../CHANGELOG.md).

## Status and maintenance

- **Open:** confirmed, unfixed defect.
- **In progress:** analysis/implementation underway.
- **Verification pending:** reported finding/fix not yet verified against current code.
- **Closed:** current code path, fix commit and a matching test/check command with a recorded result.

A commit alone is not closure. Regressions reopen the same ID; fixed entries stay for release mapping. Simplifications are not proven defects.

## Settings, shutdown and persistence

| ID | Reported finding / change | Type | Status | Fix reference |
| --- | --- | --- | --- | --- |
| 01 | Settings shutdown: bound unlimited waits on file I/O, subscribers or the worker. | Defect | Closed | `0a38f31` |
| 02 | Secure worker lifetime after a shutdown timeout without later unbounded waits. | Defect | Closed | `0a38f31` |
| 03 | A wake-up immediately before waiting must not be lost. | Defect | Closed | `076959d` |
| 04 | Publish settings snapshots as immutable revisions; copy outside locks. | Simplification | Closed | `255f337` |
| 05 | Block mutation and flush when preserving corrupt settings fails. | Defect | Closed | `d98def2` |
| 06 | Discard stale reconnect configuration on competing settings changes. | Defect | Closed | `ff91b89` |
| 07 | Shutdown from a callback must not wait on itself; separate stop request from drain. | Defect | Closed | `cf4b829` |
| 08 | Run the autostart persistence callback outside the request lock. | Defect | Closed | `22e91ac` |
| 09 | Keep late device events away from the UI after shutdown. | Defect | Closed | `0a133c5` |
| 10 | Migrate the unversioned 0.9.1 settings format into schema 2; preserve other incompatible files. | Upgrade regression | Verification pending | 0.9.2 preparation |
| 11 | Reject invalid device IDs and aliases before calling the backend. | Defect | Closed | `7ae26a0` |

Evidence 01–11 (through 2026-09-23): SettingsStoreTests cover blocked I/O/subscriber budgets
(TestShutdownBudgetFencesBlockedLoad/-LateWriteCompletion/-IncludesBlockedSubscriber/-RetainsBlockedWriterLifetime,
TestFrozenPersistenceClockCannotExtendShutdownBudget, TestConcurrentShutdownCallerHasItsOwnBudget),
wake versioning (TestManualDebounceAndIdleWait, ManualSettingsWakeup), blocked subscribers
(TestSubscriberInitiatedShutdownDoesNotDeadlock, TestCallbackCanDestroyStoreDuringPublicationDrain,
TestResetFencesBlockedCallback), format and validation rules (TestUnsupportedFormatsArePreserved,
Test091SettingsMigration,
TestDeviceIdValidationRejectsWithoutRevision, TestValidationAndLoadNormalizationMatrix) and
policy discarding (TestSettingsPolicyRejectsOldDuplicateAndCancelledRevisions). Full CoreTests run
and ten stress runs with seeds 920100–920109 green (%TEMP%/apc-concurrency-19b10717fba2451d874f895ca31d9236).

## Devices, events and UI

| ID | Reported finding / change | Type | Status | Fix reference |
| --- | --- | --- | --- | --- |
| 12 | Record completion of device operations durably per operation epoch. | Defect | Closed | `c5f53da` |
| 13 | Deliver subscribers in order and account for running callbacks on unsubscribe. | Defect | Closed | `bc57e2f` |
| 14 | Close the gap between snapshot and subscription; publish settings in the controller event stream. | Defect | Closed | `f98889b` |
| 15 | Late error/reconnect events must not overwrite newer session state. | Defect | Closed | `29d0016` |
| 16 | Disconnecting must not appear as connecting. | Defect | Closed | `29d0016` |
| 17 | Remove the second device-event history; prevent stale UI delivery. | Simplification / defect | Closed | `9ee8212` |
| 18 | Remove the self-wait path in picker preload and resource diagnostics access. | Defect | Closed | `0bc9119` |
| 19 | Do not keep showing removed devices as inactive session rows in the picker. | Defect | Closed | `4095a84` |
| 20 | Still allow resetting the default device when it no longer exists. | Defect | Closed | `4095a84` |
| 21 | Render tray icon, tooltip, privacy and device status from the same snapshot. | Defect | Closed | `0bc9119` |
| 22 | Stored tray callbacks must not reach a released controller. | Defect | Closed | `7c4d45d`, `4250f31` |
| 23 | Remove the bridge, central in-process command dispatch and redundant forwarding layers. | Simplification | Closed | `0a133c5`, `a3af825`, `ff91b89`, `9cd18e1` |

Evidence 12–23 (2026-09-21): operation epochs (TestOperationEpochRejectsStaleCompletion,
TestCompletionRetainsFirstTerminalResult), ordered delivery (TestSubscriptionsAreOrderedAndReentrant,
TestConcurrentAndReentrantPublicationsHaveOneOrder), the snapshot/subscription gap
(TestSnapshotRegistrationReconcilesChangesDuringCapture, TestSnapshotRegistrationDoesNotReceiveOlderQueuedDelivery),
normalized facts (TestFactsCarryNormalizedSnapshots, TestFailureFactsRetainOperationKind),
device removal and default reset (TestDeviceRemovalClosesCurrentSessionAndRejectsLateCallbacks,
TestTargetResolutionAndDefaultModes, TestQueriesReconcileAllProjectedFieldsAfterOwnerChanges),
snapshot authority and lifetimes (INVARIANTS "Device snapshot authority"/"Controller event delivery",
weak controller access in TrayController). CoreTests and stress runs green (evidence as 01–11).

## Logging and crash handler

| ID | Reported finding / change | Type | Status | Fix reference |
| --- | --- | --- | --- | --- |
| 24 | Bound logger shutdown on blocked file I/O; keep in-flight resources alive. | Defect | Closed | `61fae1b`, `b176b12` |
| 25 | Remove global logger access; make late writes after shutdown inert. | Defect | Closed | `b176b12` |
| 26 | Handle native UTF-16 log paths with umlauts reliably. | Defect | Closed | `61fae1b` |
| 27 | Keep the crash path independent of the async logger, queue and normal logger locks. | Robustness | Closed | `b176b12` |
| 28 | Unregister the crash handler safely and restore previous handlers. | Defect | Closed | `b176b12` |
| 29 | Fix the character offset when reading persisted error codes. | Defect | Closed | `b176b12` |
| 30 | Truncate long crash log lines without invalid UTF-8 tail sequences. | Defect | Closed | `b176b12` |
| 31 | Remove redundant locks in the UI-bound tray path. | Simplification | Closed | `0bc9119`, `774646c`, `eac2535`, `edf28de` |

Evidence 24–31 (2026-09-21): LoggerTests (rotation, blocked exclusive lock, owner release,
Unicode paths, emergency buffer after logger release), CrashHandlerTests with five native
crash child processes (real dumps and emergency records after logger destruction), INVARIANTS
"Logging ownership and native crash registration". CoreTests and stress runs green (evidence as 01–11).

## Theme and language

| ID | Reported finding / change | Type | Status | Fix reference |
| --- | --- | --- | --- | --- |
| 32 | Replace the global theme handler list with direct UI delivery. | Simplification | Closed | `ed73d1f` |
| 33 | Provide language resources through an explicit owner instead of global access. | Robustness | Closed | `d6b3f82` |
| 34 | Swap resources before updating translated window texts. | Defect | Closed | `d6b3f82` |

Evidence 32–34 (2026-09-21): StringResources suite (eight languages, English fallback, parallel
publication and lifetime), theme ownership in the UI, INVARIANTS "Tray theme delivery"/"Localization ownership",
localization gate 0b8fb00 (complete identical key sets, validate/test-localizations green).

## CLI, pipe and security

| ID | Reported finding / change | Type | Status | Fix reference |
| --- | --- | --- | --- | --- |
| 35 | CLI11 migration: preserve acceptance, `--id -- -device`, error texts and exit codes. | Rewrite regression | Closed | `a4b10da` |
| 36 | Verify replay identity and prevent unpackaged clients from starting the packaged app. | Security boundary | Closed | `376ee51` |
| 37 | Manage pipe, event and threadpool resources with clear WIL ownership. | Simplification | Closed | `637ce50` |
| 38 | Protect the active pipe cache entry between result selection and delivery ownership from eviction. | Defect | Closed | `0afb169` |
| 39 | Replace pipe test hooks with real transport/timer boundaries. | Test quality | Closed | `41672c4`; continued in `65d282e`, `63d8c1c`; see 50 |
| 40 | A test macro must not let unpackaged clients bypass the real release policy. | Test quality / security boundary | Closed | `dab37c4` |
| 41 | Remove the global mutable identity/package status cache. | Simplification | Closed | `309f237` |

Evidence 35–41 (2026-09-21): CLI golden matrix and CliParser suites (acceptance/normalization/exit codes,
`update-cli-reference.ps1 -Check` green), CommandPipeSecurity suite (DACL, rogue instance, trust predicates,
`eabbf0f`), server suite with production trust rejection of unpackaged clients, wire contract consolidated in
CommandProtocol.hpp (`5ac59f9`), auto matching through one contract (`cc79707`), bounded test host without
policy weakening. CoreTests and stress runs green (evidence as 01–11).

## Build, tests and CI

| ID | Reported finding / change | Type | Status | Fix reference |
| --- | --- | --- | --- | --- |
| 42 | Resolve the C++/WinRT dependency cycle in parallel builds. | Build defect | Closed | `6527a7e` |
| 43 | Prevent app and package builds from writing the same PCH concurrently. | Build defect | Closed | `ca480d0` |
| 44 | The boundary verifier must account for project-specific include/PCH contexts. | Check defect | Closed | `e770ada` |
| 45 | Architecture restores must not remove each other's vcpkg packages. | Build defect | Closed | `a150a05` |
| 46 | Build tests and the app with the same pinned C++/WinRT projection. | Test quality | Closed | `12ebacf` |
| 47 | Preserve the apartment lifetime in the test runner across all WinRT-using suites. | Test crash | Closed | `e9b5320` |
| 48 | Passing negative tests must not leave a failing exit code behind. | Check defect | Closed | `03b5e9c` |
| 49 | Fix the release metadata check for LF/CRLF and multiple PropertyGroups. | Check defect | Closed | `6f64e89`; version contract later changed in `b15365c` |

Evidence 42–49 (2026-09-21): parallel full builds (`msbuild AudioPlaybackConnector2.slnx -m -t:Rebuild`)
for x64 and ARM64 each with 0 warnings/0 errors (%TEMP%/apc-final-parallel-{x64,arm64}.log) — 42/43 thus
proven beyond serial runs; boundary self-test green (%TEMP%/gate-boundary.log); isolated triplet restores
for x64/ARM64 back to back without package loss; same pinned package versions for app and tests;
RuntimeApartment in the runner; `test-release-metadata.ps1` and `test-build-version.ps1` green.

| 50 | Remove the separate pipe test compilation and test layout; use production code from CoreRuntime. | Test quality | Closed | `63d8c1c`, duplicate-compilation guard `103ed63` |

### Acceptance note for 50

The last separate server compilation and `APC_COMMAND_PIPE_SERVER_TESTING` were removed. For all
46 production files under `AudioPlaybackConnector2/src`, exactly one compiling project was confirmed.
The pipe suite with seed 919076 and three full test runs with seeds 919077–919079 passed; x64/ARM64
built without warnings or errors. ARM64 is a cross-build, not an executed ARM64 test run.

Reproducible checks:

```powershell
pwsh ./scripts/test-core-runtime-boundary.ps1 -VerifierPath ./scripts/verify-core-runtime-boundary.ps1
pwsh ./scripts/verify-core-runtime-boundary.ps1 -ProjectPath ./AudioPlaybackConnector2.CoreTests/AudioPlaybackConnector2.CoreTests.vcxproj
./x64/Release/tests/AudioPlaybackConnector2.CoreTests.exe --suite CommandLineControlServer --seed 919076
```

This acceptance covers the shared pipe implementation and the declared source assignment. It does not
claim a complete release acceptance or a review of every earlier test hook from entry 39.

## New findings

| ID | Finding | Type | Status | Evidence / next step |
| --- | --- | --- | --- | --- |
| 51 | MSVC analysis failed with the PCH-specific WIL diagnostics level 1 on SAL/template declarations. | Analysis/build defect | Closed | PCH special definition removed; SAL/template parser errors cleared in the rerun. x64/ARM64 full builds without warnings/errors each. |
| 52 | Reentrantly queued Connect/Disconnect/Reconnect/CancelReconnect calls returned an already-moved, empty device ID. | Rewrite regression | Closed | `902e767`; test `TestReentrantCommandsRetainDeviceIdentity`: four failures before the fix, passing after (seed 920001). Full CoreTests run with seed 920002 and targeted Cppcheck run passed; x64/ARM64 full builds without warnings/errors. |
| 53 | Dispatch dereferenced an empty selector optional when the wire grammar accepted a payload that the stricter selector validation rejected (control characters). | Defect | Closed | `38c841d`; MSVC lifetime rule C26830 flagged the site; target commands without a valid selector are now rejected with InvalidInput. Adapter and server suites green in the full CoreTests run. |
| 54 | DeviceWatcher stored the started registration before publishing; a Publish exception left it running without stop/revoke while the cleanup tail saw only the moved-from local. | Defect | Closed | `38c841d`; MSVC lifetime rule C26800 flagged the moved-from use; ownership now returns to the local on failure and the tail stops/revokes it. DeviceWatcher suite green. |
| 55 | The PCH-off build broke when the AudioConnectionService fold removed the header that transitively supplied winrt/Windows.Foundation.h and winrt/Windows.Media.Audio.h to DeviceSession.cpp. | Build defect | Closed | `7eefd21`; direct includes restored, PCH-off headless x64/ARM64 green (%TEMP%/apc-pchoff-devicesession2.log, apc-pchoff-arm64-final.log). CI Headless (PCH disabled) job green in run 35687337473. |
| 56 | test-build-version.ps1 left a nonzero $LASTEXITCODE from the last intentionally failing fixture, failing the CI quality job despite passing. | Check defect | Closed | `e47a9e2`; the script now resets the exit code after the negative fixtures; quality job green in run 35687337473. |
| 57 | The CI clang-tidy path used the MSBuild task's default clang-analyzer-* checks, failing on generated C++/WinRT header noise, and printed no findings to the workflow log. | Analysis/check defect | Closed | `3dbaa30`; the script pins the nine configured checks, scopes WarningsAsErrors to them, suppresses compiler-frontend noise from generated headers and prints deduplicated findings on failure. Full 22.1.7 scan exit 0 (%TEMP%/apc-clang-tidy-2217q.log); CI Headless tidy step green in run 35687337473. |

The MSVC analysis with the concurrency/lifetime ruleset has **passed** since `38c841d` (x64,
exit 0, %TEMP%/apc-msvc-analysis3.log). Earlier runs showed 1,913 and later 6,372 findings; triage found
three real first-party issues (empty optional in dispatch, ownership transfer on Publish exception in
DeviceWatcher, iterator/pointer forms in codec and server) and one rule (C26823) that fires thousands of
times inside the pinned WIL implementation. C26823 is disabled in the ruleset with a documented rationale;
NuGet/vcpkg headers are marked external (`/analyze:external-` when analysis is enabled). No blanket
suppressions were added.

## Open release acceptance

These tasks are not additionally claimed product defects:

- **Done (2026-09-21):** historical entries 01–49 verified against current code and closed with evidence.
- **Done (2026-09-21):** static analysis green — MSVC lifetime/concurrency exit 0 (`38c841d`), clang-tidy 0 findings in first-party sources (`1720dec`, full CI-toolchain scan `3dbaa30`), Cppcheck 2.20.0 clean.
- **Done (2026-09-21):** local package/supply-chain evidence — parallel x64/ARM64 full builds, package verification for both architectures, notices, SBOM, licenses, OSV (15 queries, 0 advisories).
- **Done (2026-09-22):** final GitHub branch CI and CodeQL — Build run 35687337473 and CodeQL run 35564074704 both green after fixes 7eefd21, 3dbaa30, e47a9e2 and f587f15.
- **Documented instead of executed:** native WinUI, installation and hardware checks are not executable within the agreed boundaries (interactive UI session, certificate trust, ARM64 hardware, Store/feed publication).

There is currently no newly confirmed, untreated product defect from this list. "Verification pending"
explicitly does not mean that correctness or a fix has been proven. The actual release version and the
public summary are assigned only during release preparation.
