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
3. Open **Settings > Support** to copy redacted diagnostics or open the log folder.
4. Include the app version, Windows build, installation method, and reproducible steps in a bug report.

Privacy mode redacts device names and IDs. Crash dumps remain local unless you share them and may contain sensitive memory, so review them first.

## A2DP Sink audio always plays through the default output device

Windows may ignore the per-app output device selected in **Settings > System > Sound > Volume mixer** for an
`A2DP Sink` entry. In that case the Bluetooth source audio keeps playing through the current default playback device.

`AudioPlaybackConnection` does not expose a supported output-device selector. Windows may therefore ignore a per-app output selected for an A2DP Sink.

As a last resort, an advanced registry workaround can expose the sink as a recording endpoint and route it through the legacy Sound control panel. This changes protected system audio configuration and can break the endpoint if the wrong value is removed. Create a restore point and export the affected registry key before proceeding.

1. Open Registry Editor.
2. Go to `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture`.
3. Find the capture endpoint whose `Properties` include the Bluetooth device name and export its complete key.
4. Under that endpoint's `Properties`, delete only the value named `{9c119480-ddc2-4954-a150-5bd240d454ad},6`.
5. Open `control mmsys.cpl sounds`, then switch to the **Recording** tab.
6. Open the A2DP Sink device properties, switch to **Listen**, enable **Listen to this device**, and select the target
   playback device.

Do not enable both Windows listening and another routing application for the same endpoint, or audio may play twice. Restore the exported key if the endpoint stops working.

Reference: [Microsoft Answers workaround](https://learn.microsoft.com/en-us/answers/questions/4123061/bluetooth-a2dp-snk-device-not-showing-up-in-sound).
