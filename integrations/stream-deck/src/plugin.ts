import streamDeck, {
  action,
  SingletonAction,
  type KeyDownEvent,
  type WillAppearEvent,
  type WillDisappearEvent
} from "@elgato/streamdeck";
import { execFile } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

type ActionSettings = {
  apc2ctlPath?: string;
};

type DeviceInfo = {
  id?: string;
  displayName?: string;
  connected?: boolean;
};

type Apc2Response = {
  ok?: boolean;
  message?: string;
  displayName?: string;
  connectedCount?: number;
  connectedDevices?: DeviceInfo[];
  devices?: DeviceInfo[];
  id?: string;
  resolved?: boolean;
  connected?: boolean;
};

type VisualState = "offline" | "connected" | "connecting" | "error";
type StatusScope = "default" | "global" | "open";

const execFileAsync = promisify(execFile);
const pluginRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
const baseIcon = readBaseIcon();
const pollIntervalMs = 5000;
const connectingFrameMs = 260;

function readBaseIcon(): string | undefined {
  try {
    return readFileSync(join(pluginRoot, "imgs", "actions", "apc2@2x.png")).toString("base64");
  } catch {
    try {
      return readFileSync(join(pluginRoot, "imgs", "actions", "apc2.png")).toString("base64");
    } catch {
      return undefined;
    }
  }
}

function resolveApc2ctl(settings: ActionSettings | undefined): string {
  return settings?.apc2ctlPath || process.env.APC2CTL_PATH || "apc2ctl.exe";
}

async function runApc2ctl(settings: ActionSettings | undefined, args: string[]): Promise<Apc2Response | undefined> {
  const executable = resolveApc2ctl(settings);
  const result = await execFileAsync(executable, args, {
    windowsHide: true,
    timeout: 15000,
    maxBuffer: 128 * 1024
  });

  const output = result.stdout.trim();
  if (!output) {
    return undefined;
  }

  return JSON.parse(output) as Apc2Response;
}

function iconColor(state: VisualState): string {
  switch (state) {
    case "connected":
      return "#2ECA65";
    case "connecting":
      return "#F79009";
    case "error":
      return "#F04438";
    default:
      return "#8A94A6";
  }
}

function makeIconSvg(state: VisualState, frame = 0): string {
  const color = iconColor(state);
  const dashOffset = state === "connecting" ? 56 - (frame % 8) * 7 : 0;
  const ringDash = state === "connecting" ? `stroke-dasharray="44 18" stroke-dashoffset="${dashOffset}"` : "";
  const iconImage = baseIcon
    ? `<image href="data:image/png;base64,${baseIcon}" x="18" y="18" width="108" height="108" preserveAspectRatio="xMidYMid meet" />`
    : `<circle cx="72" cy="72" r="35" fill="#1F6FEB" /><path d="M65 39l28 25-20 17 20 18-28 25V87L51 100l-9-10 20-18-20-18 9-10 14 13V39zm12 27v-9l5 5-5 4zm0 21l5 5-5 4v-9z" fill="white" />`;
  const badge = state === "connected" ? "M100 111l9 9 18-21" : state === "error" ? "M100 100l20 20M120 100l-20 20" : "";

  return `<svg xmlns="http://www.w3.org/2000/svg" width="144" height="144" viewBox="0 0 144 144">
  <rect width="144" height="144" rx="26" fill="#0F1115"/>
  ${iconImage}
  <circle cx="72" cy="72" r="55" fill="none" stroke="${color}" stroke-width="7" ${ringDash} stroke-linecap="round"/>
  <circle cx="110" cy="110" r="20" fill="${color}" opacity="${state === "offline" ? "0.28" : "0.95"}"/>
  ${badge ? `<path d="${badge}" fill="none" stroke="white" stroke-width="7" stroke-linecap="round" stroke-linejoin="round"/>` : ""}
</svg>`;
}

