# Command-line reference

`apc2ctl.exe` controls AudioPlaybackConnector2 in the current Windows user session. Run `apc2ctl help` for the built-in synopsis. The client attempts to start the installed app when no instance is available.

The following block is synchronized with the executable by `scripts/update-cli-reference.ps1`.

<!-- BEGIN GENERATED APC2CTL HELP -->
```text
AudioPlaybackConnector2 command line control

Usage:
  apc2ctl show
  apc2ctl settings
  apc2ctl status [--json]
  apc2ctl list [--json]
  apc2ctl connect (--id ID | --name NAME | --mac MAC | --alias ALIAS | --last | --default | TARGET)
  apc2ctl disconnect (--id ID | --name NAME | --mac MAC | --alias ALIAS | --last | --default | TARGET)
  apc2ctl reconnect (--id ID | --name NAME | --mac MAC | --alias ALIAS | --last | --default | TARGET)
  apc2ctl toggle [--last | --default | --id ID | --name NAME | --mac MAC | --alias ALIAS | TARGET]
  apc2ctl disconnect-all
  apc2ctl reconnect-all
  apc2ctl default show [--json] [--raw]
  apc2ctl default set (--id ID | --name NAME | --mac MAC | --alias ALIAS | TARGET)
  apc2ctl default clear
  apc2ctl alias list [--json] [--raw]
  apc2ctl alias set (--id ID | --name NAME | --mac MAC | --alias ALIAS | TARGET) (--value VALUE | VALUE)
  apc2ctl alias clear (--id ID | --name NAME | --mac MAC | --alias ALIAS | TARGET)

Use -- before a target or alias value that begins with '-'. TARGET is resolved as an exact device ID, then an exact
alias or device name, then a MAC address contained in the device ID, and finally an alias/name substring. Equal-rank
matches are rejected as ambiguous; use an explicit selector to disambiguate.
```
<!-- END GENERATED APC2CTL HELP -->

## General commands

```text
apc2ctl show
apc2ctl settings
apc2ctl status [--json] [--raw]
apc2ctl list [--json] [--raw]
apc2ctl disconnect-all [--json] [--raw]
apc2ctl reconnect-all [--json] [--raw]
```

`show` opens the device picker and `settings` opens Settings. `status` returns application and connection state. `list` returns known devices.

## Device actions

```text
apc2ctl connect TARGET [--json] [--raw]
apc2ctl disconnect TARGET [--json] [--raw]
apc2ctl reconnect TARGET [--json] [--raw]
apc2ctl toggle [TARGET] [--json] [--raw]
```

Use exactly one target form:

```text
--id ID
--name NAME
--mac MAC
--alias ALIAS
--last
--default
TARGET
```

`--last` and `--default` are available for connect, disconnect, reconnect, and toggle. `toggle` uses `--default` when no target is supplied. Commands that configure an alias or default require an explicit device.

A positional `TARGET` is resolved in this order:

1. exact Windows device ID;
2. exact alias or device name;
3. MAC address contained in the Windows device ID;
4. alias or device-name substring.

Equal-rank matches are rejected as ambiguous. Use `--id`, `--name`, `--mac`, or `--alias` to disambiguate. Insert `--` immediately before a value that starts with `-`.

Examples:

```powershell
apc2ctl connect --name "WH-1000XM5"
apc2ctl reconnect --alias "Desk speakers"
apc2ctl disconnect --mac "11:22:33:44:55:66"
apc2ctl toggle --default
apc2ctl connect -- --device-name-starting-with-a-dash
```

## Default device

```text
apc2ctl default show [--json] [--raw]
apc2ctl default set TARGET [--json] [--raw]
apc2ctl default clear [--json] [--raw]
```

`default set` accepts `--id`, `--name`, `--mac`, `--alias`, or a positional target. The default is used by tray-icon double-click and `toggle --default`.

## Aliases

```text
apc2ctl alias list [--json] [--raw]
apc2ctl alias set TARGET (--value VALUE | VALUE) [--json] [--raw]
apc2ctl alias clear TARGET [--json] [--raw]
```

Examples:

```powershell
apc2ctl alias set --name "WH-1000XM5" --value "Headphones"
apc2ctl alias set --id '<Windows device id>' 'Desk speakers'
apc2ctl alias clear --alias 'Headphones'
```

## Output and privacy

`--json` requests machine-readable output. JSON responses contain at least `ok` and `exitCode`; command-specific fields may also be present. Scripts must use the process exit code rather than parsing localized human-readable text.

Privacy mode redacts known device details by default. `--raw` explicitly requests real names and IDs for the current command. Treat raw output as sensitive and avoid writing it to shared logs.

## Exit codes

| Code | Meaning |
| ---: | --- |
| `0` | Success |
| `3` | Invalid command or arguments |
| `4` | Target not found |
| `5` | Target is ambiguous |
| `6` | Operation failed |
| `7` | App or requested service unavailable |
| `8` | Target is busy with another operation |
| `9` | The outcome could not be confirmed |

Exit code `9` means the command may have completed but the client could not safely confirm the result. Inspect current state before retrying a non-idempotent workflow.
