#pragma once

#include <core/DeviceDiscoveryService.hpp>
#include <core/DeviceOperationCoordinator.hpp>
#include <core/DeviceSessionStore.hpp>
#include <core/DeviceTrayPresentation.hpp>
#include <core/ReconnectController.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <util/Event.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Manager ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

enum class DeviceStatusKind { None, Ready, Connecting, Reconnecting, Connected, Error };

class DeviceManager : public std::enable_shared_from_this<DeviceManager> {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using DeviceConnectedEvent = Event<winrt::hstring>;
    using DeviceDisconnectedEvent = Event<winrt::hstring>;
    using ConnectionErrorEvent = Event<winrt::hstring, winrt::hstring>;
    using DeviceStatusEvent = Event<winrt::hstring,
                                    winrt::hstring,
                                    winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions,
                                    DeviceStatusKind>;
    using DeviceActivityEvent = Event<winrt::hstring>;
    using DeviceInventoryChangedEvent = Event<>;
    using AutoReconnectTriggeredEvent = Event<winrt::hstring>;
    using AutoReconnectFailedEvent = Event<winrt::hstring>;
    using ReconnectOnConnectionLossPredicate = std::function<bool(winrt::hstring const&)>;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    DeviceManager();
    void StartDeviceWatcher();
    void StopDeviceWatcher();
    void ShutdownForProcessExit() noexcept;
    void SuspendForPowerTransition() noexcept;
    void ResumeAfterPowerTransition();
    void CancelPendingReconnects();
    void SetReconnectOnConnectionLossPredicate(ReconnectOnConnectionLossPredicate pred);
    void SetIncomingConnectionsEnabled(bool enabled);
    winrt::Windows::Foundation::IAsyncAction ConnectAsync(winrt::hstring deviceId);
    void ConnectDetached(winrt::hstring deviceId);
    winrt::Windows::Foundation::IAsyncAction ReconnectAsync(winrt::hstring deviceId);
    void ReconnectDetached(winrt::hstring deviceId);
    void Disconnect(winrt::hstring deviceId);
    void DisconnectAll();
    void ReconnectAll();
    void SetReconnectOnConnectionLoss(winrt::hstring deviceId, bool enabled);
    void ApplyReconnectOnConnectionLossPolicy(bool globallyEnabled,
                                              std::span<const std::wstring> individuallyEnabledDeviceIds);
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
    RefreshDevicesAsync();

    std::vector<DeviceConnectionInfo> GetConnectedDevices() const;
    std::vector<DeviceConnectionInfo> GetConnectionSessions() const;
    [[nodiscard]] bool IsDeviceConnected(winrt::hstring const& deviceId) const;
    [[nodiscard]] std::optional<std::wstring> GetConnectionDisplayName(winrt::hstring const& deviceId) const;
    bool HasConnections() const;
    bool HasBusyOperations() const;
    bool IsDeviceBusy(winrt::hstring const& deviceId) const;
    [[nodiscard]] apc::device_picker::DeviceActivitySnapshot GetDevicePickerActivitySnapshot() const;
    [[nodiscard]] apc::device_picker::DeviceInventorySnapshot GetDevicePickerInventorySnapshot() const;
    [[nodiscard]] DeviceTrayPresentationSnapshot GetTrayPresentationSnapshot() const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Events ////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    DeviceConnectedEvent DeviceConnected;
    DeviceDisconnectedEvent DeviceDisconnected;
    ConnectionErrorEvent ConnectionError;
    DeviceStatusEvent DeviceStatusChanged;
    DeviceActivityEvent DeviceActivityChanged;
    DeviceInventoryChangedEvent DeviceInventoryChanged;
    AutoReconnectTriggeredEvent AutoReconnectTriggered;
    AutoReconnectFailedEvent AutoReconnectFailed;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using ConnectionIntent = apc::device_operation::Intent;
    using OperationPhase = apc::device_operation::Phase;
    using OperationToken = apc::device_operation::Token;

    struct CloseBarrier {
        CloseBarrier() { Completed.create(wil::EventOptions::ManualReset); }

        wil::unique_event Completed;
    };

    struct ReconnectPolicyCleanup {
        winrt::hstring DeviceId;
        winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
        winrt::event_token StateChangedToken{};
        std::shared_ptr<CloseBarrier> Barrier;
        bool RestoreIncoming = false;
    };

