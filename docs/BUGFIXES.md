# Fehlerübersicht – Unreleased

Technische Arbeitsliste für den nächsten, noch nicht versionierten Release. Die Nummern 01–50 entsprechen
der eingebrachten Sammlung. Sie behaupten weder, dass alle Probleme im veröffentlichten Release 0.9.1
bestanden, noch dass jeder historische Fix im aktuellen Stand vollständig geprüft ist.

## Status und Pflege

- **Offen:** bestätigter Fehler ohne abgeschlossene Behebung.
- **In Arbeit:** konkrete Analyse oder Umsetzung läuft.
- **Prüfung offen:** gemeldete Behebung; aktueller Befund, Fix oder erforderliche Verifikation noch zu prüfen.
- **Abgeschlossen:** Behebung und passende Prüfung sind belegt; Nachweis und Fix-Commit stehen am Eintrag.

Ein vorhandener Commit allein schließt keinen Eintrag. Beim Abschluss werden aktueller Codepfad,
Regressionstest beziehungsweise reproduzierbarer Prüfbefehl und dessen Ergebnis ergänzt. Bei einer
Regression wird derselbe Eintrag wieder geöffnet. „Vereinfachung“ bezeichnet keine nachgewiesene Fehlfunktion.
Neue Befunde erhalten die nächste freie ID. Behobene Einträge bleiben für die Release-Zuordnung erhalten.

Die folgenden historischen Zeilen sind zunächst gemeldete Befunde und Behebungsreferenzen. Noch fehlende
Prüfnachweise sind ausdrücklich keine erfolgreichen Tests. Die öffentliche Zusammenfassung steht im
[Changelog](../CHANGELOG.md); bekannte Einschränkungen werden vor Veröffentlichung daraus abgeleitet.

## Settings, Shutdown und Persistenz

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 01 | Settings-Shutdown: unbegrenztes Warten auf Datei-I/O, Subscriber oder Worker begrenzen. | Fehler | Prüfung offen | `0a38f31` |
| 02 | Worker-Lebenszeit nach Shutdown-Timeout sichern, ohne später unbegrenzt zu warten. | Fehler | Prüfung offen | `0a38f31` |
| 03 | Wake-up unmittelbar vor dem Warten darf nicht verloren gehen. | Fehler | Prüfung offen | `076959d` |
| 04 | Settings-Snapshots als unveränderliche Revisionen veröffentlichen; Kopieren außerhalb von Locks. | Vereinfachung | Prüfung offen | `255f337` |
| 05 | Bei fehlgeschlagener Sicherung beschädigter Settings Mutation und Flush sperren. | Fehler | Prüfung offen | `d98def2` |
| 06 | Veraltete Reconnect-Konfiguration bei konkurrierenden Settings-Änderungen verwerfen. | Fehler | Prüfung offen | `ff91b89` |
| 07 | Shutdown aus Callback darf nicht auf sich selbst warten; Stop-Anforderung vom Drain trennen. | Fehler | Prüfung offen | `cf4b829` |
| 08 | Autostart-Persistenz-Callback außerhalb des Request-Locks ausführen. | Fehler | Prüfung offen | `22e91ac` |
| 09 | Verspätete Geräteereignisse nach Shutdown von der UI fernhalten. | Fehler | Prüfung offen | `0a133c5` |
| 10 | Ausschließlich aktuelles Settings-Format; inkompatible Dateien sichern, keine Altwerte übernehmen. | Gewollte Verhaltensänderung | Prüfung offen | `a150a05` |
| 11 | Ungültige Geräte-IDs und Aliase vor dem Backend-Aufruf abweisen. | Fehler | Prüfung offen | `7ae26a0` |

