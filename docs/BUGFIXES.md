# Fehlerübersicht – Unreleased

Technische Arbeitsliste, keine Behauptung über Fehler in Release 0.9.1 oder vollständig verifizierte historische Fixes. IDs 01–50 entsprechen der eingebrachten Sammlung; neue Befunde bekommen fortlaufende IDs. Öffentliche Änderungen stehen im [Changelog](../CHANGELOG.md).

## Status und Pflege

- **Offen:** bestätigter, nicht behobener Fehler.
- **In Arbeit:** Analyse/Umsetzung läuft.
- **Prüfung offen:** gemeldeter Befund/Fix noch nicht am aktuellen Stand verifiziert.
- **Abgeschlossen:** aktueller Codepfad, Fix-Commit und passender Test/Prüfbefehl mit Ergebnis belegt.

Ein Commit allein ist kein Abschluss. Regressionen öffnen dieselbe ID wieder; behobene Einträge bleiben für Release-Zuordnung erhalten. Vereinfachungen sind keine nachgewiesenen Fehler.

## Settings, Shutdown und Persistenz

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 01 | Settings-Shutdown: unbegrenztes Warten auf Datei-I/O, Subscriber oder Worker begrenzen. | Fehler | Abgeschlossen | `0a38f31` |
| 02 | Worker-Lebenszeit nach Shutdown-Timeout sichern, ohne später unbegrenzt zu warten. | Fehler | Abgeschlossen | `0a38f31` |
| 03 | Wake-up unmittelbar vor dem Warten darf nicht verloren gehen. | Fehler | Abgeschlossen | `076959d` |
| 04 | Settings-Snapshots als unveränderliche Revisionen veröffentlichen; Kopieren außerhalb von Locks. | Vereinfachung | Abgeschlossen | `255f337` |
| 05 | Bei fehlgeschlagener Sicherung beschädigter Settings Mutation und Flush sperren. | Fehler | Abgeschlossen | `d98def2` |
| 06 | Veraltete Reconnect-Konfiguration bei konkurrierenden Settings-Änderungen verwerfen. | Fehler | Abgeschlossen | `ff91b89` |
| 07 | Shutdown aus Callback darf nicht auf sich selbst warten; Stop-Anforderung vom Drain trennen. | Fehler | Abgeschlossen | `cf4b829` |
| 08 | Autostart-Persistenz-Callback außerhalb des Request-Locks ausführen. | Fehler | Abgeschlossen | `22e91ac` |
| 09 | Verspätete Geräteereignisse nach Shutdown von der UI fernhalten. | Fehler | Abgeschlossen | `0a133c5` |
| 10 | Ausschließlich aktuelles Settings-Format; inkompatible Dateien sichern, keine Altwerte übernehmen. | Gewollte Verhaltensänderung | Abgeschlossen | `a150a05` |
| 11 | Ungültige Geräte-IDs und Aliase vor dem Backend-Aufruf abweisen. | Fehler | Abgeschlossen | `7ae26a0` |

Nachweis 01–11 (2026-09-21): SettingsStoreTests decken blockierte I/O-/Subscriber-Budgets
(TestShutdownBudgetFencesBlockedLoad/-LateWriteCompletion/-IncludesBlockedSubscriber/-RetainsBlockedWriterLifetime,
TestFrozenPersistenceClockCannotExtendShutdownBudget, TestConcurrentShutdownCallerHasItsOwnBudget),
Wake-Version (TestManualDebounceAndIdleWait, ManualSettingsWakeup), blockierte Subscriber
(TestSubscriberInitiatedShutdownDoesNotDeadlock, TestCallbackCanDestroyStoreDuringPublicationDrain,
TestResetFencesBlockedCallback), Format- und Validierungsregeln (TestOnlyCurrentFormatIsAccepted,
TestDeviceIdValidationRejectsWithoutRevision, TestValidationAndLoadNormalizationMatrix) sowie
Policy-Verwerfung (TestSettingsPolicyRejectsOldDuplicateAndCancelledRevisions). Vollständiger CoreTests-Lauf
und zehn Stressläufe Seeds 920100–920109 grün (%TEMP%/apc-concurrency-19b10717fba2451d874f895ca31d9236).

