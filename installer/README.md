# Bootstrapper Installer (Inno Setup)

English-only, minimal-click installers (2 clicks: **Install** → **Finish**).
Per-user, no admin rights needed.

## Flow

1. **Start page** — shows the install state and adapts:
   - *Not installed:* "Version X will be installed" → Install
   - *Same version:* choice of **Repair / reinstall** or **Uninstall**
   - *Older version installed:* **Update to version X** or Uninstall
   - *Newer version installed (downgrade):* bold warning, **Keep current (close setup)** or Uninstall
2. **Download page** (web variant only) — native Inno download progress per file
3. **Progress page** — live step output (certificate → package → verify/launch)
   in a console-style memo plus progress bar
4. **Finished page** — context-dependent: installed & launched / uninstalled /
   failed (with log path and manual fallback)

Both variants import the self-signed certificate into
`Cert:\CurrentUser\TrustedPeople` (no admin, no security prompt — MSIX
sideloading trusts this store) and register the MSIX via `Add-AppxPackage`.

## Variants

| | `AudioPlaybackConnector2-Setup-<version>.exe` | `AudioPlaybackConnector2-WebSetup.exe` |
|---|---|---|
| Size | ~54 MB | ~2 MB |
| Payload | Certificate, MSIX and dependencies (offline capable) | Pinned certificate; MSIX and dependencies downloaded from the latest GitHub release |
| Version | Fixed at build time | Always installs the newest release |
| Build | `-Mode Bundle -MsixPath ...` | `-Mode Web` |

## Build

```powershell
# Bundle (needs the signed release MSIX):
.\installer\build-installer.ps1 -Mode Bundle -MsixPath "dist\v0.8.1\AudioPlaybackConnector2_0.8.1.0_x64.msix"

# Web (pass the release version for deterministic/offline compilation):
.\installer\build-installer.ps1 -Mode Web -Version 0.8.1
```

| Parameter | Default | Purpose |
|---|---|---|
| `-Mode` | `Bundle` | `Bundle` embeds payloads, `Web` downloads them |
| `-MsixPath` | — (required for Bundle) | Signed release MSIX |
| `-CertPath` | `certs\AudioPlaybackConnector2.cer` | Self-signed cert embedded in both variants |
| `-DependenciesDir` | `<msix dir>\Dependencies` | Dependency packages (VCLibs, WinAppRuntime) |
| `-Version` | MSIX filename / latest tag | Version shown in the wizard and filename; pass explicitly for deterministic web builds |
| `-OutputDir` | `dist\installer` | Where the setup `.exe` lands |

Requires Inno Setup 6 (`winget install JRSoftware.InnoSetup`).

## Behavior details

- **Running app:** the worker closes only the app's exact process names and uses
  `Add-AppxPackage -ForceApplicationShutdown` so updates are not blocked by the tray process.
- **Shared framework in use:** if `0x80073D02` lists *other* apps (Spotify,
  widgets, ... all use WindowsAppRuntime), the install retries **without** the
  bundled dependencies — a framework is already present and must not be
  re-registered underneath running apps.
- **Newer version installed** (`0x80073D06`): treated as success; the newer
  version is left untouched. In silent mode the wizard never auto-closes on
  downgrade — this guard keeps the machine state safe.
- **Uninstall** removes the package and the certificate from TrustedPeople;
  user settings under `%LOCALAPPDATA%` are kept.
- **Logging:** `%LOCALAPPDATA%\AudioPlaybackConnector2\install.log`.
- **Silent mode:** `AudioPlaybackConnector2-WebSetup.exe /VERYSILENT /NORESTART`
  (always installs/repairs; uninstall is GUI-only by design).
- **The setup `.exe` is unsigned**, so SmartScreen still shows a one-time
  "unknown publisher" warning. Once a trusted code-signing certificate is
  available, sign the bootstrapper too (Inno `SignTool=` directive).

## Files

| File | Role |
|---|---|
| `setup.iss` | Inno Setup script (per-user, English UI, branding, state-aware start page, download page, progress memo; `WEBBOOT` define switches modes) |
| `build-installer.ps1` | Stages payloads, resolves version, compiles via ISCC |
| `stage\install-app.ps1` | Install/uninstall worker, called per `-Step` (cert/install/verify/uninstall). Tracked in git; everything else in `stage\` is ignored |
| `assets\` | Setup icon and wizard images (generated from the app logo) |
| `README-INSTALL.txt` | Placed in the install dir for end users |

## CI integration

The release and release-dry-run workflows install Inno Setup after producing and
verifying the signed MSIX. They build both setup variants from that exact payload
and include them in the release artifact set. The web bootstrapper is rebuilt for
each release so its displayed version and pinned certificate remain current.