function truncateTitle(value: string | undefined): string | undefined {
  if (!value) return undefined;
  const trimmed = value.replace(/\s+/g, " ").trim();
  if (trimmed.length <= 18) return trimmed;
  return `${trimmed.slice(0, 15)}...`;
}

function statusTitle(state: VisualState, label: string | undefined): string {
  if (state === "error") return "Error";
  if (state === "connecting") return "Connecting";

  const prefix = state === "connected" ? "On" : "Off";
  const name = truncateTitle(label);
  return name ? `${prefix}\n${name}` : prefix;
}

async function setVisual(
  event: WillAppearEvent<ActionSettings> | KeyDownEvent<ActionSettings>,
  state: VisualState,
  label?: string,
  frame = 0,
  useStatusTitle = true
): Promise<void> {
  await Promise.all([
    event.action.setImage(makeIconSvg(state, frame)),
    event.action.setTitle(useStatusTitle ? statusTitle(state, label) : (label ?? ""))
  ]);
}

async function readDefaultStatus(settings: ActionSettings | undefined): Promise<{ state: VisualState; label?: string }> {
  const defaultDevice = await runApc2ctl(settings, ["default", "show", "--json"]);
  if (!defaultDevice?.resolved) {
    return { state: "offline", label: defaultDevice?.displayName };
  }

  return {
    state: defaultDevice.connected ? "connected" : "offline",
    label: defaultDevice.displayName
  };
}

async function readGlobalStatus(settings: ActionSettings | undefined): Promise<{ state: VisualState; label?: string }> {
  const status = await runApc2ctl(settings, ["status", "--json"]);
  const count = status?.connectedCount ?? 0;
  const label = count > 1 ? `${count} devices` : status?.connectedDevices?.[0]?.displayName;
  return { state: count > 0 ? "connected" : "offline", label };
}

class Apc2CommandAction extends SingletonAction<ActionSettings> {
  private readonly pollers = new Map<string, ReturnType<typeof setInterval>>();
  private readonly animations = new Map<string, ReturnType<typeof setInterval>>();
  private readonly pollsInFlight = new Set<string>();

  constructor(
    private readonly args: string[],
    private readonly scope: StatusScope,
    private readonly fallbackTitle: string
  ) {
    super();
  }

  override async onWillAppear(event: WillAppearEvent<ActionSettings>): Promise<void> {
    this.startPolling(event);
    await this.updateStatus(event);
  }

  override onWillDisappear(event: WillDisappearEvent<ActionSettings>): void {
    this.stopPolling(event.action.id);
    this.stopAnimation(event.action.id);
  }

  override async onKeyDown(event: KeyDownEvent<ActionSettings>): Promise<void> {
    this.startConnectingAnimation(event);
    try {
      const response = await runApc2ctl(event.payload.settings, [...this.args, "--json"]);
      if (response?.ok === false) {
        throw new Error(response.message || "Command failed");
      }

      this.stopAnimation(event.action.id);
      await event.action.showOk();
      await this.updateStatus(event, response?.displayName || response?.message);
      setTimeout(() => void this.updateStatus(event), 1500);
    } catch (error) {
      this.stopAnimation(event.action.id);
      streamDeck.logger.error("AudioPlaybackConnector2 command failed", error);
      await setVisual(event, "error");
      await event.action.showAlert();
    }
  }

  private startPolling(event: WillAppearEvent<ActionSettings>): void {
    this.stopPolling(event.action.id);
    this.pollers.set(
      event.action.id,
      setInterval(() => {
        void this.updateStatus(event);
      }, pollIntervalMs)
    );
  }

  private stopPolling(actionId: string): void {
    const poller = this.pollers.get(actionId);
    if (poller) {
      clearInterval(poller);
      this.pollers.delete(actionId);
    }
    this.pollsInFlight.delete(actionId);
  }