## Geräte, Events und UI

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 12 | Abschluss von Geräteoperationen dauerhaft pro Operationsepoche erfassen. | Fehler | Abgeschlossen | `c5f53da` |
| 13 | Subscriber geordnet beliefern und laufende Callbacks beim Abmelden berücksichtigen. | Fehler | Abgeschlossen | `bc57e2f` |
| 14 | Lücke zwischen Snapshot und Subscription schließen; Settings im Controller-Ereignisstrom veröffentlichen. | Fehler | Abgeschlossen | `f98889b` |
| 15 | Verspätete Fehler-/Reconnect-Ereignisse dürfen neueren Sessionzustand nicht überschreiben. | Fehler | Abgeschlossen | `29d0016` |
| 16 | Disconnecting darf nicht als Connecting erscheinen. | Fehler | Abgeschlossen | `29d0016` |
| 17 | Zweite Geräteereignis-Historie entfernen; veraltete UI-Zustellung verhindern. | Vereinfachung / Fehler | Abgeschlossen | `9ee8212` |
| 18 | Selbst-Wartepfad beim Picker-Preload und Ressourcendiagnose-Zugriff entfernen. | Fehler | Abgeschlossen | `0bc9119` |
| 19 | Entfernte Geräte nicht weiter als inaktive Session-Zeilen im Picker anzeigen. | Fehler | Abgeschlossen | `4095a84` |
| 20 | Nicht mehr vorhandenes Standardgerät weiterhin zurücksetzen können. | Fehler | Abgeschlossen | `4095a84` |
| 21 | Tray-Symbol, Tooltip, Privacy und Gerätestatus aus demselben Snapshot rendern. | Fehler | Abgeschlossen | `0bc9119` |
| 22 | Gespeicherte Tray-Callbacks dürfen freigegebene Controller nicht erreichen. | Fehler | Abgeschlossen | `7c4d45d`, `4250f31` |
| 23 | Bridge, zentrale In-Process-Command-Verteilung und redundante Weiterleitungsschichten entfernen. | Vereinfachung | Abgeschlossen | `0a133c5`, `a3af825`, `ff91b89`, `9cd18e1` |

Nachweis 12–23 (2026-09-21): Operationsepochen (TestOperationEpochRejectsStaleCompletion,
TestCompletionRetainsFirstTerminalResult), geordnete Zustellung (TestSubscriptionsAreOrderedAndReentrant,
TestConcurrentAndReentrantPublicationsHaveOneOrder), Snapshot-/Subscription-Lücke
(TestSnapshotRegistrationReconcilesChangesDuringCapture, TestSnapshotRegistrationDoesNotReceiveOlderQueuedDelivery),
normalisierte Fakten (TestFactsCarryNormalizedSnapshots, TestFailureFactsRetainOperationKind),
Geräteentfernung und Standard-Reset (TestDeviceRemovalClosesCurrentSessionAndRejectsLateCallbacks,
TestTargetResolutionAndDefaultModes, TestQueriesReconcileAllProjectedFieldsAfterOwnerChanges),
Snapshot-Autorität und Lebenszeiten (INVARIANTS „Device snapshot authority“/„Controller event delivery“,
weak-Controller-Zugriff im TrayController). CoreTests- und Stressläufe grün (Nachweis wie 01–11).

## Logging und Crashhandler

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 24 | Logger-Shutdown bei blockiertem Datei-I/O begrenzen; laufende Ressourcen erhalten. | Fehler | Abgeschlossen | `61fae1b`, `b176b12` |
| 25 | Globale Logger-Zugänge entfernen; späte Schreibaufrufe nach Shutdown wirkungslos machen. | Fehler | Abgeschlossen | `b176b12` |
| 26 | Native UTF-16-Logpfade mit Umlauten zuverlässig verarbeiten. | Fehler | Abgeschlossen | `61fae1b` |
| 27 | Crashpfad von Async-Logger, Queue und normalen Logger-Locks unabhängig machen. | Robustheit | Abgeschlossen | `b176b12` |
| 28 | Crashhandler sicher abmelden und vorherige Handler wiederherstellen. | Fehler | Abgeschlossen | `b176b12` |
| 29 | Zeichenoffset beim Lesen gespeicherter Fehlercodes korrigieren. | Fehler | Abgeschlossen | `b176b12` |
| 30 | Lange Crashlog-Zeilen ohne ungültige UTF-8-Endsequenzen kürzen. | Fehler | Abgeschlossen | `b176b12` |
| 31 | Redundante Locks im UI-gebundenen Tray-Pfad entfernen. | Vereinfachung | Abgeschlossen | `0bc9119`, `774646c`, `eac2535`, `edf28de` |

Nachweis 24–31 (2026-09-21): LoggerTests (Rotation, blockierte exklusive Sperre, Besitzerfreigabe,
Unicode-Pfade, Notfall-Puffer nach Logger-Freigabe), CrashHandlerTests mit fünf nativen
Crash-Unterprozessen (echte Dumps und Notfall-Einträge nach Logger-Zerstörung), INVARIANTS
„Logging ownership and native crash registration“. CoreTests- und Stressläufe grün (Nachweis wie 01–11).

