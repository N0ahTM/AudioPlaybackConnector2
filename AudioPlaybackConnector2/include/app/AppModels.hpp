#pragma once

#include <core/DeviceId.hpp>
#include <app/StartupTaskSnapshot.hpp>
#include <core/SettingsData.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace apc::app {

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Identity and Selectors ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Decoded command text is bounded by the control protocol's 64 KiB payload limit.
// Persistence-specific limits remain in their owning settings boundary.
inline constexpr std::size_t c_maxAppCommandTextCharacters = 64u * 1024u / sizeof(wchar_t);

enum class DeviceConnectionState { Idle, Connecting, Connected, Disconnecting, WaitingForReconnect, Failed };

// A snapshot identity is bounded by the control command payload, not by the
// smaller persistence field.  DeviceId remains the validated persistence
// value; this type keeps a valid external identity lossless. Persistence
// validates its own bounded DeviceId at the storage boundary.
class ExternalDeviceId {
public:
    [[nodiscard]] static std::optional<ExternalDeviceId> TryCreate(std::wstring_view value) {
        if (value.empty() || value.size() > c_maxAppCommandTextCharacters || value.contains(L'\0')) {
            return std::nullopt;
        }
        return ExternalDeviceId(std::wstring(value));
    }

    ExternalDeviceId(apc::core::DeviceId const& value) : m_value(value.View()) {}

    [[nodiscard]] std::wstring_view View() const noexcept { return m_value; }
    // This conversion deliberately owns its result; View() is the borrowing alternative.
    // cppcheck-suppress returnByReference
    [[nodiscard]] std::wstring ToString() const { return m_value; }

    friend bool operator==(ExternalDeviceId const&, ExternalDeviceId const&) = default;

private:
    explicit ExternalDeviceId(std::wstring value) noexcept : m_value(std::move(value)) {}

    std::wstring m_value;
};

// Selectors are either exact external ID text, a non-empty matching query, or
// one of the two stateful selectors. IdText() preserves all valid control input;
// persistence validates its own identity bound.
enum class DeviceSelectorKind { Id, Name, Mac, Last, Auto, Alias, Default };

class DeviceSelector {
public:
    [[nodiscard]] static std::optional<DeviceSelector> ById(std::wstring_view value) {
        if (!IsValidCommandText(value)) return std::nullopt;
        return DeviceSelector(DeviceSelectorKind::Id, std::wstring(value));
    }

    [[nodiscard]] static DeviceSelector ById(apc::core::DeviceId const& value) {
        return DeviceSelector(DeviceSelectorKind::Id, std::wstring(value.View()));
    }

    [[nodiscard]] static std::optional<DeviceSelector> ByQuery(DeviceSelectorKind kind, std::wstring_view value) {
        if (!IsQueryKind(kind) || !IsValidCommandText(value)) {
            return std::nullopt;
        }
        return DeviceSelector(kind, std::wstring(value));
    }

    [[nodiscard]] static DeviceSelector Last() { return DeviceSelector(DeviceSelectorKind::Last, {}); }
    [[nodiscard]] static DeviceSelector Default() { return DeviceSelector(DeviceSelectorKind::Default, {}); }

    [[nodiscard]] DeviceSelectorKind Kind() const noexcept { return m_kind; }

    // Returns the exact external ID text, including IDs too large for the
    // validated internal DeviceId bound.
    [[nodiscard]] std::wstring_view IdText() const noexcept {
        if (m_kind != DeviceSelectorKind::Id) return {};
        return m_value;
    }

    // Non-empty only for Name, Mac, Auto, and Alias selectors.
    [[nodiscard]] std::wstring_view Query() const noexcept {
        if (!IsQueryKind(m_kind)) return {};
        return m_value;
    }

    friend bool operator==(DeviceSelector const&, DeviceSelector const&) = default;

private:
    DeviceSelector(DeviceSelectorKind kind, std::wstring value) noexcept : m_kind(kind), m_value(std::move(value)) {}

