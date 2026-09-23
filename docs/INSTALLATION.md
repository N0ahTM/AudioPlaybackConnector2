# Installation

AudioPlaybackConnector2 requires Windows 10 version 2004 (build 19041) or newer. Pair the Bluetooth audio source in Windows before launching the app.

**Upgrading from 0.9.1:** Version 0.9.2 migrates existing preferences, including device aliases and connection policies, to the new settings format on first launch. Unsupported or damaged files are preserved before defaults are used.

If 0.9.1 was installed with a GitHub Web/Offline Setup, open the 0.9.2 App Installer feed in section 2 manually. Those setups did not register the feed, and the app no longer checks GitHub for updates. Keep the existing GitHub installation channel when upgrading; the Microsoft Store package has a separate identity.

## 1. Microsoft Store (recommended)

Install [AudioPlaybackConnector2 from the Microsoft Store](https://apps.microsoft.com/detail/9n366pgkjz0k). The Store provides the Microsoft-signed package, selects x64 or ARM64, installs framework dependencies, and delivers updates automatically.

## 2. GitHub App Installer (automatic updates)

Use this only when Microsoft Store installation is unavailable. GitHub packages have a separate package identity and use the repository's self-signed certificate. They are not an update for a Store installation.

1. Download `AudioPlaybackConnector2.cer` from the [latest GitHub release](https://github.com/N0ahTM/AudioPlaybackConnector2/releases/latest).
2. Verify that it came from this repository, then import it into the current user's **Trusted People** store:

   ```powershell
   Import-Certificate -FilePath ".\AudioPlaybackConnector2.cer" -CertStoreLocation "Cert:\CurrentUser\TrustedPeople"
   ```

3. Download and open the stable [AudioPlaybackConnector2.appinstaller feed](https://n0ahtm.github.io/AudioPlaybackConnector2/AudioPlaybackConnector2.appinstaller).

Windows App Installer selects the correct architecture, installs the exact framework packages referenced by the feed, and registers automatic update checks. If current-user trust is rejected, an administrator may import the same verified certificate into `Cert:\LocalMachine\TrustedPeople`. Never put this self-signed leaf certificate in **Trusted Root Certification Authorities**.

## 3. GitHub MSIX bundle (manual updates)

The release contains `AudioPlaybackConnector2_<version>_x64_ARM64.msixbundle`, the matching certificate, and architecture-specific packages below `Dependencies`.

1. Import the certificate into **Trusted People** as described above.
2. Install the matching x64 or ARM64 dependency packages from the same release.
3. Open the `.msixbundle`.

This path works offline after all files have been downloaded, but it does not register the App Installer feed. Install future versions manually. Do not substitute generic or differently versioned framework downloads for the packages referenced by the release.

## 4. Build from source

Clone the repository when you want to inspect, change, or contribute to the app. [CONTRIBUTING.md](../CONTRIBUTING.md) lists the Visual Studio components, restore, build, test, packaging, and signing commands.

For installation errors, see [Troubleshooting](TROUBLESHOOTING.md).
