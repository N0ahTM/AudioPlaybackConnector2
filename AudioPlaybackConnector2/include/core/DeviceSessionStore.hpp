#pragma once

#include <core/DeviceConnectionInfo.hpp>
#include <core/DevicePickerSnapshot.hpp>
#include <core/DeviceTrayPresentation.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Device Session Store ////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

class DeviceSessionStore {
public:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Queries //////////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    [[nodiscard]] std::vector<DeviceConnectionInfo> ConnectedDevices() const;
    [[nodiscard]] std::vector<DeviceTrayPresentationItem> ConnectedDevicePresentations() const;
    [[nodiscard]] bool HasConnections() const;
    [[nodiscard]] bool HasConnection(winrt::hstring const& deviceId) const;
    [[nodiscard]] std::optional<DeviceConnectionInfo> FindConnection(winrt::hstring const& deviceId) const;
    [[nodiscard]] bool IsDisconnecting(winrt::hstring const& deviceId) const;
    [[nodiscard]] std::optional<DeviceConnectionInfo> ExtractConnection(winrt::hstring const& deviceId);
    [[nodiscard]] std::vector<std::pair<std::wstring, DeviceConnectionInfo>> ExtractAllConnections();
    [[nodiscard]] std::vector<std::pair<std::wstring, DeviceConnectionInfo>> GetConnectionsSnapshot() const;
    [[nodiscard]] apc::device_picker::DeviceActivitySnapshot GetDevicePickerActivitySnapshot() const;

    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Mutations //////////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    void Clear();
    void SetReconnectOnConnectionLoss(winrt::hstring const& deviceId, bool enabled);
    void SetAcceptIncomingConnections(winrt::hstring const& deviceId, bool enabled);
    void UpdateConnectionIsOpen(winrt::hstring const& deviceId, bool isOpen);
    void InsertOrUpdateConnection(winrt::hstring const& deviceId, DeviceConnectionInfo info);
    void MarkDisconnecting(winrt::hstring const& deviceId);
    void UnmarkDisconnecting(winrt::hstring const& deviceId);
    void UnmarkDisconnecting(std::wstring_view deviceId) noexcept;

private:
    /*------------------------------------------------------------------------------------------------------------*/
    /*//////// Member Variables ////////////////////////////////////////////////////////////////////////////////*/
    /*------------------------------------------------------------------------------------------------------------*/

    using ConnectionMap = std::unordered_map<std::wstring, DeviceConnectionInfo>;
    using DeviceIdSet = std::unordered_set<std::wstring>;

    ConnectionMap m_connections;
    DeviceIdSet m_disconnectingIds;
    mutable wil::srwlock m_lock;
};
