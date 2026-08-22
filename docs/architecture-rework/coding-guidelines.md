# Coding Guidelines for the Architecture Rework

These rules are mandatory for new rework code and for legacy code when a file is substantially migrated. They complement the repository build, localization, formatting, and warnings-as-errors rules.

## 1. Core engineering principles

- Prefer code that is easy to read, trace, test, and maintain.
- Make control flow, ownership, lifetime, units, invariants, threading, and error behavior explicit.
- Choose the smallest design that fully satisfies the real requirements and has the lowest total comprehension cost.
- Do not optimize for the fewest classes, files, or lines. Do not add structure merely to make the architecture look sophisticated.
- Treat compiler warnings, undefined behavior, resource leaks, data races, deadlocks, and silent data loss as defects.
- Do not claim that a change works until its relevant build, automated checks, and required manual validation have succeeded.
- Measure behavior before and after performance-sensitive or structural changes. Do not trade clarity for speculative micro-optimizations.

### Language target

- C++26 is the target language standard for the completed rework.
- The current projects use `stdcpp23`; C++26 becomes the enforced baseline only through a dedicated, reviewable toolchain change that passes Release builds and tests for x64 and ARM64 in CI.
- A C++26 library or language feature may be used only when the supported Visual Studio/MSVC toolchain implements it correctly on every shipping architecture. Local compiler acceptance alone is insufficient.
- Prefer a well-supported C++23 form when an equivalent C++26 facility would make the build dependent on incomplete or architecture-specific compiler support.
- Do not mix a broad language-standard migration with an unrelated feature migration. Establish and verify the toolchain baseline first.

## 2. Names communicate intent

- Use complete words. Do not invent abbreviations to make identifiers shorter.
- Conventional technical terms are allowed when they are clearer than the expanded form: `ID`, `URL`, `URI`, `HTTP`, `JSON`, `CPU`, `GPU`, `UI`, `CLI`, `IPC`, `RAII`, `GUID`, `JPEG`, `DPI`, `HWND`, `WinRT`, `XAML`, `A2DP`, `MCU`, and protocol-defined terms.
- Avoid project-specific shorthand such as `cfg`, `mgr`, `ctx`, `proc`, `tmpObj`, or `devMgr`.
- Follow one spelling for an established term. Do not mix `Id`, `ID`, `Identifier`, and `DeviceKey` for the same concept inside one API.
- Name values after their meaning, not their type: `deviceId`, not `stringValue`; `retryDeadline`, not `timePoint`.
- Include units in names whenever the type does not encode them: `timeoutMilliseconds`, `payloadBytes`, `workingSetBytes`.
- Boolean values use positive predicates: `isConnected`, `canReconnect`, `shouldNotify`, `hasPendingWrite`.
- Do not encode type information redundantly in a name.
- Follow the repository casing convention consistently.
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

## 3. Readability and visual structure

- Keep code easy to scan. Prefer early returns when they reveal the primary path and reduce nesting.
- Avoid compressed expressions and clever one-liners when an explicit form is easier to understand.
- Let `clang-format` own indentation, wrapping, and ordinary whitespace. Do not manually align declarations in ways that create diff churn.
- Remove empty structural sections.
- In a sufficiently large file, a visual heading may group several related definitions:

```cpp
// -------------------------------------------------------------------------------------------------
// Public operations
// -------------------------------------------------------------------------------------------------
```

- Use consistent, short section names such as `Public interface`, `Protected interface`, and `Private implementation`.
- Do not add a three-line heading before one trivial function or repeat information already obvious from a nearby class access specifier.
- A substantially migrated file adopts this separator style. Do not create formatting-only changes across untouched legacy files.

### Optional class regions

- `#pragma region` may group a substantial, cohesive part of a large class when it materially improves Visual Studio navigation.
- Suitable region names include `Types`, `Constants`, `Construction`, `Creators`, `Operations`, `Overrides`, `Event handling`, `Helpers`, and `State`.
- Do not create a region merely to wrap a visual heading, an access specifier, or one trivial member.
- Do not nest regions unless the outer region represents a substantial subsystem and nesting clearly improves navigation.
- Use this exact style:

```cpp
#pragma region Event handling ------------------------------------------------------------------

// Related declarations or definitions

#pragma endregion
```

Regions are navigation aids, not architectural boundaries. If understanding a simple operation requires opening several regions and files, reassess the design.

## 4. Prefer domain types over primitive ambiguity

