# Troubleshooting

## Windows reports an untrusted publisher

GitHub releases use a self-signed certificate. Download `AudioPlaybackConnector2.cer` only from the [official release page](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest) and import it into `Cert:\CurrentUser\TrustedPeople`. If Windows rejects current-user trust with `0x800B0109`, an administrator may import the same verified certificate into `Cert:\LocalMachine\TrustedPeople`.

Never import the certificate into **Trusted Root Certification Authorities**, and never trust a copy obtained from another website. The Microsoft Store package does not need this step because Microsoft signs it after certification.

## A framework package is missing

Prefer the Microsoft Store or the `.appinstaller`; both resolve the applicable framework packages. A direct `.msixbundle` installation requires the matching x64 or ARM64 packages from the same GitHub release's `Dependencies` assets.

If Windows mentions `MicrosoftCorporationII.WinAppRuntime.Main.2` or `MicrosoftCorporationII.WinAppRuntime.Singleton`, the official [Windows App SDK downloads](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads) can repair the runtime. The release's pinned dependency packages remain the reproducible choice for manual installation.

## The `ms-appinstaller:` protocol is disabled

No policy change is required. Download the [`.appinstaller` file](https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller) and open it locally.

## A direct MSIX installation does not update automatically

Opening the raw `.msixbundle` does not associate the installation with the App Installer feed. Open the `.appinstaller` for future automatic updates, or install each new `.msixbundle` manually. Microsoft Store installations continue to update through the Store.

## The app or a Bluetooth connection fails

1. Confirm that the device is paired and available in Windows Bluetooth settings.
2. Exit AudioPlaybackConnector2 from the tray menu and start it again.
3. Choose **Help** in the tray menu, or **? Help** in Settings, then expand diagnostics to copy the report or open the log folder.
4. Include the app version, Windows build, installation method, and reproducible steps in a bug report.

Privacy mode redacts device names and IDs. Crash dumps remain local unless you share them and may contain sensitive memory, so review them first.

## A connection drops unexpectedly

Record the time of the drop, whether audio was playing, and whether the phone, Bluetooth adapter, or PC changed power state. Include your Windows build, app version, and whether reconnect is enabled globally or for that device. Copy diagnostics soon after the event; a successful reconnect alone does not identify what caused the loss.

Use the circular-arrow action for an explicit reconnect. Clicking the connected device name disconnects it intentionally. Device-specific policies are under **…** in the picker; an enabled global policy is also shown there and cannot be disabled for only one device.

## Connected, but audio does not resume after a pause

Before reconnecting, note the time, check whether the source's playback position advances, and confirm its selected audio output. Check the Windows output and volume mixer for mute or volume changes, then copy the app diagnostics from **Help**. Include the approximate pause duration and whether the source was locked or either device changed power state.

The connected indicator reports connection state; it does not measure audible playback. A successful retry does not rule out an intermittent failure. Use the circular-arrow action to reconnect after collecting the details. The cause of the reported intermittent pause/resume failure is still under investigation.

## A2DP Sink audio always plays through the default output device

Windows may ignore the per-app output device selected in **Settings > System > Sound > Volume mixer** for an `A2DP Sink` entry. In that case the Bluetooth source audio keeps playing through the current default playback device.

`AudioPlaybackConnection` does not expose a supported output-device selector. Windows may therefore ignore a per-app output selected for an A2DP Sink. AudioPlaybackConnector2 does not recommend registry modifications to change this unsupported Windows behavior.
