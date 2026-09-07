# Intermittent silent audio after a pause

Status, 2026-09-06: observed and recovered; root cause and prevention remain open. This is a release-candidate investigation, not evidence of a confirmed Windows or application defect.

## Observed occurrence

The installed preview was 0.8.1.11 on Windows 11 Pro, build 26200, with Intel Wireless Bluetooth driver 24.40.10.3. The user reported silence after leaving a locked phone without playback for approximately five minutes. Another attempt of similar length succeeded.

During the captured recurrence at 11:22–11:25 local time:

- The user confirmed that playback time advanced and the PC remained selected as the phone's output.
- The app's cached snapshot reported one connected device. UI resource residency was Hot; energy saver was off.
- The default Windows render endpoint was unmuted at 90% volume. The Audiosrv-hosted audio session was active and unmuted at full session volume, but all ten initial peak samples were zero.
- Measurements of all three active render endpoints showed no appreciable signal. Small readings on the default endpoint were associated with Discord. Process-level session attribution does not prove which Bluetooth stream a session represents.
- Pausing and resuming playback did not restore audio.
- One targeted reconnect through the installed app CLI succeeded. Ten subsequent samples from the Audiosrv-hosted session ranged from 0.00575063 to 0.0100352. The user confirmed audible phone playback had returned.

No audio-service restart, driver change, volume adjustment, or application replacement was made. Measurements read levels and state; they did not record audio. Local evidence is retained under `validation-logs/audio-failure-*.json`, outside release artifacts. The probe's endpoint indices identify collection order only, not persistent endpoint identity.

## Code comparison with v0.8.1

`AudioConnectionService.cpp` is unchanged. Both versions enable the sink with `StartAsync` and use `OpenAsync` for an outgoing connection. The old `DeviceManager` and current `DeviceSession` use Windows connection-state notifications; neither inspected connection implementation measures audio delivery or defines a five-minute silence timeout.

The current session retains its connection object as a member. The inspected adaptive-resource action in `ApplicationHost` preloads/releases the device picker; it does not close Bluetooth connections. These observations do not rule out every lifecycle race or establish that the old release reproduces the symptom.

Microsoft documents [Opened and Closed as connection states](https://learn.microsoft.com/en-us/uwp/api/windows.media.audio.audioplaybackconnectionstate?view=winrt-26100), and describes the [StartAsync/OpenAsync lifecycle](https://learn.microsoft.com/en-in/windows/uwp/audio-video-camera/enable-remote-audio-playback). These API states do not supply a per-device audio-delivery measurement.

## What is still missing

The app snapshot was read, but the native connection object's live `State` was not queried independently during the failure. Therefore a stale application state cannot yet be distinguished from a genuinely open but stalled native audio path. The measurements also cannot distinguish source-side streaming failure from the Windows Bluetooth/decoder/render path. The enabled Bluetooth operational event channels found locally contained no records; this is not a trace of the failure.

Next diagnostic steps:

1. Record the source player and try the same pause/resume sequence with another player, changing no other variable.
2. Add bounded diagnostic evidence of native StateChanged/Open completion and the live native state, preserving operation epochs and privacy, before another reproduction. Capture an actual Windows Bluetooth/audio trace if the native connection remains open while audio stalls.
3. Compare the same phone, player, driver, output and pause/lock sequence with public 0.8.1. Keep candidate settings and installation recoverable. A single successful old-version trial is not proof of a regression; repeated comparable observations are needed.

Do not add silence-triggered reconnects or background noise as an unverified fix. Legitimate silence would trigger such behavior, and a Windows output peak does not identify which source should be producing sound. The existing explicit reconnect is a verified recovery action for this occurrence.

Release decision, 2026-09-07: after reporting a further day without problems, the maintainer authorized release preparation with this issue still open. Keep the symptom and reconnect workaround visible in the changelog and README. This decision does not establish a root cause, a permanent fix, or universal Bluetooth reliability.