- Do not expose multiple boolean parameters in a public function.
- Use `enum class` when a value selects a mode, reason, phase, policy, or outcome.
- Use a named options structure when several independent switches are genuinely required.
- Keep a `bool` for a true predicate or stored yes/no fact; do not replace every boolean member with an enum mechanically.
- Use scoped enums and handle every enumerator explicitly. Do not add a generic `Default` or `Unknown` state unless it is a real domain state.
- Do not use integers as status codes, modes, time units, sizes, or device identities when a stronger type is practical.
- Use `std::chrono` durations/time points for time and explicit byte/count types for sizes.
- Mark an important return value `[[nodiscard]]` when ignoring it is likely a defect.
- Apply `const`, `constexpr`, `consteval`, `noexcept`, and ref-qualifiers when they communicate and enforce a real contract.
- Avoid returning references, iterators, spans, or views whose lifetime is not obvious and guaranteed.
- Prefer value semantics when ownership is clear and the cost is appropriate.

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

## 5. One owner for mutable state and resources

- Every mutable state has one obvious owning class or object.
- Do not mirror the same truth in `AppRuntime`, UI, and a feature service.
- Other modules receive immutable snapshots, typed events, or command results.
- A cache must identify its source, invalidation rule, and owner.
- Global mutable state is prohibited except for narrowly justified process primitives that cannot be represented otherwise.
- Use RAII for event tokens, handles, timers, registrations, and cancellation lifetimes where practical.
- Ownership must be visible from member types: prefer values and `std::unique_ptr`; use `std::shared_ptr` only for genuinely shared lifetime.
- A `weak_ptr` is not a substitute for defining ownership.
- Avoid owning raw pointers and manual `new`/`delete` in application code.
- References and non-owning pointers require an obvious lifetime contract.
- Make ownership transfer visible in function signatures.
- Pair every registration, subscription, hook, timer, background operation, COM initialization, and acquired handle with deterministic cleanup.
- Break possible shared-ownership cycles explicitly, normally with `std::weak_ptr`.
- Prefer stack allocation and deterministic lifetime when practical.

## 6. Threading is part of the contract

- Every state-owning component declares its execution context: UI thread, serialized device context, worker, or internally synchronized.
- Minimize shared mutable state; prefer message passing, immutable snapshots, and serialized owners.
- Marshal at the module boundary, then keep the implementation on its declared context.
- Never hold a lock while invoking unknown external code, publishing an event, awaiting a coroutine, waiting on I/O, performing lengthy work, or calling UI code unless a documented invariant makes it unavoidable.
- Do not use detached work without an explicit lifetime, cancellation, and completion policy.
- Every long-running or asynchronous operation defines what happens during shutdown and superseding requests.
- Generation/epoch tokens are used where stale asynchronous completions are possible, but remain private to the owning operation/session.
- Timer callbacks must not dereference an object whose lifetime is not guaranteed.
- `Sleep` is not synchronization. A timed delay is allowed only when delay itself is the required behavior.
- Test shutdown, cancellation, reconnect, timeout, and rapid repeated invocation paths.

## 7. Commands in, snapshots and events out

- Feature facades accept typed commands and expose immutable snapshots plus one cohesive typed event stream.
- UI renders presentation snapshots and emits intent. UI does not reach into settings locks, device sessions, pipe instances, or resource-policy internals.
- CLI and UI invoke the same `AppController` use cases.
- Events report facts that already happened. Commands request behavior. Do not use events as disguised commands.
- Event payloads contain enough context to understand the event without querying mutable internals during the callback.
- Do not introduce a generic global event bus.

## 8. Abstractions must lower comprehension cost

Abstractions are encouraged when they make the surrounding code easier to understand or safer to use. Do not judge an abstraction only by its size or current number of implementations. Prefer the smallest abstraction that provides a clear net improvement to caller and implementation together.

Create a class or interface only when at least one is true:

- it gives an important domain concept a precise name;
- it owns state and a lifecycle;
- it enforces a domain invariant;
- it makes invalid states difficult to represent;
- it makes ownership or lifetime explicit;
- it isolates an operating-system, process, protocol, storage, hardware, or third-party boundary;
- it has multiple real implementations;
- it creates a valuable deterministic test seam;
- it removes meaningful duplication without combining unrelated behavior;
- it separates components with genuinely different reasons to change;
- it measurably simplifies the normal calling code.

Otherwise prefer a private function, a local structure, or a type in the owning `.cpp` file.

