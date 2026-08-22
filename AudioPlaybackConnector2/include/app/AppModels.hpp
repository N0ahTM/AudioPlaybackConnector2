#pragma once

#include <core/DeviceId.hpp>

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

// Decoded command text is bounded by the existing P01 64 KiB payload limit.
// Persistence-specific limits remain in their owning settings boundary.
inline constexpr std::size_t c_maxAppCommandTextCharacters = 64u * 1024u / sizeof(wchar_t);

enum class DeviceConnectionState { Idle, Connecting, Connected, Disconnecting, WaitingForReconnect, Failed };

// Selectors are either exact external ID text, a non-empty matching query, or
// one of the two stateful selectors. Id() yields a validated internal ID when
// the text also satisfies the P07 identity bound; IdText() preserves all valid
// P01 input for transport-compatible handling.
enum class DeviceSelectorKind { Id, Name, Mac, Last, Auto, Alias, Default };

class DeviceSelector {
public:
    [[nodiscard]] static std::optional<DeviceSelector> ById(std::wstring_view value) {
        if (!IsValidCommandText(value)) return std::nullopt;
        return DeviceSelector(DeviceSelectorKind::Id, std::wstring(value));
    }

    [[nodiscard]] static DeviceSelector ById(apc::core::DeviceId value) {
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

    // Returns a validated internal copy, if the external text satisfies the
    // P07 identity bound. Use IdText() to retain longer P01 input.
    [[nodiscard]] std::optional<apc::core::DeviceId> Id() const {
        if (m_kind != DeviceSelectorKind::Id) return std::nullopt;
        if (auto const* idText = std::get_if<std::wstring>(&m_value)) {
            return apc::core::DeviceId::TryCreate(*idText);
        }
        return std::nullopt;
    }

    // Returns the exact external ID text, including IDs too large for the
    // validated internal DeviceId P07 bound.
    [[nodiscard]] std::wstring_view IdText() const noexcept {
        if (m_kind != DeviceSelectorKind::Id) return {};
        if (auto const* idText = std::get_if<std::wstring>(&m_value)) return *idText;
        return {};
    }

    // Non-empty only for Name, Mac, Auto, and Alias selectors.
    [[nodiscard]] std::wstring_view Query() const noexcept {
        if (!IsQueryKind(m_kind)) return {};
        if (auto const* query = std::get_if<std::wstring>(&m_value)) return *query;
        return {};
    }

    friend bool operator==(DeviceSelector const&, DeviceSelector const&) = default;

private:
    using Value = std::variant<std::monostate, std::wstring>;

    DeviceSelector(DeviceSelectorKind kind, Value value) noexcept : m_kind(kind), m_value(std::move(value)) {}

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
    Value m_value;
};

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

// The command carries intent only. It has no pipe headers, JSON flags,
// localized text, or platform handles; adapters translate those concerns at
// the boundary. Target and Alias are meaningful only for the command kinds
// checked by IsWellFormed(). Tray Exit is intentionally not a command here:
// it is an AppRuntime lifecycle intent, not a shared UI/CLI use case. The
// command grammar retains valid P01 alias text above the P07 persistence
// limit; SettingsController owns that later failure/result translation.
struct AppCommand {
    AppCommandKind Kind = AppCommandKind::Status;
    std::optional<DeviceSelector> Target;
    std::wstring Alias;

    [[nodiscard]] bool IsWellFormed() const noexcept {
        const bool hasTarget = Target.has_value();
        const bool hasAlias = !Alias.empty();
        const bool aliasIsValid = hasAlias && Alias.size() <= c_maxAppCommandTextCharacters && !Alias.contains(L'\r') &&
                                  !Alias.contains(L'\n') && !Alias.contains(L'\0');

        switch (Kind) {
            case AppCommandKind::SetAlias: return hasTarget && IsExplicitTarget(*Target) && aliasIsValid;
            case AppCommandKind::SetDefault: return hasTarget && IsExplicitTarget(*Target) && !hasAlias;
            case AppCommandKind::ClearAlias: return hasTarget && IsExplicitTarget(*Target) && !hasAlias;
            case AppCommandKind::Connect:
            case AppCommandKind::Disconnect:
            case AppCommandKind::Reconnect:
            case AppCommandKind::ToggleLast: return hasTarget && !hasAlias;
            case AppCommandKind::ShowDevicePicker:
            case AppCommandKind::ShowSettings:
            case AppCommandKind::ListDevices:
            case AppCommandKind::Status:
            case AppCommandKind::ShowDefault:
            case AppCommandKind::ClearDefault:
            case AppCommandKind::ListAliases:
            case AppCommandKind::DisconnectAll:
            case AppCommandKind::ReconnectAll: return !hasTarget && !hasAlias;
        }
        return false;
    }