## Theme und Sprache

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 32 | Globale Theme-Handlerliste durch direkte UI-Zustellung ersetzen. | Vereinfachung | Abgeschlossen | `ed73d1f` |
| 33 | Sprachressourcen mit explizitem Besitzer statt globalem Zugriff bereitstellen. | Robustheit | Abgeschlossen | `d6b3f82` |
| 34 | Ressourcen vor Aktualisierung der übersetzten Fenstertexte austauschen. | Fehler | Abgeschlossen | `d6b3f82` |

Nachweis 32–34 (2026-09-21): StringResources-Suite (acht Sprachen, English-Fallback, parallele
Publikation und Lebenszeit), Theme-Besitz in der UI, INVARIANTS „Tray theme delivery“/„Localization ownership“,
Sprach-Gate 0b8fb00 (vollständige identische Schlüsselsätze, validate/test-localizations grün).

## CLI, Pipe und Sicherheit

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 35 | CLI11-Umstellung: Akzeptanz, `--id -- -device`, Fehlertexte und Exitcodes bewahren. | Rewrite-Regression | Abgeschlossen | `a4b10da` |
| 36 | Replay-Identität prüfen und unzulässigen Start der Paket-App durch ungepackte Clients verhindern. | Sicherheitsgrenze | Abgeschlossen | `376ee51` |
| 37 | Pipe-, Event- und Threadpool-Ressourcen mit eindeutigem WIL-Besitz verwalten. | Vereinfachung | Abgeschlossen | `637ce50` |
| 38 | Aktiven Pipe-Cache-Eintrag zwischen Ergebnisauswahl und Zustellungsbesitz gegen Verdrängung schützen. | Fehler | Abgeschlossen | `0afb169` |
| 39 | Pipe-Testhooks durch reale Transport-/Timer-Grenzen ersetzen. | Testqualität | Abgeschlossen | `41672c4`; Fortsetzung `65d282e`, `63d8c1c`; siehe 50 |
| 40 | Testmakro darf ungepackte Clients nicht an der echten Release-Policy vorbeilassen. | Testqualität / Sicherheitsgrenze | Abgeschlossen | `dab37c4` |
| 41 | Globalen veränderlichen Identitäts-/Paketstatus-Cache entfernen. | Vereinfachung | Abgeschlossen | `309f237` |

Nachweis 35–41 (2026-09-21): CLI-Goldenmatrix und CliParser-Suiten (Acceptance/Normalisierung/Exitcodes,
`update-cli-reference.ps1 -Check` grün), CommandPipeSecurity-Suite (DACL, Rogue-Instanz, Trust-Prädikate,
`eabbf0f`), Server-Suite mit Produktions-Trust-Ablehnung ungepackter Clients, Wire-Vertrag gebündelt in
CommandProtocol.hpp (`5ac59f9`), Auto-Matching über einen Vertrag (`cc79707`), Begrenzung des Test-Hosts ohne
Policy-Abschwächung. CoreTests- und Stressläufe grün (Nachweis wie 01–11).

## Build, Tests und CI

| ID | Gemeldeter Befund / Änderung | Art | Status | Behebungsreferenz |
| --- | --- | --- | --- | --- |
| 42 | C++/WinRT-Abhängigkeitszyklus im parallelen Build auflösen. | Buildfehler | Abgeschlossen | `6527a7e` |
| 43 | Gleichzeitiges Schreiben derselben PCH durch App- und Paketbuild verhindern. | Buildfehler | Abgeschlossen | `ca480d0` |
| 44 | Boundary-Prüfer muss projektspezifische Include-/PCH-Kontexte berücksichtigen. | Prüffehler | Abgeschlossen | `e770ada` |
| 45 | Architektur-Restores dürfen einander keine vcpkg-Pakete entfernen. | Buildfehler | Abgeschlossen | `a150a05` |
| 46 | Tests und App mit derselben gepinnten C++/WinRT-Projektion bauen. | Testqualität | Abgeschlossen | `12ebacf` |
| 47 | Apartment-Lebenszeit im Test-Runner über alle WinRT-verwendenden Suites erhalten. | Testabsturz | Abgeschlossen | `e9b5320` |
| 48 | Erfolgreich bestandene Negativtests dürfen keinen Fehler-Exitcode hinterlassen. | Prüffehler | Abgeschlossen | `03b5e9c` |
| 49 | Release-Metadata-Prüfung für LF/CRLF und mehrere PropertyGroups korrigieren. | Prüffehler | Abgeschlossen | `6f64e89`; Versionsvertrag inzwischen geändert in `b15365c` |

