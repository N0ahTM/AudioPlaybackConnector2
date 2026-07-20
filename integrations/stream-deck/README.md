# AudioPlaybackConnector2 Stream Deck plugin (Beta)

> **Beta:** This plugin is in an early stage and may contain bugs. Actions, behavior, and the underlying `apc2ctl` interface can still change. Please report issues at <https://github.com/N0ahTM/AudioPlaybackConnector2/issues>.

Official Stream Deck SDK plugin for AudioPlaybackConnector2. The plugin runs `apc2ctl --json` and uses the app's default-device setting.

## Build

```powershell
npm install
npm run build
npm run pack
```

`npm run pack` creates the installable `com.n0ahtm.audioplaybackconnector2.streamDeckPlugin` release artifact.

Set `APC2CTL_PATH` if `apc2ctl.exe` is not available on `PATH`.

## Actions

- Toggle default device
- Connect default
- Disconnect default
- Reconnect default
- Reconnect all
- Disconnect all
- Open picker
- Open settings
