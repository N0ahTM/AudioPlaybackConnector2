#pragma once

#include <app/AppModels.hpp>
#include <app/AppPresentation.hpp>
#include <core/SettingsStore.hpp>
#include <condition_variable>
#include <mutex>
#include <app/DeviceFactPublicationFence.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

class StartupTaskCoordinator;

namespace apc::device {
class DeviceService;
struct DeviceServiceSnapshot;
} // namespace apc::device

namespace apc::app {

// The controller is a transport- and UI-free application boundary.
class AppController final {
    struct EventState;

public:
    struct EventNotification {
        std::uint64_t Revision = 0;
        AppEvent Event;
        std::optional<DeviceFactPublicationFence::Token> DeviceToken;
    };
    using EventHandler = std::function<void(EventNotification const&)>;
    using SubscriptionId = std::uint64_t;

    class Subscription final {
    public:
        Subscription() noexcept = default;
        ~Subscription();

        Subscription(Subscription const&) = delete;
        Subscription& operator=(Subscription const&) = delete;
        Subscription(Subscription&& other) noexcept;
        Subscription& operator=(Subscription&& other) noexcept;

        void Reset() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept { return m_id != 0; }

    private:
        friend class AppController;

        Subscription(std::weak_ptr<EventState> state, SubscriptionId id) noexcept
            : m_state(std::move(state)), m_id(id) {}

        std::weak_ptr<EventState> m_state;
        SubscriptionId m_id = 0;
    };

    struct Observation {
        AppSnapshot Snapshot;
        std::uint64_t Revision = 0;
        Subscription Updates;
    };

    AppController(std::shared_ptr<SettingsStore> settings,
                  std::shared_ptr<apc::device::DeviceService> devices,
                  std::weak_ptr<AppPresentation> presentation = {},
                  std::shared_ptr<StartupTaskCoordinator> startupTask = {});
    enum class StartupConnectionStatus { Unavailable, NoTargets, Submitted };
    // Lifecycle action: submits targets from one settings snapshot to the device owner.
    // Submitted does not mean the asynchronous connections have succeeded.
    [[nodiscard]] StartupConnectionStatus RestoreStartupConnections() const;

    // Callback-safe: closes admission without waiting for admitted calls or observers.
    // Already admitted calls may finish; the lifecycle owner drains them with Shutdown.
    void RequestStop() noexcept;
    // Lifecycle join: call outside controller calls and owner/observer callbacks.
    // Keep the controller alive until every admitted caller has returned.
    void Shutdown() noexcept;
    ~AppController();

    AppController(AppController const&) = delete;
    AppController& operator=(AppController const&) = delete;
    AppController(AppController&&) = delete;
    AppController& operator=(AppController&&) = delete;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Settings Actions //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    enum class StartupTaskRequestResult { Accepted, Unavailable };
    StartupTaskRequestResult RefreshStartupTask() const noexcept;
    StartupTaskRequestResult SetStartWithWindows(bool enabled) const noexcept;

    SettingsMutationResult SetGlobalConnectOnStartup(bool enabled) const;
    SettingsMutationResult SetGlobalReconnectOnConnectionLoss(bool enabled) const;
    SettingsMutationResult SetAllowIncomingConnections(bool enabled) const;
    SettingsMutationResult SetShowNotifications(bool enabled) const;
    SettingsMutationResult SetSystemBackdropEffects(bool enabled) const;
    SettingsMutationResult SetPrivacyMode(bool enabled) const;
    SettingsMutationResult SetLanguage(std::wstring language) const;
    SettingsMutationResult SetSettingsWindowBounds(PersistedWindowBounds bounds) const;
    SettingsMutationResult ClearSettingsWindowBounds() const;
    SettingsMutationResult SetDeviceConnectOnStartup(std::wstring const& id, bool enabled) const;
    SettingsMutationResult SetDeviceReconnectOnConnectionLoss(std::wstring const& id, bool enabled) const;
    SettingsMutationResult ForgetDevice(std::wstring const& id) const;

