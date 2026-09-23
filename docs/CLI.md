# Command-line reference

`apc2ctl.exe` controls the current Windows user session and attempts to start the installed app if needed. `apc2ctl help` shows this synopsis, synchronized by `scripts/update-cli-reference.ps1`:

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

## Commands and targets

`show` opens the picker; `settings` opens Settings; `status` returns app/connection state; `list` returns known devices. Bulk actions operate on all connections.

Use exactly one target. `--last`/`--default` apply to connect/disconnect/reconnect/toggle; targetless `toggle` uses the default. Alias/default configuration requires an explicit device (`--id`, `--name`, `--mac`, `--alias` or positional). Default selection also controls tray double-click. Resolution precedence and `--` escaping are specified above.

```powershell
apc2ctl reconnect --alias "Desk speakers"
apc2ctl disconnect --mac "11:22:33:44:55:66"
apc2ctl connect -- --device-name-starting-with-a-dash
apc2ctl alias set --name "WH-1000XM5" --value "Headphones"
apc2ctl alias set --id '<Windows device id>' 'Desk speakers'
apc2ctl alias clear --alias 'Headphones'
```

## Output and privacy

Queries, device/bulk actions and alias/default commands accept `--json` and `--raw`. JSON contains at least `ok` and `exitCode`; use the process exit code, not localized output, in scripts. Privacy redacts known device details by default. `--raw` requests real names/IDs for this command; avoid shared logs.

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
