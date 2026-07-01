# Troubleshooting

## `DEP0840` / missing WinAppRuntime packages

If you installed via `.appinstaller`, the required framework dependencies (including the Windows App SDK runtime)
should be installed automatically. If you still see `DEP0840` mentioning missing packages such as
`MicrosoftCorporationII.WinAppRuntime.Main.2` or `MicrosoftCorporationII.WinAppRuntime.Singleton`, install them manually:

1. [WinAppRuntime.Singleton](https://apps.microsoft.com/detail/9p5z076k079h)
2. [Windows App SDK 2.0 runtime](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/downloads#windows-app-sdk-20)

Then retry install/launch.

> This usually only happens when installing the raw `.msix` directly without the `.appinstaller` flow, or when the
> framework packages were removed from the system after the app was installed.

## App Installer protocol is disabled

Windows can report that the `ms-appinstaller:` protocol is disabled if a web install link is used:

```powershell
Start-Process "ms-appinstaller:?source=https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller"
```

This is not specific to AudioPlaybackConnector2 and is not controlled by the MSIX package manifest. Microsoft disabled
the protocol by default on consumer devices in December 2023 after it was abused to distribute malicious MSIX packages.
Enterprise administrators can re-enable it with the `EnableMSAppInstallerProtocol` policy, but public GitHub releases
should avoid depending on that protocol.

Download the `.appinstaller` file and open the local file instead:

```powershell
$installer = Join-Path $env:TEMP "AudioPlaybackConnector2.appinstaller"
Invoke-WebRequest -Uri "https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller" -OutFile $installer
Start-Process $installer
```

Microsoft references:

- [Installing Windows apps from a web page](https://learn.microsoft.com/windows/msix/app-installer/installing-windows10-apps-web)
- [Financially motivated threat actors misusing App Installer](https://www.microsoft.com/security/blog/2023/12/28/financially-motivated-threat-actors-misusing-app-installer/)
- [DesktopAppInstaller policy CSP](https://learn.microsoft.com/windows/client-management/mdm/policy-csp-desktopappinstaller)

## Certificate trust errors during MSIX install

If Windows reports an untrusted publisher:

1. Install the `.cer` from the release.
2. Use either machine-wide trust (`Cert:\LocalMachine\Root`) or per-user trust (`Cert:\CurrentUser\TrustedPeople`).
3. Retry installing the `.msix` or `.appinstaller`.

See [Installation](INSTALLATION.md) for commands.

## A2DP Sink audio always plays through the default output device

Windows may ignore the per-app output device selected in **Settings > System > Sound > Volume mixer** for an
`A2DP Sink` entry. In that case the Bluetooth source audio keeps playing through the current default playback device.

AudioPlaybackConnector2 uses Windows' `AudioPlaybackConnection` API, which does not expose a supported output-device
selector. The most reliable workaround is to make the A2DP Sink visible as a recording device, then use the legacy
Sound control panel:

1. Open Registry Editor.
2. Go to `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture`.
3. Find the capture endpoint whose `Properties` include the Bluetooth device name.
4. Back up the endpoint key before changing it.
5. Under that endpoint's `Properties`, delete the value named `{9c119480-ddc2-4954-a150-5bd240d454ad},6`.
6. Open `control mmsys.cpl sounds`, then switch to the **Recording** tab.
7. Open the A2DP Sink device properties, switch to **Listen**, enable **Listen to this device**, and select the target
   playback device.

Be careful when editing `HKLM`; deleting the wrong value can break device configuration for the current Windows
install. If you use Voicemeeter or similar routing software, route the newly visible recording endpoint there and
avoid enabling **Listen to this device** at the same time, otherwise Windows may play the audio twice.

Reference: [Microsoft Answers workaround](https://learn.microsoft.com/en-us/answers/questions/4123061/bluetooth-a2dp-snk-device-not-showing-up-in-sound).

## Packaging fails because certificate thumbprint is invalid

In Visual Studio:

1. Open `Package.appxmanifest`.
2. Go to **Packaging**.
3. Create/select a local test certificate.
4. Update `PackageCertificateThumbprint` in `AudioPlaybackConnector2 (Package)`.

## C++23 compile issues in Visual Studio

If the compiler does not accept required C++ features, set language standard to `/std:c++latest` and rebuild.