  private startConnectingAnimation(event: KeyDownEvent<ActionSettings>): void {
    this.stopAnimation(event.action.id);
    let frame = 0;
    void setVisual(event, "connecting", undefined, frame);
    this.animations.set(
      event.action.id,
      setInterval(() => {
        frame += 1;
        void setVisual(event, "connecting", undefined, frame);
      }, connectingFrameMs)
    );
  }

  private stopAnimation(actionId: string): void {
    const animation = this.animations.get(actionId);
    if (animation) {
      clearInterval(animation);
      this.animations.delete(actionId);
    }
  }

  private async updateStatus(
    event: WillAppearEvent<ActionSettings> | KeyDownEvent<ActionSettings>,
    overrideLabel?: string
  ): Promise<void> {
    if (this.pollsInFlight.has(event.action.id)) return;
    this.pollsInFlight.add(event.action.id);
    try {
      if (this.scope === "open") {
        await setVisual(event, "offline", overrideLabel || this.fallbackTitle, 0, false);
        return;
      }

      const status =
        this.scope === "default"
          ? await readDefaultStatus(event.payload.settings)
          : await readGlobalStatus(event.payload.settings);
      await setVisual(event, status.state, overrideLabel || status.label || this.fallbackTitle);
    } catch (error) {
      streamDeck.logger.error("AudioPlaybackConnector2 status failed", error);
      await setVisual(event, "error", this.fallbackTitle);
    } finally {
      this.pollsInFlight.delete(event.action.id);
    }
  }
}

@action({ UUID: "com.n0ahtm.audioplaybackconnector2.toggle-default" })
class ToggleDefaultAction extends Apc2CommandAction {
  constructor() {
    super(["toggle", "--default"], "default", "Toggle");
  }
}

@action({ UUID: "com.n0ahtm.audioplaybackconnector2.connect-default" })
class ConnectDefaultAction extends Apc2CommandAction {
  constructor() {
    super(["connect", "--default"], "default", "Connect");
  }
}

@action({ UUID: "com.n0ahtm.audioplaybackconnector2.disconnect-default" })
class DisconnectDefaultAction extends Apc2CommandAction {
  constructor() {
    super(["disconnect", "--default"], "default", "Disconnect");
  }
}

@action({ UUID: "com.n0ahtm.audioplaybackconnector2.reconnect-default" })
class ReconnectDefaultAction extends Apc2CommandAction {
  constructor() {
    super(["reconnect", "--default"], "default", "Reconnect");
  }
}

@action({ UUID: "com.n0ahtm.audioplaybackconnector2.reconnect-all" })
class ReconnectAllAction extends Apc2CommandAction {
  constructor() {
    super(["reconnect-all"], "global", "Reconnect all");
  }
}

@action({ UUID: "com.n0ahtm.audioplaybackconnector2.disconnect-all" })
class DisconnectAllAction extends Apc2CommandAction {
  constructor() {
    super(["disconnect-all"], "global", "Disconnect all");
  }
}

@action({ UUID: "com.n0ahtm.audioplaybackconnector2.open-picker" })
class OpenPickerAction extends Apc2CommandAction {
  constructor() {
    super(["show"], "open", "Picker");
  }
}

@action({ UUID: "com.n0ahtm.audioplaybackconnector2.open-settings" })
class OpenSettingsAction extends Apc2CommandAction {
  constructor() {
    super(["settings"], "open", "Settings");
  }
}

streamDeck.actions.registerAction(new ToggleDefaultAction());
streamDeck.actions.registerAction(new ConnectDefaultAction());
streamDeck.actions.registerAction(new DisconnectDefaultAction());
streamDeck.actions.registerAction(new ReconnectDefaultAction());
streamDeck.actions.registerAction(new ReconnectAllAction());
streamDeck.actions.registerAction(new DisconnectAllAction());
streamDeck.actions.registerAction(new OpenPickerAction());
streamDeck.actions.registerAction(new OpenSettingsAction());

streamDeck.connect();
