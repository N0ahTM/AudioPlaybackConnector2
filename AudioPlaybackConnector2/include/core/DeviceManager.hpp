#pragma once

#include <core/DeviceDiscoveryService.hpp>
#include <core/DeviceSessionStore.hpp>
#include <core/ReconnectController.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <util/Util.hpp>

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
    winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DeviceInformationCollection>
    RefreshDevicesAsync();

    std::vector<DeviceConnectionInfo> GetConnectedDevices() const;
    std::vector<DeviceConnectionInfo> GetConnectionSessions() const;
    [[nodiscard]] bool IsDeviceConnected(winrt::hstring const& deviceId) const;
    [[nodiscard]] std::optional<std::wstring> GetConnectionDisplayName(winrt::hstring const& deviceId) const;
    bool HasConnections() const;
    bool HasBusyOperations() const;
    bool IsDeviceBusy(winrt::hstring const& deviceId) const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Events ////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    DeviceConnectedEvent DeviceConnected;
    DeviceDisconnectedEvent DeviceDisconnected;
    ConnectionErrorEvent ConnectionError;
    DeviceStatusEvent DeviceStatusChanged;
    DeviceActivityEvent DeviceActivityChanged;
    AutoReconnectTriggeredEvent AutoReconnectTriggered;
    AutoReconnectFailedEvent AutoReconnectFailed;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    winrt::Windows::Foundation::IAsyncAction
    ConnectInternalAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device, bool openImmediately);
    winrt::Windows::Foundation::IAsyncAction
    EnableIncomingConnectionAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device);
    void EnableIncomingConnectionDetached(winrt::Windows::Devices::Enumeration::DeviceInformation device);
    void EnableIncomingConnectionsForDiscoveredDevicesDetached();
    void ReenableIncomingConnectionDetached(winrt::hstring deviceId);
    enum class DisconnectReason { UserInitiated, UserInitiatedCascade, Unexpected, Cleanup };

    void ReportConnectionFailure(winrt::hstring const& deviceId, winrt::hstring const& message, bool cleanupConnection);
    void Disconnect(winrt::hstring deviceId, DisconnectReason reason);
    void Disconnect(winrt::hstring deviceId, DisconnectReason reason, bool suppressCascade);
    bool IsConnectAttemptCurrent(winrt::hstring const& deviceId, std::size_t attemptId) const;
    void TrackUserActionCascadeLocked(winrt::hstring const& deviceId);
    bool ConsumeUserActionCascadeLocked(winrt::hstring const& deviceId);
    void PruneUserActionCascadeLocked(std::chrono::steady_clock::time_point now);
    void RestoreCascadeConnectionDetached(winrt::hstring deviceId);
    void OnConnectionStateChanged(winrt::hstring deviceId,
                                  winrt::Windows::Media::Audio::AudioPlaybackConnection sender,
                                  winrt::Windows::Foundation::IInspectable);
    void ScheduleReconnect(winrt::hstring deviceId);
    void StartConnectionHeartbeat();
    void StopConnectionHeartbeat();
    void LogConnectionSnapshot(winrt::hstring const& reason) const;
    void EnsureDiscoveryEventHandlers();
    void OnDeviceAdded(winrt::Windows::Devices::Enumeration::DeviceInformation args);
    void OnDeviceRemoved(winrt::Windows::Devices::Enumeration::DeviceInformationUpdate args);

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    mutable wil::srwlock m_lock;
    DeviceSessionStore m_sessions;
    ReconnectController m_reconnectController;
    ReconnectOnConnectionLossPredicate m_reconnectOnConnectionLossPred;
    std::unordered_map<std::wstring, std::size_t> m_connectAttemptIds;
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> m_userActionCascadeIds;
    std::unordered_set<std::wstring> m_cascadeRestoreIds;
    bool m_powerTransitionSuspended = false;
    bool m_shutdownForProcessExit = false;
    bool m_incomingConnectionsEnabled = false;

    std::shared_ptr<DeviceDiscoveryService> m_discoveryService;
    std::size_t m_discoveryDeviceAddedToken = 0;
    std::size_t m_discoveryDeviceRemovedToken = 0;
    mutable std::mutex m_heartbeatTimerMutex;
    winrt::Windows::System::Threading::ThreadPoolTimer m_heartbeatTimer{nullptr};
};