- Do not create an interface solely because a concrete class exists.
- A single-implementation interface is acceptable when it expresses a real platform, process, protocol, storage, or test boundary.
- Do not create one file per trivial enum, token, counter, or result wrapper.
- Do not split a linear operation across classes merely to reduce function length.
- Do not duplicate behavior to avoid a small dependency; fix the dependency direction.
- Prefer composition over inheritance. Runtime polymorphism needs a concrete reason.
- A forwarding wrapper that adds no invariant, translation, ownership, or test seam should not exist.
- Do not remove a useful abstraction merely to reduce class, project, file, or line counts.
- Before adding a boundary, compare how the complete operation is read before and after. Choose the version with lower total comprehension cost.
- If an abstraction's benefit is not evident from the code, record the architectural reason briefly in the change description or design document.

## 9. Functions and files remain navigable

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

## 10. Error handling is explicit

- Expected failures use a typed result that carries an actionable error category and safe message.
- Exceptions are appropriate for exceptional construction/platform failures, not ordinary state transitions.
- Catch at the boundary that can add context, recover, translate, or terminate safely.
- Never silently swallow an exception. A deliberate best-effort cleanup may ignore failure only with a concise reason or bounded diagnostic.
- Do not catch and immediately rethrow without adding value.
- User-facing errors use localized resources and do not expose sensitive device identifiers under Privacy Mode.
- Retry only transient failures, with a bounded policy and observable final outcome.
- Preserve the original error code/context when translating a platform error.
- Distinguish normal disconnection, cancellation, timeout, unavailable resources, invalid input, and internal defects when callers can respond differently.
- Validate input at process, protocol, storage, and other trust boundaries before using it.

## 11. Comments explain why

- Names and structure explain what the code does.
- Comments document non-obvious constraints, platform behavior, concurrency invariants, security assumptions, compatibility requirements, and reasons for an unusual choice.
- Do not narrate the next statement.
- Workarounds name the affected platform/API condition and, when available, a source or removal condition.
- Public API documentation describes lifetime, threading, ownership, errors, and invariants when those cannot be encoded in the type system.
- Self-explaining code reduces local commentary; it does not eliminate architecture, protocol, or security documentation.

## 12. State machines are explicit

- Use an enum and named transition functions when behavior depends on a lifecycle state.
- Invalid transitions fail visibly in tests and diagnostics; they are not silently coerced.
- Keep transition decisions separate from platform side effects where practical.
- A collection of booleans must not encode mutually exclusive states.
- Cancellation, superseding, retry, timeout, and shutdown are first-class transitions when they affect correctness.

## 13. Dependencies and includes are narrow

- Depend on the smallest stable facade, not another feature's implementation header.
- UI/WinUI types do not leak into `Apc.Core` contracts.
- Pipe transport types do not leak into `AppController`.
- Settings persistence does not call device or UI code.
- Diagnostics consumes snapshots instead of acquiring feature locks.
- Avoid catch-all feature headers.
- Include what a file uses; forward-declare only when it meaningfully reduces coupling without obscuring ownership.
- New third-party dependencies require a written benefit, size/deployment impact, maintenance status, and removal comparison against standard/Windows facilities.

## 14. Security and privacy are design constraints

- Named-pipe identity and ACL checks remain at the transport boundary and are tested negatively.
- Validate protocol lengths and counts before allocation or parsing.
- Do not log secrets, unredacted device identities in Privacy Mode, or raw external payloads by default.
- A timeout or cancellation must not permit an unreported late mutation.
- File writes use bounded paths and atomic replacement where data integrity matters.
- Security checks are not removed or weakened to reduce code size.

## 15. UI and localization

- Every user-visible string comes from `StringCatalog`/localized resources.
- Presentation models contain display-ready state, not XAML controls.
- Code-behind forwards intent and renders state; it does not implement domain workflows.
- UI updates occur on the UI thread.
- Accessibility names, keyboard behavior, theme, DPI, and Privacy Mode are part of feature completion.
- Static tray visual states remain explicit. Do not reintroduce connecting animation without a new product decision and performance measurement.
- Normal disconnect is not a notification event. Retained notification kinds keep their dedicated images.

## 16. Tests describe behavior and invariants

- Test names state scenario and expected outcome.
- Prefer deterministic policy/state tests over sleeps and timing luck.
- Concurrency tests control scheduling or use bounded synchronization.
- Every fixed race receives a regression test.
- Protocol/security changes require positive and negative compatibility tests.
- Resource-policy changes require transition, freshness, sequence, and failure-retry tests.
- A test should fail for one understandable behavioral reason.

## 17. Review checklist

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
- Did the relevant x64 and ARM64 build/validation actually run before success was claimed?
- For a structural or performance-sensitive change, is there a comparable before/after measurement?
