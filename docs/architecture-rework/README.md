# Architecture Rework Plan

Status: planning baseline  
Branch: `docs/architecture-rework-plan`  
Base commit: `de1ac9ec2285b8fe953960c33433de6a9ff47972`

## Purpose

This document set defines a ground-up structural rework of AudioPlaybackConnector2 while preserving its product-critical behavior. The goal is not a small line count at any cost. The goal is a codebase whose normal execution path can be understood through a small number of cohesive components, with concurrency and platform complexity contained inside the feature that owns it.

The current baseline has approximately 25,000 lines across 138 hand-written C++/XAML source files. The rework targets roughly 16,000-19,000 product lines and 55-70 hand-written C++/XAML files, but those figures are directional rather than acceptance criteria.

## Locked product decisions

- CLI and named-pipe control remain first-class supported features.
- Named-pipe authentication, deadlines, cancellation, correlation IDs, request replay/deduplication behavior, JSON output, raw/privacy behavior, and command compatibility must not be weakened merely to reduce code.
- Adaptive resource management remains a first-class feature. Cold/Warm/Hot residency, memory pressure, energy saver, fullscreen/presentation awareness, UI pinning, freshness/sequence protection, retry scheduling, and diagnostics remain supported.
- Tray connecting animations are removed. The tray continues to expose distinct static visual states.
- A normal device disconnect does not produce a notification.
- Distinct toast images remain and continue to be selected by notification kind.
- Bluetooth operation ordering, stale-result protection, close-before-reconnect, incoming connections, reconnect policy, suspend/resume, privacy, localization, and atomic settings persistence remain supported unless a later explicit decision changes them.

## Engineering principles

1. Every mutable state has exactly one owner.
2. Feature modules accept commands and expose immutable snapshots plus typed events.
3. Threading, retries, generations, timers, and cancellation stay inside the module that owns the operation.
4. Small state machines may exist, but single-use helpers live privately in a `.cpp` file instead of becoming public architectural concepts.
5. Concrete constructor injection is preferred. Interfaces exist only for a genuine platform boundary, multiple implementations, or a valuable test seam.
6. There is no global service locator and no generic application-wide event bus.
7. UI components render prepared presentation models and emit user intent; they do not coordinate domain services.
8. No production `.cpp` file should normally exceed 1,200 lines. Exceptions require a written justification.
9. Existing behavior is characterized before its owner is replaced.
10. Each migration phase must build and be reviewable independently.

## Documents

- [Target architecture](target-architecture.md)
- [Feature scope and decisions](feature-scope-and-decisions.md)
- [Migration plan](migration-plan.md)
- [Verification plan](verification-plan.md)

## Definition of success

The rework is complete when:

- the startup-to-tray path can be followed through `AppRuntime`, `AppController`, `DeviceService`, and `TrayUi` without reading unrelated platform helpers;
- `AppRuntime` owns composition and lifecycle only;
- `AppController` owns application use cases but no Win32 transport, XAML, persistence, Bluetooth implementation, or resource probing;
- `DeviceService`, `SettingsStore`, `ResourceResidencyManager`, and `ControlServer` each own their mutable state and concurrency;
- UI and CLI execute the same application commands through one application API;
- all locked product decisions above have automated or documented manual coverage;
- the old implementation can be removed without compatibility shims remaining indefinitely;
- architecture documentation describes the code that actually ships.

## Scope control

This plan intentionally separates structural work from optional product reductions. Unless an item is listed as a locked decision, the migration preserves current behavior first. Proposed reductions such as simpler logging, a smaller crash reporter, or a manual-only update flow are decision gates in [Feature scope and decisions](feature-scope-and-decisions.md), not implicit authorization to remove behavior.
