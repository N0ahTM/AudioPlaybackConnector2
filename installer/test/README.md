# Installer-Tests in Windows Sandbox

Wegwerf-Windows-Instanz für echte End-to-End-Tests des Inno-Bootstrappers:
sauberer Zustand pro Lauf, keine Gefahr für das Host-System, Networking aktiv
(der Web-Bootstrapper kann das neueste GitHub-Release laden).

## Voraussetzung

Windows Sandbox ist aktiviert (Windows **Pro/Enterprise/Education**, nicht Home),
einmalig als Administrator:

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All
# danach neu starten
```

## Aufruf (Repo-Root)

```powershell
# Frische Installation mit dem neuesten Bundle-Setup aus dist\installer:
.\installer\test\Test-InSandbox.ps1

# Web-Bootstrapper:
.\installer\test\Test-InSandbox.ps1 -SetupExe dist\installer\AudioPlaybackConnector2-WebSetup.exe

# Update-Pfad: erst 0.7.0 installieren, dann das aktuelle Setup:
.\installer\test\Test-InSandbox.ps1 -PreInstallSetup dist\installer\AudioPlaybackConnector2-Setup-0.7.0.exe

# Zusätzlich prüfen, dass die App startet:
.\installer\test\Test-InSandbox.ps1 -LaunchApp
```

Der Lauf ist vollautomatisch: Die Sandbox bootet, `sandbox\run-install-test.ps1`
installiert per `/VERYSILENT`, prüft `Get-AppxPackage` + Zertifikatsspeicher,
kopiert die Logs raus und schreibt ein `DONE`-Sentinel — danach schließt das
Host-Skript die Sandbox und druckt PASS/FAIL.

## Ergebnisse (`dist\installer\sandbox-out\<Zeitstempel>\`)

| Datei | Inhalt |
|---|---|
| `result.json` | Exit-Code, installierte Version vorher/nachher, Zertifikat, PASS/FAIL |
| `setup.log` | Inno-Setup-Log (`/LOG`) |
| `install.log` | Transcript von `install-app.ps1` (Zertifikat + Add-AppxPackage) |
| `sandbox-transcript.txt` | kompletter Ablauf in der Sandbox |
| `test.wsb` | die verwendete Sandbox-Konfiguration |

## Grenzen

- **Kein Wizard-GUI-Test**: Die Sandbox rendert in einer eigenen RDP-ähnlichen
  Session — UI Automation auf dem Host sieht die Fenster darin nicht. Der Lauf
  prüft den Silent-Pfad (identische Codepfade wie der Wizard: `ssPostInstall`
  → `install-app.ps1`). Der Wizard muss auf dem Host manuell geprüft werden.
- **Downgrade-Hinweis** (`0x80073D06`): Wird als Erfolg gewertet, weil
  `install-app.ps1` das bewusst so behandelt.
- Bluetooth gibt es in der Sandbox nicht — A2DP-Verbindungen lassen sich dort
  nicht testen, nur Installation und App-Start.
