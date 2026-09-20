# Usage

Pair an A2DP audio source in Windows first. AudioPlaybackConnector2 receives its audio and plays it through the PC.

## Tray controls

| Action | Result |
| --- | --- |
| Left-click | Device picker |
| Right-click | Tray menu |
| Double-click | Toggle default device, falling back to most recently connected |
| Exit | Close connections and stop the app |

The icon shows idle/connecting/connected/error. Repeated actions are guarded while a device operation runs.

## Device picker

Click a device to connect/disconnect; use its circular arrow for a disconnect/connect cycle. With multiple connections, bulk reconnect/disconnect is available. **…** opens device options; the back arrow returns.

## Device options

- **Display name:** local alias for picker, notifications, diagnostics and CLI.
- **Default device:** tray double-click and `apc2ctl toggle --default` target.
- **Connect at app startup / Reconnect automatically:** per-device startup and unexpected-loss policies.

An enabled global policy applies to all applicable devices; its per-device display is informational and cannot override it off.

## Incoming connections

Optional incoming mode keeps the Windows A2DP sink ready for a paired phone's Bluetooth menu. Windows and the source still determine success.

## Settings

Open the picker gear or tray Settings: language, Windows startup, connection policies, notifications, incoming mode, privacy and saved window placement. Settings remain in the current user profile; they do not roam. Incompatible formats are backed up and reset to current defaults, without migration.

## Privacy and diagnostics

Privacy mode redacts known names, aliases, IDs and local user paths where possible. CLI `--raw` requests underlying identifiers. Tray **Help** / Settings **? Help** opens diagnostics and logs. Review reports before sharing; minidumps may contain sensitive memory and remain local unless uploaded.

The app sends no telemetry and performs no update checks. Microsoft Store or Windows App Installer handles updates; direct MSIX installations require manual updates.

## Automation

`apc2ctl.exe` starts the installed app when necessary and controls the current Windows user session. See [CLI](CLI.md), [Installation](INSTALLATION.md) and [Troubleshooting](TROUBLESHOOTING.md).