    [[nodiscard]] AppSnapshot Snapshot() const noexcept;
    // Snapshot capture and registration share an event-revision fence. A callback
    // can run before return; consumers serialize applying the initial value and
    // updates, retaining the largest Revision they have already applied.
    [[nodiscard]] Observation SnapshotAndSubscribe(EventHandler handler);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// UI Actions ////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] AppResult ShowDevicePicker(DevicePickerOpenMode mode, AppCommandContext context = {}) const noexcept;
    [[nodiscard]] AppResult ShowSettings(AppCommandContext context = {}) const noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Device Actions ////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] AppResult Connect(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult Disconnect(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult Reconnect(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult ToggleDefault(AppCommandContext context) const;
    [[nodiscard]] AppResult Toggle(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult DisconnectAll(AppCommandContext context) const noexcept;
    [[nodiscard]] AppResult ReconnectAll(AppCommandContext context) const noexcept;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Device Settings ///////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] AppResult SetDefault(std::wstring_view deviceId) const;
    [[nodiscard]] AppResult ClearDefault(AppCommandContext context = {}) const;
    [[nodiscard]] AppResult SetAlias(std::wstring_view deviceId, std::wstring_view alias) const;
    [[nodiscard]] AppResult ClearAlias(std::wstring_view deviceId) const;
    [[nodiscard]] AppResult SetDefault(DeviceSelector target, AppCommandContext context) const;
    [[nodiscard]] AppResult SetAlias(DeviceSelector target, std::wstring_view alias, AppCommandContext context) const;
    [[nodiscard]] AppResult ClearAlias(DeviceSelector target, AppCommandContext context) const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Queries ///////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] AppResult ListDevices(AppCommandContext context) const noexcept;
    [[nodiscard]] AppResult Status(AppCommandContext context) const noexcept;
    [[nodiscard]] AppResult ShowDefault(AppCommandContext context) const noexcept;
    [[nodiscard]] AppResult ListAliases(AppCommandContext context) const noexcept;

    // Publications have one total revision order. Reentrant and concurrent publications queue behind the current
    // delivery; callbacks never overlap. Reset drains an admitted callback, except when called by that callback.
    [[nodiscard]] Subscription Subscribe(EventHandler handler);
    void Publish(AppEvent const& event) const noexcept;
    [[nodiscard]] bool IsCurrent(EventNotification const& notification) const;

private:
    template <typename Action>
    [[nodiscard]] AppResult
    WithAdmission(AppCommandKind kind, AppCommandContext context, Action&& action) const noexcept;
    template <typename Action>
    [[nodiscard]] AppResult
    WithSettings(AppCommandKind kind, AppCommandContext context, Action&& action) const noexcept;
    [[nodiscard]] AppResult
    PresentationResult(AppCommandKind kind, AppUiActionResult const& action, SettingsData const& settings) const;
    [[nodiscard]] AppResult WriteAlias(AppCommandKind kind,
                                       DeviceSelector const& target,
                                       std::wstring_view alias,
                                       AppCommandContext context) const;

    template <typename Mutation> SettingsMutationResult MutateSettings(Mutation&& mutation) const noexcept;
    bool RememberKnownDevice(std::wstring const& id) const;

    static constexpr auto c_refreshTimeout = std::chrono::milliseconds{2500};
    using OperationStatus = AppActionStatus;
    struct OperationResult {
        OperationStatus Status = OperationStatus::Failed;
    };
    struct DeviceRecord {
        // External identities remain opaque even when they exceed the
        // persistence format's DeviceId bound.
        std::wstring Id;
        std::wstring Name;
        std::wstring Alias;
        DeviceConnectionState State = DeviceConnectionState::Idle;
        bool IsConnected = false;
        bool IsKnown = false;
        bool IsBusy = false;

        friend bool operator==(DeviceRecord const&, DeviceRecord const&) = default;
    };

    // Commands and snapshots acquire a lease before accessing their owners.
    // Shutdown closes admission and waits for these leases before host teardown.
    class CallLease final {
    public:
        explicit CallLease(AppController const& owner) noexcept;
        CallLease(CallLease const&) = delete;
        CallLease& operator=(CallLease const&) = delete;
        ~CallLease();

        [[nodiscard]] bool Acquired() const noexcept { return m_acquired; }

    private:
        AppController const& m_owner;
        bool m_acquired = false;
    };

    struct Resolution {
        AppResultCode Code = AppResultCode::Success;
        AppOutcomeReason Reason = AppOutcomeReason::None;
        std::wstring RequestedTarget;
        std::optional<DeviceRecord> Device;
        AppTargetSnapshot Target;
        bool HasTarget = false;
    };

    [[nodiscard]] AppResult RunDeviceOperation(AppCommandKind kind,
                                               DeviceSelector const& target,
                                               AppCommandContext const& context,
                                               std::vector<DeviceRecord> const& devices,
                                               SettingsData const& settings) const;
    [[nodiscard]] Resolution Resolve(DeviceSelector const& selector,
                                     std::vector<DeviceRecord> const& devices,
                                     SettingsData const& settings) const;

    [[nodiscard]] std::vector<DeviceRecord>
    BuildDevices(bool refresh, AppCommandContext const& context, SettingsData const& settings) const;
    [[nodiscard]] std::vector<DeviceRecord> BuildDevicesWithoutRefresh(SettingsData const& settings) const;
    [[nodiscard]] std::optional<SettingsSnapshot> ReadSettings() const noexcept;
    // Concurrent readers may complete out of order. Retry outside the mutex
    // when another reader has already observed a newer Store revision.
    [[nodiscard]] std::optional<SettingsSnapshot> ReadCoherentSettings() const noexcept;
    [[nodiscard]] static std::vector<DeviceRecord> SessionRecords(apc::device::DeviceServiceSnapshot const& snapshot);
    [[nodiscard]] AppSnapshot CaptureSnapshot() const;
    void RefreshDevices(AppCommandContext const& context) const;
    [[nodiscard]] std::vector<DeviceRecord> MergeDevices(std::vector<DeviceRecord> refreshed,
                                                         std::vector<DeviceRecord> connected,
                                                         SettingsData const& settings) const;
    [[nodiscard]] AppSnapshot BuildSnapshot(std::vector<DeviceRecord> devices,
                                            SettingsData const& settings,
                                            std::uint64_t generation,
                                            std::uint64_t pickerGeneration,
                                            bool isRunning) const;

    [[nodiscard]] AppResult DeviceQueryResult(AppSnapshot snapshot) const;
    [[nodiscard]] AppResult MakeFailure(AppCommandKind command,
                                        AppResultCode code,
                                        AppOutcomeReason reason,
                                        std::wstring requestedTarget = {}) const;
    [[nodiscard]] AppResult MakeFailure(AppCommandKind command,
                                        AppResultCode code,
                                        AppOutcomeReason reason,
                                        SettingsData const& settings,
                                        std::wstring requestedTarget = {}) const;
    [[nodiscard]] AppResult MakeTargetResult(AppCommandKind command,
                                             Resolution const& resolution,
                                             SettingsData const& settings,
                                             AppResultCode code,
                                             AppOutcomeReason reason) const;
    [[nodiscard]] std::optional<DeviceSnapshot> ToSnapshot(DeviceRecord const& record) const;
    [[nodiscard]] std::optional<AppTargetSnapshot> ToTarget(DeviceRecord const& record) const;
    [[nodiscard]] std::optional<DeviceSnapshot> PostOperationDevice(std::wstring_view deviceId,
                                                                    std::vector<DeviceRecord> const& devices) const;

    [[nodiscard]] static std::wstring DeviceLabel(DeviceRecord const& device);
    [[nodiscard]] static bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right);
    [[nodiscard]] static bool ContainsIgnoreCase(std::wstring_view value, std::wstring_view query);
    [[nodiscard]] static std::wstring NormalizeHex(std::wstring_view value);
    [[nodiscard]] static std::optional<DeviceRecord> FindById(std::vector<DeviceRecord> const& devices,
                                                              std::wstring_view id);
    [[nodiscard]] static std::optional<apc::core::DeviceId> TryDeviceId(std::wstring_view id);
    [[nodiscard]] static AppResultCode ToResultCode(OperationStatus status) noexcept;
    [[nodiscard]] static AppOutcomeReason OperationReason(AppCommandKind command) noexcept;
    [[nodiscard]] static bool IsSuccess(OperationStatus status) noexcept;
    // A refresh may use a shorter private deadline and fall back to current
    // inputs. Mutations recheck the caller's context after resolution so a
    // cancelled command cannot act on that fallback.
    [[nodiscard]] static std::optional<AppResultCode>
    MutationAdmissionFailure(AppCommandContext const& context) noexcept;
    [[nodiscard]] static bool IsRefreshNeeded(DeviceSelectorKind selectorKind) noexcept;
    [[nodiscard]] static AppCommandContext CappedRefreshContext(AppCommandContext const& context);
    static void AdvanceGeneration(std::uint64_t& generation) noexcept;
    void ApplySessionStates(std::vector<DeviceRecord>& devices,
                            std::vector<DeviceRecord> const& connectedDevices) const;
    [[nodiscard]] bool PrivacyMode(SettingsData const& settings) const noexcept;

    [[nodiscard]] OperationResult
    PerformDeviceOperation(AppCommandKind command, std::wstring_view id, AppCommandContext const& context) const;
    std::shared_ptr<SettingsStore> m_settings;
    std::weak_ptr<AppPresentation> m_presentation;
    mutable std::mutex m_stateMutex;
    mutable std::condition_variable m_noActiveCalls;
    mutable std::optional<std::uint64_t> m_lastSettingsRevision;
    mutable std::optional<std::uint64_t> m_lastDeviceGeneration;
    mutable std::optional<std::uint64_t> m_lastStartupPublication;
    mutable std::uint64_t m_generation = 0;
    mutable std::uint64_t m_pickerGeneration = 0;
    mutable std::size_t m_activeCalls = 0;
    bool m_running = true;
    std::shared_ptr<EventState> m_eventState;
    std::shared_ptr<apc::device::DeviceService> m_devices;
    std::uint64_t m_deviceSubscription = 0;
    SettingsStore::Subscription m_settingsSubscription;
    std::shared_ptr<StartupTaskCoordinator> m_startupTask;
    std::uint64_t m_startupSubscription = 0;
};

} // namespace apc::app
