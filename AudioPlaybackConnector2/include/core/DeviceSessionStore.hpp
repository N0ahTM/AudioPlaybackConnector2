#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Connection Info //////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

struct DeviceConnectionInfo {
    winrt::Windows::Devices::Enumeration::DeviceInformation Device{nullptr};
    winrt::Windows::Media::Audio::AudioPlaybackConnection Connection{nullptr};
    winrt::event_token StateChangedToken{};
    bool AutoReconnect = false;
    bool IsOpen = false;
};

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Session Store ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class DeviceSessionStore {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Queries //////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] std::vector<DeviceConnectionInfo> ConnectedDevices() const;
    [[nodiscard]] bool HasConnections() const;
    [[nodiscard]] bool HasBusyOperations() const;
    [[nodiscard]] bool IsDeviceBusy(winrt::hstring const& deviceId) const;
    [[nodiscard]] bool HasConnection(winrt::hstring const& deviceId) const;
    [[nodiscard]] std::optional<DeviceConnectionInfo> FindConnection(winrt::hstring const& deviceId) const;
    [[nodiscard]] bool IsDisconnecting(winrt::hstring const& deviceId) const;
    [[nodiscard]] bool IsReconnecting(winrt::hstring const& deviceId) const;
    [[nodiscard]] bool IsConnecting(winrt::hstring const& deviceId) const;
    [[nodiscard]] std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> TakeZombieConnections();
    [[nodiscard]] std::size_t ConnectionCount() const;
    [[nodiscard]] std::optional<DeviceConnectionInfo> ExtractConnection(winrt::hstring const& deviceId);
    [[nodiscard]] std::vector<std::pair<winrt::hstring, DeviceConnectionInfo>> ExtractAllConnections();
    [[nodiscard]] std::vector<std::pair<winrt::hstring, DeviceConnectionInfo>> GetConnectionsSnapshot() const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Mutations //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Clear();
    void SetConnectionAutoReconnect(winrt::hstring const& deviceId, bool enabled);
    void UpdateConnectionIsOpen(winrt::hstring const& deviceId, bool isOpen);
    void InsertOrUpdateConnection(winrt::hstring const& deviceId, DeviceConnectionInfo info);
    void EraseConnection(winrt::hstring const& deviceId);
    void MarkDisconnecting(winrt::hstring const& deviceId);
    void UnmarkDisconnecting(winrt::hstring const& deviceId);
    void MarkReconnecting(winrt::hstring const& deviceId);
    void UnmarkReconnecting(winrt::hstring const& deviceId);
    void MarkConnecting(winrt::hstring const& deviceId);
    void UnmarkConnecting(winrt::hstring const& deviceId);
    void AddZombie(winrt::Windows::Media::Audio::AudioPlaybackConnection connection);

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables ////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using ConnectionMap = std::unordered_map<winrt::hstring, DeviceConnectionInfo>;
    using DeviceIdSet = std::unordered_set<winrt::hstring>;
    using ZombieConnectionList = std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection>;

    ConnectionMap m_connections;
    DeviceIdSet m_disconnectingIds;
    DeviceIdSet m_reconnectingIds;
    DeviceIdSet m_connectingIds;
    ZombieConnectionList m_zombieConnections;
    mutable wil::srwlock m_lock;
};
