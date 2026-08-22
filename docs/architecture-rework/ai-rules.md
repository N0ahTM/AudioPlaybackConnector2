# AI Implementation Rules

These rules are mandatory for new rework code and substantially migrated legacy files.

## Before changing code

- MUST read [decisions.md](decisions.md).
- MUST read [architecture.md](architecture.md) for structural or cross-module work.
- MUST read the active phase in [migration.md](migration.md).
- MUST read the relevant checks in [verification.md](verification.md).
- MUST preserve behavior unless `decisions.md` explicitly changes it.
- MUST characterize high-risk legacy behavior before replacing its owner.
- MUST NOT claim success without running the stated validation.

## Design

- Optimize for the lowest total comprehension cost, not minimum lines, files, or classes.
- Give every mutable state exactly one owner.
- Pass commands in; expose immutable snapshots, typed results, and typed fact events.
- Keep threading, timers, retries, cancellation, generations, and shutdown inside their owning module.
- MUST NOT add a global service locator or generic application event bus.
- UI renders prepared models and emits intent; it MUST NOT coordinate domain services.
- UI and CLI MUST call the same `AppController` use cases.
- Prefer concrete constructor injection.
- Add an abstraction only if it names a domain concept, owns lifecycle/state, enforces an invariant, makes invalid states difficult, isolates a real boundary, removes meaningful duplication, separates different reasons to change, or creates a necessary test seam.
- A single-implementation interface is allowed for a real OS, process, protocol, storage, or test boundary.
- MUST NOT add speculative factories, managers, providers, adapters, interfaces, or pass-through wrappers.
- Keep one-use helpers and types private in the owning `.cpp` file.
- Prefer composition. Runtime polymorphism requires a concrete reason.

## Names and types

- Use complete, purpose-revealing names; avoid `cfg`, `mgr`, `ctx`, `proc`, `tmpObj`, and `devMgr`.
- Established terms are allowed: ID, URL, URI, HTTP, JSON, CPU, GPU, UI, CLI, IPC, RAII, GUID, JPEG, DPI, HWND, WinRT, XAML, A2DP, MCU, and protocol terms.
- Use one term and casing for each concept.
- Name values by meaning, not type. Include units when the type does not.
- Name booleans as positive predicates: `is`, `has`, `can`, `should`, `requires`.
- Use `enum class` for modes, states, policies, reasons, outcomes, and ambiguous public booleans.
- Use an options type for several independent switches. MUST NOT expose calls like `Open(true, false, true)`.
- Prefer domain types, `std::chrono`, value semantics, and explicit ownership.
- Use `[[nodiscard]]`, `const`, `constexpr`, `consteval`, `noexcept`, and ref qualifiers only when they enforce a real contract.
- MUST NOT return a reference, iterator, span, or view with unclear lifetime.

## Ownership and concurrency

- Use RAII for memory, handles, files, locks, registrations, callbacks, COM, timers, and background work.
- Prefer stack/value ownership, then `std::unique_ptr`; use `std::shared_ptr` only for genuine shared lifetime.
- MUST NOT use owning raw pointers or manual `new`/`delete` in application code.
- Non-owning references/pointers require an obvious lifetime contract.
- Break ownership cycles explicitly. `weak_ptr` does not define ownership.
- State each mutable owner's execution context.
- Marshal once at the module boundary.
- MUST NOT hold a lock while calling external code, publishing, awaiting, doing I/O, doing lengthy work, or touching UI unless a documented invariant requires it.
- Background work MUST have owner, cancellation, completion, supersession, and shutdown behavior.
- Detached work is allowed only for deliberate documented process lifetime.
- Timer callbacks MUST NOT outlive accessed state. `Sleep` is not synchronization.
- Use private epochs/generations where stale async completion is possible.

## Errors, security, and privacy

- Use typed, actionable error categories for expected failures.
- Distinguish disconnect, cancellation, timeout, unavailable resource, invalid input, and internal defect when callers react differently.
- Use exceptions for exceptional construction/platform failures, not normal control flow.
- Catch only to recover, translate, add context, or terminate safely. MUST NOT silently swallow failures.
- Retry only transient failures with bounded policy and observable final result.
- Preserve platform error context when translating.
- Validate all process, protocol, file, and external-data boundaries before allocation or use.
- MUST preserve named-pipe identity/ACL checks, framing bounds, deadlines, cancellation, and replay/correlation security.
- MUST NOT log secrets, raw external payloads by default, or unredacted device identity in Privacy Mode.
- Timeout/cancellation MUST NOT permit an unreported late mutation.
- Security and atomic data writes MUST NOT be weakened to reduce code.

## Code shape

- Prefer early returns and explicit control flow over nesting or clever one-liners.
- Let `clang-format` own whitespace; MUST NOT hand-align for appearance.
- Functions should normally stay near 50 lines.
- Production `.cpp` files should normally stay below 1,200 lines; XAML code-behind below 800.
- Limits are review signals, not reasons to split cohesive logic. Document justified exceptions.
- Public headers expose minimal contracts; implementation-only types stay in `.cpp`.
- Colocate headers and implementations by feature; keep only genuinely shared protocol headers public.
- Comments explain non-obvious constraints, invariants, workarounds, security, or compatibility; they MUST NOT narrate code.
- Use this heading only for meaningful multi-item groups:

```cpp
// -------------------------------------------------------------------------------------------------
// Public operations
// -------------------------------------------------------------------------------------------------
```

- `#pragma region Name ---------------------------------------------------------------------------` is optional for substantial class groups only. MUST NOT nest it without clear navigation benefit or use it for one trivial item.

## Dependencies and UI

- Depend on the smallest stable facade; MUST NOT include another feature's internals.
- WinUI/XAML types MUST NOT enter Core contracts.
- Pipe transport MUST NOT enter `AppController`.
- Settings persistence MUST NOT call device or UI code.
- Diagnostics reads snapshots; it MUST NOT acquire feature locks.
- New third-party dependencies require written benefit, deployment impact, maintenance status, and a standard/Windows alternative comparison.
- Every user-visible string MUST use localized resources in every supported language.
- XAML access occurs only on the UI thread.
- Accessibility, keyboard behavior, theme, DPI, and Privacy Mode are completion requirements.

## Language, tests, and performance

- C++23 (`stdcpp23`) is the current baseline.
- C++26 becomes mandatory only after one dedicated toolchain change passes Release builds, tests, packaging, and CI on x64 and ARM64.
- MUST NOT use a C++26 feature unsupported by any shipping architecture/toolchain component.
- Tests MUST describe one behavior, avoid timing luck, and deterministically cover races where practical.
- Every fixed race requires a regression test.
- Protocol/security changes require positive and negative tests.
- Resource-policy changes require transition, freshness, sequence, and retry tests.
- Measure before and after structural or performance-sensitive changes using comparable Release scenarios.

## Review gate

- One owner per mutable state?
- Thread, lifetime, cancellation, supersession, and shutdown explicit?
- New abstraction lowers total comprehension cost?
- Dependency direction allowed by `architecture.md`?
- Locked decisions and security preserved?
- Errors observable and retries bounded?
- Strings localized and UI thread-safe?
- Cheapest deterministic tests added?
- Required build, tests, manual checks, and performance measurements actually run?
