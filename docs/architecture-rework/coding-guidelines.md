# Coding Guidelines for the Architecture Rework

These rules are mandatory for new rework code and for legacy code when a file is substantially migrated. They complement the repository build, localization, formatting, and warnings-as-errors rules.

## 1. Names communicate intent

- Use complete words. Do not invent abbreviations to make identifiers shorter.
- Conventional technical terms are allowed when they are clearer than the expanded form: `UI`, `ID`, `JSON`, `HTTP`, `URI`, `CPU`, `DPI`, `HWND`, `WinRT`, `XAML`, `A2DP`, and protocol-defined terms.
- Follow one spelling for an established term. Do not mix `Id`, `ID`, `Identifier`, and `DeviceKey` for the same concept inside one API.
- Name values after their meaning, not their type: `deviceId`, not `stringValue`; `retryDeadline`, not `timePoint`.
- Include units in names whenever the type does not encode them: `timeoutMilliseconds`, `payloadBytes`, `workingSetBytes`.
- Boolean values use positive predicates: `isConnected`, `canReconnect`, `shouldNotify`, `hasPendingWrite`.
- Avoid vague production names such as `Helper`, `Utils`, `Common`, `Misc`, `DataManager`, `Processor`, or `Thing`.
- `Manager`, `Service`, `Controller`, and `Context` require a precise owned responsibility. A name must still explain what is managed, served, controlled, or contextualized.
- A function name describes its observable result: `PublishDeviceSnapshot`, `ScheduleReconnect`, `TryLoadSettings`. Avoid generic verbs such as `Handle`, `Process`, or `DoWork` unless the noun makes the exact event/use case clear.

Good:

```cpp
ReconnectDecision CalculateReconnectDecision(ReconnectInput const& input);
bool isDevicePickerVisible;
std::chrono::milliseconds connectionTimeout;
```

Avoid:

```cpp
Result Proc(Ctx& ctx, bool flag, int value);
bool state;
int timeout;
```

## 2. Prefer domain types over primitive ambiguity

- Do not expose multiple boolean parameters in a public function.
- Use `enum class` when a value selects a mode, reason, phase, policy, or outcome.
- Use a named options structure when several independent switches are genuinely required.
- Keep a `bool` for a true predicate or stored yes/no fact; do not replace every boolean member with an enum mechanically.
- Use scoped enums and handle every enumerator explicitly. Do not add a generic `Default` or `Unknown` state unless it is a real domain state.
- Do not use integers as status codes, modes, time units, sizes, or device identities when a stronger type is practical.
- Use `std::chrono` durations/time points for time and explicit byte/count types for sizes.

Good:

```cpp
enum class DisconnectReason {
    UserRequest,
    UnexpectedConnectionLoss,
    DeviceRemoved,
    ProcessShutdown,
};

void Disconnect(DeviceId deviceId, DisconnectReason reason);
```

Avoid:

```cpp
void Disconnect(std::wstring id, bool unexpected, bool shutdown, int reason);
```

## 3. One owner for mutable state

- Every mutable state has one obvious owning class or object.
- Do not mirror the same truth in `AppRuntime`, UI, and a feature service.
- Other modules receive immutable snapshots, typed events, or command results.
- A cache must identify its source, invalidation rule, and owner.
- Global mutable state is prohibited except for narrowly justified process primitives that cannot be represented otherwise.
- Use RAII for event tokens, handles, timers, registrations, and cancellation lifetimes where practical.
- Ownership must be visible from member types: prefer values and `std::unique_ptr`; use `std::shared_ptr` only for genuinely shared lifetime.
- A `weak_ptr` is not a substitute for defining ownership.

## 4. Threading is part of the contract

- Every state-owning component declares its execution context: UI thread, serialized device context, worker, or internally synchronized.
- Marshal at the module boundary, then keep the implementation on its declared context.
- Never hold a lock while invoking an external callback, publishing an event, awaiting a coroutine, or calling UI code.
- Do not use detached work without an explicit lifetime, cancellation, and completion policy.
- Every long-running or asynchronous operation defines what happens during shutdown and superseding requests.
- Generation/epoch tokens are used where stale asynchronous completions are possible, but remain private to the owning operation/session.
- Timer callbacks must not dereference an object whose lifetime is not guaranteed.
- `Sleep` is not synchronization. A timed delay is allowed only when delay itself is the required behavior.

## 5. Commands in, snapshots and events out

- Feature facades accept typed commands and expose immutable snapshots plus one cohesive typed event stream.
- UI renders presentation snapshots and emits intent. UI does not reach into settings locks, device sessions, pipe instances, or resource-policy internals.
- CLI and UI invoke the same `AppController` use cases.
- Events report facts that already happened. Commands request behavior. Do not use events as disguised commands.
- Event payloads contain enough context to understand the event without querying mutable internals during the callback.
- Do not introduce a generic global event bus.

## 6. Abstractions must pay for themselves

Create a class or interface only when at least one is true:

- it owns state and a lifecycle;
- it enforces a domain invariant;
- it isolates a platform or process boundary;
- it has multiple real implementations;
- it creates a valuable deterministic test seam;
- it represents a stable concept used by more than one caller.

Otherwise prefer a private function, a local structure, or a type in the owning `.cpp` file.