## Geräte, Events und UI

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 12 | Abschluss von Geräteoperationen dauerhaft pro Operationsepoche erfassen. | Fehler | Prüfung offen | `c5f53da` |
| 13 | Subscriber geordnet beliefern und laufende Callbacks beim Abmelden berücksichtigen. | Fehler | Prüfung offen | `bc57e2f` |
| 14 | Lücke zwischen Snapshot und Subscription schließen; Settings im Controller-Ereignisstrom veröffentlichen. | Fehler | Prüfung offen | `f98889b` |
| 15 | Verspätete Fehler-/Reconnect-Ereignisse dürfen neueren Sessionzustand nicht überschreiben. | Fehler | Prüfung offen | `29d0016` |
| 16 | Disconnecting darf nicht als Connecting erscheinen. | Fehler | Prüfung offen | `29d0016` |
| 17 | Zweite Geräteereignis-Historie entfernen; veraltete UI-Zustellung verhindern. | Vereinfachung / Fehler | Prüfung offen | `9ee8212` |
| 18 | Selbst-Wartepfad beim Picker-Preload und Ressourcendiagnose-Zugriff entfernen. | Fehler | Prüfung offen | `0bc9119` |
| 19 | Entfernte Geräte nicht weiter als inaktive Session-Zeilen im Picker anzeigen. | Fehler | Prüfung offen | `4095a84` |
| 20 | Nicht mehr vorhandenes Standardgerät weiterhin zurücksetzen können. | Fehler | Prüfung offen | `4095a84` |
| 21 | Tray-Symbol, Tooltip, Privacy und Gerätestatus aus demselben Snapshot rendern. | Fehler | Prüfung offen | `0bc9119` |
| 22 | Gespeicherte Tray-Callbacks dürfen freigegebene Controller nicht erreichen. | Fehler | Prüfung offen | `7c4d45d`, `4250f31` |
| 23 | Bridge, zentrale In-Process-Command-Verteilung und redundante Weiterleitungsschichten entfernen. | Vereinfachung | Prüfung offen | `0a133c5`, `a3af825`, `ff91b89`, `9cd18e1` |

## Logging und Crashhandler

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 24 | Logger-Shutdown bei blockiertem Datei-I/O begrenzen; laufende Ressourcen erhalten. | Fehler | Prüfung offen | `61fae1b`, `b176b12` |
| 25 | Globale Logger-Zugänge entfernen; späte Schreibaufrufe nach Shutdown wirkungslos machen. | Fehler | Prüfung offen | `b176b12` |
| 26 | Native UTF-16-Logpfade mit Umlauten zuverlässig verarbeiten. | Fehler | Prüfung offen | `61fae1b` |
| 27 | Crashpfad von Async-Logger, Queue und normalen Logger-Locks unabhängig machen. | Robustheit | Prüfung offen | `b176b12` |
| 28 | Crashhandler sicher abmelden und vorherige Handler wiederherstellen. | Fehler | Prüfung offen | `b176b12` |
| 29 | Zeichenoffset beim Lesen gespeicherter Fehlercodes korrigieren. | Fehler | Prüfung offen | `b176b12` |
| 30 | Lange Crashlog-Zeilen ohne ungültige UTF-8-Endsequenzen kürzen. | Fehler | Prüfung offen | `b176b12` |
| 31 | Redundante Locks im UI-gebundenen Tray-Pfad entfernen. | Vereinfachung | Prüfung offen | Commitzuordnung fehlt |

## Theme und Sprache

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 32 | Globale Theme-Handlerliste durch direkte UI-Zustellung ersetzen. | Vereinfachung | Prüfung offen | `ed73d1f` |
| 33 | Sprachressourcen mit explizitem Besitzer statt globalem Zugriff bereitstellen. | Robustheit | Prüfung offen | `d6b3f82` |
| 34 | Ressourcen vor Aktualisierung der übersetzten Fenstertexte austauschen. | Fehler | Prüfung offen | `d6b3f82` |

## CLI, Pipe und Sicherheit

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 35 | CLI11-Umstellung: Akzeptanz, `--id -- -device`, Fehlertexte und Exitcodes bewahren. | Rewrite-Regression | Prüfung offen | `a4b10da` |
| 36 | Replay-Identität prüfen und unzulässigen Start der Paket-App durch ungepackte Clients verhindern. | Sicherheitsgrenze | Prüfung offen | `376ee51` |
| 37 | Pipe-, Event- und Threadpool-Ressourcen mit eindeutigem WIL-Besitz verwalten. | Vereinfachung | Prüfung offen | `637ce50` |
| 38 | Aktiven Pipe-Cache-Eintrag zwischen Ergebnisauswahl und Zustellungsbesitz gegen Verdrängung schützen. | Fehler | Prüfung offen | `0afb169` |
| 39 | Pipe-Testhooks durch reale Transport-/Timer-Grenzen ersetzen. | Testqualität | Prüfung offen | `41672c4`; Fortsetzung `65d282e`, `63d8c1c`; siehe 50 |
| 40 | Testmakro darf ungepackte Clients nicht an der echten Release-Policy vorbeilassen. | Testqualität / Sicherheitsgrenze | Prüfung offen | `dab37c4` |
| 41 | Globalen veränderlichen Identitäts-/Paketstatus-Cache entfernen. | Vereinfachung | Prüfung offen | `309f237` |

