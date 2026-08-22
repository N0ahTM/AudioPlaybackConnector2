# Verification

MUST run relevant automated checks and focused manual scenarios before deleting replaced code. Final acceptance covers all supported builds and packaged behavior. MUST NOT report a result that was not run.

## Build gate

At minimum run:

```powershell
msbuild AudioPlaybackConnector2.slnx -t:restore -p:RestorePackagesConfig=true
msbuild AudioPlaybackConnector2.slnx /p:Configuration=Debug /p:Platform=x64
msbuild AudioPlaybackConnector2.slnx /p:Configuration=Release /p:Platform=x64 /t:Rebuild
clang-format --dry-run --Werror AudioPlaybackConnector2/src/**/*.cpp AudioPlaybackConnector2/include/**/*.hpp
cppcheck --enable=warning,performance,portability --std=c++20 --platform=win64 AudioPlaybackConnector2/src/
```

Run the built core tests. Final acceptance also requires every supported architecture/package and package-verification script. Update paths after source relocation.

P10 gate: all product/test projects use the same mode; MSVC, Windows SDK, C++/WinRT, dependencies, tests, packaging, x64, and ARM64 pass before declaring C++26 baseline.

## Automated matrices

| Area | Required cases |
|---|---|
| P06 devices | Watcher start/stop/restart/stale callback; connect/disconnect/reconnect success/failure/cancel/duplicate/overlap/bulk; close barrier timeout; stale completion; loss/retry/exhaust/recovery; manual cancellation; global/per-device policy; incoming; startup; suspend/resume; shutdown busy. |
| P07 settings | Current/legacy/missing/malformed/oversized/partial/corrupt data; bounds/all mutations; change during write; failure/retry; atomic replace/temp cleanup; suspend/shutdown flush; no lost revision; privacy/alias/default/last/per-device persistence. |
| P02 resources | Initial/Cold/Warm/Hot; pressure and delays; energy/fullscreen; visible/pinned/interaction hold; release deferral; authorization/freshness/heartbeat/out-of-order/clock rollback; scheduler fallback; preload/release failure retry; shutdown; diagnostics. |
| P01 control | Every command/argument; text/JSON/raw/privacy/redaction; exits; framing/malformed/bounds; trusted/untrusted identities; correlation/replay/cache; concurrency/recycling; deadline before dispatch and after mutation; indeterminate result; cancel/shutdown/start retry; baseline compatibility. |
| App | UI/CLI use-case parity; event→snapshot; settings side effects; default/last toggle; notification policy P04/P05; refresh coalescing retains final state. |
| Services | Latest-wins startup; approved G04; update cancellation/shutdown; toast escaping/actions/P05; P08 diagnostics; approved G01/G02. |

Tests MUST be deterministic where practical. Every fixed race gets a regression test. Security/protocol changes require positive and negative cases.

## Manual desktop matrices

Run on the oldest supported Windows environment and current Windows 11 when available.

| Area | Required scenarios |
|---|---|
| Tray/picker | First/second launch; Explorer restart; left/right/double click; discovery changes while open; P03 animation start/progress/stop for success/error/cancel/shutdown with no orphan timer; all tray states; light/dark; 100/125/150/200% DPI; multi-monitor placement; P02 preload/release/pin. |
| Bluetooth | At least two A2DP devices; picker/tray/CLI commands; bulk and rapid actions; power-off during connect/connected; adapter disable/enable; incoming; startup/loss retry; sleep/resume; shutdown busy. |
| Notifications | Every retained kind; P05 image; actions; P08 localization/redaction; P04 no toast/balloon. |
| Settings | Every page/control; aliases/policies reflected in picker/CLI; live language/privacy; startup; approved updates; diagnostics; retained placement; corrupt recovery; restart persistence. |
| Package | Packaged app/CLI; frameworks; startup identity; toast activation; approved update handoff; upgrade from last release with settings; documented uninstall/reinstall. |

## Performance

Compare Release builds under the same conditions. Use [baseline procedure](performance-baseline-2026-08-22.md) where applicable.

- Startup to tray registration and control ready.
- First and preloaded picker open.
- Private working set/private commit in Cold/Warm/Hot and with Settings open.
- Idle CPU/wakeups/polling; handles/threads after repeated picker/CLI use.
- UI/CLI connect latency; settings write count/duration; shutdown idle/busy.
- P01 server/cache memory bounds under repeated requests.
- MUST NOT disable or regress P02 to improve a benchmark.

## Security

- P01 ACL is limited to intended identities; client verifies server; server rejects unexpected client.
- Deadlines/cancellation cannot cause an unauthorized or unreported late mutation.
- Correlation/replay cannot expose another caller's response.
- Length/count limits prevent unbounded allocation.
- P08 redacts every external presentation; diagnostics/logs stay bounded/local.
- Update destinations follow trusted release policy.

## Final gate

- [ ] P01 compatibility/security pass.
- [ ] P02 parity pass.
- [ ] P03 Connecting animation and every start/stop/cleanup path pass.
- [ ] P04 never notifies.
- [ ] P05 mapping passes.
- [ ] P06 race/lifecycle matrix passes.
- [ ] P07 is atomic and loses no revision.
- [ ] P08 localization/privacy/accessibility pass.
- [ ] P09 retained behavior passes.
- [ ] Supported Debug/Release/architectures/packages pass.
- [ ] No obsolete owner, duplicate pipeline, or migration adapter remains.
- [ ] Dependency checks and final documentation match shipping code.
- [ ] Final performance is recorded and explained.
