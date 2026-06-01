#include <pch.h>

#include <core/DeviceSessionStore.hpp>

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Queries //////////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

std::vector<DeviceConnectionInfo> DeviceSessionStore::ConnectedDevices() const {
    auto guard = m_lock.lock_shared();
    std::vector<DeviceConnectionInfo> result;
    result.reserve(m_connections.size());
    for (const auto& entry : m_connections) {
        if (entry.second.IsOpen) result.push_back(entry.second);
    }
    return result;
}

bool DeviceSessionStore::HasConnections() const {
    auto guard = m_lock.lock_shared();
    return std::ranges::any_of(m_connections, [](auto const& entry) { return entry.second.IsOpen; });
}

bool DeviceSessionStore::HasBusyOperations() const {
    auto guard = m_lock.lock_shared();
    auto isPendingOpen = [&](winrt::hstring const& id) {
        auto iter = m_connections.find(id);
        return iter == m_connections.end() || !iter->second.IsOpen;
    };

    return std::ranges::any_of(m_connectingIds, isPendingOpen) || !m_reconnectingIds.empty();
}

bool DeviceSessionStore::IsDeviceBusy(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    auto iter = m_connections.find(deviceId);
    const bool hasConnection = iter != m_connections.end();
    const bool isOpen = hasConnection && iter->second.IsOpen;
    return m_reconnectingIds.count(deviceId) > 0 || (m_connectingIds.count(deviceId) > 0 && !isOpen) ||
           (m_disconnectingIds.count(deviceId) > 0 && hasConnection);
}

bool DeviceSessionStore::HasConnection(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    return m_connections.count(deviceId) > 0;
}

std::optional<DeviceConnectionInfo> DeviceSessionStore::FindConnection(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    auto iter = m_connections.find(deviceId);
    if (iter != m_connections.end()) return iter->second;
    return std::nullopt;
}

bool DeviceSessionStore::IsDisconnecting(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    return m_disconnectingIds.count(deviceId) > 0;
}

bool DeviceSessionStore::IsReconnecting(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    return m_reconnectingIds.count(deviceId) > 0;
}

bool DeviceSessionStore::IsConnecting(winrt::hstring const& deviceId) const {
    auto guard = m_lock.lock_shared();
    return m_connectingIds.count(deviceId) > 0;
}

std::vector<winrt::Windows::Media::Audio::AudioPlaybackConnection> DeviceSessionStore::TakeZombieConnections() {
    auto guard = m_lock.lock_exclusive();
    return std::move(m_zombieConnections);
}

std::size_t DeviceSessionStore::ConnectionCount() const {
    auto guard = m_lock.lock_shared();
    return m_connections.size();
}

std::optional<DeviceConnectionInfo> DeviceSessionStore::ExtractConnection(winrt::hstring const& deviceId) {
    auto guard = m_lock.lock_exclusive();
    auto iter = m_connections.find(deviceId);
    if (iter == m_connections.end()) return std::nullopt;
    auto info = std::move(iter->second);
    m_connections.erase(iter);
    return info;
}

std::vector<std::pair<winrt::hstring, DeviceConnectionInfo>> DeviceSessionStore::ExtractAllConnections() {
    auto guard = m_lock.lock_exclusive();
    std::vector<std::pair<winrt::hstring, DeviceConnectionInfo>> result;
    result.reserve(m_connections.size());
    for (auto& entry : m_connections) {
        result.emplace_back(entry.first, std::move(entry.second));
    }
    m_connections.clear();
    return result;
}

std::vector<std::pair<winrt::hstring, DeviceConnectionInfo>> DeviceSessionStore::GetConnectionsSnapshot() const {
    auto guard = m_lock.lock_shared();
    std::vector<std::pair<winrt::hstring, DeviceConnectionInfo>> result;
    result.reserve(m_connections.size());
    for (auto const& entry : m_connections) {
        result.emplace_back(entry.first, entry.second);
    }
    return result;
}

/*------------------------------------------------------------------------------------------------------------*/
/*//////// Mutations //////////////////////////////////////////////////////////////////////////////////////*/
/*------------------------------------------------------------------------------------------------------------*/

void DeviceSessionStore::Clear() {
    auto guard = m_lock.lock_exclusive();
    m_connections.clear();
    m_disconnectingIds.clear();
    m_reconnectingIds.clear();
    m_connectingIds.clear();
    m_zombieConnections.clear();
}

void DeviceSessionStore::InsertOrUpdateConnection(winrt::hstring const& deviceId, DeviceConnectionInfo info) {
    auto guard = m_lock.lock_exclusive();
    m_connections[deviceId] = std::move(info);
}

void DeviceSessionStore::EraseConnection(winrt::hstring const& deviceId) {
    auto guard = m_lock.lock_exclusive();
    m_connections.erase(deviceId);
}

void DeviceSessionStore::MarkDisconnecting(winrt::hstring const& deviceId) {
    auto guard = m_lock.lock_exclusive();
    m_disconnectingIds.insert(deviceId);
}

void DeviceSessionStore::UnmarkDisconnecting(winrt::hstring const& deviceId) {
    auto guard = m_lock.lock_exclusive();
    m_disconnectingIds.erase(deviceId);
}

void DeviceSessionStore::MarkReconnecting(winrt::hstring const& deviceId) {
    auto guard = m_lock.lock_exclusive();
    m_reconnectingIds.insert(deviceId);
}

void DeviceSessionStore::UnmarkReconnecting(winrt::hstring const& deviceId) {
    auto guard = m_lock.lock_exclusive();
    m_reconnectingIds.erase(deviceId);
}

void DeviceSessionStore::MarkConnecting(winrt::hstring const& deviceId) {
    auto guard = m_lock.lock_exclusive();
    m_connectingIds.insert(deviceId);
}

void DeviceSessionStore::UnmarkConnecting(winrt::hstring const& deviceId) {
    auto guard = m_lock.lock_exclusive();
    m_connectingIds.erase(deviceId);
}

void DeviceSessionStore::AddZombie(winrt::Windows::Media::Audio::AudioPlaybackConnection connection) {
    auto guard = m_lock.lock_exclusive();
    m_zombieConnections.push_back(std::move(connection));
}

void DeviceSessionStore::SetConnectionAutoReconnect(winrt::hstring const& deviceId, bool enabled) {
    auto guard = m_lock.lock_exclusive();
    auto iter = m_connections.find(deviceId);
    if (iter != m_connections.end()) iter->second.AutoReconnect = enabled;
}

void DeviceSessionStore::UpdateConnectionIsOpen(winrt::hstring const& deviceId, bool isOpen) {
    auto guard = m_lock.lock_exclusive();
    auto iter = m_connections.find(deviceId);
    if (iter != m_connections.end()) iter->second.IsOpen = isOpen;
}
