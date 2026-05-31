#pragma once

#include <core/DeviceDiscoveryService.hpp>
#include <core/DeviceSessionStore.hpp>
#include <core/ReconnectController.hpp>
#include <string>
#include <unordered_set>
#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Manager ////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class DeviceManager : public std::enable_shared_from_this<DeviceManager> {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using DeviceConnectedEvent = Event<winrt::hstring>;
    using DeviceDisconnectedEvent = Event<winrt::hstring>;
    using ConnectionErrorEvent = Event<winrt::hstring, winrt::hstring>;
    using DeviceStatusEvent =
        Event<winrt::hstring, winrt::hstring, winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions>;
    using DeviceActivityEvent = Event<winrt::hstring>;
    using AutoReconnectTriggeredEvent = Event<winrt::hstring>;
    using AutoReconnectFailedEvent = Event<winrt::hstring>;
    using AutoReconnectPredicate = std::function<bool(winrt::hstring const&)>;

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
    void SetAutoReconnectPredicate(AutoReconnectPredicate pred);
    winrt::Windows::Foundation::IAsyncAction ConnectAsync(winrt::hstring deviceId);
    void ConnectDetached(winrt::hstring deviceId);
    winrt::fire_and_forget ReconnectAsync(winrt::hstring deviceId);
    void Disconnect(winrt::hstring deviceId);
    void SetAutoReconnect(winrt::hstring deviceId, bool enabled);

    std::vector<DeviceConnectionInfo> GetConnectedDevices() const;
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
    ConnectInternalAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device);
    enum class DisconnectReason { UserInitiated, Unexpected, Cleanup };

    void ReportConnectionFailure(winrt::hstring const& deviceId, winrt::hstring const& message, bool cleanupConnection);
    void Disconnect(winrt::hstring deviceId, DisconnectReason reason);
    bool IsConnectAttemptCurrent(winrt::hstring const& deviceId, std::size_t attemptId) const;
    void OnConnectionStateChanged(winrt::Windows::Media::Audio::AudioPlaybackConnection sender,
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
    AutoReconnectPredicate m_autoReconnectPred;
    std::unordered_map<std::wstring, std::size_t> m_connectAttemptIds;
    bool m_powerTransitionSuspended = false;
    bool m_shutdownForProcessExit = false;

    std::unique_ptr<DeviceDiscoveryService> m_discoveryService;
    std::size_t m_discoveryDeviceAddedToken = 0;
    std::size_t m_discoveryDeviceRemovedToken = 0;
    winrt::Windows::System::Threading::ThreadPoolTimer m_heartbeatTimer{nullptr};
};