    winrt::Windows::Foundation::IAsyncOperation<bool> ConnectWithIntentAsync(winrt::hstring deviceId,
                                                                             OperationToken operation);
    winrt::Windows::Foundation::IAsyncOperation<bool> ReconnectWithIntentAsync(winrt::hstring deviceId,
                                                                               OperationToken operation);
    winrt::Windows::Foundation::IAsyncOperation<bool>
    ConnectInternalAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device,
                         bool openImmediately,
                         OperationToken operation,
                         bool reportFailures);
    winrt::Windows::Foundation::IAsyncAction
    EnableIncomingConnectionAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device);
    void EnableIncomingConnectionDetached(winrt::Windows::Devices::Enumeration::DeviceInformation device);
    void EnableIncomingConnectionsForDiscoveredDevicesDetached();
    void ReenableIncomingConnectionDetached(winrt::hstring deviceId);
    enum class DisconnectReason { UserInitiated, UserInitiatedCascade, Unexpected, Cleanup };

    void ReportConnectionFailure(winrt::hstring const& deviceId, winrt::hstring const& message, bool cleanupConnection);
    void Disconnect(winrt::hstring deviceId, DisconnectReason reason);
    void Disconnect(winrt::hstring deviceId, DisconnectReason reason, bool suppressCascade);
    [[nodiscard]] std::optional<OperationToken>
    TryBeginOperationLocked(winrt::hstring const& deviceId, ConnectionIntent intent, OperationPhase phase);
    void InvalidateDeviceOperationLocked(winrt::hstring const& deviceId);
    [[nodiscard]] std::optional<ReconnectPolicyCleanup>
    PrepareReconnectPolicyCleanupLocked(winrt::hstring const& deviceId);
    void StartReconnectPolicyCleanup(ReconnectPolicyCleanup cleanup) noexcept;
    void CompleteDeviceOperation(OperationToken const& operation) noexcept;
    [[nodiscard]] bool IsOperationCurrent(OperationToken const& operation) const;
    [[nodiscard]] bool IsOperationCurrentLocked(OperationToken const& operation) const;
    [[nodiscard]] bool IsConnectAttemptCurrent(OperationToken const& operation, std::size_t attemptId) const;
    winrt::Windows::Foundation::IAsyncOperation<bool> WaitForCloseBarrierAsync(OperationToken operation,
                                                                               bool waitIndefinitely);
    [[nodiscard]] std::shared_ptr<CloseBarrier> InstallCloseBarrierLocked(winrt::hstring const& deviceId);
    void
    CompleteCloseBarrierDetached(winrt::hstring deviceId, std::shared_ptr<CloseBarrier> barrier, bool restoreIncoming);
    void AutoReconnectAttemptDetached(ReconnectController::TimerToken token);
    void StartReconnectTimer(ReconnectController::ScheduleDecision const& decision, bool notifyTriggered);
    void NotifyAutoReconnectFailed(winrt::hstring const& deviceId, std::size_t maxAttempts);
    void TrackUserActionCascadeLocked(winrt::hstring const& deviceId);
    bool ConsumeUserActionCascadeLocked(winrt::hstring const& deviceId);
    void PruneUserActionCascadeLocked(std::chrono::steady_clock::time_point now);
    void OnConnectionStateChanged(winrt::hstring deviceId,
                                  winrt::Windows::Media::Audio::AudioPlaybackConnection sender,
                                  winrt::Windows::Foundation::IInspectable);
    void ScheduleReconnect(winrt::hstring deviceId);
    void LogConnectionSnapshot(winrt::hstring const& reason) const;
    void EnsureDiscoveryEventHandlers();
    void OnDeviceAdded(winrt::Windows::Devices::Enumeration::DeviceInformation args);
    void OnDeviceRemoved(winrt::Windows::Devices::Enumeration::DeviceInformationUpdate args);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    mutable wil::srwlock m_lock;
    DeviceSessionStore m_sessions;
    apc::device_operation::DeviceOperationCoordinator m_deviceOperations;
    ReconnectController m_reconnectController;
    ReconnectOnConnectionLossPredicate m_reconnectOnConnectionLossPred;
    std::unordered_map<std::wstring, std::size_t> m_connectAttemptIds;
    std::unordered_map<std::wstring, std::shared_ptr<CloseBarrier>> m_closeBarriers;
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> m_userActionCascadeIds;
    bool m_powerTransitionSuspended = false;
    bool m_shutdownForProcessExit = false;
    bool m_incomingConnectionsEnabled = false;

    std::shared_ptr<DeviceDiscoveryService> m_discoveryService;
    std::size_t m_discoveryDeviceAddedToken = 0;
    std::size_t m_discoveryDeviceRemovedToken = 0;
    std::size_t m_discoveryInventoryChangedToken = 0;
};