    [[nodiscard]] static bool IsValidCommandText(std::wstring_view value) noexcept {
        // CommandProtocol::IsRequestValid deliberately accepts any bounded
        // UTF-16 code units other than NUL. Keep that compatibility at the
        // typed boundary; persistence or presentation may normalize later.
        return !value.empty() && value.size() <= c_maxAppCommandTextCharacters && !value.contains(L'\0');
    }

    [[nodiscard]] static bool IsQueryKind(DeviceSelectorKind kind) noexcept {
        return kind == DeviceSelectorKind::Name || kind == DeviceSelectorKind::Mac ||
               kind == DeviceSelectorKind::Auto || kind == DeviceSelectorKind::Alias;
    }

    DeviceSelectorKind m_kind;
    std::wstring m_value;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Actions and Outcomes //////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

enum class AppCommandKind {
    ShowDevicePicker,
    ShowSettings,
    ListDevices,
    Status,
    ShowDefault,
    SetDefault,
    ClearDefault,
    ListAliases,
    SetAlias,
    ClearAlias,
    Connect,
    Disconnect,
    Reconnect,
    ToggleLast,
    DisconnectAll,
    ReconnectAll
};

// A tray primary activation toggles an already-open picker, while a control
// command is idempotent and only ensures that the picker is open. The typed
// mode carries this distinction to the UI without exposing transport details.
enum class DevicePickerOpenMode { EnsureOpen, ToggleIfOpen };

enum class AppResultCode {
    Success,
    InvalidInput,
    NotFound,
    Ambiguous,
    OperationFailed,
    Unavailable,
    Busy,
    Cancelled,
    TimedOut,
    Indeterminate,
    InternalError
};

// AppController reports whether a result was produced before or after the
// application executor was entered.  Transport adapters need this distinction
// for cancellation and deadline results: a pre-dispatch result is definite,
// while a result after dispatch may describe work whose final state is
// unknown.
enum class AppDispatchPhase { NotStarted, Started };

// A result reason is stable application vocabulary. Presentation adapters map
// it to localized text while the controller remains independent of transport
// and resource lookup.
enum class AppOutcomeReason {
    None,
    AlreadyConnected,
    AlreadyDisconnected,
    ConnectSucceeded,
    ConnectFailed,
    DisconnectSucceeded,
    DisconnectFailed,
    ReconnectSucceeded,
    ReconnectFailed,
    ShowOpened,
    SettingsOpened,
    DefaultSet,
    DefaultCleared,
    AliasSet,
    AliasSetFailed,
    AliasCleared,
    AliasClearFailed,
    DisconnectAllSucceeded,
    ReconnectAllSucceeded,
    NotReady,
    TargetRequired,
    TargetNotFound,
    TargetAmbiguous,
    DefaultTargetMissing,
    LastTargetMissing,
    InvalidAliasPayload,
    Unsupported,
    InternalError
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Application Snapshots /////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

// Snapshot values own all strings and containers. The controller publishes
// copies; they do not expose service locks, XAML objects, or transport state.
struct DeviceSnapshot {
    // Name is retained for trusted matching and settings decisions. External
    // presentation uses DisplayName and the AppSnapshot privacy flag.
    ExternalDeviceId Id;
    std::wstring Name;
    std::wstring Alias;
    std::wstring DisplayName;
    DeviceConnectionState State = DeviceConnectionState::Idle;
    bool IsKnown = false;
    // IsConnected is the normalized live observation; State remains the
    // richer lifecycle state used to render progress and failure.
    bool IsConnected = false;
    bool IsBusy = false;
    bool IsAvailable = false;