Nachweis 42–49 (2026-09-21): parallele Gesamtbuilds (`msbuild AudioPlaybackConnector2.slnx -m -t:Rebuild`)
für x64 und ARM64 jeweils 0 Warnungen/0 Fehler (%TEMP%/apc-final-parallel-{x64,arm64}.log) — 42/43 damit
nicht nur seriell belegt; Boundary-Selbsttest grün (%TEMP%/gate-boundary.log); isolierte Triplet-Restores
x64/ARM64 hintereinander ohne Paketverlust; gemeinsame gepinnte Paketversionen für App und Tests;
RuntimeApartment im Runner; `test-release-metadata.ps1` und `test-build-version.ps1` grün.
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

## Neue Befunde

| ID | Befund | Art | Status | Nachweis / nächster Schritt |
| --- | --- | --- | --- | --- |
| 51 | MSVC-Analyse scheitert mit PCH-spezifischem WIL-Diagnostiklevel 1 an SAL-/Template-Deklarationen. | Analyse-/Buildfehler | Abgeschlossen | PCH-Sonderdefinition entfernt (Commit dieser Dokumentationsänderung); SAL-/Template-Parserfehler im erneuten Lauf beseitigt. x64-/ARM64-Gesamtbuilds jeweils ohne Warnungen/Fehler; vollständige Analyse bleibt separat offen (siehe unten). |
| 52 | Reentrant eingereihte Connect-/Disconnect-/Reconnect-/CancelReconnect-Aufrufe lieferten eine bereits verschobene, leere Geräte-ID zurück. | Rewrite-Regression | Abgeschlossen | `902e767`; Test `TestReentrantCommandsRetainDeviceIdentity`: vier Fehler vor dem Fix, danach erfolgreich (Seed 920001). Vollständiger CoreTests-Lauf mit Seed 920002 und gezielter Cppcheck-Lauf bestanden; x64-/ARM64-Gesamtbuilds ohne Warnungen/Fehler. |
| 53 | Dispatch packte eine leere Selektor-Option aus, wenn die Wire-Grammatik eine Nutzlast akzeptierte, die die strengere Selektor-Validierung ablehnte (Steuerzeichen). | Fehler | Abgeschlossen | `38c841d`; MSVC-Lifetime-Regel C26830 meldete die Stelle, Zielbefehle ohne gültigen Selektor werden jetzt mit InvalidInput abgewiesen. Adapter- und Server-Suiten im vollständigen CoreTests-Lauf grün. |

Die MSVC-Analyse mit dem Concurrency-/Lifetime-Regelsatz ist seit `38c841d` **bestanden** (x64,
Exit 0, %TEMP%/apc-msvc-analysis3.log). Zuvor lagen 1.913 bzw. 6.372 Meldungen vor; die Triage ergab
drei echte erste-Party-Befunde (leere Option im Dispatch, Besitzübergabe bei Publish-Ausnahme im
DeviceWatcher, Iterator-/Zeigerformen in Codec und Server) und eine Regel (C26823), die tausendfach in
der gepinnten WIL-Implementierung ausgelöst wird. C26823 ist mit Begründung im Regelsatz deaktiviert;
NuGet-/vcpkg-Header sind als extern markiert (`/analyze:external-` bei aktivierter Analyse). Es wurden
keine pauschalen Unterdrückungen hinzugefügt.

## Offene Release-Abnahme

Diese Aufgaben sind keine zusätzlich behaupteten Produktfehler:

- **Erledigt (2026-09-21):** historische Einträge 01–49 am aktuellen Code geprüft und mit Nachweisen abgeschlossen.
- **Erledigt (2026-09-21):** statische Analyse grün — MSVC-Lifetime/Concurrency Exit 0 (`38c841d`), clang-tidy 0 Befunde in eigenen Quellen (`1720dec`), Cppcheck 2.20.0 ohne Befund.
- **Erledigt (2026-09-21):** lokale Paket-/Lieferkettennachweise — parallele x64-/ARM64-Gesamtbuilds, Paketprüfung beider Architekturen, Notices, SBOM, Lizenzen, OSV (15 Abfragen, 0 Advisories).
- **Offen:** abschließender GitHub-CI-/CodeQL-Lauf nach lokaler Vorbereitung.
- **Belegt statt ausgeführt:** native WinUI-, Installations- und Hardwareprüfungen sind innerhalb der Grenzen nicht ausführbar (interaktive UI-Sitzung, Zertifikatsvertrauen, ARM64-Hardware, Store-/Feed-Publikation); siehe Plan.md Restliste 11.

Es gibt aktuell keinen allein aus dieser Liste neu bestätigten, unbehandelten Produktfehler. „Prüfung offen“
bedeutet ausdrücklich nicht, dass Fehlerfreiheit oder eine Behebung nachgewiesen wäre. Die tatsächliche
Release-Version und die öffentliche Zusammenfassung werden erst bei der Release-Vorbereitung zugeordnet.
