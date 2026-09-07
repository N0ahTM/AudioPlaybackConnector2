# 0.9.0 candidate checklist

Status, 2026-09-07: release preparation authorized by the maintainer after a further day of use without problems. The intermittent audio-resume report remains open and must be disclosed in the release notes. This authorization accepts that known limitation; it does not convert missing test coverage into passing evidence. Publication still requires successful candidate builds, tests, and package checks. The local 0.8.1.x preview packages are not public release tags.

## Candidate identity

Freeze the final source revision and attach build/test logs and package hashes. The current development checkout contains uncommitted changes on top of 0478028387204920c7314bb64d29c4bf51ca8c4a; that base hash alone does not identify the candidate. Run the release dry-run workflow on the frozen revision before tagging v0.9.0.

The dry-run requires a matching changelog heading. The approved candidate now has a `[0.9.0]` section dated 2026-09-07, including known limitations. The local preview packages do not exercise the hosted signing secrets or the entire installer pipeline.

## Required evidence

- [ ] Restore, x64 Debug, x64 Release rebuild, ARM64 Release rebuild, and runnable native regression tests pass for the frozen revision.
- [ ] Formatting, cppcheck, dependency-boundary tests, and CI/CodeQL pass. An unchanged warning baseline is not a passing CI result.
- [ ] Verify x64 and ARM64 release packages, installers, framework dependencies, certificate, and App Installer feed using the release scripts.
- [ ] Upgrade from public 0.8.1 with existing settings; test a clean install on supported Windows versions. Updating a local 0.8.1.x preview is a separate smoke test.
- [ ] Check actual native UI in all eight languages, light/dark themes, 100/125/150/200% scaling, keyboard navigation, long device names, and multiple monitors.
- [ ] Verify hover and keyboard disconnect feedback, reset-icon alignment, fast repeated page changes, closing mid-animation, and disabled Windows animations.
- [ ] Exercise two physical A2DP sources: connect, disconnect, reconnect, bulk actions, incoming connections, manual retry cancellation, source power-off, adapter disable/enable, sleep/resume, and shutdown while busy.
- [ ] Run an extended audio session and record elapsed time, Windows/adapter/device versions, every unexpected loss, and diagnostics. No fixed test duration proves universal Bluetooth reliability.
- [ ] Extend the successful local update regression to Windows sign-out/shutdown and shutdown during busy Bluetooth operations.
- [ ] Record final resource/performance results and the remaining architecture migration exceptions. Do not describe the entire architecture rework as complete while transitional owners remain.
- [ ] Complete an independent final review and replace this checklist's pending statuses with links to actual evidence.

The detailed acceptance matrix is in [architecture verification](architecture-rework/verification.md). Missing hardware or visual coverage remains pending, rather than being inferred from a successful build.

## Release validation — 2026-09-07

Application/UI candidate 377f6507761913945758d82ee2088a467a5a37da passed fresh local restore, Debug x64, Release x64 and Release ARM64 rebuilds; both runnable x64 CoreTests configurations; formatting; cppcheck 2.19; localization/XAML checks; and the boundary-verifier self-test. GitHub Build and CodeQL also passed for this candidate.

The first hosted Release Dry Run built and verified the signed bundle and passed CoreTests, but failed while compiling the offline installer: its file list still required `*.msix` while the validated payload was a `.msixbundle`. The corrected builder passes the exact validated payload filename to Inno Setup. Local compiler checks cover offline bundle embedding, web mode and rejection of a missing payload. These narrow compiler probes are not installable release artifacts. The corrected hosted release pipeline must pass before publication.

The external README GIF is publicly accessible by an anonymous GET and renders on GitHub. Its SHA-256 matches the supplied recording: `46B53CEF9DF88BFAE241C991DDD890516B339B410C310A409379E20A03EEBF5D`.

## Local evidence — 2026-09-06

Local preview 0.8.1.9 includes the final fixes. Release x64 and ARM64 were rebuilt, then the final UI/shutdown changes were compiled in x64 Debug, ARM64 Release, and the final x64 package. All completed with zero compiler warnings/errors. Release and Debug CoreTests passed; cppcheck 2.19's CI invocation returned zero; formatting, localization/XAML checks, and positive/negative CoreRuntime boundary tests passed.