    friend bool operator==(AppCommand const&, AppCommand const&) = default;

private:
    [[nodiscard]] static bool IsExplicitTarget(DeviceSelector const& target) noexcept {
        switch (target.Kind()) {
            case DeviceSelectorKind::Id:
            case DeviceSelectorKind::Name:
            case DeviceSelectorKind::Mac:
            case DeviceSelectorKind::Auto:
            case DeviceSelectorKind::Alias: return true;
            case DeviceSelectorKind::Last:
            case DeviceSelectorKind::Default: return false;
        }
        return false;
    }
};

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

// Snapshot values own all strings and containers. The controller publishes
// copies; they do not expose service locks, XAML objects, or transport state.
struct DeviceSnapshot {
    // Name is retained for trusted matching and settings decisions. External
    // presentation uses DisplayName and the AppSnapshot privacy flag.
    apc::core::DeviceId Id;
    std::wstring Name;
    std::wstring Alias;
    std::wstring DisplayName;
    DeviceConnectionState State = DeviceConnectionState::Idle;
    bool IsKnown = false;
    // IsConnected is the normalized live observation; State remains the
    // richer lifecycle state used to render progress and failure.
    bool IsConnected = false;
    bool IsBusy = false;

    friend bool operator==(DeviceSnapshot const&, DeviceSnapshot const&) = default;
};

// A resolved control target may be an external identifier that is valid for
// P01 but intentionally too large for the bounded persistence DeviceId type.
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

    friend bool operator==(AppSnapshot const&, AppSnapshot const&) = default;
};

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

    [[nodiscard]] bool IsCancellationRequested() const noexcept { return StopToken.stop_requested(); }
    [[nodiscard]] bool IsExpired(TimePoint now) const noexcept {
        return Deadline != TimePoint::max() && now >= Deadline;
    }
};

struct DeviceConnectedEvent {
    apc::core::DeviceId Id;
    friend bool operator==(DeviceConnectedEvent const&, DeviceConnectedEvent const&) = default;
};

struct DeviceDisconnectedEvent {
    apc::core::DeviceId Id;
    friend bool operator==(DeviceDisconnectedEvent const&, DeviceDisconnectedEvent const&) = default;
};

struct DeviceConnectionErrorEvent {
    apc::core::DeviceId Id;
    // The legacy free-form message is deliberately normalized away here.
    // Presentation chooses localized text from this actionable category.
    AppResultCode Code = AppResultCode::OperationFailed;
    friend bool operator==(DeviceConnectionErrorEvent const&, DeviceConnectionErrorEvent const&) = default;
};

struct DeviceStatusChangedEvent {
    apc::core::DeviceId Id;
    // The legacy presentation status string is deliberately normalized to
    // this stable state; localized text is not part of a fact event.
    DeviceConnectionState State = DeviceConnectionState::Idle;
    friend bool operator==(DeviceStatusChangedEvent const&, DeviceStatusChangedEvent const&) = default;
};

// These facts intentionally carry no payload: the legacy producer publishes
// them without an ID or generation, so the bridge must not invent one.
struct DeviceActivityChangedEvent {
    friend bool operator==(DeviceActivityChangedEvent const&, DeviceActivityChangedEvent const&) = default;
};

struct DeviceInventoryChangedEvent {
    friend bool operator==(DeviceInventoryChangedEvent const&, DeviceInventoryChangedEvent const&) = default;
};

struct AutoReconnectTriggeredEvent {
    apc::core::DeviceId Id;
    friend bool operator==(AutoReconnectTriggeredEvent const&, AutoReconnectTriggeredEvent const&) = default;
};

struct AutoReconnectFailedEvent {
    apc::core::DeviceId Id;
    friend bool operator==(AutoReconnectFailedEvent const&, AutoReconnectFailedEvent const&) = default;
};

using AppEvent = std::variant<DeviceConnectedEvent,
                              DeviceDisconnectedEvent,
                              DeviceConnectionErrorEvent,
                              DeviceStatusChangedEvent,
                              DeviceActivityChangedEvent,
                              DeviceInventoryChangedEvent,
                              AutoReconnectTriggeredEvent,
                              AutoReconnectFailedEvent>;

} // namespace apc::app
