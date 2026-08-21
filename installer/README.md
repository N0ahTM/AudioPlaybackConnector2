# Bootstrapper Installer

The project ships two per-user Inno Setup bootstrapper variants for x64 and ARM64 Windows. Both validate the shared MSIX bundle and signer, select only the native architecture's Microsoft-signed framework dependencies, import the embedded self-signed release certificate into `Cert:\CurrentUser\TrustedPeople`, register the package, verify installation, and launch the app. They do not require administrator rights.

| Variant | Payload | Intended use |
|---|---|---|
| `AudioPlaybackConnector2-WebSetup.exe` | Pinned certificate; downloads the current GitHub release package and dependencies | Recommended installation |
| `AudioPlaybackConnector2-Setup-<version>.exe` | Certificate, x64/ARM64 MSIX bundle, and dependencies for both architectures | Offline installation and reproducible version installs |

## Build

Install [Inno Setup 6](https://jrsoftware.org/isinfo.php), then run from the repository root:

```powershell
# Offline bundle built from a signed release package
.\installer\build-installer.ps1 -Mode Bundle -MsixPath "path\AudioPlaybackConnector2_<package-version>_x64_ARM64.msixbundle" -Version "<version>"

# Web installer with deterministic displayed version
.\installer\build-installer.ps1 -Mode Web -Version "<version>"
```

`-CertPath` defaults to `certs\AudioPlaybackConnector2.cer`, `-DependenciesDir` defaults to the MSIX bundle's sibling `Dependencies` directory, and `-OutputDir` defaults to `dist\installer`. Bundle mode requires `-MsixPath`; pass `-Version` explicitly for release builds.

## Installer Behavior

- A new installation, repair, update, and uninstall are selected on the start page.
- A detected newer version is preserved; the installer does not silently downgrade it.
- The worker closes only the app's known process names before package registration.
- If a shared Windows App SDK framework is already in use, installation retries without re-registering bundled framework packages.
- Uninstall removes the package and pinned certificate but keeps settings under `%LOCALAPPDATA%`.
- Logs are written to `%LOCALAPPDATA%\AudioPlaybackConnector2\install.log`.
- The log rotates at 2 MiB and retains one previous file.
- Silent install or repair is supported with `/VERYSILENT /NORESTART`; uninstall remains interactive.

The setup `.exe` is currently unsigned and may trigger SmartScreen. Its embedded certificate prevents a package downloaded by an authentic installer from being silently exchanged for one signed by another key, but it does not authenticate a modified setup executable. Download setup only from the official release page. Sign the bootstrapper too when a trusted code-signing certificate becomes available.

## Source Layout

| File | Role |
|---|---|
| `setup.iss` | Inno Setup UI and orchestration; `WEBBOOT` selects web mode |
| `build-installer.ps1` | Validates inputs, stages release payloads in a unique temporary directory, invokes ISCC, and always removes the staging directory |
| `stage\install-app.ps1` | Certificate, package, verification, launch, and uninstall worker |

Only the runtime worker is stored under `installer\stage`. Certificates, packages, and dependencies exist there neither before nor after a normal build. Compiled installers are ignored under `dist\`.

## Validation

Before a release, test both variants on clean x64 and ARM64 Windows user accounts:

1. Fresh install and first launch.
2. Repair of the same version.
3. Upgrade from the previous release.
4. Newer-version downgrade protection.
5. Uninstall with settings retained.
6. Offline bundle with networking disabled.
7. Web installer download failure and log output.
8. `/VERYSILENT /NORESTART` install or repair.

## CI Integration

The shared release-artifact workflow builds both installers from one verified, signed x64/ARM64 MSIX bundle. A tag push creates or refreshes a draft release with the App Installer, bundle, certificate, and dependency assets. A manual workflow dispatch rebuilds and verifies the tagged source before publishing the draft and invoking the reusable feed-deployment workflow. If publication, Pages deployment, or feed verification fails, the release returns to draft; when Pages may have changed, the previous feed is restored and verified. The dry-run workflow calls the same artifact workflow without creating a release.