The original 15 cppcheck findings consisted of range-temporary diagnostics and copy recommendations. Snapshot lifetimes are now explicit, a synchronous bridge input is passed by const reference, and the two deliberately owning string conversions have narrowly documented suppressions. This is not a claim that 15 runtime defects were found.

A real package update exposed a settings-window close crash. The dump resolved to SettingsWindowPresenter::CloseWindow, which borrowed owner->Current while a synchronous Closed callback reset it. The fixed function retains its own shared state. Preview 0.8.1.8 was then upgraded to 0.8.1.9 with Settings open: installation, settings preservation, startup, Settings and picker commands passed; all 80 payload hashes matched; no new crashdump or previously observed cleanup error appeared in the new log interval. This is the regression scenario for the close-lifetime fix. Intermediate previews 0.8.1.6 and 0.8.1.7 must not be distributed.

The local artifacts include command logs, source hashes, a source patch, package/signature verification, and install/update JSON results. They do not establish hosted CI, public installer compatibility, a complete visual matrix, or long-duration Bluetooth stability.

## Open audio-resume investigation — 2026-09-06

The user reported intermittent silence after approximately five minutes without playback while the phone was locked. Both the phone and app still showed a connected device; manually reconnecting restored audio. A later five-minute attempt resumed normally. This successful retry does not resolve the intermittent report or identify whether the application, Windows audio/Bluetooth stack, or source device is responsible. Public stable publication remains pending this investigation.

Capture the next occurrence before reconnecting: timestamp, whether the phone's playback position advances, selected phone output, app diagnostics, and Windows output/session mute and level state. Connection status alone does not establish that audio is flowing. The local read-only audio probe can inspect the Windows output but cannot by itself attribute silence to Bluetooth. Its first snapshot had no connected device according to the app and is not a healthy Bluetooth baseline.

The currently installed preview is 0.8.1.11, including the Help button and the hover disconnect feedback confirmed by the user. The earlier 0.8.1.9 build evidence above must not be represented as a full validation matrix for this newer preview.

Live recurrence captured at approximately 11:22–11:25 local time on 2026-09-06: the user confirmed advancing playback and the PC selected as output. App status reported one connected device; the Windows default output was unmuted at 90%. Ten initial readings of the Audiosrv-hosted session had zero peak. Further readings across all three active render endpoints showed no appreciable signal. Pause/resume on the source did not restore sound. A single targeted reconnect through the installed app CLI succeeded, after which the same Audiosrv process/session showed nonzero peaks. No Windows audio service restart, volume change, app replacement, or other-device reconnect was performed. This establishes a measurable before/after difference, not the root cause or a permanent fix. Local JSON captures are in validation-logs/audio-failure-*.json; they are diagnostic artifacts, not release payloads.

The user confirmed that audible phone playback returned after the targeted reconnect. Recovery is verified for this occurrence; prevention remains unresolved.

## README GIF

The user supplied Animation2.gif: 510 × 490 pixels, 371 frames, 10.66 seconds, 2,244,206 bytes. Nine distributed frame samples were inspected and all frames decoded successfully. The recording shows the tray picker, device options, and Settings with neutral aliases. At the maintainer's request, the README now uses a GitHub attachment and the local source-tree GIF copy has been removed. Verify public attachment availability before publication. This recording is presentation material, not evidence that the full interaction or release test matrix passed.

The audio investigation, evidence limits, and next diagnostic steps are recorded in [AUDIO-RESUME-INVESTIGATION.md](AUDIO-RESUME-INVESTIGATION.md).

Record the real final packaged app after visual fixes have been checked. Do not use a mockup or an older build as evidence of the new interaction.

Suggested 12–18 second sequence, using non-sensitive device aliases:

1. Open the tray picker and connect a source.
2. Hover the connected row to show the disconnect action, without disconnecting yet.
3. Open **…**, show the compact device options, and return.
4. Open Settings, then Help, and return to the picker.
5. Disconnect through the device row.

For future recordings, capture only the app and the relevant tray area; exclude notifications, unrelated windows, and identifying device names. Keep the cursor visible, avoid repetitive loops or long pauses, and verify the exported GIF at the README display size.
