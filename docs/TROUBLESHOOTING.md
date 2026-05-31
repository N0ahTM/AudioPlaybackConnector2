# Troubleshooting

## `DEP0840` / missing WinAppRuntime packages

If launch or packaging fails with `DEP0840` and mentions missing:

- `MicrosoftCorporationII.WinAppRuntime.Main.2`
- `MicrosoftCorporationII.WinAppRuntime.Singleton`

Install both runtime dependencies once:

1. [WinAppRuntime.Singleton](https://apps.microsoft.com/detail/9p5z076k079h)
2. [Windows App SDK 2.0 runtime](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/downloads#windows-app-sdk-20)

Then retry install/launch.

## App Installer link does not open

If this command does nothing:

```powershell
Start-Process "ms-appinstaller:?source=https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller"
```

Download the `.appinstaller` file manually from the release and open it directly.

## Certificate trust errors during MSIX install

If Windows reports an untrusted publisher:

1. Install the `.cer` from the release.
2. Use either machine-wide trust (`Cert:\LocalMachine\Root`) or per-user trust (`Cert:\CurrentUser\TrustedPeople`).
3. Retry installing the `.msix` or `.appinstaller`.

See [Installation](INSTALLATION.md) for commands.

## Packaging fails because certificate thumbprint is invalid

In Visual Studio:

1. Open `Package.appxmanifest`.
2. Go to **Packaging**.
3. Create/select a local test certificate.
4. Update `PackageCertificateThumbprint` in `AudioPlaybackConnector2 (Package)`.

## C++23 compile issues in Visual Studio

If the compiler does not accept required C++ features, set language standard to `/std:c++latest` and rebuild.