## Build, Tests und CI

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 42 | C++/WinRT-Abhängigkeitszyklus im parallelen Build auflösen. | Buildfehler | Prüfung offen | `6527a7e` |
| 43 | Gleichzeitiges Schreiben derselben PCH durch App- und Paketbuild verhindern. | Buildfehler | Prüfung offen | `ca480d0` |
| 44 | Boundary-Prüfer muss projektspezifische Include-/PCH-Kontexte berücksichtigen. | Prüffehler | Prüfung offen | `e770ada` |
| 45 | Architektur-Restores dürfen einander keine vcpkg-Pakete entfernen. | Buildfehler | Prüfung offen | `a150a05` |
| 46 | Tests und App mit derselben gepinnten C++/WinRT-Projektion bauen. | Testqualität | Prüfung offen | `12ebacf` |
| 47 | Apartment-Lebenszeit im Test-Runner über alle WinRT-verwendenden Suites erhalten. | Testabsturz | Prüfung offen | `e9b5320` |
| 48 | Erfolgreich bestandene Negativtests dürfen keinen Fehler-Exitcode hinterlassen. | Prüffehler | Prüfung offen | `03b5e9c` |
| 49 | Release-Metadata-Prüfung für LF/CRLF und mehrere PropertyGroups korrigieren. | Prüffehler | Prüfung offen | `6f64e89`; Versionsvertrag inzwischen geändert in `b15365c` |
| 50 | Separate Pipe-Testkompilierung und Testlayout entfernt; Produktion aus CoreRuntime verwenden. | Testqualität | Abgeschlossen | `63d8c1c`, Schutz gegen doppelte Kompilierung `103ed63` |

### Prüfnachweis zu 50

Die letzte separate Server-Kompilierung und `APC_COMMAND_PIPE_SERVER_TESTING` wurden entfernt. Für alle
46 Produktionsdateien unter `AudioPlaybackConnector2/src` wurde genau ein kompilierendes Projekt festgestellt.
Die Pipe-Suite mit Seed 919076 und drei vollständige Testläufe mit Seeds 919077–919079 bestanden; x64/ARM64
bauten ohne Warnungen oder Fehler. ARM64 ist dabei ein Cross-Build, kein ausgeführter ARM64-Testlauf.

Reproduzierbare Prüfungen:

```powershell
pwsh ./scripts/test-core-runtime-boundary.ps1 -VerifierPath ./scripts/verify-core-runtime-boundary.ps1
pwsh ./scripts/verify-core-runtime-boundary.ps1 -ProjectPath ./AudioPlaybackConnector2.CoreTests/AudioPlaybackConnector2.CoreTests.vcxproj
./x64/Release/tests/AudioPlaybackConnector2.CoreTests.exe --suite CommandLineControlServer --seed 919076
```

Diese Abnahme betrifft die gemeinsame Pipe-Implementierung und die deklarierte Quellzuordnung. Sie behauptet
keine vollständige Release-Abnahme und keine Prüfung jedes früheren Test-Hooks aus Eintrag 39.

## Offene Release-Abnahme

Diese Aufgaben sind keine zusätzlich behaupteten Produktfehler:

- **In Arbeit:** historische Einträge am aktuellen Code und ihren Tests überprüfen; Abschlussnachweise ergänzen.
- **Offen:** vollständige Plan-Abnahme, verbleibende statische Analyse und Paket-/Lieferkettennachweise.
- **Offen:** abschließender GitHub-CI-/CodeQL-Lauf nach lokaler Vorbereitung.
- **Offen:** native WinUI-, Installations- und Hardwareprüfungen durchführen oder nicht ausführbare Prüfungen belegen.

Es gibt aktuell keinen allein aus dieser Liste neu bestätigten, unbehandelten Produktfehler. „Prüfung offen“
bedeutet ausdrücklich nicht, dass Fehlerfreiheit oder eine Behebung nachgewiesen wäre. Die tatsächliche
Release-Version und die öffentliche Zusammenfassung werden erst bei der Release-Vorbereitung zugeordnet.
