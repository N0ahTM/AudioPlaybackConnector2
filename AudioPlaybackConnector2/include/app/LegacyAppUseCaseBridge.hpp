#pragma once

#include <app/AppModels.hpp>
#include <core/SettingsData.hpp>

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace apc::app {

// Temporary Phase 1 owner for the use cases still implemented by
// ApplicationHost. The production operation table is filled with weak
// callbacks by the host; it deliberately contains no DeviceManager,
// Settings, WinUI, pipe, JSON, notification, or localization type.
//
// Phase 4 deletes this bridge after AppController owns the use cases,
// settings side effects, snapshots, and presentation policy directly. Until
// then, the bridge is the only CoreRuntime state owner for this migration
// slice and callers must pass facts to Observe() before AppController::Publish.
class LegacyAppUseCaseBridge final {
public:
    static constexpr auto c_refreshTimeout = std::chrono::milliseconds{2500};

    struct DeviceRecord {
        // Id remains plain text here so a P01-valid external ID larger than
        // the P07 DeviceId bound can still be executed as an opaque target.
        std::wstring Id;
        std::wstring Name;
        std::wstring Alias;
        DeviceConnectionState State = DeviceConnectionState::Idle;
        bool IsConnected = false;
        bool IsKnown = false;
        bool IsBusy = false;

        friend bool operator==(DeviceRecord const&, DeviceRecord const&) = default;
    };

    enum class OperationStatus { Succeeded, Failed, Cancelled, TimedOut, Indeterminate };

    struct OperationResult {
        OperationStatus Status = OperationStatus::Failed;

        friend bool operator==(OperationResult const&, OperationResult const&) = default;
    };

    struct RefreshResult {
        OperationStatus Status = OperationStatus::Failed;
        std::vector<DeviceRecord> Devices;

        friend bool operator==(RefreshResult const&, RefreshResult const&) = default;
    };

    struct UiActionResult {
        OperationStatus Status = OperationStatus::Failed;
        std::optional<std::uint64_t> DevicePickerOpenedGeneration;

        friend bool operator==(UiActionResult const&, UiActionResult const&) = default;
    };

    struct Operations {
        using ReadDevices = std::function<std::vector<DeviceRecord>()>;
        using RefreshDevices = std::function<RefreshResult(AppCommandContext const&)>;
        using AwaitedDeviceOperation =
            std::function<OperationResult(std::wstring_view deviceId, AppCommandContext const&)>;
        using DetachedDeviceOperation = std::function<void(std::wstring_view deviceId)>;
        using DisconnectOperation = std::function<void(std::wstring_view deviceId)>;
        using SetDefaultOperation = std::function<bool(std::wstring_view deviceId)>;
        using ClearDefaultOperation = std::function<bool()>;
        using SetAliasOperation =
            std::function<bool(std::wstring_view deviceId, std::wstring_view alias, std::wstring_view deviceName)>;
        using ShowDevicePickerAction =
            std::function<UiActionResult(DevicePickerOpenMode openMode, AppCommandContext const&)>;
        using UiAction = std::function<UiActionResult(AppCommandContext const&)>;
        using ReadResourceStatus = std::function<AppSnapshot::ResourceStatusSnapshot()>;
        using ReadPickerGeneration = std::function<std::uint64_t()>;
        using IsRunning = std::function<bool()>;
        using HasBusyOperations = std::function<bool()>;
        using IsDeviceBusy = std::function<bool(std::wstring_view deviceId)>;

        // Settings are read through this value callback before every command
        // and snapshot. The bridge never acquires a Settings lock itself.
        std::function<SettingsData()> ReadSettings;
        ReadDevices ReadConnectedDevices;
        RefreshDevices Refresh;

        // Awaited callbacks must not return until the underlying operation
        // has reached a terminal status. Detached callbacks are fire-and-
        // forget and must return without waiting for that status.
        AwaitedDeviceOperation Connect;
        DetachedDeviceOperation ConnectDetached;
        AwaitedDeviceOperation Reconnect;
        DetachedDeviceOperation ReconnectDetached;
        DisconnectOperation Disconnect;
        std::function<void()> DisconnectAll;
        std::function<void()> ReconnectAllDetached;

        SetDefaultOperation SetDefaultDevice;
        ClearDefaultOperation ClearDefaultDevice;
        SetAliasOperation SetDeviceAlias;

        ShowDevicePickerAction ShowDevicePicker;
        UiAction ShowSettings;

        ReadResourceStatus ResourceStatus;
        ReadPickerGeneration PickerOpenedGeneration;
        IsRunning Running;
        HasBusyOperations HasBusy;
        IsDeviceBusy DeviceBusy;
    };

    enum class FactKind {
        DeviceConnected,
        DeviceDisconnected,
        ConnectionError,
        DeviceStatusChanged,
        DeviceActivityChanged,
        DeviceInventoryChanged,
        AutoReconnectTriggered,
        AutoReconnectFailed
    };

    // This is the narrow input shape emitted by the existing DeviceEventRouter.
    // Free-form status text is intentionally absent; the host maps its legacy
    // status kind to DeviceConnectionState before calling Observe().
    struct DeviceFact {
        FactKind Kind = FactKind::DeviceActivityChanged;
        std::wstring Id;
        DeviceConnectionState State = DeviceConnectionState::Idle;
        AppResultCode ErrorCode = AppResultCode::OperationFailed;
    };

    explicit LegacyAppUseCaseBridge(Operations operations, SettingsData settings = {});

    LegacyAppUseCaseBridge(LegacyAppUseCaseBridge const&) = delete;
    LegacyAppUseCaseBridge& operator=(LegacyAppUseCaseBridge const&) = delete;
    LegacyAppUseCaseBridge(LegacyAppUseCaseBridge&&) = delete;
    LegacyAppUseCaseBridge& operator=(LegacyAppUseCaseBridge&&) = delete;
    ~LegacyAppUseCaseBridge() = default;

    [[nodiscard]] AppResult Execute(AppCommand command, AppCommandContext context = {}) noexcept;
    [[nodiscard]] AppSnapshot Snapshot() const noexcept;

    // Normalize one legacy fact, update bridge-owned state, and return the
    // exact typed fact for the host to pass to AppController::Publish. The
    // returned value owns all data and creates no observer/lifetime cycle.
    [[nodiscard]] std::optional<AppEvent> Observe(DeviceFact fact) noexcept;

    // Runtime teardown is a composition concern, but the snapshot must stop
    // advertising a live app before the host tears down its callbacks.
    void SetRunning(bool running) noexcept;

private:
    // Every public entry point acquires this lease before it can touch an
    // operation callback or bridge state. SetRunning(false) closes admission
    // and waits for outstanding leases without holding m_stateMutex, so host
    // teardown cannot race a late callback or fact publication.
    class CallLease final {
    public:
        explicit CallLease(LegacyAppUseCaseBridge const& owner) noexcept;
        CallLease(CallLease const&) = delete;
        CallLease& operator=(CallLease const&) = delete;
        ~CallLease();

        [[nodiscard]] bool Acquired() const noexcept { return m_acquired; }

    private:
        LegacyAppUseCaseBridge const& m_owner;
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

    [[nodiscard]] AppResult ExecuteCommand(AppCommand const& command, AppCommandContext const& context);
    [[nodiscard]] AppResult ExecuteTargetOperation(AppCommand const& command,
                                                   AppCommandContext const& context,
                                                   std::vector<DeviceRecord> const& devices);
    [[nodiscard]] AppResult ExecuteToggle(AppCommand const& command,
                                          AppCommandContext const& context,
                                          std::vector<DeviceRecord> const& devices);
    [[nodiscard]] Resolution Resolve(DeviceSelector const& selector, std::vector<DeviceRecord> const& devices) const;

    [[nodiscard]] std::vector<DeviceRecord> BuildDevices(bool refresh, AppCommandContext const& context);
    [[nodiscard]] std::vector<DeviceRecord> BuildDevicesWithoutRefresh() const;
    [[nodiscard]] bool SyncSettingsFromSource() const noexcept;
    [[nodiscard]] std::vector<DeviceRecord> ReadConnectedDevices() const;
    [[nodiscard]] AppSnapshot SnapshotFromDevices(std::vector<DeviceRecord> devices) const noexcept;
    [[nodiscard]] std::vector<DeviceRecord>
    MergeDevices(std::vector<DeviceRecord> refreshed, std::vector<DeviceRecord> connected, SettingsData settings) const;
    [[nodiscard]] AppSnapshot BuildSnapshot(std::vector<DeviceRecord> devices,
                                            SettingsData settings,
                                            std::uint64_t generation,
                                            std::uint64_t pickerGeneration,
                                            bool isRunning) const noexcept;

    [[nodiscard]] AppResult MakeFailure(AppCommandKind command,
                                        AppResultCode code,
                                        AppOutcomeReason reason,
                                        std::wstring requestedTarget = {}) const;
    [[nodiscard]] AppResult MakeTargetResult(AppCommandKind command,
                                             Resolution const& resolution,
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
    // inputs. Mutations instead admit against the original P01 context at the
    // callback commit point, so a cancelled command cannot act on that
    // fallback after it has already entered the bridge.
    [[nodiscard]] static std::optional<AppResultCode>
    MutationAdmissionFailure(AppCommandContext const& context) noexcept;
    [[nodiscard]] static bool IsRefreshNeeded(AppCommandKind command,
                                              DeviceSelectorKind selectorKind = DeviceSelectorKind::Id) noexcept;
    [[nodiscard]] static AppCommandContext CappedRefreshContext(AppCommandContext const& context);
    static void AdvanceGeneration(std::uint64_t& generation) noexcept;
    void ApplyObservedStates(std::vector<DeviceRecord>& devices) const;
    [[nodiscard]] bool PrivacyMode() const noexcept;

    Operations m_operations;
    mutable std::mutex m_stateMutex;
    mutable std::condition_variable m_noActiveCalls;
    mutable SettingsData m_settings;
    mutable std::unordered_map<std::wstring, DeviceRecord> m_observedStates;
    std::uint64_t m_generation = 0;
    std::uint64_t m_pickerGeneration = 0;
    mutable std::size_t m_activeCalls = 0;
    bool m_running = true;
};

} // namespace apc::app