- Do not create an interface solely because a concrete class exists.
- Do not create one file per trivial enum, token, counter, or result wrapper.
- Do not split a linear operation across classes merely to reduce function length.
- Do not duplicate behavior to avoid a small dependency; fix the dependency direction.
- Prefer composition over inheritance. Runtime polymorphism needs a concrete reason.
- A forwarding wrapper that adds no invariant, translation, ownership, or test seam should not exist.

## 7. Functions and files remain navigable

Guidelines, not incentives to hide logic:

- functions should normally fit within roughly 50 lines;
- a class should have one sentence that describes its responsibility without using “and” repeatedly;
- public headers expose the minimum needed contract;
- implementation-only types stay in the `.cpp` file;
- production `.cpp` files should normally remain below 1,200 lines;
- XAML code-behind should normally remain below 800 lines;
- a file exceeding a guideline requires a short architectural justification in review;
- do not split files mechanically if the result increases navigation without creating a real boundary.

Headers and implementations are colocated by feature. A public SDK-style include tree is reserved for genuinely shared contracts such as the control protocol.

## 8. Error handling is explicit

- Expected failures use a typed result that carries an actionable error category and safe message.
- Exceptions are appropriate for exceptional construction/platform failures, not ordinary state transitions.
- Catch at the boundary that can add context, recover, translate, or terminate safely.
- Never silently swallow an exception. A deliberate best-effort cleanup may ignore failure only with a concise reason or bounded diagnostic.
- Do not catch and immediately rethrow without adding value.
- User-facing errors use localized resources and do not expose sensitive device identifiers under Privacy Mode.
- Retry only transient failures, with a bounded policy and observable final outcome.
- Preserve the original error code/context when translating a platform error.

## 9. Comments explain why

- Names and structure explain what the code does.
- Comments document non-obvious constraints, platform behavior, concurrency invariants, security assumptions, compatibility requirements, and reasons for an unusual choice.
- Do not narrate the next statement.
- Workarounds name the affected platform/API condition and, when available, a source or removal condition.
- Public API documentation describes lifetime, threading, ownership, errors, and invariants when those cannot be encoded in the type system.
- Self-explaining code reduces local commentary; it does not eliminate architecture, protocol, or security documentation.

## 10. State machines are explicit

- Use an enum and named transition functions when behavior depends on a lifecycle state.
- Invalid transitions fail visibly in tests and diagnostics; they are not silently coerced.
- Keep transition decisions separate from platform side effects where practical.
- A collection of booleans must not encode mutually exclusive states.
- Cancellation, superseding, retry, timeout, and shutdown are first-class transitions when they affect correctness.

## 11. Dependencies and includes are narrow

- Depend on the smallest stable facade, not another feature's implementation header.
- UI/WinUI types do not leak into `Apc.Core` contracts.
- Pipe transport types do not leak into `AppController`.
- Settings persistence does not call device or UI code.
- Diagnostics consumes snapshots instead of acquiring feature locks.
- Avoid catch-all feature headers.
- Include what a file uses; forward-declare only when it meaningfully reduces coupling without obscuring ownership.
- New third-party dependencies require a written benefit, size/deployment impact, maintenance status, and removal comparison against standard/Windows facilities.

## 12. Security and privacy are design constraints

- Named-pipe identity and ACL checks remain at the transport boundary and are tested negatively.
- Validate protocol lengths and counts before allocation or parsing.
- Do not log secrets, unredacted device identities in Privacy Mode, or raw external payloads by default.
- A timeout or cancellation must not permit an unreported late mutation.
- File writes use bounded paths and atomic replacement where data integrity matters.
- Security checks are not removed or weakened to reduce code size.

## 13. UI and localization

- Every user-visible string comes from `StringCatalog`/localized resources.
- Presentation models contain display-ready state, not XAML controls.
- Code-behind forwards intent and renders state; it does not implement domain workflows.
- UI updates occur on the UI thread.
- Accessibility names, keyboard behavior, theme, DPI, and Privacy Mode are part of feature completion.
- Static tray visual states remain explicit. Do not reintroduce connecting animation without a new product decision and performance measurement.
- Normal disconnect is not a notification event. Retained notification kinds keep their dedicated images.

## 14. Tests describe behavior and invariants

- Test names state scenario and expected outcome.
- Prefer deterministic policy/state tests over sleeps and timing luck.
- Concurrency tests control scheduling or use bounded synchronization.
- Every fixed race receives a regression test.
- Protocol/security changes require positive and negative compatibility tests.
- Resource-policy changes require transition, freshness, sequence, and failure-retry tests.
- A test should fail for one understandable behavioral reason.

## 15. Review checklist

- Does every new type own a meaningful responsibility?
- Can names be understood without opening the implementation?
- Are modes/reasons represented by enums instead of ambiguous booleans?
- Is mutable state owned in exactly one place?
- Are threading, lifetime, cancellation, and shutdown behavior clear?
- Is the dependency direction permitted by the architecture plan?
- Does the code preserve CLI security, adaptive resources, privacy, and atomic persistence?
- Are user-visible strings localized?
- Are errors observable and retries bounded?
- Could a private function/local type replace a new public abstraction?
- Is the behavior covered at the cheapest deterministic layer?
