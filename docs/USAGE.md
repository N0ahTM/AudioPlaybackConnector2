# Usage

AudioPlaybackConnector2 receives Bluetooth A2DP audio from a paired phone or other source and plays it through Windows. Pair the source in Windows before using the app.

## Tray controls

- **Left-click** the tray icon to open the device picker.
- **Right-click** it to open the tray menu.
- **Double-click** it to toggle the configured default device. If no default is configured, the most recently connected device is used.
- Choose **Exit** from the tray menu to close active connections and stop the app.

The icon indicates idle, connecting, connected, and error states. Repeated actions for the same device are guarded while an operation is in progress.

## Device picker

Click a device name to connect it. Click a connected device to disconnect it. Use the circular-arrow button beside a connected device to force a disconnect-and-connect cycle when Windows reports a connection but audio is silent.

When multiple devices are connected, the picker offers actions to disconnect or reconnect all of them. Click **…** beside a device for its options and use the back arrow to return to the list.

## Device options

- **Display name:** Store a local alias used in the picker, notifications, diagnostics, and command output.
- **Default device:** Select the target used by tray-icon double-click and `apc2ctl toggle --default`.
- **Connect at app startup:** Connect this device when the app starts.
- **Reconnect automatically:** Reconnect this device after an unexpected interruption.

A global connection policy can enable startup connection or reconnection for every applicable device. When a global policy is active, the corresponding device option is informational and cannot disable the global behavior for only one device.

## Incoming connections

The optional incoming-connection setting keeps the Windows A2DP sink ready so a paired phone can initiate or end the connection from its own Bluetooth menu. Windows and the source device still control whether an incoming connection succeeds.

## Settings

Open Settings from the picker gear or tray menu. Settings include language, startup behavior, global connection policies, notifications, incoming connections, privacy mode, update checks, and saved window placement.

Settings are stored in the current user's profile. They do not roam to other computers.

## Privacy and diagnostics

Privacy mode replaces known device names, aliases, IDs, and local user paths where possible in UI, command output, and copied diagnostics. Use `apc2ctl --raw` only when a script explicitly needs the underlying identifiers.

Open **Help** from the tray menu or **? Help** in Settings to copy a diagnostic report or open the log folder. Review logs before sharing them. Crash reports and minidumps stay on the machine unless you upload them; minidumps may contain sensitive memory.

The app sends no telemetry. Manual update checks request GitHub release metadata.

## Automation

`apc2ctl.exe` exposes the same common operations for PowerShell, shortcuts, and MacroPads. It starts the installed app when necessary and then communicates with the instance in the current Windows user session. See the complete [command-line reference](CLI.md).

For failures or unexpected audio behavior, see [Troubleshooting](TROUBLESHOOTING.md).