    friend bool operator==(DeviceSnapshot const&, DeviceSnapshot const&) = default;
};

// A resolved control target may be an external identifier that is valid for
// transport but intentionally too large for the bounded persistence DeviceId type.
// It therefore remains a plain, transport-neutral value object.
struct AppTargetSnapshot {
    std::wstring Id;
    std::wstring Name;
    std::wstring Alias;
    std::wstring DisplayName;
    bool Exists = false;
    bool IsConnected = false;
    bool IsKnown = false;

    friend bool operator==(AppTargetSnapshot const&, AppTargetSnapshot const&) = default;
};

enum class DefaultDeviceMode { LastConnected, SpecificDevice };

struct DefaultDeviceSnapshot {
    // In LastConnected mode Id is the resolved MRU target when one exists.
    // In SpecificDevice mode Id is the persisted selection even when it no
    // longer resolves to a discovered device.
    DefaultDeviceMode Mode = DefaultDeviceMode::LastConnected;
    std::optional<apc::core::DeviceId> Id;
    std::wstring DisplayName;
    bool IsResolved = false;
    bool IsConnected = false;

    friend bool operator==(DefaultDeviceSnapshot const&, DefaultDeviceSnapshot const&) = default;
};

struct TraySnapshot {
    std::uint64_t Generation = 0;
    std::uint64_t DevicePickerOpenedGeneration = 0;
    std::vector<DeviceSnapshot> ConnectedDevices;
    bool HasBusyOperations = false;

    friend bool operator==(TraySnapshot const&, TraySnapshot const&) = default;
};

struct AppSnapshot {
    std::uint64_t Generation = 0;
    bool IsRunning = true;
    bool PrivacyModeEnabled = false;
    std::vector<DeviceSnapshot> Devices;
    std::vector<apc::core::DeviceId> LastConnectedDeviceIds;
    std::optional<DefaultDeviceSnapshot> DefaultDevice;
    TraySnapshot Tray;
    struct ResourceStatusSnapshot {
        enum class Residency { Cold, Warm, Hot };
        enum class MemoryPressure { Unknown, Low, Neutral, High };
        enum class UserActivity {
            Unknown,
            Available,
            NotPresent,
            Busy,
            Fullscreen,
            Presentation,
            QuietTime,
            ImmersiveApp
        };

        bool Evaluated = false;
        Residency ForegroundResidency = Residency::Warm;
        Residency BackgroundResidency = Residency::Warm;
        bool SnapshotFresh = false;
        bool PositiveAuthorizationCurrent = false;
        bool PreloadAllowed = false;
        bool UiResourcesLoaded = false;
        bool UiResourcesInitialized = false;
        MemoryPressure Memory = MemoryPressure::Unknown;
        UserActivity Activity = UserActivity::Unknown;
        std::optional<bool> EnergySaver;

        friend bool operator==(ResourceStatusSnapshot const&, ResourceStatusSnapshot const&) = default;
    } AdaptiveResources;
    // Versions of the concrete owners from the successful capture.
    SettingsData Settings;
    std::optional<StartupTaskSnapshot> StartupTask;
    std::uint64_t SettingsRevision = 0;
    std::uint64_t DeviceGeneration = 0;
    bool InventoryComplete = false;

    friend bool operator==(AppSnapshot const&, AppSnapshot const&) = default;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Action Results and Context ////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct AppResult {
    AppResultCode Code = AppResultCode::Success;
    AppCommandKind Command = AppCommandKind::Status;
    std::optional<DeviceSnapshot> Device;
    std::vector<DeviceSnapshot> Devices;
    std::optional<DefaultDeviceSnapshot> DefaultDevice;
    std::optional<TraySnapshot> Tray;
    AppOutcomeReason Reason = AppOutcomeReason::None;
    std::wstring Alias;
    std::optional<AppSnapshot> Snapshot;
    std::optional<AppTargetSnapshot> Target;
    std::wstring RequestedTarget;
    std::optional<bool> PrivacyModeEnabled;
    AppDispatchPhase DispatchPhase = AppDispatchPhase::NotStarted;

