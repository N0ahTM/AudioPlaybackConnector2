# Product Decisions

This file is the only source of product-scope decisions. `LOCKED` rules are mandatory. `GATE` rules require explicit approval before behavior changes.

## Locked

| ID | Rule |
|---|---|
| P01 | Keep CLI and named pipes first-class. Preserve commands, arguments, exit behavior, JSON schema, raw/privacy behavior, authentication, ACL/identity checks, framing bounds, concurrency, deadlines, cancellation, correlation IDs, replay/deduplication, response bounds, startup retry, and graceful shutdown. |
| P02 | Keep adaptive resources. Preserve Cold/Warm/Hot; configured pressure/recovery/preload delays; memory pressure; energy saver; fullscreen/presentation awareness; UI visibility/pinning; interaction hold; release deferral; freshness/sequence/authorization checks; clock rollback; scheduling fallback; failure retry; heartbeat and diagnostics. MUST NOT replace this with simple lazy loading or idle release. |
| P03 | Remove tray connecting animation, frame state, and animation timer. Keep static Idle, Connecting, Connected, and Error icons. |
| P04 | Normal disconnect updates state, tooltip, CLI, and diagnostics/logging but produces no toast or fallback balloon. |
| P05 | Keep a distinct image mapping for every retained notification kind. |
| P06 | Preserve Bluetooth operation ordering, serialized mutation, stale-completion epochs, close-before-reconnect, connection-token cleanup, incoming connections, manual cancellation of reconnect, retry policy, bulk actions, startup connect, suspend/resume, and safe shutdown. |
| P07 | Keep JSON settings compatibility, validation/bounds, corrupt-file preservation, no-lost-update behavior, temporary write, `FlushFileBuffers`, atomic replace, bounded retry, and synchronous suspend/shutdown flush. MUST NOT move settings to database or registry. |
| P08 | Preserve Privacy Mode/redaction, localization of every user-visible string, accessibility, theme/DPI behavior, and UI-thread-only XAML access. |
| P09 | Preserve tray left/right/double-click behavior and retained notification actions unless separately approved. |
| P10 | Target C++26, but keep C++23 until the dedicated x64/ARM64 toolchain gate passes. |

## Approval gates

Until a gate is approved, preserve current behavior behind the new boundary.

| ID | Recommendation | Must retain | Target |
|---|---|---|---:|
| G01 Logging | Bounded in-memory ring; persist warnings/errors; flush for export/crash. Remove general batch thread/queue/retry and routine stack capture unless measurements require them. | Timestamp, severity/context, Bluetooth timing history, development `OutputDebugString`, bounded private output, crash/diagnostic tail. | 200–300 lines |
| G02 Crash | Minimal local minidump reporter; avoid overlapping handlers, C signals/vectored handling without need, crash UI, GitHub URL, marker/prompt state, and multiple artifact pipelines. | Unhandled capture, `MiniDumpWriteDump`, timestamp/version/code, recent log tail, next-start dump indication. | 180–300 lines |
| G03 Settings | One snapshot owner, dirty revision, debounce timer, one writer, bounded retry, final flush. | All P07 guarantees. | 700–900 lines excluding UI |
| G04 Updates | Prefer manual check. Decide separately whether automatic checks remain. | Until approval, preserve current automatic behavior. Manual flow reports current/available/failure and opens trusted destination. | — |
| G05 Diagnostics | One privacy-aware report from immutable snapshots; avoid collector/builder layers without a concrete format need. | App/Windows version, device/connection/resource snapshots, important settings, recent logs, P08 redaction. | 150–250 lines |
| G06 Placement | Keep simple validated size/position persistence only if valued; avoid exact restoration coupling to general Settings. | Preserve current behavior until approval. | — |
| G07 Fallback | Decide whether tray-balloon fallback is required. | Toasts, P04, P05, retained actions, privacy, and localization. | — |

## Size guidance

Planning baseline by feature: Bluetooth 3,934; tray/picker 3,244; CLI/control 3,150; Settings UI 3,055; settings persistence 1,503; adaptive resources 1,069 plus host integration; updates 1,012; notifications 808; crash 773; logging 661; startup 500; diagnostics 490; suspend/resume 399 production lines.

- Full compatibility plus P01–P10: directional target 16,000–19,000 product lines.
- Approved lean G01–G07 profile: directional target 14,500–17,000.
- These are not quotas. Correctness, ownership, readability, and measured behavior win.
