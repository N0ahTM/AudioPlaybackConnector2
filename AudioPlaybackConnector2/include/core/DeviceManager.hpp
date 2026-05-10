#pragma once

#include <unordered_set>
#include <util/Util.hpp>

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Device Connection Info ////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

struct DeviceConnectionInfo {
    winrt::Windows::Devices::Enumeration::DeviceInformation Device{nullptr};
    winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
    winrt::event_token StateChangedToken{};
    bool AutoReconnect = false;
    bool IsOpen = false;
};

/*------------------------------------------------------------------------------------------------------------------*/
/*//////// Device Manager ////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------------*/

class DeviceManager : public std::enable_shared_from_this<DeviceManager> {
public:
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Type Aliases //////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    using DeviceConnectedEvent = Event<winrt::hstring>;
    using DeviceDisconnectedEvent = Event<winrt::hstring>;
    using ConnectionErrorEvent = Event<winrt::hstring, winrt::hstring>;
    using DeviceStatusEvent = Event<winrt::hstring, winrt::hstring, winrt::Windows::Devices::Enumeration::DevicePickerDisplayStatusOptions>;
    using AutoReconnectTriggeredEvent = Event<winrt::hstring>;
    using AutoReconnectFailedEvent = Event<winrt::hstring>;
    using AutoReconnectPredicate = std::function<bool(winrt::hstring const&)>;

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Public Interface //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    void StartDeviceWatcher();
    void StopDeviceWatcher();
    void CancelPendingReconnects();
    void SetAutoReconnectPredicate(AutoReconnectPredicate pred);
    winrt::Windows::Foundation::IAsyncAction ConnectAsync(winrt::hstring deviceId);
    winrt::fire_and_forget ReconnectAsync(winrt::hstring deviceId);
    void Disconnect(winrt::hstring deviceId);
    void SetAutoReconnect(winrt::hstring deviceId, bool enabled);

    std::vector<DeviceConnectionInfo> GetConnectedDevices() const;
    bool HasConnections() const;

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Events ////////////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    DeviceConnectedEvent DeviceConnected;
    DeviceDisconnectedEvent DeviceDisconnected;
    ConnectionErrorEvent ConnectionError;
    DeviceStatusEvent DeviceStatusChanged;
    AutoReconnectTriggeredEvent AutoReconnectTriggered;
    AutoReconnectFailedEvent AutoReconnectFailed;

private:
    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Private Implementation ////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    winrt::Windows::Foundation::IAsyncAction ConnectInternalAsync(winrt::Windows::Devices::Enumeration::DeviceInformation device);
    enum class DisconnectReason { UserInitiated,
                                  Reconnect,
                                  Unexpected,
                                  Cleanup };

    void Disconnect(winrt::hstring deviceId, DisconnectReason reason);
    bool IsConnectAttemptCurrent(winrt::hstring const& deviceId, std::size_t attemptId) const;
    void OnConnectionStateChanged(winrt::Windows::Media::Audio::AudioPlaybackConnection sender, winrt::Windows::Foundation::IInspectable);
    void ScheduleReconnect(winrt::hstring deviceId);
    void StartConnectionHeartbeat();
    void StopConnectionHeartbeat();
    void LogConnectionSnapshot(winrt::hstring const& reason) const;
    void OnDeviceAdded(winrt::Windows::Devices::Enumeration::DeviceWatcher sender, winrt::Windows::Devices::Enumeration::DeviceInformation args);
    void OnDeviceRemoved(winrt::Windows::Devices::Enumeration::DeviceWatcher sender, winrt::Windows::Devices::Enumeration::DeviceInformationUpdate args);

    /*------------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------------*/

    mutable wil::srwlock m_lock;
    std::unordered_map<winrt::hstring, DeviceConnectionInfo> m_connections;
    std::unordered_set<winrt::hstring> m_disconnectingIds;
    std::unordered_set<winrt::hstring> m_reconnectingIds;
    std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> m_zombieConnections;
    std::vector<winrt::Windows::Devices::Enumeration::DeviceInformation> m_deviceCache;
    AutoReconnectPredicate m_autoReconnectPred;
    std::unordered_set<winrt::hstring> m_cancelledReconnectIds;
    std::unordered_map<winrt::hstring, std::size_t> m_reconnectTimerCounts;
    std::unordered_map<winrt::hstring, std::size_t> m_reconnectAttempts;
    std::unordered_map<winrt::hstring, std::size_t> m_connectAttemptIds;
    bool m_watcherStopping = false;
    bool m_allReconnectsCancelled = false;

    winrt::Windows::Devices::Enumeration::DeviceWatcher m_watcher{nullptr};
    winrt::event_token m_watcherAddedToken{};
    winrt::event_token m_watcherRemovedToken{};
    winrt::Windows::System::Threading::ThreadPoolTimer m_heartbeatTimer{nullptr};
};