    [[nodiscard]] bool Succeeded() const noexcept { return Code == AppResultCode::Success; }
    friend bool operator==(AppResult const&, AppResult const&) = default;
};

struct AppCommandContext {
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    enum class CompletionMode { WaitForCompletion, Detached };

    std::stop_token StopToken;
    TimePoint Deadline = TimePoint::max();
    CompletionMode Completion = CompletionMode::WaitForCompletion;

    [[nodiscard]] static AppCommandContext Detached() noexcept { return {.Completion = CompletionMode::Detached}; }

    [[nodiscard]] bool IsCancellationRequested() const noexcept { return StopToken.stop_requested(); }
    [[nodiscard]] bool IsExpired(TimePoint now) const noexcept {
        return Deadline != TimePoint::max() && now >= Deadline;
    }
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Application Events ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct DeviceConnectedEvent {
    // Device events originate at the transport/device boundary. They
    // must retain a valid external identity even when it cannot be persisted
    // in the smaller persistence DeviceId field.
    ExternalDeviceId Id;
    friend bool operator==(DeviceConnectedEvent const&, DeviceConnectedEvent const&) = default;
};

struct DeviceDisconnectedEvent {
    ExternalDeviceId Id;
    bool NotifyUser = false;
    friend bool operator==(DeviceDisconnectedEvent const&, DeviceDisconnectedEvent const&) = default;
};

struct DeviceConnectionErrorEvent {
    ExternalDeviceId Id;
    // Presentation chooses localized text from this actionable category.
    AppResultCode Code = AppResultCode::OperationFailed;
    enum class Reason { Unknown, TimedOut, Denied, ReconnectExhausted };
    Reason FailureReason = Reason::Unknown;
    friend bool operator==(DeviceConnectionErrorEvent const&, DeviceConnectionErrorEvent const&) = default;
};

struct DeviceStatusChangedEvent {
    ExternalDeviceId Id;
    // Facts carry stable states; localized text belongs to presentation.
    DeviceConnectionState State = DeviceConnectionState::Idle;
    friend bool operator==(DeviceStatusChangedEvent const&, DeviceStatusChangedEvent const&) = default;
};

// These invalidations tell consumers to obtain current presentation data.
struct DeviceActivityChangedEvent {
    friend bool operator==(DeviceActivityChangedEvent const&, DeviceActivityChangedEvent const&) = default;
};

struct DeviceInventoryChangedEvent {
    friend bool operator==(DeviceInventoryChangedEvent const&, DeviceInventoryChangedEvent const&) = default;
};

struct StartupTaskChangedEvent {
    StartupTaskSnapshot Snapshot;
    friend bool operator==(StartupTaskChangedEvent const&, StartupTaskChangedEvent const&) = default;
};

struct SettingsChangedEvent {
    std::uint64_t SettingsRevision = 0;
    std::wstring Language;
    bool UseSystemBackdropEffects = true;
    friend bool operator==(SettingsChangedEvent const&, SettingsChangedEvent const&) = default;
};

struct AutoReconnectTriggeredEvent {
    ExternalDeviceId Id;
    friend bool operator==(AutoReconnectTriggeredEvent const&, AutoReconnectTriggeredEvent const&) = default;
};

struct AutoReconnectFailedEvent {
    ExternalDeviceId Id;
    friend bool operator==(AutoReconnectFailedEvent const&, AutoReconnectFailedEvent const&) = default;
};

using AppEvent = std::variant<DeviceConnectedEvent,
                              DeviceDisconnectedEvent,
                              DeviceConnectionErrorEvent,
                              DeviceStatusChangedEvent,
                              DeviceActivityChangedEvent,
                              DeviceInventoryChangedEvent,
                              SettingsChangedEvent,
                              StartupTaskChangedEvent,
                              AutoReconnectTriggeredEvent,
                              AutoReconnectFailedEvent>;

} // namespace apc::app
