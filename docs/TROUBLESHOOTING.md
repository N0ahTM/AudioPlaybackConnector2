# Troubleshooting

## SmartScreen warns about the setup executable

The setup `.exe` is not yet code-signed. Download it only from the [official release page](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest), verify the filename, and use **More info > Run anyway** if you trust the download. The MSIX inside the setup is signed separately.

## Setup fails

Open `%LOCALAPPDATA%\AudioPlaybackConnector2\install.log`. Retry after closing AudioPlaybackConnector2. If Web Setup could not download the release, check the connection or use the versioned offline setup. The manual App Installer path in [Installation](INSTALLATION.md) is also available.

## Windows reports an untrusted publisher

The releases currently use a self-signed certificate. Web Setup and offline Setup install the pinned certificate for the current user. For a manual MSIX installation, import the `.cer` file from the same release into `Cert:\CurrentUser\TrustedPeople`, then retry. Do not trust a certificate obtained from another website.

## A framework package is missing

This usually occurs after installing the raw `.msix`. Prefer Web Setup, offline Setup, or the `.appinstaller`, which also install the required VCLibs and Windows App SDK packages.

If Windows mentions `MicrosoftCorporationII.WinAppRuntime.Main.2` or `MicrosoftCorporationII.WinAppRuntime.Singleton`, install the current [Windows App SDK runtime](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads) and retry.

## The `ms-appinstaller:` protocol is disabled

Public Windows installations may block web links using this protocol. Download the `.appinstaller` file locally and open it instead; the command is documented in [Installation](INSTALLATION.md). No policy change is required.

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

Windows may ignore the per-app output device selected in **Settings > System > Sound > Volume mixer** for an
`A2DP Sink` entry. In that case the Bluetooth source audio keeps playing through the current default playback device.

`AudioPlaybackConnection` does not expose a supported output-device selector. Windows may therefore ignore a per-app output selected for an A2DP Sink. AudioPlaybackConnector2 does not recommend registry modifications to change this unsupported Windows behavior.
